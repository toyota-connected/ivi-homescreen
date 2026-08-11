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

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <shell/platform/embedder/embedder.h>

#include "semantics_translator.h"

// Enum representing semantic roles for accessibility nodes.
// These roles are used to describe the purpose of a UI element.
enum SemanticRole {
  SEMANTIC_ROLE_UNKNOWN = 0,  // Default role for unknown elements.
  SEMANTIC_ROLE_BUTTON = 18,  // Role for button elements.
  SEMANTIC_ROLE_WINDOW = 137  // Role for window elements.
};

// A custom semantics action declared by the application, resolved from the
// FlutterSemanticsCustomAction2 batch that accompanies a semantics update.
// Nodes reference these by ID; the labels and hints live here once rather than
// being duplicated onto every referencing node.
struct AccessibilityCustomAction {
  int32_t id = -1;
  // For an action override, the standard action this replaces; zero otherwise.
  FlutterSemanticsAction override_action =
      static_cast<FlutterSemanticsAction>(0);
  std::string label;  // Owned copy; the embedder's string is transient.
  std::string hint;   // Owned copy; the embedder's string is transient.
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

  // Refreshes all fields (label / hint / value / tooltip, bounds, transform,
  // flags, actions, role) and REPLACES the children and custom-action ID lists
  // from a Flutter semantics node. Called on create and on every subsequent
  // update, so stale labels or detached children never accumulate across
  // updates.
  void Update(const FlutterSemanticsNode2& fl_node);

  // Returns the role of the node, in the numeric encoding the AccessKit bridge
  // consumes. Narrower than GetSpec().role: only the roles that have a
  // SemanticRole constant are distinguished, the rest collapse to unknown.
  // Prefer GetSpec() for anything that is not the AccessKit boundary.
  [[nodiscard]] uint8_t GetRole() const;

  // Returns the derived, backend-neutral role/state/action attributes of the
  // node. This is the full translation of flags and actions; see
  // semantics_translator.h.
  [[nodiscard]] const accessibility::NodeSpec& GetSpec() const {
    return m_spec;
  };

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

  // Returns the application-assigned stable identifier, or "" when the node
  // carries no annotation. Unlike GetId(), this survives tree rebuilds, so it
  // is the addressing key a caller should prefer where it is present.
  [[nodiscard]] const char* GetIdentifier() const {
    return m_identifier.c_str();
  };

  // Returns the bounds of the node, in its own coordinate system. Composing
  // GetTransform() down the traversal path yields screen space.
  [[nodiscard]] FlutterRect GetBounds() const { return m_bounds; };

  // Returns the transform from this node's coordinate system to its parent's.
  [[nodiscard]] const FlutterTransformation& GetTransform() const {
    return m_transform;
  };

  // Returns the flags associated with the node.
  [[nodiscard]] FlutterSemanticsFlag GetFlags() const { return m_flags; };

  // Returns the set of semantics actions applicable to this node, as a
  // FlutterSemanticsAction bitmask.
  [[nodiscard]] FlutterSemanticsAction GetActions() const { return m_actions; };

  // Returns whether every bit in `action` is set on this node. Pass a single
  // action bit to test one action.
  [[nodiscard]] bool HasAction(const FlutterSemanticsAction action) const {
    return (static_cast<uint32_t>(m_actions) & static_cast<uint32_t>(action)) ==
           static_cast<uint32_t>(action);
  }

  // Returns the number of child nodes.
  [[nodiscard]] uint32_t NumberOfChildren() const {
    return static_cast<uint32_t>(m_children.size());
  };

  // Returns the child node ID at the specified index.
  // If the index is out of bounds, returns -1.
  [[nodiscard]] int32_t GetChild(const int32_t idx) const {
    return idx >= 0 && static_cast<size_t>(idx) < m_children.size()
               ? m_children[static_cast<size_t>(idx)]
               : -1;
  }

  // Returns the number of custom semantics actions referenced by this node.
  [[nodiscard]] uint32_t NumberOfCustomActions() const {
    return static_cast<uint32_t>(m_custom_actions.size());
  };

  // Returns the custom action ID at the specified index. Resolve it to a
  // label and hint with AccessibilityTree::FindCustomAction().
  // If the index is out of bounds, returns -1.
  [[nodiscard]] int32_t GetCustomActionId(const int32_t idx) const {
    return idx >= 0 && static_cast<size_t>(idx) < m_custom_actions.size()
               ? m_custom_actions[static_cast<size_t>(idx)]
               : -1;
  }

 private:
  // The scalar members are set unconditionally by Update() (called from the
  // constructor), but carry in-class initializers so the type is never left
  // with an indeterminate field on any construction path.
  int32_t m_id = -1;         // ID of the node.
  std::string m_label;       // Label of the node (owned copy).
  std::string m_hint;        // Hint text of the node (owned copy).
  std::string m_value;       // Value of the node (owned copy).
  std::string m_tooltip;     // Tooltip text of the node (owned copy).
  std::string m_identifier;  // App-assigned stable ID (owned copy); may be "".
  FlutterRect m_bounds{};    // Bounds of the node.
  FlutterTransformation m_transform{};  // Node-to-parent transform.
  FlutterSemanticsFlag m_flags =
      static_cast<FlutterSemanticsFlag>(0);  // Flags associated with the node.
  FlutterSemanticsAction m_actions =
      static_cast<FlutterSemanticsAction>(0);  // Applicable action bitmask.
  accessibility::NodeSpec m_spec;   // Role and states derived from the above.
  std::vector<int32_t> m_children;  // List of child node IDs.
  std::vector<int32_t> m_custom_actions;  // Referenced custom action IDs.
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

  // Resolves a custom action ID (from AccessibilityNode::GetCustomActionId())
  // to its declaration. Returns nullptr if the ID has not been declared.
  // Declarations are never erased, so a non-null result stays valid for the
  // life of the tree, but a later update may refresh its label and hint.
  [[nodiscard]] const AccessibilityCustomAction* FindCustomAction(
      int32_t id) const;

  // Returns the number of declared custom semantics actions.
  [[nodiscard]] size_t NumberOfCustomActions() const {
    return custom_actions.size();
  };

  // Retrieves a node by its tree id, or nullptr when absent. O(1); prefer this
  // to walking GetNodeByIdx when following a parent's child ids.
  [[nodiscard]] AccessibilityNode* FindNode(const int32_t node_id) const {
    const auto it = node_index.find(node_id);
    return it != node_index.end() ? it->second : nullptr;
  }

  // Invokes `fn` for each declared custom action, in ascending id order so
  // repeated traversals of an unchanged tree agree with each other.
  template <typename Fn>
  void ForEachCustomAction(Fn&& fn) const {
    std::vector<int32_t> ids;
    ids.reserve(custom_actions.size());
    for (const auto& entry : custom_actions) {
      ids.push_back(entry.first);
    }
    std::sort(ids.begin(), ids.end());
    for (const int32_t id : ids) {
      fn(custom_actions.at(id));
    }
  }

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
  // Declared custom actions, keyed by ID. Deliberately not pruned alongside
  // the nodes: Flutter assigns a custom action its ID once and may send the
  // declaration in an earlier batch than the node that references it, so
  // dropping an unreferenced entry would lose it permanently. The set is
  // bounded by the number of distinct CustomSemanticsAction values the
  // application declares, not by tree size or update count.
  std::unordered_map<int32_t, AccessibilityCustomAction> custom_actions;
  int32_t focused_node;  // ID of the currently focused node.
};

#endif  // SHELL_ACCESSIBILITY_ACCESSIBILITY_TREE_H_