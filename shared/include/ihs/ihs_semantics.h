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
 * ihs_shared semantics hub: the single fan-out point for the Flutter semantics
 * tree, and the single funnel for actions dispatched back into it.
 *
 * The shell mirrors each FlutterSemanticsUpdate2 into a mutable tree on the
 * platform thread. That tree is never exposed. Instead, at every batch
 * boundary the hub publishes an immutable, refcounted IhsSemanticsSnapshot;
 * consumers read snapshots and never touch the mutable tree, so a slow
 * consumer cannot stall the platform thread.
 *
 * This surface is deliberately free of any dependency on embedder.h. A
 * consumer may be an out-of-tree FFI plugin that has no Flutter headers at
 * all, so roles, states and actions are re-expressed here rather than
 * forwarded. See docs/PLUGIN_ABI.md for the boundary rules.
 *
 * Lifecycle of one consumer:
 *   1. Fill an IhsSemanticsConsumerDesc (name, allowed actions, notify fd) and
 *      call ihs_semantics_register. Registration is process-global and may
 *      happen at any time -- a client attaching mid-session needs no restart.
 *   2. The hub reference-counts registrations to drive semantics on and off in
 *      the engine. With no consumer registered the engine is never asked to
 *      produce semantics at all, so a build with adapters compiled in but none
 *      configured pays nothing.
 *   3. Wait on the notify fd. Each readable event means "a newer generation
 *      exists"; events are edge-coalesced, so a consumer that wakes once for
 *      three updates has missed nothing. Always fetch the latest snapshot on
 *      wake rather than trying to track individual updates.
 *   4. ihs_semantics_acquire_snapshot for the current tree; release it when
 *      done. Acquire/release are lock-free and callable from any thread.
 *   5. ihs_semantics_dispatch to invoke an action. The hub serializes onto the
 *      platform thread, enforces the consumer's action_allow_mask, and traces
 *      the dispatch with the consumer name so "which actor did this" is
 *      answerable after the fact.
 *   6. ihs_semantics_unregister drains any in-flight dispatch for that
 *      consumer before returning.
 *
 * Notification is by file descriptor, not callback, on purpose: one consumer
 * that blocks in a callback would delay notification of every other, and
 * "must not block" is a contract that erodes. Consumers poll in their own
 * event loop instead.
 *
 * Threading: register/unregister and dispatch are callable from any thread.
 * Snapshot acquire/release are callable from any thread and are lock-free. The
 * done callback passed to dispatch is invoked on the platform thread. Snapshot
 * contents are immutable for the lifetime of the reference held.
 */

#ifndef IHS_SEMANTICS_H_
#define IHS_SEMANTICS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ihs/ihs_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An immutable published tree. Refcounted; contents never change for the
 * lifetime of a reference. */
typedef struct IhsSemanticsSnapshot IhsSemanticsSnapshot;

/* A registered consumer. Valid until ihs_semantics_unregister returns. */
typedef struct IhsSemanticsConsumer IhsSemanticsConsumer;

/* Status codes. Negative values are errors. */
typedef enum IhsSemanticsStatus {
  IHS_SEMANTICS_OK = 0,
  /* A required argument was null, or a struct_size was not recognized. */
  IHS_SEMANTICS_ERR_INVALID = -1,
  /* The action is not set in the consumer's action_allow_mask. This is a
   * policy denial, not a missing node -- see IhsSemanticsConsumerDesc. */
  IHS_SEMANTICS_ERR_DENIED = -2,
  /* No node with the requested id in the current tree. */
  IHS_SEMANTICS_ERR_NO_SUCH_NODE = -3,
  /* The node exists but does not offer the requested action. */
  IHS_SEMANTICS_ERR_UNSUPPORTED_ACTION = -4,
  /* The hub is not running: semantics never started, or is shutting down. */
  IHS_SEMANTICS_ERR_UNAVAILABLE = -5
} IhsSemanticsStatus;

/*
 * A property that may not apply at all. The distinction matters: a checkbox
 * that is unchecked and a node for which "checked" is meaningless are not the
 * same thing, and flattening both to false loses the difference. Consumers
 * should surface all three states rather than collapsing to a bool.
 */
