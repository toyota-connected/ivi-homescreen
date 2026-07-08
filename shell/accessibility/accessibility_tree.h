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

#ifndef SHELL_ACCESSIBILITY_ACCESSIBILITY_TREE_H_
#define SHELL_ACCESSIBILITY_ACCESSIBILITY_TREE_H_

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#if ENABLE_ACCESSKIT
#include <accesskit.h>
#endif

#include <shell/platform/embedder/embedder.h>

// Enum representing semantic roles for accessibility nodes.
// These roles are used to describe the purpose of a UI element.
enum SemanticRole {
  SEMANTIC_ROLE_UNKNOWN = 0,  // Default role for unknown elements.
  SEMANTIC_ROLE_BUTTON = 18,  // Role for button elements.
  SEMANTIC_ROLE_WINDOW = 137  // Role for window elements.
};

// Class representing an accessibility node in the accessibility tree.
class AccessibilityNode {
 public:
  // Constructor that initializes the node with a Flutter semantics node.
  explicit AccessibilityNode(const FlutterSemanticsNode2& fl_node);

  // Destructor.
  ~AccessibilityNode() = default;

  // Returns the ID of the node.
  [[nodiscard]] int32_t GetId() const { return m_id; };

  // Refreshes all fields (label / hint / value / tooltip, bounds, flags, role)
  // and REPLACES the children list from a Flutter semantics node. Called on
  // create and on every subsequent update, so stale labels or detached
  // children never accumulate across updates.
  void Update(const FlutterSemanticsNode2& fl_node);

  // Returns the role of the node.
  [[nodiscard]] uint8_t GetRole() const { return m_role; };

  // Returns the label of the node. Backed by an owned copy, so the pointer
  // stays valid for the node's lifetime (unlike the embedder's transient
  // FlutterSemanticsNode2 strings, which live only for the update callback).
  [[nodiscard]] const char* GetLabel() const { return m_label.c_str(); };

  // Returns the hint text of the node.
  [[nodiscard]] const char* GetHint() const { return m_hint.c_str(); };

  // Returns the tooltip text of the node.
  [[nodiscard]] const char* GetTooltip() const { return m_tooltip.c_str(); };

  // Returns the value of the node.
  [[nodiscard]] const char* GetValue() const { return m_value.c_str(); };

  // Returns the bounds of the node.
  [[nodiscard]] FlutterRect GetBounds() const { return m_bounds; };

  // Returns the flags associated with the node.
  [[nodiscard]] FlutterSemanticsFlag GetFlags() const { return m_flags; };

  // Returns the number of child nodes.
  [[nodiscard]] uint32_t NumberOfChildren() const { return m_children.size(); };

  // Returns the child node ID at the specified index.
  // If the index is out of bounds, returns -1.
  [[nodiscard]] int32_t GetChild(const int32_t idx) const {
    return idx >= 0 && static_cast<size_t>(idx) < m_children.size()
               ? m_children[static_cast<size_t>(idx)]
               : -1;
  }

 private:
  // The scalar members are set unconditionally by Update() (called from the
  // constructor), but carry in-class initializers so the type is never left
  // with an indeterminate field on any construction path.
  uint8_t m_role = SEMANTIC_ROLE_UNKNOWN;  // Role of the node.
  int32_t m_id = -1;                       // ID of the node.
  std::string m_label;                     // Label of the node (owned copy).
  std::string m_hint;      // Hint text of the node (owned copy).
  std::string m_value;     // Value of the node (owned copy).
  std::string m_tooltip;   // Tooltip text of the node (owned copy).
  FlutterRect m_bounds{};  // Bounds of the node.
  FlutterSemanticsFlag m_flags =
      static_cast<FlutterSemanticsFlag>(0);  // Flags associated with the node.
  std::vector<int32_t> m_children;           // List of child node IDs.
};

// Class representing the accessibility tree.
class AccessibilityTree {
 public:
  // Constructor.
  AccessibilityTree();

  // Destructor.
  ~AccessibilityTree();

  // Handles updates from Flutter semantics.
  void HandleFlutterUpdate(const FlutterSemanticsUpdate2* update);

  // Retrieves an accessibility node corresponding to a Flutter semantics node.
  AccessibilityNode* GetNode(const FlutterSemanticsNode2& fl_node);

  // Retrieves an accessibility node by its index.
  // If the index is out of bounds, returns nullptr.
  [[nodiscard]] AccessibilityNode* GetNodeByIdx(const int32_t idx) const {
    return (idx >= 0 && static_cast<size_t>(idx) < nodes.size())
               ? nodes[static_cast<size_t>(idx)].get()
               : nullptr;
  }

  // Returns the ID of the currently focused node.
  [[nodiscard]] int32_t GetFocusedNode() const { return focused_node; };

  // Sets the ID of the currently focused node.
  void SetFocusedNode(const int32_t node) { focused_node = node; };

  // Returns whether the accessibility tree has been built.
  [[nodiscard]] bool IsTreeBuilt() const { return tree_built; };

  // Sets the state of the tree build flag.
  void SetTreeBuilt(const bool state) { tree_built = state; };

  // Dumps the accessibility tree to a file.
  void DumpTree(const char* target_file) const;

  // Returns the number of nodes in the tree.
  [[nodiscard]] int32_t NumberOfNodes() const {
    return static_cast<int32_t>(nodes.size());
  };

#if ENABLE_ACCESSKIT
  // Initializes AccessKit support.
  void Init_AccessKit();

  // Sets the window focus state for AccessKit.
  void AccessKit_SetWindowFocus(bool focused);

  // Sets the focused node for AccessKit.
  void AccessKit_SetFocusedNode(accesskit_node_id focused_node);
#endif

 private:
  // Drops nodes no longer reachable from the root (id 0), keeping `nodes` and
  // `node_index` consistent after a subtree is detached in an update.
  void PruneUnreachable();

  bool tree_built = false;  // Flag indicating if the tree is built.
  // Owns the nodes. node_index below holds non-owning raw pointers into these,
  // so the tree destructs cleanly without a manual delete loop.
  std::vector<std::unique_ptr<AccessibilityNode>> nodes;
  // Index from node id to node, kept in sync with `nodes` so GetNode() is
  // O(1) instead of a linear scan (semantics updates touch many nodes per
  // update, which would otherwise be O(n^2)). Non-owning.
  std::unordered_map<int32_t, AccessibilityNode*> node_index;
  int32_t focused_node;  // ID of the currently focused node.

#if ENABLE_ACCESSKIT
  accesskit_unix_adapter* adapter{};  // AccessKit adapter for Unix systems.
#endif
};

#endif  // SHELL_ACCESSIBILITY_ACCESSIBILITY_TREE_H_