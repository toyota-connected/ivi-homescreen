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

// The semantics MCP provider: `ui_*` tools and `ui://` resources over the
// semantics hub. See include/ihs/ihs_mcp_semantics.h for the lifecycle.

#include "ihs/ihs_mcp_semantics.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "ihs/ihs_mcp_provider.h"
#include "ihs/ihs_semantics.h"

namespace {

// One tool as this provider declares it, paired with the hub action it
// dispatches. A zero action marks a tool that is answered from the snapshot
// rather than sent to the framework.
struct ToolEntry {
  const char* name;
  const char* description;
  const char* schema;
  uint64_t action;
  uint64_t capability;
};

// A node argument, shared by every action tool: either the numeric tree id or
// the application's stable identifier. Both are offered because `identifier`
// is absent on the engine revisions currently deployed, so a client that has
// only ids must still be able to act.
constexpr char kNodeSchema[] =
    R"({"type":"object","properties":{)"
    R"("node_id":{"type":"integer","description":"Semantics tree id."},)"
    R"("identifier":{"type":"string","description":"Application-assigned identifier, when the app sets one."})"
    R"(},"anyOf":[{"required":["node_id"]},{"required":["identifier"]}]})";

constexpr char kEmptySchema[] = R"({"type":"object","properties":{}})";

constexpr char kQuerySchema[] =
    R"({"type":"object","properties":{)"
    R"("identifier":{"type":"string"},)"
    R"("label":{"type":"string","description":"Substring match, case sensitive."},)"
    R"("role":{"type":"string","description":"Role name, e.g. button or slider."})"
    R"(}})";

const ToolEntry kTools[] = {
    {"snapshot", "The whole semantics tree as JSON.", kEmptySchema, 0,
     IHS_MCP_CAP_INSPECT},
    {"query", "Find nodes by identifier, label substring, or role.",
     kQuerySchema, 0, IHS_MCP_CAP_INSPECT},
    {"tap", "Activate a node, as a tap would.", kNodeSchema,
     IHS_SEMANTICS_ACTION_TAP, IHS_MCP_CAP_INTERACT},
    {"long_press", "Long-press a node.", kNodeSchema,
     IHS_SEMANTICS_ACTION_LONG_PRESS, IHS_MCP_CAP_INTERACT},
    {"increase", "Increment a value, as on a slider.", kNodeSchema,
     IHS_SEMANTICS_ACTION_INCREASE, IHS_MCP_CAP_INTERACT},
    {"decrease", "Decrement a value.", kNodeSchema,
     IHS_SEMANTICS_ACTION_DECREASE, IHS_MCP_CAP_INTERACT},
    {"show_on_screen", "Scroll a node into view.", kNodeSchema,
     IHS_SEMANTICS_ACTION_SHOW_ON_SCREEN, IHS_MCP_CAP_INTERACT},
    {"dismiss", "Dismiss a node, as a swipe-away would.", kNodeSchema,
     IHS_SEMANTICS_ACTION_DISMISS, IHS_MCP_CAP_INTERACT},
    {"expand", "Expand a node.", kNodeSchema, IHS_SEMANTICS_ACTION_EXPAND,
     IHS_MCP_CAP_INTERACT},
    {"collapse", "Collapse a node.", kNodeSchema, IHS_SEMANTICS_ACTION_COLLAPSE,
     IHS_MCP_CAP_INTERACT},
};

// The accessibility-focus actions are deliberately absent from the table
// above. They move a screen reader's cursor, and an agent driving the UI must
// not be able to yank it away from someone reading the screen. The hub's
// allow mask below enforces the same thing a second time, at the funnel.
constexpr uint64_t kAllowMask = IHS_SEMANTICS_ACTION_NO_A11Y_FOCUS;

const char* RoleName(const IhsSemanticsRole role) {
  switch (role) {
    case IHS_SEMANTICS_ROLE_WINDOW:
      return "window";
    case IHS_SEMANTICS_ROLE_BUTTON:
      return "button";
    case IHS_SEMANTICS_ROLE_TEXT_INPUT:
      return "text_input";
    case IHS_SEMANTICS_ROLE_MULTILINE_TEXT_INPUT:
      return "multiline_text_input";
    case IHS_SEMANTICS_ROLE_PASSWORD_INPUT:
      return "password_input";
    case IHS_SEMANTICS_ROLE_SLIDER:
      return "slider";
    case IHS_SEMANTICS_ROLE_SWITCH:
      return "switch";
    case IHS_SEMANTICS_ROLE_CHECK_BOX:
      return "check_box";
    case IHS_SEMANTICS_ROLE_RADIO_BUTTON:
      return "radio_button";
    case IHS_SEMANTICS_ROLE_LINK:
      return "link";
    case IHS_SEMANTICS_ROLE_IMAGE:
      return "image";
    case IHS_SEMANTICS_ROLE_HEADING:
      return "heading";
    case IHS_SEMANTICS_ROLE_SCROLL_VIEW:
      return "scroll_view";
    case IHS_SEMANTICS_ROLE_PANE:
      return "pane";
    case IHS_SEMANTICS_ROLE_LABEL:
      return "label";
    case IHS_SEMANTICS_ROLE_GENERIC_CONTAINER:
      return "generic_container";
    case IHS_SEMANTICS_ROLE_UNKNOWN:
      break;
  }
  return "unknown";
}

// Tristates cross the wire as three values rather than a bool. A client
// reading "enabled": false must be able to tell a disabled control from one
// where enablement is meaningless, which is the whole reason the hub carries
// the distinction.
const char* TristateName(const IhsSemanticsTristate value) {
  switch (value) {
    case IHS_SEMANTICS_TRISTATE_TRUE:
      return "true";
    case IHS_SEMANTICS_TRISTATE_FALSE:
      return "false";
    case IHS_SEMANTICS_TRISTATE_NONE:
      break;
  }
  return "not_applicable";
}

const char* CheckStateName(const IhsSemanticsCheckState value) {
  switch (value) {
    case IHS_SEMANTICS_CHECK_TRUE:
      return "true";
    case IHS_SEMANTICS_CHECK_FALSE:
      return "false";
    case IHS_SEMANTICS_CHECK_MIXED:
      return "mixed";
    case IHS_SEMANTICS_CHECK_NONE:
      break;
  }
  return "not_applicable";
}

// The verbs a client may invoke on this node, named as the tools that carry
// them, so a caller reads the answer and calls it without a second mapping.
void WriteActions(rapidjson::Writer<rapidjson::StringBuffer>& w,
                  const uint64_t actions) {
  w.Key("actions");
  w.StartArray();
  for (const ToolEntry& tool : kTools) {
    if (tool.action != 0 && (actions & tool.action) != 0) {
      w.String(tool.name);
    }
  }
  w.EndArray();
}

void WriteNode(rapidjson::Writer<rapidjson::StringBuffer>& w,
               const IhsSemanticsNode* node,
               const IhsSemanticsSnapshot* snapshot) {
  w.StartObject();
  w.Key("id");
  w.Int(node->id);
  if (node->identifier[0] != '\0') {
    // Omitted rather than emitted empty, so a client can tell "this app does
    // not annotate" from "this engine cannot report it" only by its absence
    // everywhere -- which is the honest signal either way.
    w.Key("identifier");
    w.String(node->identifier);
  }
  w.Key("role");
  w.String(RoleName(node->role));
  w.Key("label");
  w.String(node->label);
  if (node->hint[0] != '\0') {
    w.Key("hint");
    w.String(node->hint);
  }
  // A password field's contents are not something to hand to a client. The
  // role is the reliable signal, since it is derived from the same obscured
  // flag a screen reader uses to suppress the characters.
  if (node->role != IHS_SEMANTICS_ROLE_PASSWORD_INPUT && !node->obscured &&
      node->value[0] != '\0') {
    w.Key("value");
    w.String(node->value);
  }
  if (node->tooltip[0] != '\0') {
    w.Key("tooltip");
    w.String(node->tooltip);
  }

  w.Key("rect");
  w.StartObject();
  w.Key("x");
  w.Double(node->rect.left);
  w.Key("y");
  w.Double(node->rect.top);
  w.Key("w");
  w.Double(node->rect.right - node->rect.left);
  w.Key("h");
  w.Double(node->rect.bottom - node->rect.top);
  w.EndObject();

  w.Key("state");
  w.StartObject();
  w.Key("enabled");
  w.String(TristateName(node->enabled));
  w.Key("checked");
  w.String(CheckStateName(node->checked));
  w.Key("selected");
  w.String(TristateName(node->selected));
  w.Key("toggled");
  w.String(TristateName(node->toggled));
  w.Key("expanded");
  w.String(TristateName(node->expanded));
  w.Key("focused");
  w.String(TristateName(node->focused));
  w.Key("required");
  w.String(TristateName(node->required));
  w.Key("hidden");
  w.Bool(node->hidden);
  w.Key("read_only");
  w.Bool(node->read_only);
  w.EndObject();

  WriteActions(w, node->actions);

  if (node->custom_action_count > 0) {
    w.Key("custom_actions");
    w.StartArray();
    for (size_t i = 0; i < node->custom_action_count; i++) {
      const IhsSemanticsCustomAction* action = ihs_semantics_find_custom_action(
          snapshot, node->custom_action_ids[i]);
      w.StartObject();
      w.Key("id");
      w.Int(node->custom_action_ids[i]);
      if (action != nullptr) {
        w.Key("label");
        w.String(action->label);
      }
      w.EndObject();
    }
    w.EndArray();
  }

  w.Key("children");
  w.StartArray();
  for (size_t i = 0; i < node->child_count; i++) {
    const IhsSemanticsNode* child =
        ihs_semantics_snapshot_node_at(snapshot, node->child_indices[i]);
    if (child != nullptr) {
      w.Int(child->id);
    }
  }
  w.EndArray();
  w.EndObject();
}

std::string SerializeTree(const IhsSemanticsSnapshot* snapshot) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
  w.StartObject();
  w.Key("generation");
  w.Uint64(ihs_semantics_snapshot_generation(snapshot));
  w.Key("nodes");
  w.StartArray();
  const size_t count = ihs_semantics_snapshot_node_count(snapshot);
  for (size_t i = 0; i < count; i++) {
    const IhsSemanticsNode* node = ihs_semantics_snapshot_node_at(snapshot, i);
    if (node != nullptr) {
      WriteNode(w, node, snapshot);
    }
  }
  w.EndArray();
  w.EndObject();
  return buffer.GetString();
}

