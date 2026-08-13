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

// Semantics hub: snapshot publication, consumer registry, notification, and
// the action dispatch funnel. See include/ihs/ihs_semantics.h for the consumer
// contract and include/ihs/ihs_semantics_host.h for the shell seam.

#include "ihs/ihs_semantics.h"
#include "ihs/ihs_semantics_host.h"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <unistd.h>

#include "ihs/logging.h"

namespace {

// A published tree. Immutable once built; every pointer a consumer sees points
// into the pools below, which are sized once and never touched again, so the
// pointers stay valid for as long as the snapshot does.
struct Snapshot {
  // Starts at 1 for the publisher's own reference.
  std::atomic<uint32_t> refcount{1};
  uint64_t generation = 0;

  // C-visible node array. Built last: its pointers reference the pools, which
  // must be fully populated first or a reallocation would dangle them.
  std::vector<IhsSemanticsNode> nodes;
  std::vector<IhsSemanticsCustomAction> custom_actions;

  // Backing storage. Reserved to exact size before anything points into it.
  std::vector<std::string> strings;
  std::vector<uint32_t> child_pool;
  std::vector<int32_t> custom_id_pool;
  std::vector<std::string> custom_strings;

  std::unordered_map<int32_t, size_t> id_to_index;
  std::unordered_map<int32_t, size_t> custom_id_to_index;
};

struct Consumer {
  std::string name;
  uint64_t action_allow_mask = 0;
  int notify_fd = -1;
};

// One process-wide hub. A mutex rather than a lock-free protocol: it is held
// only to swap a pointer or touch the registry, never across consumer work or
// a host call, so no consumer can stall the platform thread or another
// consumer. That is the property the design needs; lock-freedom for its own
// sake would buy nothing and cost a reclamation scheme.
struct Hub {
  std::mutex mutex;
  Snapshot* current = nullptr;  // owns one reference
  uint64_t next_generation = 1;
  std::vector<std::unique_ptr<Consumer>> consumers;
  IhsSemanticsHost host{};
  bool host_installed = false;
};

Hub& TheHub() {
  static Hub* hub = new Hub();  // leaked deliberately: outlives static teardown
  return *hub;
}

// Attribution logging for the dispatch funnel (DR-11). Opened lazily so a
// process that never registers a consumer never allocates a log context; the
// index is stable for the process once opened.
int32_t TraceContext() {
  static const int32_t ctx = ihs_log_context_open("SEMA", nullptr);
  return ctx;
}

// Emits under the hub's context, skipping the formatting entirely when the
// level is below the configured floor.
void LogInfo(const std::string& text) {
  const int32_t ctx = TraceContext();
  if (ctx < 0 || ihs_log_enabled(ctx, IHS_LEVEL_INFO) == 0) {
    return;
  }
  ihs_log(ctx, IHS_LEVEL_INFO, text.c_str(), text.size());
}

void Retain(Snapshot* snapshot) {
  snapshot->refcount.fetch_add(1, std::memory_order_relaxed);
}

void Release(Snapshot* snapshot) {
  if (snapshot == nullptr) {
    return;
  }
  // acq_rel so the delete happens-after every other thread's last use.
  if (snapshot->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    delete snapshot;
  }
}

std::string ToHex(uint64_t value) {
  static const char kDigits[] = "0123456789abcdef";
  if (value == 0) {
    return "0";
  }
  std::string out;
  while (value != 0) {
    out.insert(out.begin(), kDigits[value & 0xF]);
    value >>= 4;
  }
  return out;
}

// Consumers are promised non-null strings, so a null from the shell becomes
// "" here rather than being passed through.
void Own(std::vector<std::string>& pool, const char* s) {
  pool.emplace_back(s != nullptr ? s : "");
}

// Wakes a consumer. The value is irrelevant -- an eventfd counter or a pipe
// byte both just mean "generation changed, go look". EAGAIN means a previous
// wake has not been drained yet, which is exactly the coalescing the contract
// promises, so it is success rather than an error.
void Notify(int fd) {
  if (fd < 0) {
    return;
  }
  const uint64_t one = 1;
  ssize_t written;
  do {
    written = ::write(fd, &one, sizeof(one));
  } while (written < 0 && errno == EINTR);
}

Snapshot* BuildSnapshot(const IhsSemanticsPublishInfo* info,
                        uint64_t generation) {
  auto* snapshot = new Snapshot();
  snapshot->generation = generation;

  const size_t node_count = info->nodes != nullptr ? info->node_count : 0;
  const size_t ca_count =
      info->custom_actions != nullptr ? info->custom_action_count : 0;

  // Pass 1: fill every pool to its final size. Five strings per node and two
  // per custom action, so the reserves are exact and no later emplace can
  // reallocate and invalidate a pointer taken in pass 2.
  snapshot->strings.reserve(node_count * 5);
  snapshot->custom_strings.reserve(ca_count * 2);
  size_t total_children = 0;
  size_t total_custom_ids = 0;
  for (size_t i = 0; i < node_count; i++) {
    total_children +=
        info->nodes[i].child_ids != nullptr ? info->nodes[i].child_count : 0;
    total_custom_ids += info->nodes[i].custom_action_ids != nullptr
                            ? info->nodes[i].custom_action_count
                            : 0;
  }
  snapshot->child_pool.reserve(total_children);
  snapshot->custom_id_pool.reserve(total_custom_ids);

  for (size_t i = 0; i < node_count; i++) {
    const IhsSemanticsPublishNode& in = info->nodes[i];
    Own(snapshot->strings, in.identifier);
    Own(snapshot->strings, in.label);
    Own(snapshot->strings, in.hint);
    Own(snapshot->strings, in.value);
    Own(snapshot->strings, in.tooltip);
    snapshot->id_to_index.emplace(in.id, i);
  }
  for (size_t i = 0; i < ca_count; i++) {
    const IhsSemanticsPublishCustomAction& in = info->custom_actions[i];
    snapshot->custom_strings.emplace_back(in.label != nullptr ? in.label : "");
    snapshot->custom_strings.emplace_back(in.hint != nullptr ? in.hint : "");
    snapshot->custom_id_to_index.emplace(in.id, i);
  }

  // Pass 2: children resolve from ids to indices here, once, so a consumer
  // walking the tree never needs a lookup. An id with no node in this batch is
  // dropped: a dangling child reference is useless to a consumer and a
  // half-valid index is worse than an absent one.
  snapshot->nodes.resize(node_count);
  for (size_t i = 0; i < node_count; i++) {
    const IhsSemanticsPublishNode& in = info->nodes[i];
    IhsSemanticsNode& out = snapshot->nodes[i];

    out.id = in.id;
    out.identifier = snapshot->strings[i * 5 + 0].c_str();
    out.label = snapshot->strings[i * 5 + 1].c_str();
    out.hint = snapshot->strings[i * 5 + 2].c_str();
    out.value = snapshot->strings[i * 5 + 3].c_str();
    out.tooltip = snapshot->strings[i * 5 + 4].c_str();
    out.role = in.role;
    out.rect = in.rect;
    out.checked = in.checked;
    out.enabled = in.enabled;
    out.selected = in.selected;
    out.toggled = in.toggled;
    out.expanded = in.expanded;
    out.focused = in.focused;
    out.required = in.required;
    out.hidden = in.hidden;
    out.read_only = in.read_only;
    out.obscured = in.obscured;
    out.live_region = in.live_region;
    out.focusable = in.focusable;
    out.a11y_focus_blocked = in.a11y_focus_blocked;
    out.actions = in.actions;

    // Consumers are promised finite, ordered bounds, so a publisher that got
    // it wrong drops to "no numeric value" rather than propagating a NaN or an
    // inverted range into a platform accessibility API.
    out.has_numeric_value = in.has_numeric_value &&
                            std::isfinite(in.numeric_value) &&
                            std::isfinite(in.numeric_value_min) &&
                            std::isfinite(in.numeric_value_max) &&
                            in.numeric_value_min <= in.numeric_value &&
                            in.numeric_value <= in.numeric_value_max;
    out.numeric_value = out.has_numeric_value ? in.numeric_value : 0.0;
    out.numeric_value_min = out.has_numeric_value ? in.numeric_value_min : 0.0;
    out.numeric_value_max = out.has_numeric_value ? in.numeric_value_max : 0.0;

    const size_t child_begin = snapshot->child_pool.size();
    if (in.child_ids != nullptr) {
      for (size_t c = 0; c < in.child_count; c++) {
        const auto it = snapshot->id_to_index.find(in.child_ids[c]);
        if (it != snapshot->id_to_index.end()) {
          snapshot->child_pool.push_back(static_cast<uint32_t>(it->second));
        }
      }
    }
    out.child_count = snapshot->child_pool.size() - child_begin;
    out.child_indices = nullptr;  // patched in pass 3

    const size_t ca_begin = snapshot->custom_id_pool.size();
    if (in.custom_action_ids != nullptr) {
      for (size_t c = 0; c < in.custom_action_count; c++) {
        snapshot->custom_id_pool.push_back(in.custom_action_ids[c]);
      }
    }
    out.custom_action_count = snapshot->custom_id_pool.size() - ca_begin;
    out.custom_action_ids = nullptr;  // patched in pass 3
  }

  snapshot->custom_actions.resize(ca_count);
  for (size_t i = 0; i < ca_count; i++) {
    snapshot->custom_actions[i].id = info->custom_actions[i].id;
    snapshot->custom_actions[i].label =
        snapshot->custom_strings[i * 2 + 0].c_str();
    snapshot->custom_actions[i].hint =
        snapshot->custom_strings[i * 2 + 1].c_str();
  }

  // Pass 3: the pools are final, so their addresses are now stable and the
  // per-node spans can point into them. Splitting this out is what makes the
  // single-allocation pools safe.
  size_t child_cursor = 0;
  size_t ca_cursor = 0;
  for (size_t i = 0; i < node_count; i++) {
    IhsSemanticsNode& out = snapshot->nodes[i];
    out.child_indices =
        out.child_count > 0 ? &snapshot->child_pool[child_cursor] : nullptr;
    child_cursor += out.child_count;
    out.custom_action_ids = out.custom_action_count > 0
                                ? &snapshot->custom_id_pool[ca_cursor]
                                : nullptr;
    ca_cursor += out.custom_action_count;
  }

  return snapshot;
}

}  // namespace

