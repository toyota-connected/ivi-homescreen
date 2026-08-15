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

#include "semantics_publisher.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

#include "ihs/ihs_semantics_host.h"

namespace accessibility {

namespace {

IhsSemanticsTristate ToIhsTristate(const Tristate value) {
  switch (value) {
    case Tristate::kTrue:
      return IHS_SEMANTICS_TRISTATE_TRUE;
    case Tristate::kFalse:
      return IHS_SEMANTICS_TRISTATE_FALSE;
    case Tristate::kNone:
      break;
  }
  return IHS_SEMANTICS_TRISTATE_NONE;
}

IhsSemanticsCheckState ToIhsCheckState(const CheckState value) {
  switch (value) {
    case CheckState::kTrue:
      return IHS_SEMANTICS_CHECK_TRUE;
    case CheckState::kFalse:
      return IHS_SEMANTICS_CHECK_FALSE;
    case CheckState::kMixed:
      return IHS_SEMANTICS_CHECK_MIXED;
    case CheckState::kNone:
      break;
  }
  return IHS_SEMANTICS_CHECK_NONE;
}

}  // namespace

Affine FromFlutter(const FlutterTransformation& t) {
  Affine out;
  out.a = t.scaleX;
  out.b = t.skewY;
  out.c = t.skewX;
  out.d = t.scaleY;
  out.e = t.transX;
  out.f = t.transY;
  return out;
}

Affine Concat(const Affine& outer, const Affine& inner) {
  Affine out;
  out.a = outer.a * inner.a + outer.c * inner.b;
  out.b = outer.b * inner.a + outer.d * inner.b;
  out.c = outer.a * inner.c + outer.c * inner.d;
  out.d = outer.b * inner.c + outer.d * inner.d;
  out.e = outer.a * inner.e + outer.c * inner.f + outer.e;
  out.f = outer.b * inner.e + outer.d * inner.f + outer.f;
  return out;
}

IhsSemanticsRect MapRect(const Affine& t, const FlutterRect& rect) {
  // Map all four corners: under rotation or skew the transformed min/max are
  // not the transforms of the original min/max.
  const double xs[4] = {rect.left, rect.right, rect.right, rect.left};
  const double ys[4] = {rect.top, rect.top, rect.bottom, rect.bottom};

  double min_x = 0.0;
  double max_x = 0.0;
  double min_y = 0.0;
  double max_y = 0.0;
  for (int i = 0; i < 4; i++) {
    const double x = t.a * xs[i] + t.c * ys[i] + t.e;
    const double y = t.b * xs[i] + t.d * ys[i] + t.f;
    if (i == 0) {
      min_x = max_x = x;
      min_y = max_y = y;
    } else {
      min_x = std::min(min_x, x);
      max_x = std::max(max_x, x);
      min_y = std::min(min_y, y);
      max_y = std::max(max_y, y);
    }
  }

  IhsSemanticsRect out;
  out.left = min_x;
  out.top = min_y;
  out.right = max_x;
  out.bottom = max_y;
  return out;
}

IhsSemanticsRole ToIhsRole(const Role role) {
  switch (role) {
    case Role::kWindow:
      return IHS_SEMANTICS_ROLE_WINDOW;
    case Role::kButton:
      return IHS_SEMANTICS_ROLE_BUTTON;
    case Role::kTextInput:
      return IHS_SEMANTICS_ROLE_TEXT_INPUT;
    case Role::kMultilineTextInput:
      return IHS_SEMANTICS_ROLE_MULTILINE_TEXT_INPUT;
    case Role::kPasswordInput:
      return IHS_SEMANTICS_ROLE_PASSWORD_INPUT;
    case Role::kSlider:
      return IHS_SEMANTICS_ROLE_SLIDER;
    case Role::kSwitch:
      return IHS_SEMANTICS_ROLE_SWITCH;
    case Role::kCheckBox:
      return IHS_SEMANTICS_ROLE_CHECK_BOX;
    case Role::kRadioButton:
      return IHS_SEMANTICS_ROLE_RADIO_BUTTON;
    case Role::kLink:
      return IHS_SEMANTICS_ROLE_LINK;
    case Role::kImage:
      return IHS_SEMANTICS_ROLE_IMAGE;
    case Role::kHeading:
      return IHS_SEMANTICS_ROLE_HEADING;
    case Role::kScrollView:
      return IHS_SEMANTICS_ROLE_SCROLL_VIEW;
    case Role::kPane:
      return IHS_SEMANTICS_ROLE_PANE;
    case Role::kLabel:
      return IHS_SEMANTICS_ROLE_LABEL;
    case Role::kGenericContainer:
      return IHS_SEMANTICS_ROLE_GENERIC_CONTAINER;
    case Role::kUnknown:
      break;
  }
  return IHS_SEMANTICS_ROLE_UNKNOWN;
}

// Fills in the node's numeric position, or clears it when there is none.
//
// Scroll offset is the only numeric value the framework hands us: a slider's
// position arrives as a label like "22 degrees", with no number behind it, so
// a slider legitimately has no numeric value here. That is a limitation of the
// embedder API rather than a gap in the translation.
//
// Consumers are promised finite, ordered bounds, so anything that cannot be
// stated honestly is dropped rather than approximated. An unbounded scrollable
// reports an infinite extent, which no accessibility API can render as a
// position, so it publishes no numeric value at all. Overscroll is different:
// it puts the offset briefly outside its own range, which is transient and
// real, so clamp it -- a scroll view that dropped out of the value API every
// time it bounced would be worse than one reading its own limit.
void SetNumericValue(const AccessibilityNode& node,
                     const NodeSpec& spec,
                     IhsSemanticsPublishNode& out) {
  const bool scrollable = spec.action_scroll_up || spec.action_scroll_down ||
                          spec.action_scroll_left || spec.action_scroll_right;
  const double position = node.GetScrollPosition();
  const double min = node.GetScrollExtentMin();
  const double max = node.GetScrollExtentMax();

  if (!scrollable || !std::isfinite(position) || !std::isfinite(min) ||
      !std::isfinite(max) || min > max) {
    out.has_numeric_value = false;
    out.numeric_value = 0.0;
    out.numeric_value_min = 0.0;
    out.numeric_value_max = 0.0;
    return;
  }

  out.has_numeric_value = true;
  out.numeric_value = std::clamp(position, min, max);
  out.numeric_value_min = min;
  out.numeric_value_max = max;
}

uint64_t ToIhsActions(const NodeSpec& spec) {
  uint64_t actions = 0;
  if (spec.action_tap) {
    actions |= IHS_SEMANTICS_ACTION_TAP;
  }
  if (spec.action_long_press) {
    actions |= IHS_SEMANTICS_ACTION_LONG_PRESS;
  }
  if (spec.action_scroll_left) {
    actions |= IHS_SEMANTICS_ACTION_SCROLL_LEFT;
  }
  if (spec.action_scroll_right) {
    actions |= IHS_SEMANTICS_ACTION_SCROLL_RIGHT;
  }
  if (spec.action_scroll_up) {
    actions |= IHS_SEMANTICS_ACTION_SCROLL_UP;
  }
  if (spec.action_scroll_down) {
    actions |= IHS_SEMANTICS_ACTION_SCROLL_DOWN;
  }
  if (spec.action_increase) {
    actions |= IHS_SEMANTICS_ACTION_INCREASE;
  }
  if (spec.action_decrease) {
    actions |= IHS_SEMANTICS_ACTION_DECREASE;
  }
  if (spec.action_show_on_screen) {
    actions |= IHS_SEMANTICS_ACTION_SHOW_ON_SCREEN;
  }
  if (spec.action_set_text) {
    actions |= IHS_SEMANTICS_ACTION_SET_TEXT;
  }
  if (spec.action_focus) {
    actions |= IHS_SEMANTICS_ACTION_FOCUS;
  }
  if (spec.action_scroll_to_offset) {
    actions |= IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET;
  }
  if (spec.action_expand) {
    actions |= IHS_SEMANTICS_ACTION_EXPAND;
  }
  if (spec.action_collapse) {
    actions |= IHS_SEMANTICS_ACTION_COLLAPSE;
  }
  if (spec.action_custom_action) {
    actions |= IHS_SEMANTICS_ACTION_CUSTOM_ACTION;
  }
  return actions;
}

FlutterSemanticsAction ToFlutterSemanticsAction(const uint64_t ihs_action) {
  // Reject a combination outright rather than picking one of the bits: the
  // framework performs a single action, so honoring part of the request would
  // be worse than refusing all of it.
  if (ihs_action == 0 || (ihs_action & (ihs_action - 1)) != 0) {
    return static_cast<FlutterSemanticsAction>(0);
  }
  switch (ihs_action) {
    case IHS_SEMANTICS_ACTION_TAP:
      return kFlutterSemanticsActionTap;
    case IHS_SEMANTICS_ACTION_LONG_PRESS:
      return kFlutterSemanticsActionLongPress;
    case IHS_SEMANTICS_ACTION_SCROLL_LEFT:
      return kFlutterSemanticsActionScrollLeft;
    case IHS_SEMANTICS_ACTION_SCROLL_RIGHT:
      return kFlutterSemanticsActionScrollRight;
    case IHS_SEMANTICS_ACTION_SCROLL_UP:
      return kFlutterSemanticsActionScrollUp;
    case IHS_SEMANTICS_ACTION_SCROLL_DOWN:
      return kFlutterSemanticsActionScrollDown;
    case IHS_SEMANTICS_ACTION_INCREASE:
      return kFlutterSemanticsActionIncrease;
    case IHS_SEMANTICS_ACTION_DECREASE:
      return kFlutterSemanticsActionDecrease;
    case IHS_SEMANTICS_ACTION_SHOW_ON_SCREEN:
      return kFlutterSemanticsActionShowOnScreen;
    case IHS_SEMANTICS_ACTION_MOVE_CURSOR_FORWARD_CHAR:
      return kFlutterSemanticsActionMoveCursorForwardByCharacter;
    case IHS_SEMANTICS_ACTION_MOVE_CURSOR_BACKWARD_CHAR:
      return kFlutterSemanticsActionMoveCursorBackwardByCharacter;
    case IHS_SEMANTICS_ACTION_SET_SELECTION:
      return kFlutterSemanticsActionSetSelection;
    case IHS_SEMANTICS_ACTION_COPY:
      return kFlutterSemanticsActionCopy;
    case IHS_SEMANTICS_ACTION_CUT:
      return kFlutterSemanticsActionCut;
    case IHS_SEMANTICS_ACTION_PASTE:
      return kFlutterSemanticsActionPaste;
    case IHS_SEMANTICS_ACTION_DID_GAIN_A11Y_FOCUS:
      return kFlutterSemanticsActionDidGainAccessibilityFocus;
    case IHS_SEMANTICS_ACTION_DID_LOSE_A11Y_FOCUS:
      return kFlutterSemanticsActionDidLoseAccessibilityFocus;
    case IHS_SEMANTICS_ACTION_CUSTOM_ACTION:
      return kFlutterSemanticsActionCustomAction;
    case IHS_SEMANTICS_ACTION_DISMISS:
      return kFlutterSemanticsActionDismiss;
    case IHS_SEMANTICS_ACTION_MOVE_CURSOR_FORWARD_WORD:
      return kFlutterSemanticsActionMoveCursorForwardByWord;
    case IHS_SEMANTICS_ACTION_MOVE_CURSOR_BACKWARD_WORD:
      return kFlutterSemanticsActionMoveCursorBackwardByWord;
    case IHS_SEMANTICS_ACTION_SET_TEXT:
      return kFlutterSemanticsActionSetText;
    case IHS_SEMANTICS_ACTION_FOCUS:
      return kFlutterSemanticsActionFocus;
    case IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET:
      return kFlutterSemanticsActionScrollToOffset;
    case IHS_SEMANTICS_ACTION_EXPAND:
      return kFlutterSemanticsActionExpand;
    case IHS_SEMANTICS_ACTION_COLLAPSE:
      return kFlutterSemanticsActionCollapse;
    default:
      return static_cast<FlutterSemanticsAction>(0);
  }
}

int PublishTree(const AccessibilityTree& tree, IhsSemanticsSource* source) {
  const AccessibilityNode* root = tree.FindNode(0);
  if (root == nullptr) {
    // No root yet: Flutter sends it in the first update, and until it arrives
    // there is no tree worth publishing.
    return IHS_SEMANTICS_OK;
  }

  // Traversal order with the root first, which is what the hub promises its
  // consumers. Nodes unreachable from the root are left out rather than
  // appended: index 0 must be the root and every child index must resolve.
  struct Pending {
    const AccessibilityNode* node;
    Affine to_screen;
  };

  std::vector<const AccessibilityNode*> ordered;
  std::vector<Affine> transforms;
  std::unordered_set<int32_t> visited;
  std::vector<Pending> stack;

  ordered.reserve(static_cast<size_t>(tree.NumberOfNodes()));
  transforms.reserve(static_cast<size_t>(tree.NumberOfNodes()));
  stack.push_back({root, FromFlutter(root->GetTransform())});
  visited.insert(root->GetId());

  while (!stack.empty()) {
    const Pending current = stack.back();
    stack.pop_back();
    ordered.push_back(current.node);
    transforms.push_back(current.to_screen);

    // Push children reversed so they come off the stack in traversal order.
    const uint32_t child_count = current.node->NumberOfChildren();
    for (uint32_t i = child_count; i > 0; i--) {
      const int32_t child_id =
          current.node->GetChild(static_cast<int32_t>(i - 1));
      const AccessibilityNode* child = tree.FindNode(child_id);
      // A cycle would otherwise walk forever; Flutter does not produce one,
      // but the mirror is not the place to find that out the hard way.
      if (child == nullptr || !visited.insert(child_id).second) {
        continue;
      }
      stack.push_back({child, Concat(current.to_screen,
                                     FromFlutter(child->GetTransform()))});
    }
  }

  std::vector<IhsSemanticsPublishNode> nodes(ordered.size());
  std::vector<std::vector<int32_t>> child_ids(ordered.size());
  std::vector<std::vector<int32_t>> custom_ids(ordered.size());

  for (size_t i = 0; i < ordered.size(); i++) {
    const AccessibilityNode* node = ordered[i];
    const NodeSpec& spec = node->GetSpec();
    IhsSemanticsPublishNode& out = nodes[i];

    out.id = node->GetId();
    out.identifier = node->GetIdentifier();
    out.label = node->GetLabel();
    out.hint = node->GetHint();
    out.value = node->GetValue();
    out.tooltip = node->GetTooltip();
    out.rect = MapRect(transforms[i], node->GetBounds());
    out.role = ToIhsRole(spec.role);

    out.checked = ToIhsCheckState(spec.check_state);
    out.enabled = ToIhsTristate(spec.enabled_state);
    out.selected = ToIhsTristate(spec.selected_state);
    out.toggled = ToIhsTristate(spec.toggled_state);
    out.expanded = ToIhsTristate(spec.expanded_state);
    out.focused = ToIhsTristate(spec.focused_state);
    out.required = ToIhsTristate(spec.required_state);

    out.hidden = spec.hidden;
    out.read_only = spec.read_only;
    out.obscured = spec.role == Role::kPasswordInput;
    out.live_region = spec.live;
    out.focusable = spec.focusable;
    out.a11y_focus_blocked = spec.a11y_focus_blocked;
    out.actions = ToIhsActions(spec);
    SetNumericValue(*node, spec, out);

    // Only children that made it into the walk: a child dropped as a cycle or
    // a dangling id must not be referenced from the published tree either.
    child_ids[i].reserve(node->NumberOfChildren());
    for (uint32_t c = 0; c < node->NumberOfChildren(); c++) {
      const int32_t child_id = node->GetChild(static_cast<int32_t>(c));
      if (visited.find(child_id) != visited.end() &&
          tree.FindNode(child_id) != nullptr) {
        child_ids[i].push_back(child_id);
      }
    }
    out.child_ids = child_ids[i].empty() ? nullptr : child_ids[i].data();
    out.child_count = child_ids[i].size();

    custom_ids[i].reserve(node->NumberOfCustomActions());
    for (uint32_t c = 0; c < node->NumberOfCustomActions(); c++) {
      custom_ids[i].push_back(node->GetCustomActionId(static_cast<int32_t>(c)));
    }
    out.custom_action_ids =
        custom_ids[i].empty() ? nullptr : custom_ids[i].data();
    out.custom_action_count = custom_ids[i].size();
  }

  std::vector<IhsSemanticsPublishCustomAction> custom_actions;
  custom_actions.reserve(tree.NumberOfCustomActions());
  tree.ForEachCustomAction(
      [&custom_actions](const AccessibilityCustomAction& action) {
        IhsSemanticsPublishCustomAction out;
        out.id = action.id;
        out.label = action.label.c_str();
        out.hint = action.hint.c_str();
        custom_actions.push_back(out);
      });

  IhsSemanticsPublishInfo info{};
  info.struct_size = sizeof(IhsSemanticsPublishInfo);
  info.nodes = nodes.empty() ? nullptr : nodes.data();
  info.node_count = nodes.size();
  info.custom_actions =
      custom_actions.empty() ? nullptr : custom_actions.data();
  info.custom_action_count = custom_actions.size();
  info.source = source;
  return ihs_semantics_publish(&info);
}

}  // namespace accessibility