// A payload the host frees through the release hook. Heap-allocated because
// the host may outlive the call that produced it.
void FillPayload(IhsMcpPayload* out, std::string text) {
  auto* owned = new std::string(std::move(text));
  out->struct_size = sizeof(IhsMcpPayload);
  out->data = owned->c_str();
  out->length = owned->size();
  out->release_ctx = owned;
  out->release = [](void* ctx) { delete static_cast<std::string*>(ctx); };
}

std::string ErrorJson(const char* reason, const uint64_t generation) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
  w.StartObject();
  w.Key("error");
  w.String(reason);
  w.Key("generation");
  w.Uint64(generation);
  w.EndObject();
  return buffer.GetString();
}

// Minimal extraction for the two argument shapes this provider accepts. A full
// parser is not warranted: the host hands over a JSON object, and what is
// needed from it is one integer or one string.
bool ExtractInt(const char* json, const char* key, int32_t* out) {
  const std::string needle = std::string("\"") + key + "\"";
  const char* at = std::strstr(json, needle.c_str());
  if (at == nullptr) {
    return false;
  }
  at = std::strchr(at + needle.size(), ':');
  if (at == nullptr) {
    return false;
  }
  // strtol rather than sscanf: this reports whether anything was consumed and
  // whether the value fits, so a malformed argument is refused rather than
  // silently becoming node 0 -- which is the root, and would act on the wrong
  // thing.
  errno = 0;
  char* end = nullptr;
  const long value = std::strtol(at + 1, &end, 10);
  if (end == at + 1 || errno == ERANGE || value < INT32_MIN ||
      value > INT32_MAX) {
    return false;
  }
  *out = static_cast<int32_t>(value);
  return true;
}

