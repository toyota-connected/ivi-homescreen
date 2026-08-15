/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * SHELL-ONLY host seam for the semantics hub. This is NOT part of the plugin
 * ABI (docs/PLUGIN_ABI.md) -- a consumer sees only ihs/ihs_semantics.h. It is
 * the private contract between libihs_shared, which owns the published
 * snapshots and the consumer registry, and the one in-process shell that owns
 * the Flutter engine and the platform thread.
 *
 *   consumer  --ihs_semantics_*-->  libihs_shared  --IhsSemanticsHost-->  shell
 *
 * The split follows what each side actually knows. libihs_shared owns
 * everything a consumer touches: snapshot lifetime, registration, notification,
 * and the allow-mask check. The shell owns the two things libihs_shared cannot
 * reach -- the engine handle that turns semantics on, and the task runner that
 * gets a dispatch onto the platform thread.
 *
 * Publication is the shell's job and happens on the platform thread, once per
 * semantics batch. ihs_semantics_publish deep-copies everything it is given
 * before returning, so the caller's buffers may be transient: pointers into the
 * shell's mutable tree are fine, and the tree may be mutated the moment publish
 * returns.
 *
 * Threading: install the host once, before the first publish. Publish is
 * platform-thread only. The shell must not call back into libihs_shared from
 * inside a host callback.
 */

#ifndef IHS_SEMANTICS_HOST_H_
#define IHS_SEMANTICS_HOST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ihs/ihs_export.h"
#include "ihs/ihs_semantics.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Operations libihs_shared forwards to the shell. Each receives @user_data.
 *
 *   set_semantics_enabled
 *       Turn engine semantics on or off. The hub reference-counts consumer
 *       registrations and calls this only on the transitions 0->1 and 1->0, so
 *       a process with no consumer never asks the engine to produce semantics
 *       at all. Called from whichever thread registers or unregisters.
 *
 *   dispatch
 *       Deliver an already-validated action. By the time this is called the
 *       hub has confirmed the consumer holds the action in its allow mask,
 *       that the node exists in the current snapshot, and that the node offers
 *       the action -- so the shell does not repeat any of that.
 *
 *       The shell is responsible for getting this onto the platform thread;
 *       the hub deliberately does not, because the task runner is the shell's.
 *       Return IHS_SEMANTICS_OK once the request is accepted for delivery, or
 *       an IhsSemanticsStatus describing why it was not. @data is valid only
 *       for the duration of the call; copy it if the post outlives the return.
 *
 * A NULL entry means the capability is absent: the hub reports
 * IHS_SEMANTICS_ERR_UNAVAILABLE rather than pretending it succeeded.
 */
typedef struct IhsSemanticsHost {
  size_t struct_size; /* sizeof(IhsSemanticsHost) */
  void* user_data;

  void (*set_semantics_enabled)(void* user_data, bool enabled);

  int (*dispatch)(void* user_data,
                  int64_t view_id,
                  int32_t node_id,
                  uint64_t action,
                  const uint8_t* data,
                  size_t data_length);

  /*
   * Synthesize a tap at a point, in the logical coordinates the published
   * rects use. Distinct from dispatch because it is a different contract, not
   * a different action: this rides the ordinary input path and is therefore
   * hit-tested, where dispatch invokes a node's handler by id regardless of
   * what is drawn on top of it.
   *
   * May be NULL on a shell that cannot synthesize input, which the hub
   * reports as IHS_SEMANTICS_ERR_UNSUPPORTED_ACTION rather than pretending.
   */
  int (*send_pointer_tap)(void* user_data, int64_t view_id, double x, double y);
} IhsSemanticsHost;

/*
 * Install the host for the implicit source, at bring-up and before the first
 * publish. Pass NULL to uninstall during teardown, after which dispatch fails
 * with IHS_SEMANTICS_ERR_UNAVAILABLE rather than calling into a shell that is
 * going away. The struct is copied; it need not outlive the call.
 *
 * For one publisher this is all that is needed. **A shell running more than
 * one engine must use ihs_semantics_source_register instead** (ABI 1.4): this
 * call installs a single host, so a second caller replaces the first and its
 * actions -- including actions read from the first engine's tree -- are
 * delivered to the second.
 */
IHS_EXPORT void ihs_semantics_set_host(const IhsSemanticsHost* host);

/*
 * Registering a source is the multi-publisher form of set_host (ABI 1.4),
 * and lives here rather than beside the addressing calls in ihs_semantics.h
 * because it carries a host: creating a publisher is a producer operation,
 * and only the shell has one to offer.
 */
typedef struct IhsSemanticsSourceDesc {
  size_t struct_size; /* sizeof(IhsSemanticsSourceDesc) */

  /*
   * Stable name, unique among registered sources. It addresses this tree from
   * outside -- an MCP resource URI is built from it -- so it should be
   * meaningful to someone who did not configure the shell, and should not
   * change across a restart of the same application.
   */
  const char* name;

  /* Where this source's dispatches go. Copied; not retained by pointer. */
  IhsSemanticsHost host;
} IhsSemanticsSourceDesc;

/*
 * Registers a publisher. Fails with IHS_SEMANTICS_ERR_INVALID for a malformed
 * desc or a name already taken -- a duplicate name would make two trees
 * indistinguishable to anything addressing them by it, which is the failure
 * this whole mechanism exists to prevent.
 */