typedef enum IhsSemanticsTristate {
  IHS_SEMANTICS_TRISTATE_NONE = 0, /* property does not apply to this node */
  IHS_SEMANTICS_TRISTATE_TRUE = 1,
  IHS_SEMANTICS_TRISTATE_FALSE = 2
} IhsSemanticsTristate;

/* Check state, which unlike the other tristates has a fourth value. */
typedef enum IhsSemanticsCheckState {
  IHS_SEMANTICS_CHECK_NONE = 0, /* not a checkable node */
  IHS_SEMANTICS_CHECK_TRUE = 1,
  IHS_SEMANTICS_CHECK_FALSE = 2,
  IHS_SEMANTICS_CHECK_MIXED = 3
} IhsSemanticsCheckState;

/*
 * Backend-neutral role. Derived from the framework's flags rather than
 * forwarded, so it is stable across engine revisions that reshuffle their own
 * flag representation. IHS_SEMANTICS_ROLE_UNKNOWN is the honest fallback and
 * consumers must handle it.
 */
typedef enum IhsSemanticsRole {
  IHS_SEMANTICS_ROLE_UNKNOWN = 0,
  IHS_SEMANTICS_ROLE_WINDOW = 1,
  IHS_SEMANTICS_ROLE_BUTTON = 2,
  IHS_SEMANTICS_ROLE_TEXT_INPUT = 3,
  IHS_SEMANTICS_ROLE_MULTILINE_TEXT_INPUT = 4,
  IHS_SEMANTICS_ROLE_PASSWORD_INPUT = 5,
  IHS_SEMANTICS_ROLE_SLIDER = 6,
  IHS_SEMANTICS_ROLE_SWITCH = 7,
  IHS_SEMANTICS_ROLE_CHECK_BOX = 8,
  IHS_SEMANTICS_ROLE_RADIO_BUTTON = 9,
  IHS_SEMANTICS_ROLE_LINK = 10,
  IHS_SEMANTICS_ROLE_IMAGE = 11,
  IHS_SEMANTICS_ROLE_HEADING = 12,
  IHS_SEMANTICS_ROLE_SCROLL_VIEW = 13,
  IHS_SEMANTICS_ROLE_PANE = 14,
  IHS_SEMANTICS_ROLE_LABEL = 15,
  IHS_SEMANTICS_ROLE_GENERIC_CONTAINER = 16
} IhsSemanticsRole;

/*
 * Action bits, for IhsSemanticsNode::actions and for the per-consumer
 * action_allow_mask. Values are this ABI's own and are never renumbered;
 * they are not the framework's bit positions and must not be assumed equal to
 * them.
 *
 * An action absent from a node's `actions` is not invocable on it: the
 * framework registered no handler, and dispatching anyway is silently dropped
 * on the far side. Check before dispatching so the caller sees a real error.
 */
#define IHS_SEMANTICS_ACTION_TAP UINT64_C(0x0000000000000001)
#define IHS_SEMANTICS_ACTION_LONG_PRESS UINT64_C(0x0000000000000002)
#define IHS_SEMANTICS_ACTION_SCROLL_LEFT UINT64_C(0x0000000000000004)
#define IHS_SEMANTICS_ACTION_SCROLL_RIGHT UINT64_C(0x0000000000000008)
#define IHS_SEMANTICS_ACTION_SCROLL_UP UINT64_C(0x0000000000000010)
#define IHS_SEMANTICS_ACTION_SCROLL_DOWN UINT64_C(0x0000000000000020)
#define IHS_SEMANTICS_ACTION_INCREASE UINT64_C(0x0000000000000040)
#define IHS_SEMANTICS_ACTION_DECREASE UINT64_C(0x0000000000000080)
#define IHS_SEMANTICS_ACTION_SHOW_ON_SCREEN UINT64_C(0x0000000000000100)
#define IHS_SEMANTICS_ACTION_MOVE_CURSOR_FORWARD_CHAR \
  UINT64_C(0x0000000000000200)