bool ExtractString(const char* json, const char* key, std::string* out) {
  const std::string needle = std::string("\"") + key + "\"";
  const char* at = std::strstr(json, needle.c_str());
  if (at == nullptr) {
    return false;
  }
  at = std::strchr(at + needle.size(), ':');
  if (at == nullptr) {
    return false;
  }
  const char* open = std::strchr(at, '"');
  if (open == nullptr) {
    return false;
  }
  const char* close = std::strchr(open + 1, '"');
  if (close == nullptr) {
    return false;
  }
  out->assign(open + 1, static_cast<size_t>(close - open - 1));
  return true;
}

// Resolves the node a tool call names, by id or by identifier.
const IhsSemanticsNode* ResolveNode(const IhsSemanticsSnapshot* snapshot,
                                    const char* arguments) {
  int32_t id = 0;
  if (ExtractInt(arguments, "node_id", &id)) {
    return ihs_semantics_snapshot_node_by_id(snapshot, id);
  }
  std::string identifier;
  if (ExtractString(arguments, "identifier", &identifier) &&
      !identifier.empty()) {
    const size_t count = ihs_semantics_snapshot_node_count(snapshot);
    for (size_t i = 0; i < count; i++) {
      const IhsSemanticsNode* node =
          ihs_semantics_snapshot_node_at(snapshot, i);
      if (node != nullptr && identifier == node->identifier) {
        return node;
      }
    }
  }
  return nullptr;
}