IHS_EXPORT int ihs_semantics_source_register(const IhsSemanticsSourceDesc* desc,
                                             IhsSemanticsSource** out_source);

/*
 * Drops the source and releases its tree. Safe with NULL. Any snapshot a
 * consumer still holds stays valid: snapshots are refcounted independently of
 * the source that published them, so a consumer mid-read does not fault
 * because an engine went away.
 */
IHS_EXPORT void ihs_semantics_source_unregister(IhsSemanticsSource* source);

/*
 * One node as the shell hands it over. Distinct from IhsSemanticsNode -- this
 * is the input form, addressed by id -- because the shell has a tree of ids and
 * consumers want a flat array with index-based children. The hub does that
 * conversion once at publish rather than making every consumer do it per walk.
 *
 * Every string may be NULL and is stored as "" if so; consumers are promised
 * non-null strings, and this is where that promise is met.
 *
 * `rect` must already be in screen space: the shell composes each node's
 * transform down the traversal path, since it is the side that has the
 * transforms. Publishing parent-local rects would push that work onto every
 * consumer and they would disagree about it.
 */
typedef struct IhsSemanticsPublishNode {
  int32_t id;

  const char* identifier;
  const char* label;
  const char* hint;
  const char* value;
  const char* tooltip;

  IhsSemanticsRect rect;
  IhsSemanticsRole role;

  IhsSemanticsCheckState checked;
  IhsSemanticsTristate enabled;
  IhsSemanticsTristate selected;
  IhsSemanticsTristate toggled;
  IhsSemanticsTristate expanded;
  IhsSemanticsTristate focused;
  IhsSemanticsTristate required;

  bool hidden;
  bool read_only;
  bool obscured;
  bool live_region;
  bool focusable;
  bool a11y_focus_blocked;

  uint64_t actions; /* bitwise OR of IHS_SEMANTICS_ACTION_* */

  /* Child node ids in traversal order. The hub resolves these to indices; a
   * child id not present in this batch is dropped, since a consumer cannot do
   * anything useful with a dangling reference. */
  const int32_t* child_ids;
  size_t child_count;

  const int32_t* custom_action_ids;
  size_t custom_action_count;

  /* Numeric position; see IhsSemanticsNode for the contract consumers are
   * promised. The hub does not sanitize these: publish has_numeric_value only
   * with finite values satisfying min <= value <= max, since a consumer is
   * told it may pass them straight to a platform accessibility API. */
  bool has_numeric_value;
  double numeric_value;
  double numeric_value_min;
  double numeric_value_max;
} IhsSemanticsPublishNode;

/* A custom action declaration. Strings may be NULL and become "". */
typedef struct IhsSemanticsPublishCustomAction {
  int32_t id;
  const char* label;
  const char* hint;
} IhsSemanticsPublishCustomAction;

/*
 * A complete tree. Publication is whole-tree, not incremental: a snapshot is
 * immutable once published, so there is nothing to patch, and consumers are
 * spared reassembling a consistent view from deltas.
 *
 * Nodes must be in traversal order with the root first -- consumers are told
 * index 0 is the root.
 */
typedef struct IhsSemanticsPublishInfo {
  size_t struct_size; /* sizeof(IhsSemanticsPublishInfo) */

  const IhsSemanticsPublishNode* nodes;
  size_t node_count;

  const IhsSemanticsPublishCustomAction* custom_actions;
  size_t custom_action_count;

  /*
   * Which publisher this tree belongs to (ABI 1.4). NULL publishes to the
   * implicit source ihs_semantics_set_host creates, which is what a caller
   * that knows nothing of sources gets.
   *
   * A shell running more than one engine must set this. Without it the last
   * engine to publish owns the only tree there is, and the last to install a
   * host owns every dispatch -- so a consumer reads one application and acts
   * on another. Read is gated on struct_size, so a caller built against 1.3
   * is unaffected.
   */
  struct IhsSemanticsSource* source;
} IhsSemanticsPublishInfo;

/*
 * Publish a new tree, deep-copying everything and retiring the previous
 * snapshot. Consumers holding a reference to that snapshot keep reading it
 * safely until they release it.
 *
 * Bumps the generation and signals every registered consumer's notify fd. The
 * generation advances on every successful publish, including one that changes
 * nothing, so it orders publications rather than marking them interesting.
 *
 * Returns IHS_SEMANTICS_OK, or IHS_SEMANTICS_ERR_INVALID for a null or
 * unrecognized info. Platform thread only.
 */
IHS_EXPORT int ihs_semantics_publish(const IhsSemanticsPublishInfo* info);

/*
 * Drop the current snapshot and reset the generation counter. For shutdown,
 * so a teardown-then-restart sequence does not leave a stale tree visible.
 * Consumers holding references keep them; only the published pointer is
 * cleared, after which acquire returns null until the next publish.
 */
IHS_EXPORT void ihs_semantics_clear(void);

/*
 * Number of consumers currently registered. The shell uses this to decide
 * whether producing semantics is worth anything; the hub already drives
 * set_semantics_enabled off the same count, so this is for diagnostics rather
 * than control.
 */
IHS_EXPORT size_t ihs_semantics_consumer_count(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IHS_SEMANTICS_HOST_H_ */