#define IHS_SEMANTICS_ACTION_MOVE_CURSOR_BACKWARD_CHAR \
  UINT64_C(0x0000000000000400)
#define IHS_SEMANTICS_ACTION_SET_SELECTION UINT64_C(0x0000000000000800)
#define IHS_SEMANTICS_ACTION_COPY UINT64_C(0x0000000000001000)
#define IHS_SEMANTICS_ACTION_CUT UINT64_C(0x0000000000002000)
#define IHS_SEMANTICS_ACTION_PASTE UINT64_C(0x0000000000004000)
#define IHS_SEMANTICS_ACTION_DID_GAIN_A11Y_FOCUS UINT64_C(0x0000000000008000)
#define IHS_SEMANTICS_ACTION_DID_LOSE_A11Y_FOCUS UINT64_C(0x0000000000010000)
#define IHS_SEMANTICS_ACTION_CUSTOM_ACTION UINT64_C(0x0000000000020000)
#define IHS_SEMANTICS_ACTION_DISMISS UINT64_C(0x0000000000040000)
#define IHS_SEMANTICS_ACTION_MOVE_CURSOR_FORWARD_WORD \
  UINT64_C(0x0000000000080000)
#define IHS_SEMANTICS_ACTION_MOVE_CURSOR_BACKWARD_WORD \
  UINT64_C(0x0000000000100000)
#define IHS_SEMANTICS_ACTION_SET_TEXT UINT64_C(0x0000000000200000)
#define IHS_SEMANTICS_ACTION_FOCUS UINT64_C(0x0000000000400000)
#define IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET UINT64_C(0x0000000000800000)
#define IHS_SEMANTICS_ACTION_EXPAND UINT64_C(0x0000000001000000)
#define IHS_SEMANTICS_ACTION_COLLAPSE UINT64_C(0x0000000002000000)

/*
 * Every action this ABI version knows. Useful as an allow-mask for a fully
 * trusted consumer; prefer naming the bits you actually need.
 *
 * Spelled as the OR of the named bits rather than a literal so it cannot drift
 * out of step with them when an action is added.
 */
#define IHS_SEMANTICS_ACTION_ALL                                          \
  (IHS_SEMANTICS_ACTION_TAP | IHS_SEMANTICS_ACTION_LONG_PRESS |           \
   IHS_SEMANTICS_ACTION_SCROLL_LEFT | IHS_SEMANTICS_ACTION_SCROLL_RIGHT | \
   IHS_SEMANTICS_ACTION_SCROLL_UP | IHS_SEMANTICS_ACTION_SCROLL_DOWN |    \
   IHS_SEMANTICS_ACTION_INCREASE | IHS_SEMANTICS_ACTION_DECREASE |        \
   IHS_SEMANTICS_ACTION_SHOW_ON_SCREEN |                                  \
   IHS_SEMANTICS_ACTION_MOVE_CURSOR_FORWARD_CHAR |                        \
   IHS_SEMANTICS_ACTION_MOVE_CURSOR_BACKWARD_CHAR |                       \
   IHS_SEMANTICS_ACTION_SET_SELECTION | IHS_SEMANTICS_ACTION_COPY |       \
   IHS_SEMANTICS_ACTION_CUT | IHS_SEMANTICS_ACTION_PASTE |                \
   IHS_SEMANTICS_ACTION_DID_GAIN_A11Y_FOCUS |                             \
   IHS_SEMANTICS_ACTION_DID_LOSE_A11Y_FOCUS |                             \
   IHS_SEMANTICS_ACTION_CUSTOM_ACTION | IHS_SEMANTICS_ACTION_DISMISS |    \
   IHS_SEMANTICS_ACTION_MOVE_CURSOR_FORWARD_WORD |                        \
   IHS_SEMANTICS_ACTION_MOVE_CURSOR_BACKWARD_WORD |                       \
   IHS_SEMANTICS_ACTION_SET_TEXT | IHS_SEMANTICS_ACTION_FOCUS |           \
   IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET | IHS_SEMANTICS_ACTION_EXPAND |  \
   IHS_SEMANTICS_ACTION_COLLAPSE)

