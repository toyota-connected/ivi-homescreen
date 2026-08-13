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

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
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

constexpr char kSetTextSchema[] =
    R"({"type":"object","properties":{)"
    R"("node_id":{"type":"integer"},)"
    R"("identifier":{"type":"string"},)"
    R"("text":{"type":"string","description":"Replacement text for the field."})"
    R"(},"required":["text"]})";

constexpr char kScrollToSchema[] =
    R"({"type":"object","properties":{)"
    R"("node_id":{"type":"integer"},)"
    R"("identifier":{"type":"string"},)"
    R"("dx":{"type":"number","description":"Horizontal offset in logical pixels."},)"
    R"("dy":{"type":"number","description":"Vertical offset in logical pixels."})"
    R"(}})";

constexpr char kTapAtSchema[] =
    R"({"type":"object","properties":{)"
    R"("x":{"type":"number","description":"Screen x, in the same coordinates as a node rect."},)"
    R"("y":{"type":"number","description":"Screen y, in the same coordinates as a node rect."})"
    R"(},"required":["x","y"]})";

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
    {"set_text", "Replace the text in a text field.", kSetTextSchema,
     IHS_SEMANTICS_ACTION_SET_TEXT, IHS_MCP_CAP_INTERACT},
    {"scroll_to", "Scroll a container to a given offset.", kScrollToSchema,
     IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET, IHS_MCP_CAP_INTERACT},
    // Action 0: this one does not dispatch a semantics action at all, and the
    // call handler routes it separately. Its description says so, because the
    // difference is the caller's to reason about -- a tap that is hit-tested
    // reaches what the tree cannot describe, and misses what is covered.
    {"tap_at",
     "Tap at a screen coordinate, for targets the semantics tree does not "
     "describe. Unlike ui_tap this is hit-tested: whatever is drawn on top "
     "receives it, it races animation, and nothing confirms what it hit. "
     "Prefer ui_tap wherever a node offers it.",
     kTapAtSchema, 0, IHS_MCP_CAP_INTERACT},
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

// The extractors above scan for a key and take the next quoted run, which
// cannot survive an escaped quote or a key-like substring inside a value.
// That is tolerable for an identifier, which the application chooses, and not
// for arbitrary text a client sends, so the parameterized arguments are parsed
// properly instead.
bool ReadStringArgument(const char* json, const char* key, std::string* out) {
  rapidjson::Document doc;
  if (json == nullptr || doc.Parse(json).HasParseError() || !doc.IsObject()) {
    return false;
  }
  const auto member = doc.FindMember(key);
  if (member == doc.MemberEnd() || !member->value.IsString()) {
    return false;
  }
  out->assign(member->value.GetString(), member->value.GetStringLength());
  return true;
}

// Refuses rather than defaulting. A coordinate is not something to guess at:
// tapping (0, 0) because the caller omitted y would press whatever is in the
// corner, which is worse than an error.
bool ReadNumberArgumentRequired(const char* json,
                                const char* key,
                                double* out) {
  rapidjson::Document doc;
  if (json == nullptr || doc.Parse(json).HasParseError() || !doc.IsObject()) {
    return false;
  }
  const auto member = doc.FindMember(key);
  if (member == doc.MemberEnd() || !member->value.IsNumber()) {
    return false;
  }
  *out = member->value.GetDouble();
  return true;
}

// Absent or non-numeric yields the fallback: an axis a caller did not name is
// zero rather than an error, so scrolling a vertical list needs only dy.
double ReadNumberArgument(const char* json,
                          const char* key,
                          const double fallback) {
  rapidjson::Document doc;
  if (json == nullptr || doc.Parse(json).HasParseError() || !doc.IsObject()) {
    return fallback;
  }
  const auto member = doc.FindMember(key);
  if (member == doc.MemberEnd() || !member->value.IsNumber()) {
    return fallback;
  }
  return member->value.GetDouble();
}

// Waits for a tree newer than `generation`, or gives up. Used where an action
// only becomes available after the framework has rebuilt -- focusing a field
// being the case that matters. Polled rather than waited on a condition: the
// hub signals consumers by fd and this provider shares that descriptor with
// the MCP registry, so consuming it here would swallow a notification the
// server is waiting for.
constexpr int kFocusSettleMs = 400;

const IhsSemanticsSnapshot* WaitForNewerSnapshot(const uint64_t generation,
                                                 const int timeout_ms) {
  for (int waited = 0; waited <= timeout_ms; waited += 10) {
    const IhsSemanticsSnapshot* candidate = ihs_semantics_acquire_snapshot();
    if (candidate != nullptr &&
        ihs_semantics_snapshot_generation(candidate) > generation) {
      return candidate;
    }
    if (candidate != nullptr) {
      ihs_semantics_release_snapshot(candidate);
    }
    struct timespec ts = {0, 10 * 1000 * 1000};
    nanosleep(&ts, nullptr);
  }
  return nullptr;
}