struct Provider {
  std::mutex mutex;
  IhsMcpProvider* mcp = nullptr;
  IhsSemanticsConsumer* consumer = nullptr;
  std::vector<IhsMcpToolDesc> tools;
  std::vector<std::string> names;  // prefix-free names, owned
  IhsMcpResourceDesc resources[1]{};
};

Provider& TheProvider() {
  static auto* provider = new Provider();  // leaked; outlives static teardown
  return *provider;
}

int ListTools(void* /* user_data */,
              const IhsMcpToolDesc** out_tools,
              size_t* out_count) {
  Provider& p = TheProvider();
  *out_tools = p.tools.empty() ? nullptr : p.tools.data();
  *out_count = p.tools.size();
  return IHS_MCP_OK;
}

int ListResources(void* /* user_data */,
                  const IhsMcpResourceDesc** out_resources,
                  size_t* out_count) {
  Provider& p = TheProvider();
  *out_resources = p.resources;
  *out_count = 1;
  return IHS_MCP_OK;
}

int ReadResource(void* /* user_data */,
                 const char* uri,
                 IhsMcpPayload* out_content) {
  if (std::strcmp(uri, "ui://semantics/tree") != 0) {
    return IHS_MCP_ERR_NOT_FOUND;
  }
  const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
  if (snapshot == nullptr) {
    // Semantics has not produced a tree yet. An empty document says so
    // without pretending the UI has no nodes.
    FillPayload(out_content, R"({"generation":0,"nodes":[]})");
    return IHS_MCP_OK;
  }
  FillPayload(out_content, SerializeTree(snapshot));
  ihs_semantics_release_snapshot(snapshot);
  return IHS_MCP_OK;
}

