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

#include "accesskit_consumer.h"

#include <accesskit.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

#include "ihs/ihs_semantics.h"

#include "logging/logging.h"

namespace accessibility {

namespace {

// The adapter is process-global because AccessKit's callbacks carry only a
// userdata pointer and the unix adapter owns its own thread; there is exactly
// one screen-reader connection per process regardless.
accesskit_unix_adapter* g_adapter = nullptr;

}  // namespace

accesskit_role ToAccessKitRole(const IhsSemanticsRole role) {
  switch (role) {
    case IHS_SEMANTICS_ROLE_WINDOW:
      return ACCESSKIT_ROLE_WINDOW;
    case IHS_SEMANTICS_ROLE_BUTTON:
      return ACCESSKIT_ROLE_BUTTON;
    case IHS_SEMANTICS_ROLE_TEXT_INPUT:
      return ACCESSKIT_ROLE_TEXT_INPUT;
    case IHS_SEMANTICS_ROLE_MULTILINE_TEXT_INPUT:
      return ACCESSKIT_ROLE_MULTILINE_TEXT_INPUT;
    case IHS_SEMANTICS_ROLE_PASSWORD_INPUT:
      return ACCESSKIT_ROLE_PASSWORD_INPUT;
    case IHS_SEMANTICS_ROLE_SLIDER:
      return ACCESSKIT_ROLE_SLIDER;
    case IHS_SEMANTICS_ROLE_SWITCH:
      return ACCESSKIT_ROLE_SWITCH;
    case IHS_SEMANTICS_ROLE_CHECK_BOX:
      return ACCESSKIT_ROLE_CHECK_BOX;
    case IHS_SEMANTICS_ROLE_RADIO_BUTTON:
      return ACCESSKIT_ROLE_RADIO_BUTTON;
    case IHS_SEMANTICS_ROLE_LINK:
      return ACCESSKIT_ROLE_LINK;
    case IHS_SEMANTICS_ROLE_IMAGE:
      return ACCESSKIT_ROLE_IMAGE;
    case IHS_SEMANTICS_ROLE_HEADING:
      return ACCESSKIT_ROLE_HEADING;
    case IHS_SEMANTICS_ROLE_SCROLL_VIEW:
      return ACCESSKIT_ROLE_SCROLL_VIEW;
    case IHS_SEMANTICS_ROLE_PANE:
      return ACCESSKIT_ROLE_PANE;
    case IHS_SEMANTICS_ROLE_LABEL:
      return ACCESSKIT_ROLE_LABEL;
    case IHS_SEMANTICS_ROLE_GENERIC_CONTAINER:
      return ACCESSKIT_ROLE_GENERIC_CONTAINER;
    case IHS_SEMANTICS_ROLE_UNKNOWN:
      break;
  }
  return ACCESSKIT_ROLE_UNKNOWN;
}

// Maps an AccessKit action onto the hub's. Returns 0 for anything with no
// equivalent, which the caller reports rather than dispatching a guess.
uint64_t ToIhsAction(const accesskit_action action) {
  switch (action) {
    case ACCESSKIT_ACTION_CLICK:
      return IHS_SEMANTICS_ACTION_TAP;
    case ACCESSKIT_ACTION_FOCUS:
      return IHS_SEMANTICS_ACTION_DID_GAIN_A11Y_FOCUS;
    case ACCESSKIT_ACTION_BLUR:
      return IHS_SEMANTICS_ACTION_DID_LOSE_A11Y_FOCUS;
    case ACCESSKIT_ACTION_INCREMENT:
      return IHS_SEMANTICS_ACTION_INCREASE;
    case ACCESSKIT_ACTION_DECREMENT:
      return IHS_SEMANTICS_ACTION_DECREASE;
    case ACCESSKIT_ACTION_SCROLL_INTO_VIEW:
      return IHS_SEMANTICS_ACTION_SHOW_ON_SCREEN;
    case ACCESSKIT_ACTION_SCROLL_UP:
      return IHS_SEMANTICS_ACTION_SCROLL_UP;
    case ACCESSKIT_ACTION_SCROLL_DOWN:
      return IHS_SEMANTICS_ACTION_SCROLL_DOWN;
    case ACCESSKIT_ACTION_SCROLL_LEFT:
      return IHS_SEMANTICS_ACTION_SCROLL_LEFT;
    case ACCESSKIT_ACTION_SCROLL_RIGHT:
      return IHS_SEMANTICS_ACTION_SCROLL_RIGHT;
    case ACCESSKIT_ACTION_EXPAND:
      return IHS_SEMANTICS_ACTION_EXPAND;
    case ACCESSKIT_ACTION_COLLAPSE:
      return IHS_SEMANTICS_ACTION_COLLAPSE;
    case ACCESSKIT_ACTION_SET_SCROLL_OFFSET:
      return IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET;
    default:
      return 0;
  }
}

namespace {

// Advertises to the screen reader what the node actually accepts, so it does
// not offer a gesture the framework will silently drop.
void AddActions(accesskit_node* node,
                const uint64_t actions,
                const bool focusable) {
  if ((actions & IHS_SEMANTICS_ACTION_TAP) != 0) {
    accesskit_node_add_action(node, ACCESSKIT_ACTION_CLICK);
  }
  if ((actions & IHS_SEMANTICS_ACTION_INCREASE) != 0) {
    accesskit_node_add_action(node, ACCESSKIT_ACTION_INCREMENT);
  }
  if ((actions & IHS_SEMANTICS_ACTION_DECREASE) != 0) {
    accesskit_node_add_action(node, ACCESSKIT_ACTION_DECREMENT);
  }
  if ((actions & IHS_SEMANTICS_ACTION_SHOW_ON_SCREEN) != 0) {
    accesskit_node_add_action(node, ACCESSKIT_ACTION_SCROLL_INTO_VIEW);
  }
  if ((actions & IHS_SEMANTICS_ACTION_SCROLL_UP) != 0) {
    accesskit_node_add_action(node, ACCESSKIT_ACTION_SCROLL_UP);
  }
  if ((actions & IHS_SEMANTICS_ACTION_SCROLL_DOWN) != 0) {
    accesskit_node_add_action(node, ACCESSKIT_ACTION_SCROLL_DOWN);
  }
  if ((actions & IHS_SEMANTICS_ACTION_SCROLL_LEFT) != 0) {
    accesskit_node_add_action(node, ACCESSKIT_ACTION_SCROLL_LEFT);
  }
  if ((actions & IHS_SEMANTICS_ACTION_SCROLL_RIGHT) != 0) {
    accesskit_node_add_action(node, ACCESSKIT_ACTION_SCROLL_RIGHT);
  }
  if ((actions & IHS_SEMANTICS_ACTION_EXPAND) != 0) {
    accesskit_node_add_action(node, ACCESSKIT_ACTION_EXPAND);
  }
  if ((actions & IHS_SEMANTICS_ACTION_COLLAPSE) != 0) {
    accesskit_node_add_action(node, ACCESSKIT_ACTION_COLLAPSE);
  }
  // Setting a scrollable's offset. AccessKit carries a point for this, so both
  // axes arrive and neither has to be inferred from which way the node
  // scrolls -- which a node scrolling both ways would make unanswerable.
  if ((actions & IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET) != 0) {
    accesskit_node_add_action(node, ACCESSKIT_ACTION_SET_SCROLL_OFFSET);
  }
  // Replacing a field's text. AccessKit spells this SetValue, whose payload
  // may be a string or a number; only the string form means text, and the
  // handler checks which arrived.
  if ((actions & IHS_SEMANTICS_ACTION_SET_TEXT) != 0) {
    accesskit_node_add_action(node, ACCESSKIT_ACTION_SET_VALUE);
  }
  // Focus is offered only where the node can actually hold the accessibility
  // cursor. Advertising it everywhere -- as this did -- makes every static
  // label and layout container look like a focus target, which is what a
  // screen reader user experiences as a tree full of nothing. Walking the
  // tree does not depend on this: traversal follows children, and only an
  // explicit grabFocus needs the action.
  if (focusable) {
    accesskit_node_add_action(node, ACCESSKIT_ACTION_FOCUS);
  }
}

}  // namespace

// Translates one snapshot node. The tristates are carried across rather than
// flattened: a screen reader announces "unchecked" and "not checkable" very
// differently, and collapsing them would make every container sound like an
// unchecked checkbox.
accesskit_node* BuildNode(const IhsSemanticsNode* node,
                          const IhsSemanticsSnapshot* snapshot) {
  const accesskit_role role = ToAccessKitRole(node->role);
  accesskit_node* out = accesskit_node_new(role);

  for (size_t i = 0; i < node->child_count; i++) {
    const IhsSemanticsNode* child =
        ihs_semantics_snapshot_node_at(snapshot, node->child_indices[i]);
    if (child != nullptr) {
      accesskit_node_push_child(out, static_cast<accesskit_node_id>(child->id));
    }
  }

  // Every string is set only when it has content. The hub normalizes an absent
  // string to "" so its own consumers need no null checks, but AccessKit's
  // model is optional -- Some("") is not None -- so that normalization has to
  // be undone here rather than carried across. Forwarding "" would give a node
  // an empty label that AccessKit counts as having one, and a live region with
  // an empty name emits an announcement of nothing at all.
  if (node->label[0] != '\0') {
    accesskit_node_set_label(out, node->label);
  }
  if (node->hint[0] != '\0') {
    accesskit_node_set_description(out, node->hint);
  }

  // AccessKit derives a Label node's accessible name from its *value*, not its
  // label -- label_comes_from_value() is exactly role == Label. Flutter puts
  // the text in label, so without this every piece of static text in the UI
  // reaches a screen reader with no name at all, which is most of what there
  // is to read. A Label carrying its own value keeps it.
  const char* value = node->value;
  if (role == ACCESSKIT_ROLE_LABEL && value[0] == '\0') {
    value = node->label;
  }
  if (value[0] != '\0') {
    accesskit_node_set_value(out, value);
  }

  // The application's own stable id, which is what an automation client keys
  // on when a label is localized or ambiguous. Empty on every node at the
  // 3.38.3 deployment floor, since that engine never writes identifier -- so
  // this does nothing today and starts working on an engine bump, rather than
  // being quietly forgotten at the point where it would matter.
  if (node->identifier[0] != '\0') {
    accesskit_node_set_author_id(out, node->identifier);
  }
  if (node->tooltip[0] != '\0') {
    accesskit_node_set_tooltip(out, node->tooltip);
  }

  // A numeric value is what makes AccessKit expose the AT-SPI Value interface
  // for this node, which is how a screen reader reports a scroll position
  // rather than just naming the control. The hub guarantees a finite, ordered
  // range, so these go straight across.
  double numeric = 0.0;
  double numeric_min = 0.0;
  double numeric_max = 0.0;
  if (ihs_semantics_node_numeric_value(node, &numeric, &numeric_min,
                                       &numeric_max)) {
    accesskit_node_set_numeric_value(out, numeric);
    accesskit_node_set_min_numeric_value(out, numeric_min);
    accesskit_node_set_max_numeric_value(out, numeric_max);
  }

  const accesskit_rect bounds = {node->rect.left, node->rect.top,
                                 node->rect.right, node->rect.bottom};
  accesskit_node_set_bounds(out, bounds);

  switch (node->checked) {
    case IHS_SEMANTICS_CHECK_TRUE:
      accesskit_node_set_toggled(out, ACCESSKIT_TOGGLED_TRUE);
      break;
    case IHS_SEMANTICS_CHECK_FALSE:
      accesskit_node_set_toggled(out, ACCESSKIT_TOGGLED_FALSE);
      break;
    case IHS_SEMANTICS_CHECK_MIXED:
      accesskit_node_set_toggled(out, ACCESSKIT_TOGGLED_MIXED);
      break;
    case IHS_SEMANTICS_CHECK_NONE:
      break;  // not a checkable node; say nothing rather than "unchecked"
  }
  if (node->toggled == IHS_SEMANTICS_TRISTATE_TRUE) {
    accesskit_node_set_toggled(out, ACCESSKIT_TOGGLED_TRUE);
  } else if (node->toggled == IHS_SEMANTICS_TRISTATE_FALSE) {
    accesskit_node_set_toggled(out, ACCESSKIT_TOGGLED_FALSE);
  }

  // Only assert disabled when the node actually carries an enabled state;
  // IHS_SEMANTICS_TRISTATE_NONE means the property does not apply.
  if (node->enabled == IHS_SEMANTICS_TRISTATE_FALSE) {
    accesskit_node_set_disabled(out);
  }
  if (node->selected == IHS_SEMANTICS_TRISTATE_TRUE) {
    accesskit_node_set_selected(out, true);
  } else if (node->selected == IHS_SEMANTICS_TRISTATE_FALSE) {
    accesskit_node_set_selected(out, false);
  }
  if (node->expanded == IHS_SEMANTICS_TRISTATE_TRUE) {
    accesskit_node_set_expanded(out, true);
  } else if (node->expanded == IHS_SEMANTICS_TRISTATE_FALSE) {
    accesskit_node_set_expanded(out, false);
  }
  if (node->required == IHS_SEMANTICS_TRISTATE_TRUE) {
    accesskit_node_set_required(out);
  }
  if (node->hidden) {
    accesskit_node_set_hidden(out);
  }
  if (node->read_only) {
    accesskit_node_set_read_only(out);
  }

  // A live region is announced without the accessibility cursor moving to it,
  // which is the only way a toast or a snackbar reaches a screen reader at
  // all: by the time a user could navigate to one it has usually gone. Flutter
  // carries a single liveRegion boolean with no urgency distinction, so this
  // maps to polite -- assertive interrupts whatever is being spoken, and
  // claiming that for every transient message would make the UI hostile.
  if (node->live_region) {
    accesskit_node_set_live(out, ACCESSKIT_LIVE_POLITE);
  }

  // An application's own verbs -- "Add to favorites", "Dismiss route" -- exist
  // only as custom actions, so without these a screen reader user can reach
  // every control the app declares and still not invoke anything specific to
  // it. The label lives once in the snapshot's declaration table rather than
  // on each referencing node, so resolve it here; a node referencing an
  // undeclared id is skipped, since an action announced with no name is worse
  // than one that is absent.
  for (size_t i = 0; i < node->custom_action_count; i++) {
    const int32_t action_id = node->custom_action_ids[i];
    const IhsSemanticsCustomAction* declared =
        ihs_semantics_find_custom_action(snapshot, action_id);
    if (declared == nullptr || declared->label[0] == '\0') {
      continue;
    }
    accesskit_custom_action* custom = accesskit_custom_action_new(action_id);
    accesskit_custom_action_set_description(custom, declared->label);
    // Takes ownership, so there is nothing to free here.
    accesskit_node_push_custom_action(out, custom);
  }

  // a11y_focus_blocked is the framework saying this node must not take the
  // accessibility cursor even though it otherwise could -- a node behind a
  // modal barrier, typically. Offering focus anyway would let a screen reader
  // move onto content the app has deliberately sealed off.
  AddActions(out, node->actions, node->focusable && !node->a11y_focus_blocked);
  return out;
}

namespace {

// Builds a full tree update from a snapshot. The caller owns the result.
accesskit_tree_update* BuildTreeUpdate(const IhsSemanticsSnapshot* snapshot) {
  const size_t count = ihs_semantics_snapshot_node_count(snapshot);
  if (count == 0) {
    return nullptr;
  }

  // Focus follows whichever node reports it; the root is the fallback, since
  // AccessKit requires the focus id to name a node that exists in the update.
  accesskit_node_id focus = 0;
  for (size_t i = 0; i < count; i++) {
    const IhsSemanticsNode* node = ihs_semantics_snapshot_node_at(snapshot, i);
    if (node != nullptr && node->focused == IHS_SEMANTICS_TRISTATE_TRUE) {
      focus = static_cast<accesskit_node_id>(node->id);
      break;
    }
  }

  accesskit_tree_update* update =
      accesskit_tree_update_with_capacity_and_focus(count, focus);
  accesskit_tree_update_set_tree(update, accesskit_tree_new(0));

  for (size_t i = 0; i < count; i++) {
    const IhsSemanticsNode* node = ihs_semantics_snapshot_node_at(snapshot, i);
    if (node == nullptr) {
      continue;
    }
    accesskit_tree_update_push_node(update,
                                    static_cast<accesskit_node_id>(node->id),
                                    BuildNode(node, snapshot));
  }
  return update;
}

// AccessKit asks for the whole tree when an assistive technology attaches.
// This runs on AccessKit's thread, which is safe now only because it reads a
// snapshot rather than the shell's mutable tree.
accesskit_tree_update* ActivationHandler(void* /* user_data */) {
  const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
  if (snapshot == nullptr) {
    // Nothing published yet. Returning null tells AccessKit there is no tree
    // to show, and the next publish will push one.
    return nullptr;
  }
  accesskit_tree_update* update = BuildTreeUpdate(snapshot);
  ihs_semantics_release_snapshot(snapshot);
  return update;
}

// A screen reader asked for something. Everything the framework can act on
// goes back through the hub, which serializes onto the platform thread and
// records the actor.
void ActionHandler(accesskit_action_request* request, void* user_data) {
  if (request == nullptr) {
    return;
  }
  auto* consumer = static_cast<IhsSemanticsConsumer*>(user_data);

  // Arguments travel in the hub's plain layout and the shell encodes them for
  // the framework, so this builds bytes rather than a codec message.
  uint64_t action = 0;
  std::vector<uint8_t> argument;

  if (request->action == ACCESSKIT_ACTION_SET_VALUE) {
    // SetValue is two requests wearing one name, told apart by payload.
    //
    // A number is the live path on this platform: AT-SPI's Value.currentValue
    // setter is the only route an assistive technology has to move a value,
    // and accesskit's AT-SPI backend turns it into exactly this. It never
    // emits SetScrollOffset, which would have carried both axes and spared
    // the inference below.
    if (request->data.has_value &&
        request->data.value.tag == ACCESSKIT_ACTION_DATA_NUMERIC_VALUE) {
      // Flutter reports one scroll position per scrollable, so the number is
      // that scalar coming back and only its axis has to be recovered. Which
      // way the node scrolls answers it; a node scrolling both ways does not
      // have an answer, in Flutter's model either, so vertical wins as the
      // commoner case rather than the request being dropped.
      const double value = request->data.value.numeric_value;
      bool horizontal = false;
      if (const IhsSemanticsSnapshot* snapshot =
              ihs_semantics_acquire_snapshot();
          snapshot != nullptr) {
        if (const IhsSemanticsNode* node = ihs_semantics_snapshot_node_by_id(
                snapshot, static_cast<int32_t>(request->target_node));
            node != nullptr) {
          const bool vertical =
              (node->actions & (IHS_SEMANTICS_ACTION_SCROLL_UP |
                                IHS_SEMANTICS_ACTION_SCROLL_DOWN)) != 0;
          horizontal = !vertical && (node->actions &
                                     (IHS_SEMANTICS_ACTION_SCROLL_LEFT |
                                      IHS_SEMANTICS_ACTION_SCROLL_RIGHT)) != 0;
        }
        ihs_semantics_release_snapshot(snapshot);
      }
      const double offset[2] = {horizontal ? value : 0.0,
                                horizontal ? 0.0 : value};
      const auto* bytes = reinterpret_cast<const uint8_t*>(offset);
      action = IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET;
      argument.assign(bytes, bytes + sizeof(offset));
    } else if (request->data.has_value &&
               request->data.value.tag == ACCESSKIT_ACTION_DATA_VALUE &&
               request->data.value.value != nullptr) {
      // A string means replace the text. Correct, and unreachable through
      // AT-SPI today: accesskit's backend exposes no EditableText interface,
      // so nothing on this platform produces it. Kept because it is the right
      // mapping the moment one does.
      action = IHS_SEMANTICS_ACTION_SET_TEXT;
      const char* text = request->data.value.value;
      argument.assign(text, text + std::strlen(text));
    } else {
      ihs::log::debug("accesskit: SetValue carried no usable payload");
      return;
    }
  } else {
    action = ToIhsAction(request->action);
    if (action == 0) {
      ihs::log::debug("accesskit: no hub equivalent for action {}",
                      static_cast<int>(request->action));
      return;
    }
    if (action == IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET) {
      if (!request->data.has_value ||
          request->data.value.tag != ACCESSKIT_ACTION_DATA_SET_SCROLL_OFFSET) {
        ihs::log::debug("accesskit: scroll offset request carried no point");
        return;
      }
      const accesskit_point& point = request->data.value.set_scroll_offset;
      const double offset[2] = {point.x, point.y};
      const auto* bytes = reinterpret_cast<const uint8_t*>(offset);
      argument.assign(bytes, bytes + sizeof(offset));
    }
  }

  const int status = ihs_semantics_dispatch(
      consumer, 0, static_cast<int32_t>(request->target_node), action,
      argument.empty() ? nullptr : argument.data(), argument.size(), nullptr,
      nullptr);
  if (status != IHS_SEMANTICS_OK) {
    // Expected in normal use: a screen reader may offer an action the node
    // does not implement, and the hub refuses rather than letting it vanish.
    ihs::log::debug("accesskit: dispatch of action 0x{:x} on node {} -> {}",
                    action, request->target_node, status);
  }
}

void DeactivationHandler(void* /* user_data */) {
  ihs::log::debug("accesskit: assistive technology detached");
}

}  // namespace

AccessKitConsumer::AccessKitConsumer() {
  notify_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  shutdown_fd_ = ::eventfd(0, EFD_CLOEXEC);
  if (notify_fd_ < 0 || shutdown_fd_ < 0) {
    ihs::log::error("accesskit: could not create notification fds: {}",
                    std::strerror(errno));
    return;
  }

  IhsSemanticsConsumerDesc desc{};
  desc.struct_size = sizeof(IhsSemanticsConsumerDesc);
  desc.name = "accesskit";
  // The full set, deliberately including the accessibility-focus actions: a
  // screen reader moving its own cursor is the legitimate case those bits
  // exist for. The mask is what stops other consumers from doing it.
  desc.action_allow_mask = IHS_SEMANTICS_ACTION_ALL;
  desc.notify_fd = notify_fd_;

  if (ihs_semantics_register(&desc, &consumer_) != IHS_SEMANTICS_OK) {
    ihs::log::error("accesskit: could not register with the semantics hub");
    return;
  }

  g_adapter =
      accesskit_unix_adapter_new(ActivationHandler, nullptr, ActionHandler,
                                 consumer_, DeactivationHandler, nullptr);
  if (g_adapter == nullptr) {
    // No AT-SPI bus, most likely. Not fatal: the rest of the shell is fine
    // without a screen-reader bridge, so unwind and carry on.
    ihs::log::warn(
        "accesskit: adapter unavailable; continuing without a screen-reader "
        "bridge");
    ihs_semantics_unregister(consumer_);
    consumer_ = nullptr;
    return;
  }

  // Mark the window focused. The tree is reachable either way, but without
  // this the root frame is published without the active and focused states,
  // and that is what a screen reader keys on to decide which window it should
  // be reading. The shell owns a single fullscreen surface, so it holds focus
  // whenever it is up; revisit if the shell ever grows real focus tracking.
  accesskit_unix_adapter_update_window_focus_state(g_adapter, true);

  running_ = true;
  poll_thread_ = std::thread(&AccessKitConsumer::PollLoop, this);
  ihs::log::info("accesskit: registered as a semantics consumer");
}

AccessKitConsumer::~AccessKitConsumer() {
  if (running_) {
    running_ = false;
    const uint64_t one = 1;
    ssize_t written;
    do {
      written = ::write(shutdown_fd_, &one, sizeof(one));
    } while (written < 0 && errno == EINTR);
    if (poll_thread_.joinable()) {
      poll_thread_.join();
    }
  }

  // Unregister before dropping the adapter: unregistering drains any dispatch
  // still in flight, so the action handler cannot be running against an
  // adapter that is about to go away.
  if (consumer_ != nullptr) {
    ihs_semantics_unregister(consumer_);
    consumer_ = nullptr;
  }
  if (g_adapter != nullptr) {
    accesskit_unix_adapter_free(g_adapter);
    g_adapter = nullptr;
  }
  if (notify_fd_ >= 0) {
    ::close(notify_fd_);
  }
  if (shutdown_fd_ >= 0) {
    ::close(shutdown_fd_);
  }
}

void AccessKitConsumer::PollLoop() {
  struct pollfd fds[2];
  fds[0].fd = notify_fd_;
  fds[0].events = POLLIN;
  fds[1].fd = shutdown_fd_;
  fds[1].events = POLLIN;

  while (running_) {
    fds[0].revents = 0;
    fds[1].revents = 0;
    const int ready = ::poll(fds, 2, -1);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      ihs::log::error("accesskit: poll failed: {}", std::strerror(errno));
      return;
    }
    if ((fds[1].revents & POLLIN) != 0) {
      return;  // shutdown
    }
    if ((fds[0].revents & POLLIN) == 0) {
      continue;
    }

    // Drain the counter. Events are edge-coalesced, so several publications
    // can collapse into one wake; that is lossless because what follows reads
    // the newest snapshot rather than replaying each one.
    uint64_t token = 0;
    ssize_t got;
    do {
      got = ::read(notify_fd_, &token, sizeof(token));
    } while (got < 0 && errno == EINTR);

    const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
    if (snapshot == nullptr) {
      continue;
    }
    PushSnapshot(snapshot);
    ihs_semantics_release_snapshot(snapshot);
  }
}

void AccessKitConsumer::PushSnapshot(const IhsSemanticsSnapshot* snapshot) {
  const uint64_t generation = ihs_semantics_snapshot_generation(snapshot);
  if (generation == last_generation_) {
    return;  // a wake with nothing newer behind it
  }
  last_generation_ = generation;

  accesskit_tree_update* update = BuildTreeUpdate(snapshot);
  if (update == nullptr) {
    return;
  }

  // update_if_active hands ownership of the update to AccessKit and does
  // nothing when no assistive technology is attached -- in which case it frees
  // the update itself, so there is no leak on the common path where nobody is
  // listening.
  accesskit_unix_adapter_update_if_active(
      g_adapter,
      [](void* user_data) -> accesskit_tree_update* {
        return static_cast<accesskit_tree_update*>(user_data);
      },
      update);
}

}  // namespace accessibility