/*
 * The two accessibility-focus actions move a screen reader's cursor. A
 * consumer driving the UI programmatically should not hold these: doing so
 * lets it yank the cursor out from under someone reading the screen. This is
 * the intended allow-mask for an automation consumer.
 */
#define IHS_SEMANTICS_ACTION_NO_A11Y_FOCUS                                \
  (IHS_SEMANTICS_ACTION_ALL & ~IHS_SEMANTICS_ACTION_DID_GAIN_A11Y_FOCUS & \
   ~IHS_SEMANTICS_ACTION_DID_LOSE_A11Y_FOCUS)

/* A rectangle in screen space. The publisher composes each node's transform
 * down the traversal path once, at snapshot build, so consumers never do
 * matrix work and every rect in a snapshot is directly comparable. */
typedef struct IhsSemanticsRect {
  double left;
  double top;
  double right;
  double bottom;
} IhsSemanticsRect;

/*
 * A custom action declared by the application. Referenced by nodes via
 * custom_action_ids; the label and hint live here once rather than being
 * repeated on every referencing node.
 */
typedef struct IhsSemanticsCustomAction {
  int32_t id;
  const char* label; /* never null; "" when undeclared */
  const char* hint;  /* never null; "" when undeclared */
} IhsSemanticsCustomAction;

/*
 * One node. All pointers are owned by the snapshot and valid exactly as long
 * as the reference that produced them; copy anything you need to outlive it.
 * All string fields are non-null -- an absent string is "", never NULL, so
 * consumers need no null checks.
 */
typedef struct IhsSemanticsNode {
  /* Stable within a tree but not across rebuilds. Use `identifier` where the
   * application provides one. */
  int32_t id;

  /*
   * Application-assigned stable identifier, or "" when unannotated.
   *
   * Note this is empty on every node for engines that predate the field, not
   * only for unannotated ones, so a consumer cannot distinguish "the app did
   * not annotate" from "this engine cannot report it". Treat emptiness as
   * "fall back to label and role" in both cases.
   */
  const char* identifier;

  IhsSemanticsRole role;

  const char* label;
  const char* hint;
  const char* value;
  const char* tooltip;

  /* Screen space, transforms already composed. */
  IhsSemanticsRect rect;

  /* State. Tristates carry "does not apply" as a distinct value; see
   * IhsSemanticsTristate before collapsing any of these to a bool. */
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

  /* Bitwise OR of IHS_SEMANTICS_ACTION_*. */
  uint64_t actions;

  /* Indices into IhsSemanticsSnapshot::nodes, in traversal order. Indices, not
   * ids, so a consumer walks the tree without a lookup per step. */
  const uint32_t* child_indices;
  size_t child_count;

  /* Ids into IhsSemanticsSnapshot::custom_actions; resolve with
   * ihs_semantics_find_custom_action. */
  const int32_t* custom_action_ids;
  size_t custom_action_count;
} IhsSemanticsNode;

/*
 * A published tree. Access the fields through the accessors below rather than
 * dereferencing: the struct is opaque so it can gain members without breaking
 * consumers built against an older header.
 */

/* Monotonically increasing. A consumer that has already seen this generation
 * has nothing new to read. Never wraps in any realistic session. */
IHS_EXPORT uint64_t
ihs_semantics_snapshot_generation(const IhsSemanticsSnapshot* snapshot);

/* Node count. Index 0 is the root when count is non-zero. */
IHS_EXPORT size_t
ihs_semantics_snapshot_node_count(const IhsSemanticsSnapshot* snapshot);

/* Node by index, or null when out of range. Pointer valid for the life of the
 * reference. */
IHS_EXPORT const IhsSemanticsNode* ihs_semantics_snapshot_node_at(
    const IhsSemanticsSnapshot* snapshot,
    size_t index);

/* Node by tree id, or null when absent. O(1). */
IHS_EXPORT const IhsSemanticsNode* ihs_semantics_snapshot_node_by_id(
    const IhsSemanticsSnapshot* snapshot,
    int32_t node_id);