// How long an action waits for the tree to settle before reporting what it
// sees (DR-8).
//
// A bound on waiting, not an expectation: an action that changes nothing pays
// this in full, which is the cost of answering "did anything happen" in the
// same round trip as the act. Longer than a frame because the framework has
// to run the handler, rebuild, lay out and republish before the change is
// visible here, and a bound tight enough to miss that would report "no
// change" for actions that did change something -- the one wrong answer that
// looks like a real result.
constexpr int kVerifySettleMs = 150;

// Appends the verify-after-act fields to a tool result (DR-8, plan 4.1).
//
// generation_after equal to generation_before is a real answer rather than a
// failure: the action was accepted and the tree did not change, which is what
// firing a network call or toggling something the semantics do not describe
// looks like. Reporting it as an error would make a client retry an action
// that already happened.
void WriteOutcome(rapidjson::Writer<rapidjson::StringBuffer>& w,
                  const char* mode,
                  const uint64_t generation_before,
                  const int32_t node_id,
                  const bool report_node) {
  const IhsSemanticsSnapshot* after =
      WaitForNewerSnapshot(generation_before, kVerifySettleMs);
  if (after == nullptr) {
    // Nothing newer arrived. Read the current tree anyway rather than assuming
    // it still matches: the wait can only end early on a newer generation, so
    // this is the same tree, and looking is cheaper than reasoning about it.
    after = ihs_semantics_acquire_snapshot();
  }

  w.Key("mode");
  w.String(mode);
  w.Key("generation_before");
  w.Uint64(generation_before);
  w.Key("generation_after");
  w.Uint64(after != nullptr ? ihs_semantics_snapshot_generation(after)
                            : generation_before);

  if (report_node) {
    const IhsSemanticsNode* node =
        after != nullptr ? ihs_semantics_snapshot_node_by_id(after, node_id)
                         : nullptr;
    w.Key("node_after");
    if (node == nullptr) {
      // The node left the tree. A route change does exactly this, and it is
      // the normal outcome of tapping something that navigates -- null says
      // "gone" without dressing a successful action as a failure.
      w.Null();
    } else {
      WriteNode(w, node, after);
    }
  }

  if (after != nullptr) {
    ihs_semantics_release_snapshot(after);
  }
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
  // Signalled by the hub on publish and watched by the MCP registry; see
  // where it is created for why one descriptor serves both.
  int notify_fd = -1;
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

  // Routed before the node lookup: this tool names no node, which is the
  // whole point of it -- the target is a coordinate precisely because the
  // tree does not describe what is there.
  if (std::strcmp(name, "tap_at") == 0) {
    const uint64_t generation_now = generation;
    ihs_semantics_release_snapshot(snapshot);

    double x = 0.0;
    double y = 0.0;
    if (!ReadNumberArgumentRequired(arguments_json, "x", &x) ||
        !ReadNumberArgumentRequired(arguments_json, "y", &y)) {
      FillPayload(out_result,
                  ErrorJson("tap_at requires x and y", generation_now));
      return IHS_MCP_ERR_INVALID;
    }

    Provider& provider = TheProvider();
    const int tap_status =
        ihs_semantics_send_pointer_tap(provider.consumer, 0, x, y);
    if (tap_status != IHS_SEMANTICS_OK) {
      FillPayload(out_result, ErrorJson("pointer tap refused", generation_now));
      return IHS_MCP_ERR_REFUSED;
    }

    // Still claims no target: nothing here knows what was under the point, and
    // naming a node would invent the confirmation the caller chose this tool
    // to go without. The generations are reported regardless -- "the tree
    // changed" is exactly the evidence a coordinate tap otherwise lacks, and
    // it is the only thing this tool can honestly say about its effect.
    rapidjson::StringBuffer tap_buffer;
    rapidjson::Writer<rapidjson::StringBuffer> tap_writer(tap_buffer);
    tap_writer.StartObject();
    tap_writer.Key("dispatched");
    tap_writer.Bool(true);
    tap_writer.Key("hit_tested");
    tap_writer.Bool(true);
    tap_writer.Key("x");
    tap_writer.Double(x);
    tap_writer.Key("y");
    tap_writer.Double(y);
    WriteOutcome(tap_writer, "pointer", generation_now, 0,
                 /*report_node=*/false);
    tap_writer.EndObject();
    FillPayload(out_result, tap_buffer.GetString());
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
  // A text field only offers SetText once it holds input focus: until then
  // the framework has no editing connection to route the text into, so the
  // action is genuinely absent rather than merely unadvertised. Focus it
  // first and look again -- one tool from the caller's point of view, which
  // is the sequencing the design called for.
  //
  // Input focus, not accessibility focus: the latter moves a screen reader's
  // cursor and opens nothing.
  if (tool->action == IHS_SEMANTICS_ACTION_SET_TEXT &&
      (node->actions & IHS_SEMANTICS_ACTION_SET_TEXT) == 0 &&
      (node->actions & IHS_SEMANTICS_ACTION_FOCUS) != 0) {
    const int32_t focus_id = node->id;
    ihs_semantics_release_snapshot(snapshot);

    Provider& focus_provider = TheProvider();
    if (ihs_semantics_dispatch(focus_provider.consumer, 0, focus_id,
                               IHS_SEMANTICS_ACTION_FOCUS, nullptr, 0, nullptr,
                               nullptr) != IHS_SEMANTICS_OK) {
      FillPayload(out_result,
                  ErrorJson("could not focus the field before setting text",
                            generation));
      return IHS_MCP_ERR_REFUSED;
    }

    // Focusing takes effect on a frame, so the tree that advertises SetText is
    // the next one. Bounded so a field that never gains focus fails as a
    // refusal rather than hanging the caller.
    snapshot = WaitForNewerSnapshot(generation, kFocusSettleMs);
    if (snapshot == nullptr) {
      FillPayload(out_result,
                  ErrorJson("field did not report focus in time", generation));
      return IHS_MCP_ERR_REFUSED;
    }
    node = ihs_semantics_snapshot_node_by_id(snapshot, focus_id);
    if (node == nullptr) {
      FillPayload(out_result,
                  ErrorJson("node vanished while focusing", generation));
      ihs_semantics_release_snapshot(snapshot);
      return IHS_MCP_ERR_NOT_FOUND;
    }
  }

  if ((node->actions & tool->action) == 0) {
    FillPayload(out_result,
                ErrorJson("node does not offer this action", generation));
    ihs_semantics_release_snapshot(snapshot);
    return IHS_MCP_ERR_REFUSED;
  }

  const int32_t node_id = node->id;
  const bool obscured = node->obscured;
  ihs_semantics_release_snapshot(snapshot);

  // Arguments travel in the hub's plain layout; the shell encodes them for the
  // framework (see ihs_semantics_dispatch). Building them here rather than in
  // the caller keeps the byte layout in one place.
  std::vector<uint8_t> argument;
  if (tool->action == IHS_SEMANTICS_ACTION_SET_TEXT) {
    // Refusing to type into a password field is a deliberate limit, not an
    // oversight: an agent that can set text into an obscured field can also
    // read back what it wrote through the app's own behaviour.
    if (obscured) {
      FillPayload(
          out_result,
          ErrorJson("refusing to set text on an obscured field", generation));
      return IHS_MCP_ERR_REFUSED;
    }
    std::string text;
    if (!ReadStringArgument(arguments_json, "text", &text)) {
      FillPayload(out_result, ErrorJson("set_text requires text", generation));
      return IHS_MCP_ERR_INVALID;
    }
    argument.assign(text.begin(), text.end());
  } else if (tool->action == IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET) {
    // Absent axes are zero rather than an error: scrolling a vertical list
    // means naming dy, and demanding dx as well would be noise.
    const double offset[2] = {ReadNumberArgument(arguments_json, "dx", 0.0),
                              ReadNumberArgument(arguments_json, "dy", 0.0)};
    const auto* bytes = reinterpret_cast<const uint8_t*>(offset);
    argument.assign(bytes, bytes + sizeof(offset));
  }

  Provider& p = TheProvider();
  const int status =
      ihs_semantics_dispatch(p.consumer, 0, node_id, tool->action,
                             argument.empty() ? nullptr : argument.data(),
                             argument.size(), nullptr, nullptr);
  if (status != IHS_SEMANTICS_OK) {
    FillPayload(out_result, ErrorJson("dispatch refused", generation));
    return IHS_MCP_ERR_REFUSED;
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
  w.StartObject();
  w.Key("dispatched");
  w.Bool(true);
  w.Key("node_id");
  w.Int(node_id);
  WriteOutcome(w, "semantics", generation, node_id, /*report_node=*/true);
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
  // One eventfd serves both registrations. The hub signals it when a new tree
  // is published, and the MCP registry is watching that same descriptor for
  // "this provider changed" -- so a publish becomes a resources/updated with
  // nothing in between to forward it. Non-blocking because the registry
  // drains it until empty.
  p.notify_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (p.notify_fd < 0) {
    return IHS_MCP_ERR_UNAVAILABLE;
  }

  IhsSemanticsConsumerDesc consumer{};
  consumer.struct_size = sizeof(IhsSemanticsConsumerDesc);
  consumer.name = "mcp.semantics";
  consumer.action_allow_mask = kAllowMask;
  consumer.notify_fd = p.notify_fd;
  if (ihs_semantics_register(&consumer, &p.consumer) != IHS_SEMANTICS_OK) {
    ::close(p.notify_fd);
    p.notify_fd = -1;
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
  desc.notify_fd = p.notify_fd;

  const int status = ihs_mcp_provider_register(&desc, &p.mcp);
  if (status != IHS_MCP_OK) {
    ihs_semantics_unregister(p.consumer);
    p.consumer = nullptr;
    ::close(p.notify_fd);
    p.notify_fd = -1;
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
  // Closed only once both registrations are gone: each of them promises no
  // callback is in flight when it returns, which is what makes the descriptor
  // safe to release rather than merely unreferenced.
  if (p.notify_fd >= 0) {
    ::close(p.notify_fd);
    p.notify_fd = -1;
  }
}

bool ihs_mcp_semantics_provider_running(void) {
  Provider& p = TheProvider();
  std::lock_guard<std::mutex> lock(p.mutex);
  return p.mcp != nullptr;
}

}  // extern "C"
