/*
 * Copyright 2025 Toyota Connected North America
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

#include "accessibility_tree.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_set>

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include "../utils.h"
#include "logging/logging.h"

AccessibilityNode::AccessibilityNode(const FlutterSemanticsNode2& fl_node) {
  Update(fl_node);
}

void AccessibilityNode::Update(const FlutterSemanticsNode2& fl_node) {
  m_id = fl_node.id;
  // Own copies of the text fields. The embedder only guarantees these
  // pointers for the duration of the update callback, and any of them may be
  // null — constructing std::string from a null pointer is undefined, so
  // coalesce to empty.
  m_label = fl_node.label ? fl_node.label : "";
  m_hint = fl_node.hint ? fl_node.hint : "";
  m_value = fl_node.value ? fl_node.value : "";
  m_tooltip = fl_node.tooltip ? fl_node.tooltip : "";
  m_bounds = fl_node.rect;
  // Parent-local bounds are only meaningful alongside the node-to-parent
  // transform, so retain it here rather than composing at read time.
  m_transform = fl_node.transform;
  m_flags = fl_node.flags;
  m_actions = fl_node.actions;

  // Replace (not append) the children in traversal order, so a reorder or a
  // removed child is reflected when the node is re-sent.
  if (fl_node.child_count > 0 &&
      fl_node.children_in_traversal_order != nullptr) {
    m_children.assign(fl_node.children_in_traversal_order,
                      fl_node.children_in_traversal_order +
                          static_cast<size_t>(fl_node.child_count));
  } else {
    m_children.clear();
  }

  // Same replace-don't-append discipline for the custom action IDs: an action
  // the application withdraws from a node must not linger on it.
  if (fl_node.custom_accessibility_actions_count > 0 &&
      fl_node.custom_accessibility_actions != nullptr) {
    m_custom_actions.assign(
        fl_node.custom_accessibility_actions,
        fl_node.custom_accessibility_actions +
            static_cast<size_t>(fl_node.custom_accessibility_actions_count));
  } else {
    m_custom_actions.clear();
  }

  // Derive role and states last: Translate() disambiguates a label-only leaf
  // from a generic container, so it needs the refreshed label and children.
  m_spec = accessibility::Translate(m_id, m_flags, m_actions, !m_label.empty(),
                                    !m_children.empty());
}

uint8_t AccessibilityNode::GetRole() const {
  // The AccessKit bridge consumes accesskit_role numerically. Only the two
  // roles that have a SemanticRole constant are mapped; the rest report
  // unknown, matching what this node reported before roles were derived
  // through the translator. Widening this mapping belongs at the AccessKit
  // boundary, where the accesskit_role values are in scope.
  switch (m_spec.role) {
    case accessibility::Role::kWindow:
      return SEMANTIC_ROLE_WINDOW;
    case accessibility::Role::kButton:
      return SEMANTIC_ROLE_BUTTON;
    default:
      return SEMANTIC_ROLE_UNKNOWN;
  }
}

AccessibilityTree::AccessibilityTree() : focused_node(0) {}
AccessibilityTree::~AccessibilityTree() = default;

void AccessibilityTree::HandleFlutterUpdate(
    const FlutterSemanticsUpdate2* update) {
  // Resolve the custom action declarations before the nodes, so a node that
  // references an action declared in this same batch can be looked up as soon
  // as the batch is applied.
  if (update->custom_actions != nullptr) {
    for (size_t i = 0; i < update->custom_action_count; i++) {
      const FlutterSemanticsCustomAction2* fl_action =
          update->custom_actions[i];
      if (fl_action == nullptr) {
        continue;
      }
      AccessibilityCustomAction& action = custom_actions[fl_action->id];
      action.id = fl_action->id;
      action.override_action = fl_action->override_action;
      // Owned copies, for the same reason the node text is owned: the
      // embedder's strings are only valid for this callback, and may be null.
      action.label = fl_action->label ? fl_action->label : "";
      action.hint = fl_action->hint ? fl_action->hint : "";
    }
  }

  // Upsert every node in this update: create the ones we have not seen and
  // refresh the rest. Update() replaces fields and children, so stale labels
  // and detached children never accumulate across successive updates.
  for (size_t i = 0; i < update->node_count; i++) {
    const FlutterSemanticsNode2* node = update->nodes[i];
    AccessibilityNode* mirror = GetNode(*node);
    mirror->Update(*node);
    if (node->flags & kFlutterSemanticsFlagIsFocused) {
      SetFocusedNode(node->id);
    }
  }

  if (!IsTreeBuilt()) {
    // The tree is only usable once the root (id 0) has arrived; Flutter sends
    // it in the first update. Hold off on the initial dump until then.
    if (node_index.find(0) == node_index.end()) {
      return;
    }
    SetTreeBuilt(true);

    std::filesystem::path dir = Utils::GetConfigHomePath();
    dir /= "accessibility";
    std::error_code ec;
    if (!std::filesystem::exists(dir)) {
      std::filesystem::create_directories(dir, ec);
    }
    const std::string target = (dir / "semantic_tree_init.json").string();
    // The accessibility directory is derived from the user's environment
    // (XDG_CONFIG_HOME, or HOME as a fallback). Reject any ".." traversal
    // segment before opening the file so a crafted environment cannot
    // redirect the write outside the intended config tree.
    if (target.find("..") != std::string::npos) {
      ihs::log::error(
          "Refusing to write accessibility tree dump to '{}': path contains "
          "a '..' traversal segment",
          target);
    } else {
      DumpTree(target.c_str());
    }
#if ENABLE_ACCESSKIT
    Init_AccessKit();
#endif
  }

  // Drop any node no longer reachable from the root: a subtree Flutter
  // detached is removed from the mirror instead of lingering forever.
  PruneUnreachable();
}

const AccessibilityCustomAction* AccessibilityTree::FindCustomAction(
    const int32_t id) const {
  const auto it = custom_actions.find(id);
  return it != custom_actions.end() ? &it->second : nullptr;
}

void AccessibilityTree::PruneUnreachable() {
  // Mark everything reachable from the root (id 0) via traversal-order
  // children.
  std::unordered_set<int32_t> reachable;
  std::vector<int32_t> stack;
  if (node_index.find(0) != node_index.end()) {
    reachable.insert(0);
    stack.push_back(0);
  }
  while (!stack.empty()) {
    const int32_t id = stack.back();
    stack.pop_back();
    const auto it = node_index.find(id);
    if (it == node_index.end()) {
      continue;  // referenced by a parent but not (yet) mirrored
    }
    const AccessibilityNode* n = it->second;
    for (uint32_t i = 0; i < n->NumberOfChildren(); i++) {
      const int32_t child = n->GetChild(static_cast<int32_t>(i));
      if (child >= 0 && reachable.insert(child).second) {
        stack.push_back(child);
      }
    }
  }

  // Drop the unreachable nodes from both the index and the owning vector.
  for (auto it = node_index.begin(); it != node_index.end();) {
    if (reachable.find(it->first) == reachable.end()) {
      it = node_index.erase(it);
    } else {
      ++it;
    }
  }
  nodes.erase(
      std::remove_if(nodes.begin(), nodes.end(),
                     [&reachable](const std::unique_ptr<AccessibilityNode>& n) {
                       return reachable.find(n->GetId()) == reachable.end();
                     }),
      nodes.end());

  // If the focused node was pruned, fall back to the root.
  if (node_index.find(GetFocusedNode()) == node_index.end()) {
    SetFocusedNode(0);
  }
}

AccessibilityNode* AccessibilityTree::GetNode(
    const FlutterSemanticsNode2& fl_node) {
  // Determine if node already created and return it (O(1) via the index).
  if (const auto it = node_index.find(fl_node.id); it != node_index.end()) {
    return it->second;
  }

  auto owned = std::make_unique<AccessibilityNode>(fl_node);
  AccessibilityNode* new_node = owned.get();
  nodes.emplace_back(std::move(owned));
  node_index.emplace(new_node->GetId(), new_node);
  IHS_TRACE("New AccessibilityNode created with ID: {}, number of nodes: {}",
            new_node->GetId(), nodes.size());
  return new_node;
}

void AccessibilityTree::DumpTree(const char* target_file) const {
  rapidjson::Document doc;
  doc.SetObject();
  rapidjson::Document::AllocatorType& allocator = doc.GetAllocator();

  rapidjson::Value nodes_array(rapidjson::kArrayType);

  for (auto& node : nodes) {
    rapidjson::Value node_obj(rapidjson::kObjectType);
    node_obj.AddMember("id", node->GetId(), allocator);
    node_obj.AddMember("label", rapidjson::Value(node->GetLabel(), allocator),
                       allocator);
    node_obj.AddMember("hint", rapidjson::Value(node->GetHint(), allocator),
                       allocator);
    node_obj.AddMember(
        "tooltip", rapidjson::Value(node->GetTooltip(), allocator), allocator);
    node_obj.AddMember("value", rapidjson::Value(node->GetValue(), allocator),
                       allocator);
    node_obj.AddMember("role", node->GetRole(), allocator);

    // Bounds as object
    rapidjson::Value bounds_obj(rapidjson::kObjectType);
    bounds_obj.AddMember("left", node->GetBounds().left, allocator);
    bounds_obj.AddMember("top", node->GetBounds().top, allocator);
    bounds_obj.AddMember("right", node->GetBounds().right, allocator);
    bounds_obj.AddMember("bottom", node->GetBounds().bottom, allocator);
    node_obj.AddMember("bounds", bounds_obj, allocator);

    // Node-to-parent transform. Bounds above are in the node's own coordinate
    // system, so the transform is what makes them comparable across the tree.
    const FlutterTransformation& transform = node->GetTransform();
    rapidjson::Value transform_obj(rapidjson::kObjectType);
    transform_obj.AddMember("scaleX", transform.scaleX, allocator);
    transform_obj.AddMember("skewX", transform.skewX, allocator);
    transform_obj.AddMember("transX", transform.transX, allocator);
    transform_obj.AddMember("skewY", transform.skewY, allocator);
    transform_obj.AddMember("scaleY", transform.scaleY, allocator);
    transform_obj.AddMember("transY", transform.transY, allocator);
    transform_obj.AddMember("pers0", transform.pers0, allocator);
    transform_obj.AddMember("pers1", transform.pers1, allocator);
    transform_obj.AddMember("pers2", transform.pers2, allocator);
    node_obj.AddMember("transform", transform_obj, allocator);

    // Flags and actions, as the raw bitmasks the embedder supplied.
    node_obj.AddMember("flags", node->GetFlags(), allocator);
    node_obj.AddMember("actions", node->GetActions(), allocator);

    // Children as array
    rapidjson::Value children_array(rapidjson::kArrayType);
    for (uint32_t i = 0; i < node->NumberOfChildren(); i++) {
      children_array.PushBack(node->GetChild(static_cast<int32_t>(i)),
                              allocator);
    }
    node_obj.AddMember("children", children_array, allocator);

    // IDs of the custom actions this node offers; the declarations are
    // emitted once in the tree-level "custom_actions" table below.
    rapidjson::Value node_custom_actions(rapidjson::kArrayType);
    for (uint32_t i = 0; i < node->NumberOfCustomActions(); i++) {
      node_custom_actions.PushBack(
          node->GetCustomActionId(static_cast<int32_t>(i)), allocator);
    }
    node_obj.AddMember("custom_actions", node_custom_actions, allocator);

    nodes_array.PushBack(node_obj, allocator);
  }

  doc.AddMember("nodes", nodes_array, allocator);

  // Custom action declarations, shared by ID across the nodes above. Emitted
  // in ID order: the declarations live in an unordered_map, and a dump that
  // reshuffles between runs cannot be diffed against an earlier one.
  std::vector<int32_t> action_ids;
  action_ids.reserve(custom_actions.size());
  for (const auto& entry : custom_actions) {
    action_ids.push_back(entry.first);
  }
  std::sort(action_ids.begin(), action_ids.end());

  rapidjson::Value custom_actions_array(rapidjson::kArrayType);
  for (const int32_t id : action_ids) {
    const AccessibilityCustomAction& action = custom_actions.at(id);
    rapidjson::Value action_obj(rapidjson::kObjectType);
    action_obj.AddMember("id", action.id, allocator);
    action_obj.AddMember("override_action", action.override_action, allocator);
    action_obj.AddMember(
        "label", rapidjson::Value(action.label.c_str(), allocator), allocator);
    action_obj.AddMember(
        "hint", rapidjson::Value(action.hint.c_str(), allocator), allocator);
    custom_actions_array.PushBack(action_obj, allocator);
  }
  doc.AddMember("custom_actions", custom_actions_array, allocator);

  // Write to file
  rapidjson::StringBuffer buffer;
  rapidjson::PrettyWriter writer(buffer);
  doc.Accept(writer);

  if (std::ofstream ofs(target_file); ofs.is_open()) {
    ofs << buffer.GetString();
    ofs.close();
    ihs::log::info("Accessibility tree dumped to {}", target_file);
  } else {
    ihs::log::error("Failed to open {} for writing", target_file);
  }
}

#if ENABLE_ACCESSKIT
accesskit_tree_update* build_tree_update_with_focus_update(void* userdata) {
  const auto* tree = static_cast<AccessibilityTree*>(userdata);
  accesskit_tree_update* update =
      accesskit_tree_update_with_focus(tree->GetFocusedNode());
  return update;
}

accesskit_tree_update* activation_handler_cbk(void* userdata) {
  auto* tree = static_cast<AccessibilityTree*>(userdata);

  accesskit_tree_update* result =
      accesskit_tree_update_with_capacity_and_focus(tree->NumberOfNodes(), 0);
  accesskit_tree* ak_tree = accesskit_tree_new(0);
  accesskit_tree_update_set_tree(result, ak_tree);

  for (auto node_idx = 0; node_idx < tree->NumberOfNodes(); node_idx++) {
    const AccessibilityNode* accessibility_node = tree->GetNodeByIdx(node_idx);
    accesskit_node* ak_node =
        accesskit_node_new((accesskit_role)accessibility_node->GetRole());
    for (uint32_t i = 0; i < accessibility_node->NumberOfChildren(); i++) {
      accesskit_node_push_child(ak_node, accessibility_node->GetChild(i));
    }
    accesskit_node_set_label(ak_node, accessibility_node->GetLabel());
    accesskit_node_set_description(ak_node, accessibility_node->GetHint());
    accesskit_node_set_value(ak_node, accessibility_node->GetValue());

    accesskit_rect bounds = {accessibility_node->GetBounds().left,
                             accessibility_node->GetBounds().top,
                             accessibility_node->GetBounds().right,
                             accessibility_node->GetBounds().bottom};
    accesskit_node_set_bounds(ak_node, bounds);

    if (accessibility_node->GetFlags() & kFlutterSemanticsFlagIsFocusable) {
      accesskit_node_add_action(ak_node, ACCESSKIT_ACTION_FOCUS);
    }

    if (accessibility_node->GetFlags() &
        (kFlutterSemanticsFlagHasCheckedState |
         kFlutterSemanticsFlagHasToggledState)) {
      accesskit_node_add_action(ak_node, ACCESSKIT_ACTION_CLICK);
    }
    // Todo: Handle more flags

    accesskit_tree_update_push_node(result, accessibility_node->GetId(),
                                    ak_node);
  }

  tree->AccessKit_SetWindowFocus(true);

  return result;
}

void action_handler_cbk(accesskit_action_request* /* request */,
                        void* /* userdata */) {
  ihs::log::debug("accesskit_wrapper: action_handler++");
}

void deactivation_handler_cbk(void* /* userdata */) {
  ihs::log::debug("accesskit_wrapper: deactivation_handler++");
}

void AccessibilityTree::Init_AccessKit() {
  adapter = accesskit_unix_adapter_new(activation_handler_cbk, this,
                                       action_handler_cbk, this,
                                       deactivation_handler_cbk, this);
  assert(adapter != nullptr);
}

void AccessibilityTree::AccessKit_SetWindowFocus(bool focused) {
  accesskit_unix_adapter_update_window_focus_state(adapter, focused);
}

void AccessibilityTree::AccessKit_SetFocusedNode(
    accesskit_node_id focused_node) {
  SetFocusedNode(focused_node);
  accesskit_unix_adapter_update_if_active(
      adapter, build_tree_update_with_focus_update, this);
}
#endif