/* Custom action declaration by id, or null when never declared. */
IHS_EXPORT const IhsSemanticsCustomAction* ihs_semantics_find_custom_action(
    const IhsSemanticsSnapshot* snapshot,
    int32_t action_id);

/*
 * Take a reference on the current tree, or null when semantics is not running.
 * Lock-free and callable from any thread. Every successful acquire must be
 * paired with exactly one release.
 */
IHS_EXPORT const IhsSemanticsSnapshot* ihs_semantics_acquire_snapshot(void);

/* Drop a reference. Passing null is a no-op. */
IHS_EXPORT void ihs_semantics_release_snapshot(
    const IhsSemanticsSnapshot* snapshot);

/*
 * Consumer registration.
 *
 * notify_fd is owned by the caller and must outlive the registration. The hub
 * writes to it when a newer generation is published and never reads or closes
 * it. An eventfd is the expected choice; any writable fd the caller can poll
 * works. Pass -1 for a consumer that only ever polls on its own schedule.
 */
typedef struct IhsSemanticsConsumerDesc {
  size_t struct_size; /* sizeof(IhsSemanticsConsumerDesc) */

  /* Stable, human-meaningful, and used in dispatch traces. Not optional: the
   * audit trail is worth more than the convenience of skipping it. */
  const char* name;

  /* Bitwise OR of IHS_SEMANTICS_ACTION_*. Dispatch of anything outside this
   * set fails with IHS_SEMANTICS_ERR_DENIED and never reaches the framework.
   * See IHS_SEMANTICS_ACTION_NO_A11Y_FOCUS for the automation default. */
  uint64_t action_allow_mask;

  /* Written on publish; -1 to opt out. */
  int notify_fd;
} IhsSemanticsConsumerDesc;

/*
 * Register. On success *out_consumer receives a handle. Registering the first
 * consumer causes semantics to be enabled in the engine; the first snapshot
 * follows asynchronously, so a consumer should expect
 * ihs_semantics_acquire_snapshot to return null until one arrives.
 */
IHS_EXPORT int ihs_semantics_register(const IhsSemanticsConsumerDesc* desc,
                                      IhsSemanticsConsumer** out_consumer);

/*
 * Unregister. Drains any dispatch already in flight for this consumer before
 * returning, so no done callback can fire afterwards. Unregistering the last
 * consumer disables semantics in the engine. Snapshots the caller still holds
 * remain valid: their lifetime is independent of any consumer's.
 */
IHS_EXPORT void ihs_semantics_unregister(IhsSemanticsConsumer* consumer);

/*
 * Invoked on the platform thread when a dispatch has been handed to the
 * framework. `status` is IHS_SEMANTICS_OK, or the reason it went no further.
 *
 * Delivery to the framework is not the same as the UI having reacted: the
 * framework routes the action to whatever handler the widget registered, and
 * that may complete later, or do nothing at all. A caller that needs to know
 * what changed should compare snapshot generations rather than infer anything
 * from this callback.
 */
typedef void (*IhsSemanticsDoneCallback)(int status, void* user_data);

/*
 * Dispatch an action.
 *
 * The only path back into the framework. Consumers never call the engine
 * directly, so the hub can serialize onto the platform thread, enforce the
 * consumer's allow mask, and attribute every dispatch to a named actor.
 *
 * `data`/`data_length` carry arguments for the actions that take them (set
 * text, set selection, scroll to offset); pass NULL/0 otherwise. The buffer is
 * copied before this returns.
 *
 * Returns immediately; `done` fires on the platform thread and may be null.
 * Callable from any thread.
 */
IHS_EXPORT int ihs_semantics_dispatch(IhsSemanticsConsumer* consumer,
                                      int64_t view_id,
                                      int32_t node_id,
                                      uint64_t action,
                                      const uint8_t* data,
                                      size_t data_length,
                                      IhsSemanticsDoneCallback done,
                                      void* user_data);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IHS_SEMANTICS_H_ */