extern "C" {

uint64_t ihs_semantics_snapshot_generation(
    const IhsSemanticsSnapshot* snapshot) {
  if (snapshot == nullptr) {
    return 0;
  }
  return reinterpret_cast<const Snapshot*>(snapshot)->generation;
}

size_t ihs_semantics_snapshot_node_count(const IhsSemanticsSnapshot* snapshot) {
  if (snapshot == nullptr) {
    return 0;
  }
  return reinterpret_cast<const Snapshot*>(snapshot)->nodes.size();
}

const IhsSemanticsNode* ihs_semantics_snapshot_node_at(
    const IhsSemanticsSnapshot* snapshot,
    size_t index) {
  if (snapshot == nullptr) {
    return nullptr;
  }
  const auto* s = reinterpret_cast<const Snapshot*>(snapshot);
  return index < s->nodes.size() ? &s->nodes[index] : nullptr;
}

const IhsSemanticsNode* ihs_semantics_snapshot_node_by_id(
    const IhsSemanticsSnapshot* snapshot,
    int32_t node_id) {
  if (snapshot == nullptr) {
    return nullptr;
  }
  const auto* s = reinterpret_cast<const Snapshot*>(snapshot);
  const auto it = s->id_to_index.find(node_id);
  return it != s->id_to_index.end() ? &s->nodes[it->second] : nullptr;
}

bool ihs_semantics_node_numeric_value(const IhsSemanticsNode* node,
                                      double* out_value,
                                      double* out_min,
                                      double* out_max) {
  if (node == nullptr || !node->has_numeric_value) {
    return false;
  }
  if (out_value != nullptr) {
    *out_value = node->numeric_value;
  }
  if (out_min != nullptr) {
    *out_min = node->numeric_value_min;
  }
  if (out_max != nullptr) {
    *out_max = node->numeric_value_max;
  }
  return true;
}

const IhsSemanticsCustomAction* ihs_semantics_find_custom_action(
    const IhsSemanticsSnapshot* snapshot,
    int32_t action_id) {
  if (snapshot == nullptr) {
    return nullptr;
  }
  const auto* s = reinterpret_cast<const Snapshot*>(snapshot);
  const auto it = s->custom_id_to_index.find(action_id);
  return it != s->custom_id_to_index.end() ? &s->custom_actions[it->second]
                                           : nullptr;
}

const IhsSemanticsSnapshot* ihs_semantics_acquire_snapshot(void) {
  Hub& hub = TheHub();
  std::lock_guard<std::mutex> lock(hub.mutex);
  if (hub.current == nullptr) {
    return nullptr;
  }
  // Under the lock, so this cannot race the publisher dropping the last
  // reference: the retire in publish happens with the lock held too.
  Retain(hub.current);
  return reinterpret_cast<const IhsSemanticsSnapshot*>(hub.current);
}

void ihs_semantics_release_snapshot(const IhsSemanticsSnapshot* snapshot) {
  if (snapshot == nullptr) {
    return;
  }
  Release(const_cast<Snapshot*>(reinterpret_cast<const Snapshot*>(snapshot)));
}

void ihs_semantics_set_host(const IhsSemanticsHost* host) {
  Hub& hub = TheHub();
  std::lock_guard<std::mutex> lock(hub.mutex);
  if (host == nullptr || host->struct_size < sizeof(IhsSemanticsHost)) {
    hub.host = IhsSemanticsHost{};
    hub.host_installed = false;
    return;
  }
  hub.host = *host;
  hub.host_installed = true;
}

int ihs_semantics_publish(const IhsSemanticsPublishInfo* info) {
  if (info == nullptr || info->struct_size < sizeof(IhsSemanticsPublishInfo)) {
    return IHS_SEMANTICS_ERR_INVALID;
  }

  Hub& hub = TheHub();

  // Build outside the lock: this is the expensive part of a publish and the
  // platform thread should not hold the registry closed while it copies a
  // tree. Only the swap needs the lock.
  Snapshot* built = nullptr;
  uint64_t generation;
  {
    std::lock_guard<std::mutex> lock(hub.mutex);
    generation = hub.next_generation++;
  }
  built = BuildSnapshot(info, generation);

  std::vector<int> notify_fds;
  Snapshot* retired = nullptr;
  {
    std::lock_guard<std::mutex> lock(hub.mutex);
    retired = hub.current;
    hub.current = built;
    notify_fds.reserve(hub.consumers.size());
    for (const auto& consumer : hub.consumers) {
      notify_fds.push_back(consumer->notify_fd);
    }
  }
  Release(retired);  // drops the publisher's reference, not any consumer's

  // Notify outside the lock: a write to a full pipe must not be able to hold
  // the registry closed against every other consumer.
  for (const int fd : notify_fds) {
    Notify(fd);
  }
  return IHS_SEMANTICS_OK;
}

void ihs_semantics_clear(void) {
  Hub& hub = TheHub();
  Snapshot* retired = nullptr;
  {
    std::lock_guard<std::mutex> lock(hub.mutex);
    retired = hub.current;
    hub.current = nullptr;
    hub.next_generation = 1;
  }
  Release(retired);
}

size_t ihs_semantics_consumer_count(void) {
  Hub& hub = TheHub();
  std::lock_guard<std::mutex> lock(hub.mutex);
  return hub.consumers.size();
}

int ihs_semantics_register(const IhsSemanticsConsumerDesc* desc,
                           IhsSemanticsConsumer** out_consumer) {
  if (desc == nullptr || out_consumer == nullptr ||
      desc->struct_size < sizeof(IhsSemanticsConsumerDesc) ||
      desc->name == nullptr) {
    return IHS_SEMANTICS_ERR_INVALID;
  }

  auto consumer = std::make_unique<Consumer>();
  consumer->name = desc->name;
  consumer->action_allow_mask = desc->action_allow_mask;
  consumer->notify_fd = desc->notify_fd;

  Consumer* raw = consumer.get();
  bool first = false;
  IhsSemanticsHost host{};
  {
    Hub& hub = TheHub();
    std::lock_guard<std::mutex> lock(hub.mutex);
    first = hub.consumers.empty();
    hub.consumers.push_back(std::move(consumer));
    host = hub.host;
  }

  LogInfo("consumer '" + raw->name + "' registered, mask 0x" +
          ToHex(raw->action_allow_mask));

  // Outside the lock: the shell's enable path reaches into the engine, and
  // holding the registry across it would let engine work block every consumer.
  if (first && host.set_semantics_enabled != nullptr) {
    host.set_semantics_enabled(host.user_data, true);
  }

  *out_consumer = reinterpret_cast<IhsSemanticsConsumer*>(raw);
  return IHS_SEMANTICS_OK;
}

void ihs_semantics_unregister(IhsSemanticsConsumer* consumer) {
  if (consumer == nullptr) {
    return;
  }
  auto* raw = reinterpret_cast<Consumer*>(consumer);

  bool last = false;
  IhsSemanticsHost host{};
  std::unique_ptr<Consumer> owned;
  {
    Hub& hub = TheHub();
    std::lock_guard<std::mutex> lock(hub.mutex);
    for (auto it = hub.consumers.begin(); it != hub.consumers.end(); ++it) {
      if (it->get() == raw) {
        owned = std::move(*it);
        hub.consumers.erase(it);
        break;
      }
    }
    if (owned == nullptr) {
      return;  // not registered, or already unregistered
    }
    last = hub.consumers.empty();
    host = hub.host;
  }

  LogInfo("consumer '" + owned->name + "' unregistered");

  // Dispatch is synchronous through to the host, so removal from the registry
  // under the lock is itself the drain: no dispatch for this consumer can be
  // in flight once we are here, and none can start.
  if (last && host.set_semantics_enabled != nullptr) {
    host.set_semantics_enabled(host.user_data, false);
  }
}

int ihs_semantics_send_pointer_tap(IhsSemanticsConsumer* consumer,
                                   const int64_t view_id,
                                   const double x,
                                   const double y) {
  int status = IHS_SEMANTICS_OK;
  std::string name;

  if (consumer == nullptr || !std::isfinite(x) || !std::isfinite(y)) {
    status = IHS_SEMANTICS_ERR_INVALID;
  }

  IhsSemanticsHost host{};
  uint64_t allow_mask = 0;
  if (status == IHS_SEMANTICS_OK) {
    auto* raw = reinterpret_cast<Consumer*>(consumer);
    Hub& hub = TheHub();
    std::lock_guard<std::mutex> lock(hub.mutex);
    bool found = false;
    for (const auto& registered : hub.consumers) {
      if (registered.get() == raw) {
        found = true;
        break;
      }
    }
    if (!found) {
      status = IHS_SEMANTICS_ERR_INVALID;
    } else {
      name = raw->name;
      allow_mask = raw->action_allow_mask;
      host = hub.host;
    }
  }

  // A synthesized tap is a tap. A consumer denied the action must not be able
  // to perform it by coordinate instead, or the mask would arbitrate nothing.
  if (status == IHS_SEMANTICS_OK &&
      (IHS_SEMANTICS_ACTION_TAP & ~allow_mask) != 0) {
    status = IHS_SEMANTICS_ERR_DENIED;
  }

  // No node is consulted deliberately: the point of this path is to reach
  // what the tree does not describe, so there is nothing to validate against
  // and nothing that can confirm the tap landed anywhere.
  if (status == IHS_SEMANTICS_OK) {
    if (host.send_pointer_tap == nullptr) {
      status = IHS_SEMANTICS_ERR_UNSUPPORTED_ACTION;
    } else {
      status = host.send_pointer_tap(host.user_data, view_id, x, y);
    }
  }

  // Traced like a dispatch, and with the coordinates: an action that names no
  // node is exactly the one an audit needs the target of.
  LogInfo("pointer tap consumer='" +
          (name.empty() ? std::string("<unregistered>") : name) +
          "' at=" + std::to_string(x) + "," + std::to_string(y) +
          " status=" + std::to_string(status));
  return status;
}

int ihs_semantics_dispatch(IhsSemanticsConsumer* consumer,
                           int64_t view_id,
                           int32_t node_id,
                           uint64_t action,
                           const uint8_t* data,
                           size_t data_length,
                           IhsSemanticsDoneCallback done,
                           void* user_data) {
  int status = IHS_SEMANTICS_OK;
  std::string name;

  if (consumer == nullptr || action == 0) {
    status = IHS_SEMANTICS_ERR_INVALID;
  }

  IhsSemanticsHost host{};
  uint64_t allow_mask = 0;
  if (status == IHS_SEMANTICS_OK) {
    auto* raw = reinterpret_cast<Consumer*>(consumer);
    Hub& hub = TheHub();
    std::lock_guard<std::mutex> lock(hub.mutex);
    bool found = false;
    for (const auto& registered : hub.consumers) {
      if (registered.get() == raw) {
        found = true;
        break;
      }
    }
    if (!found) {
      status = IHS_SEMANTICS_ERR_INVALID;
    } else {
      name = raw->name;
      allow_mask = raw->action_allow_mask;
      host = hub.host;
    }
  }

  // Mask first: a denial is a policy answer and must not depend on whether the
  // node happens to exist, or a caller could probe the tree with actions it is
  // not allowed to invoke.
  if (status == IHS_SEMANTICS_OK && (action & ~allow_mask) != 0) {
    status = IHS_SEMANTICS_ERR_DENIED;
  }

  if (status == IHS_SEMANTICS_OK) {
    const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
    if (snapshot == nullptr) {
      status = IHS_SEMANTICS_ERR_UNAVAILABLE;
    } else {
      const IhsSemanticsNode* node =
          ihs_semantics_snapshot_node_by_id(snapshot, node_id);
      if (node == nullptr) {
        status = IHS_SEMANTICS_ERR_NO_SUCH_NODE;
      } else if ((action & node->actions) != action) {
        // The framework silently drops an action the widget registered no
        // handler for, so refusing here is the difference between the caller
        // learning something and the call vanishing.
        status = IHS_SEMANTICS_ERR_UNSUPPORTED_ACTION;
      }
      ihs_semantics_release_snapshot(snapshot);
    }
  }

  if (status == IHS_SEMANTICS_OK) {
    if (host.dispatch == nullptr) {
      status = IHS_SEMANTICS_ERR_UNAVAILABLE;
    } else {
      status = host.dispatch(host.user_data, view_id, node_id, action, data,
                             data_length);
    }
  }

  // Attribution is the point of the funnel: every action that reaches the UI
  // is traceable to a named actor, and so is every one that was refused.
  LogInfo("dispatch consumer='" +
          (name.empty() ? std::string("<unregistered>") : name) +
          "' node=" + std::to_string(node_id) + " action=0x" + ToHex(action) +
          " status=" + std::to_string(status));

  if (done != nullptr) {
    done(status, user_data);
  }
  return status;
}

}  // extern "C"

namespace ihs::semantics {

// Function pointers over the flat entry points above -- the same functions, so
// there is no second implementation to keep in step. Reaching them through
// IhsApi is what lets a consumer ask whether the hub exists instead of failing
// to load against a library built without it.
const IhsSemanticsApi* semantics_api() noexcept {
  static const IhsSemanticsApi api = {
      sizeof(IhsSemanticsApi),
      &ihs_semantics_acquire_snapshot,
      &ihs_semantics_release_snapshot,
      &ihs_semantics_snapshot_generation,
      &ihs_semantics_snapshot_node_count,
      &ihs_semantics_snapshot_node_at,
      &ihs_semantics_snapshot_node_by_id,
      &ihs_semantics_find_custom_action,
      &ihs_semantics_node_numeric_value,
      &ihs_semantics_register,
      &ihs_semantics_unregister,
      &ihs_semantics_dispatch,
      &ihs_semantics_send_pointer_tap,
  };
  return &api;
}

}  // namespace ihs::semantics