std::string RunQuery(const IhsSemanticsSnapshot* snapshot,
                     const char* arguments) {
  std::string want_identifier;
  std::string want_label;
  std::string want_role;
  ExtractString(arguments, "identifier", &want_identifier);
  ExtractString(arguments, "label", &want_label);
  ExtractString(arguments, "role", &want_role);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
  w.StartObject();
  w.Key("generation");
  w.Uint64(ihs_semantics_snapshot_generation(snapshot));
  w.Key("nodes");
  w.StartArray();

  const size_t count = ihs_semantics_snapshot_node_count(snapshot);
  for (size_t i = 0; i < count; i++) {
    const IhsSemanticsNode* node = ihs_semantics_snapshot_node_at(snapshot, i);
    if (node == nullptr) {
      continue;
    }
    // Every supplied selector must match. Narrowing rather than widening is
    // what makes adding a second selector useful.
    if (!want_identifier.empty() && want_identifier != node->identifier) {
      continue;
    }
    if (!want_label.empty() &&
        std::strstr(node->label, want_label.c_str()) == nullptr) {
      continue;
    }
    if (!want_role.empty() && want_role != RoleName(node->role)) {
      continue;
    }
    WriteNode(w, node, snapshot);
  }
  w.EndArray();
  w.EndObject();
  return buffer.GetString();
}

int CallTool(void* /* user_data */,
             const char* name,
             const char* arguments_json,
             size_t /* arguments_length */,
             IhsMcpPayload* out_result) {
  const ToolEntry* tool = nullptr;
  for (const ToolEntry& candidate : kTools) {
    if (std::strcmp(candidate.name, name) == 0) {
      tool = &candidate;
      break;
    }
  }
  if (tool == nullptr) {
    return IHS_MCP_ERR_NOT_FOUND;
  }

  const IhsSemanticsSnapshot* snapshot = ihs_semantics_acquire_snapshot();
  if (snapshot == nullptr) {
    FillPayload(out_result, ErrorJson("no semantics tree has been published "
                                      "yet",
                                      0));
    return IHS_MCP_ERR_REFUSED;
  }
  const uint64_t generation = ihs_semantics_snapshot_generation(snapshot);

  if (std::strcmp(name, "snapshot") == 0) {
    FillPayload(out_result, SerializeTree(snapshot));
    ihs_semantics_release_snapshot(snapshot);
    return IHS_MCP_OK;
  }
  if (std::strcmp(name, "query") == 0) {
    FillPayload(out_result, RunQuery(snapshot, arguments_json));
    ihs_semantics_release_snapshot(snapshot);
    return IHS_MCP_OK;
  }

  const IhsSemanticsNode* node = ResolveNode(snapshot, arguments_json);
  if (node == nullptr) {
    FillPayload(out_result,
                ErrorJson("no node matched node_id or identifier", generation));
    ihs_semantics_release_snapshot(snapshot);
    return IHS_MCP_ERR_NOT_FOUND;
  }

  // A disabled control is refused here rather than dispatched. The framework
  // drops such an action silently, so a client would otherwise see success and
  // no effect, which is worse than being told.
  if (node->enabled == IHS_SEMANTICS_TRISTATE_FALSE) {
    FillPayload(out_result, ErrorJson("node is disabled", generation));
    ihs_semantics_release_snapshot(snapshot);
    return IHS_MCP_ERR_REFUSED;
  }
  if ((node->actions & tool->action) == 0) {
    FillPayload(out_result,
                ErrorJson("node does not offer this action", generation));
    ihs_semantics_release_snapshot(snapshot);
    return IHS_MCP_ERR_REFUSED;
  }

  const int32_t node_id = node->id;
  ihs_semantics_release_snapshot(snapshot);

  Provider& p = TheProvider();
  const int status = ihs_semantics_dispatch(
      p.consumer, 0, node_id, tool->action, nullptr, 0, nullptr, nullptr);
  if (status != IHS_SEMANTICS_OK) {
    FillPayload(out_result, ErrorJson("dispatch refused", generation));
    return IHS_MCP_ERR_REFUSED;
  }

  // Report the generation the decision was made against. A client that wants
  // to know what changed re-reads the tree and compares; the hub cannot say
  // here whether the widget did anything, and pretending otherwise would be
  // worse than saying nothing.
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
  w.StartObject();
  w.Key("dispatched");
  w.Bool(true);
  w.Key("node_id");
  w.Int(node_id);
  w.Key("generation_before");
  w.Uint64(generation);
  w.EndObject();
  FillPayload(out_result, buffer.GetString());
  return IHS_MCP_OK;
}

}  // namespace

