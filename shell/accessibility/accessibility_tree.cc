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

#include <fstream>

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include "logging/logging.h"
#include "utils.h"

void AccessibilityNode::AddChild(const int32_t child_id) {
  m_children.push_back(child_id);
}

void AccessibilityNode::UpdateNode(FlutterSemanticsNode2* /* fl_node */) {
  // Look through all fl_node updates, compare to existing data, update where
  // necessary
}

AccessibilityNode::AccessibilityNode(const FlutterSemanticsNode2& fl_node)
    : m_id(fl_node.id),
      m_label(fl_node.label),
      m_hint(fl_node.hint),
      m_value(fl_node.value),
      m_tooltip(fl_node.tooltip),
      m_bounds(fl_node.rect),
      m_flags(fl_node.flags) {
  // Determine Role
  uint8_t node_role;
  if (fl_node.id == 0) {
    node_role = SEMANTIC_ROLE_WINDOW;
  } else if (fl_node.flags & kFlutterSemanticsFlagIsButton) {
    node_role = SEMANTIC_ROLE_BUTTON;
  } else {
    // TODO: Add more role handling
    node_role = SEMANTIC_ROLE_UNKNOWN;
  }
  m_role = node_role;

  // TODO: Initialize more info from flags and actions
}

AccessibilityTree::AccessibilityTree() : focused_node(0) {}
AccessibilityTree::~AccessibilityTree() = default;

void AccessibilityTree::HandleFlutterUpdate(
    const FlutterSemanticsUpdate2* update) {
  if (!IsTreeBuilt()) {
    // Should be the first time this callback fired, with initial states of all
    // nodes. Verify node 0 (root node) exists
    bool root_node_found = false;
    for (int i = 0; i < static_cast<int>(update->node_count); i++) {
      if (const FlutterSemanticsNode2* node = update->nodes[i]; node->id == 0) {
        root_node_found = true;
      }
    }

    if (root_node_found) {
      // Iterate through and create all the nodes
      for (int i = 0; i < static_cast<int>(update->node_count); i++) {
        const FlutterSemanticsNode2* node = update->nodes[i];
        // get_node creates the node if it doesn't already exist (which it
        // shouldn't)
        AccessibilityNode* new_node = GetNode(*node);
        for (int j = 0; j < static_cast<int>(node->child_count); j++) {
          new_node->AddChild(node->children_in_traversal_order[j]);
        }
      }
      SetTreeBuilt(true);
      std::filesystem::path base_path = Utils::GetConfigHomePath();
      base_path /= "accessibility";
      base_path /= "semantic_tree_init.json";
      DumpTree(base_path.c_str());
#if ENABLE_ACCESSKIT
      Init_AccessKit();
#endif
    }
  } else {
    // Tree is built, just provide updates
    for (int i = 0; i < static_cast<int>(update->node_count); i++) {
      FlutterSemanticsNode2* node = update->nodes[i];
      const AccessibilityNode* updated_node = GetNode(*node);
      if (node->flags & kFlutterSemanticsFlagIsFocused) {
        SetFocusedNode(updated_node->GetId());
      }
      AccessibilityNode::UpdateNode(node);
    }
  }

  for (int j = 0; j < static_cast<int>(update->custom_action_count); j++) {
    // TODO: handle custom actions
  }
}

AccessibilityNode* AccessibilityTree::GetNode(
    const FlutterSemanticsNode2& fl_node) {
  // Determine if node already created and return it
  for (const auto& node : nodes) {
    if (node->GetId() == fl_node.id) {
      return node;
    }
  }

  auto new_node = new AccessibilityNode(fl_node);
  nodes.emplace_back(new_node);
  spdlog::debug(
      "New AccessibilityNode created with ID: {}, number of nodes: {}",
      new_node->GetId(), nodes.size());
  return nodes.back();
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

    // Flags
    node_obj.AddMember("flags", node->GetFlags(), allocator);

    // Children as array
    rapidjson::Value children_array(rapidjson::kArrayType);
    for (uint32_t i = 0; i < node->NumberOfChildren(); i++) {
      children_array.PushBack(node->GetChild(static_cast<int32_t>(i)),
                              allocator);
    }
    node_obj.AddMember("children", children_array, allocator);

    nodes_array.PushBack(node_obj, allocator);
  }

  doc.AddMember("nodes", nodes_array, allocator);

  // Write to file
  rapidjson::StringBuffer buffer;
  rapidjson::PrettyWriter writer(buffer);
  doc.Accept(writer);

  if (std::ofstream ofs(target_file); ofs.is_open()) {
    ofs << buffer.GetString();
    ofs.close();
    spdlog::info("Accessibility tree dumped to {}", target_file);
  } else {
    spdlog::error("Failed to open {} for writing", target_file);
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
  spdlog::debug("accesskit_wrapper: action_handler++");
}

void deactivation_handler_cbk(void* /* userdata */) {
  spdlog::debug("accesskit_wrapper: deactivation_handler++");
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