extern "C" {

int ihs_mcp_semantics_provider_start(void) {
  Provider& p = TheProvider();
  std::lock_guard<std::mutex> lock(p.mutex);
  if (p.mcp != nullptr) {
    return IHS_MCP_OK;  // idempotent
  }

  p.names.clear();
  p.tools.clear();
  p.names.reserve(sizeof(kTools) / sizeof(kTools[0]));
  p.tools.reserve(sizeof(kTools) / sizeof(kTools[0]));
  for (const ToolEntry& entry : kTools) {
    p.names.emplace_back(entry.name);
  }
  for (size_t i = 0; i < p.names.size(); i++) {
    IhsMcpToolDesc desc{};
    desc.name = p.names[i].c_str();
    desc.description = kTools[i].description;
    desc.input_schema_json = kTools[i].schema;
    desc.capability = kTools[i].capability;
    p.tools.push_back(desc);
  }

  p.resources[0].uri = "ui://semantics/tree";
  p.resources[0].name = "semantics tree";
  p.resources[0].description = "The current UI as a semantics tree.";
  p.resources[0].mime_type = "application/json";

  // Register with the hub first: an MCP client can call the moment the
  // provider is visible, and answering with "no tree" because we had not
  // subscribed yet would be a self-inflicted race.
  IhsSemanticsConsumerDesc consumer{};
  consumer.struct_size = sizeof(IhsSemanticsConsumerDesc);
  consumer.name = "mcp.semantics";
  consumer.action_allow_mask = kAllowMask;
  consumer.notify_fd = -1;  // pulls on demand; no push needed
  if (ihs_semantics_register(&consumer, &p.consumer) != IHS_SEMANTICS_OK) {
    return IHS_MCP_ERR_UNAVAILABLE;
  }

  IhsMcpProviderDesc desc{};
  desc.struct_size = sizeof(IhsMcpProviderDesc);
  desc.name = "semantics";
  desc.tool_prefix = "ui_";
  desc.resource_scheme = "ui";
  desc.capability_mask = IHS_MCP_CAP_INSPECT | IHS_MCP_CAP_INTERACT;
  desc.callbacks.list_tools = ListTools;
  desc.callbacks.call_tool = CallTool;
  desc.callbacks.list_resources = ListResources;
  desc.callbacks.read_resource = ReadResource;
  desc.notify_fd = -1;

  const int status = ihs_mcp_provider_register(&desc, &p.mcp);
  if (status != IHS_MCP_OK) {
    ihs_semantics_unregister(p.consumer);
    p.consumer = nullptr;
    return status;
  }
  return IHS_MCP_OK;
}

void ihs_mcp_semantics_provider_stop(void) {
  Provider& p = TheProvider();
  std::lock_guard<std::mutex> lock(p.mutex);
  if (p.mcp != nullptr) {
    ihs_mcp_provider_unregister(p.mcp);
    p.mcp = nullptr;
  }
  if (p.consumer != nullptr) {
    ihs_semantics_unregister(p.consumer);
    p.consumer = nullptr;
  }
}

bool ihs_mcp_semantics_provider_running(void) {
  Provider& p = TheProvider();
  std::lock_guard<std::mutex> lock(p.mutex);
  return p.mcp != nullptr;
}

}  // extern "C"
