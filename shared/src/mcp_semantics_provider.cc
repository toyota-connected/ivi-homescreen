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
#include "ihs/logging.h"

namespace {

// One tool as this provider declares it, paired with the hub action it
// dispatches. A zero action marks a tool that is answered from the snapshot
// rather than sent to the framework.
struct ToolEntry {
  const char* name;
  const char* description;
  const char* schema;

  // The action the generic dispatch path sends. Zero for the tools answered
  // from the snapshot, and for the two the call handler routes itself.
  uint64_t action;

  // Every hub action this tool may cause, which is what the allow mask is
  // built from and is not always `action`. Two tools dispatch something that
  // field does not name: custom_action routes itself and sends CUSTOM_ACTION,
  // and set_text focuses the field first, because the framework ignores text
  // sent to a field with no input connection. Deriving the mask from `action`
  // alone drops both, and the result is a tool that is advertised and then
  // refused at the funnel -- which reads to a client as the surface being
  // broken rather than as policy.
  uint64_t dispatches;

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
    R"("role":{"type":"string","description":"Role name, e.g. button or slider."},)"
    R"("action":{"type":"string","description":"Only nodes offering this action, named as the tree reports it."},)"
    R"("custom_action":{"type":"string","description":"Only nodes declaring a custom action with this label."},)"
    R"("enabled":{"type":"string","enum":["true","false","not_applicable"],"description":"Enabled tristate, spelled as the tree reports it; not_applicable means enablement is meaningless for the node."},)"
    R"("limit":{"type":"integer","description":"Cap on returned nodes. match_count and truncated always report the full total."})"
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

constexpr char kCustomActionSchema[] =
    R"({"type":"object","properties":{)"
    R"("node_id":{"type":"integer"},)"
    R"("identifier":{"type":"string"},)"
    R"("action":{"type":"string","description":"Custom action label, exactly as the tree reports it."},)"
    R"("action_id":{"type":"integer","description":"Custom action id, as an alternative to the label."})"
    R"(}})";

constexpr char kTapAtSchema[] =
    R"({"type":"object","properties":{)"
    R"("x":{"type":"number","description":"Screen x, in the same coordinates as a node rect."},)"
    R"("y":{"type":"number","description":"Screen y, in the same coordinates as a node rect."})"
    R"(},"required":["x","y"]})";

const ToolEntry kTools[] = {
    {"snapshot", "The whole semantics tree as JSON.", kEmptySchema, 0, 0,
     IHS_MCP_CAP_INSPECT},
    {"query", "Find nodes by identifier, label substring, or role.",
     kQuerySchema, 0, 0, IHS_MCP_CAP_INSPECT},
    {"tap", "Activate a node, as a tap would.", kNodeSchema,
     IHS_SEMANTICS_ACTION_TAP, IHS_SEMANTICS_ACTION_TAP, IHS_MCP_CAP_INTERACT},
    {"long_press", "Long-press a node.", kNodeSchema,
     IHS_SEMANTICS_ACTION_LONG_PRESS, IHS_SEMANTICS_ACTION_LONG_PRESS,
     IHS_MCP_CAP_INTERACT},
    {"increase", "Increment a value, as on a slider.", kNodeSchema,
     IHS_SEMANTICS_ACTION_INCREASE, IHS_SEMANTICS_ACTION_INCREASE,
     IHS_MCP_CAP_INTERACT},
    {"decrease", "Decrement a value.", kNodeSchema,
     IHS_SEMANTICS_ACTION_DECREASE, IHS_SEMANTICS_ACTION_DECREASE,
     IHS_MCP_CAP_INTERACT},
    {"show_on_screen", "Scroll a node into view.", kNodeSchema,
     IHS_SEMANTICS_ACTION_SHOW_ON_SCREEN, IHS_SEMANTICS_ACTION_SHOW_ON_SCREEN,
     IHS_MCP_CAP_INTERACT},
    {"dismiss", "Dismiss a node, as a swipe-away would.", kNodeSchema,
     IHS_SEMANTICS_ACTION_DISMISS, IHS_SEMANTICS_ACTION_DISMISS,
     IHS_MCP_CAP_INTERACT},
    {"expand", "Expand a node.", kNodeSchema, IHS_SEMANTICS_ACTION_EXPAND,
     IHS_SEMANTICS_ACTION_EXPAND, IHS_MCP_CAP_INTERACT},
    {"collapse", "Collapse a node.", kNodeSchema, IHS_SEMANTICS_ACTION_COLLAPSE,
     IHS_SEMANTICS_ACTION_COLLAPSE, IHS_MCP_CAP_INTERACT},
    // Focus as well as the text itself: the framework ignores text sent to a
    // field with no input connection, so the tool opens one first.
    {"set_text", "Replace the text in a text field.", kSetTextSchema,
     IHS_SEMANTICS_ACTION_SET_TEXT,
     IHS_SEMANTICS_ACTION_SET_TEXT | IHS_SEMANTICS_ACTION_FOCUS,
     IHS_MCP_CAP_INTERACT},
    {"scroll_to", "Scroll a container to a given offset.", kScrollToSchema,
     IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET,
     IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET, IHS_MCP_CAP_INTERACT},
    {"custom_action",
     "Invoke one of a node's application-defined actions, named by the label "
     "the tree reports in custom_actions. These are the app's own verbs -- "
     "there is no fixed set, and a node offers only what it declared.",
     kCustomActionSchema, 0, IHS_SEMANTICS_ACTION_CUSTOM_ACTION,
     IHS_MCP_CAP_INTERACT},
    // Action 0: this one does not dispatch a semantics action at all, and the
    // call handler routes it separately. Its description says so, because the
    // difference is the caller's to reason about -- a tap that is hit-tested
    // reaches what the tree cannot describe, and misses what is covered.
    {"tap_at",
     "Tap at a screen coordinate, for targets the semantics tree does not "
     "describe. Unlike ui_tap this is hit-tested: whatever is drawn on top "
     "receives it, it races animation, and nothing confirms what it hit. "
     "Prefer ui_tap wherever a node offers it.",
     kTapAtSchema, 0, 0, IHS_MCP_CAP_INTERACT},
};

// The accessibility-focus actions are deliberately absent from the table
// above. They move a screen reader's cursor, and an agent driving the UI must
// not be able to yank it away from someone reading the screen. The hub's
// allow mask below enforces the same thing a second time, at the funnel.
constexpr uint64_t kAllowMask = IHS_SEMANTICS_ACTION_NO_A11Y_FOCUS;

constexpr size_t kToolCount = sizeof(kTools) / sizeof(kTools[0]);

// Logging goes through the C API for the same reason the rest of ihs_shared
// does: the library sits below the shell's C++ logging wrapper.
int32_t TraceContext() {
  static const int32_t ctx = ihs_log_context_open("MCPS", nullptr);
  return ctx;
}

void Log(const int32_t level, const std::string& text) {
  const int32_t ctx = TraceContext();
  if (ctx < 0 || ihs_log_enabled(ctx, static_cast<uint8_t>(level)) == 0) {
    return;
  }
  ihs_log(ctx, static_cast<uint8_t>(level), text.c_str(), text.size());
}

// Reading is what the tree is for, so the read-only tools are offered whatever
// the policy says. A surface that cannot be read has no purpose: an agent
// would see a tool list it cannot use and no way to find out why.
bool AlwaysOffered(const char* name) {
  return std::strcmp(name, "snapshot") == 0 || std::strcmp(name, "query") == 0;
}

// Index of a tool by its unprefixed name, or kToolCount when there is none.
size_t ToolIndexFor(const char* name) {
  for (size_t i = 0; i < kToolCount; i++) {
    if (std::strcmp(kTools[i].name, name) == 0) {
      return i;
    }
  }
  return kToolCount;
}

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
  // Named separately because its tool carries no action bit: ui_custom_action
  // is routed by name and resolves an id before dispatching, so the loop above
  // cannot reach it. Without this a node able to run the app's own verbs looks
  // like a node offering nothing.
  if ((actions & IHS_SEMANTICS_ACTION_CUSTOM_ACTION) != 0) {
    w.String("custom_action");
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

// One tool call's arguments, parsed once.
//
// Two properties matter here and neither is incidental.
//
// It is bounded by the length the ABI supplies rather than by a terminator.
// The callback signature is (const char*, size_t) and nothing in it promises
// a NUL, so reaching for one reads past the buffer whenever a caller passes a
// slice of a larger one. The transport happens to pass a NUL-terminated
// std::string today, which is the only reason that was not already a fault --
// an invariant held by the caller's accident rather than by the contract.
//
// It is a parse rather than a scan. The extractors this replaces searched the
// raw text for "key", which meant a key-like substring inside a value was
// indistinguishable from the argument itself: a client sending a label of
// `x", "node_id": 2, "z": "` had a node_id the request never contained. It
// also could not survive an escaped quote in any value a client chose, which
// is every query selector.
class Arguments {
 public:
  Arguments(const char* json, const size_t length) {
    // An absent argument set is "{}" by the provider ABI, so a null here is a
    // caller error rather than an empty call; either way there is nothing to
    // read and every accessor reports absence.
    if (json != nullptr) {
      document_.Parse(json, length);
    }
  }

  bool Int(const char* key, int32_t* out) const {
    const rapidjson::Value* value = Find(key);
    // IsInt is the range check: a number too large for int32 is not an int
    // here, so it is refused rather than truncated into some other node's id.
    if (value == nullptr || !value->IsInt()) {
      return false;
    }
    *out = value->GetInt();
    return true;
  }

  bool String(const char* key, std::string* out) const {
    const rapidjson::Value* value = Find(key);
    if (value == nullptr || !value->IsString()) {
      return false;
    }
    out->assign(value->GetString(), value->GetStringLength());
    return true;
  }

  // Refuses rather than defaulting. A coordinate is not something to guess at:
  // tapping (0, 0) because the caller omitted y would press whatever is in the
  // corner, which is worse than an error.
  bool Number(const char* key, double* out) const {
    const rapidjson::Value* value = Find(key);
    if (value == nullptr || !value->IsNumber()) {
      return false;
    }
    *out = value->GetDouble();
    return true;
  }

  // Absent or non-numeric yields the fallback: an axis a caller did not name
  // is zero rather than an error, so scrolling a vertical list needs only dy.
  double NumberOr(const char* key, const double fallback) const {
    double value = fallback;
    return Number(key, &value) ? value : fallback;
  }

 private:
  const rapidjson::Value* Find(const char* key) const {
    if (document_.HasParseError() || !document_.IsObject()) {
      return nullptr;
    }
    const auto member = document_.FindMember(key);
    return member == document_.MemberEnd() ? nullptr : &member->value;
  }

  rapidjson::Document document_;
};

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
                                    const Arguments& arguments) {
  int32_t id = 0;
  if (arguments.Int("node_id", &id)) {
    return ihs_semantics_snapshot_node_by_id(snapshot, id);
  }
  std::string identifier;
  if (arguments.String("identifier", &identifier) && !identifier.empty()) {
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

// Resolves an action name to its bit, using the same table WriteActions
// reports from -- so the names a client can select on are exactly the names it
// was shown, and there is no second list to fall out of step.
uint64_t ActionBitFor(const std::string& name) {
  for (const ToolEntry& tool : kTools) {
    if (tool.action != 0 && name == tool.name) {
      return tool.action;
    }
  }
  return 0;
}

bool DeclaresCustomAction(const IhsSemanticsSnapshot* snapshot,
                          const IhsSemanticsNode* node,
                          const std::string& label) {
  for (size_t i = 0; i < node->custom_action_count; i++) {
    const IhsSemanticsCustomAction* declared =
        ihs_semantics_find_custom_action(snapshot, node->custom_action_ids[i]);
    if (declared != nullptr && label == declared->label) {
      return true;
    }
  }
  return false;
}

std::string RunQuery(const IhsSemanticsSnapshot* snapshot,
                     const Arguments& arguments) {
  std::string want_identifier;
  std::string want_label;
  std::string want_role;
  std::string want_action;
  std::string want_custom_action;
  std::string want_enabled;
  arguments.String("identifier", &want_identifier);
  arguments.String("label", &want_label);
  arguments.String("role", &want_role);
  arguments.String("action", &want_action);
  arguments.String("custom_action", &want_custom_action);
  arguments.String("enabled", &want_enabled);

  int32_t limit = 0;
  const bool bounded = arguments.Int("limit", &limit) && limit > 0;

  // An action name that names nothing would otherwise select every node, since
  // a zero mask tests true against anything. Reported rather than silently
  // widened: the caller misspelled a name it was given.
  const uint64_t want_action_bit =
      want_action.empty() ? 0 : ActionBitFor(want_action);
  if (!want_action.empty() && want_action_bit == 0) {
    return std::string(R"({"error":"no such action: )") + want_action +
           R"(","generation":)" +
           std::to_string(ihs_semantics_snapshot_generation(snapshot)) + "}";
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
  w.StartObject();
  w.Key("generation");
  w.Uint64(ihs_semantics_snapshot_generation(snapshot));
  w.Key("nodes");
  w.StartArray();

  size_t matched = 0;
  size_t emitted = 0;
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
    if (want_action_bit != 0 && (node->actions & want_action_bit) == 0) {
      continue;
    }
    if (!want_custom_action.empty() &&
        !DeclaresCustomAction(snapshot, node, want_custom_action)) {
      continue;
    }
    // Selected by tristate name rather than a bool, because the hub keeps
    // "disabled" and "enablement does not apply" apart and a bool here would
    // put them back together at the last step.
    if (!want_enabled.empty() && want_enabled != TristateName(node->enabled)) {
      continue;
    }

    matched++;
    if (bounded && emitted >= static_cast<size_t>(limit)) {
      continue;  // counted, not emitted: the count is what makes the cap
                 // visible
    }
    WriteNode(w, node, snapshot);
    emitted++;
  }
  w.EndArray();

  // A truncated result that looked complete would have an agent conclude the
  // rest does not exist, so the cap reports itself.
  w.Key("match_count");
  w.Uint64(matched);
  w.Key("truncated");
  w.Bool(matched > emitted);
  w.EndObject();
  return buffer.GetString();
}

int CallTool(void* /* user_data */,
             const char* name,
             const char* arguments_json,
             const size_t arguments_length,
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

  // Parsed once, here, and read from by every path below. Bounded by the
  // length the caller supplied rather than by a terminator the ABI does not
  // promise.
  const Arguments arguments(arguments_json, arguments_length);

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
    FillPayload(out_result, RunQuery(snapshot, arguments));
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
    if (!arguments.Number("x", &x) || !arguments.Number("y", &y)) {
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

  const IhsSemanticsNode* node = ResolveNode(snapshot, arguments);
  if (node == nullptr) {
    FillPayload(out_result,
                ErrorJson("no node matched node_id or identifier", generation));
    ihs_semantics_release_snapshot(snapshot);
    return IHS_MCP_ERR_NOT_FOUND;
  }

  // Custom actions are the application's own verbs, so they are invoked by
  // name against a node rather than surfaced as tools of their own. A tool per
  // custom action would mean a tool list that changes with every route, and
  // names that collide the moment two nodes declare the same verb -- the tree
  // already reports each node's set, which is where an agent looks anyway.
  if (std::strcmp(name, "custom_action") == 0) {
    if ((node->actions & IHS_SEMANTICS_ACTION_CUSTOM_ACTION) == 0) {
      FillPayload(out_result,
                  ErrorJson("node offers no custom actions", generation));
      ihs_semantics_release_snapshot(snapshot);
      return IHS_MCP_ERR_REFUSED;
    }

    std::string wanted;
    int32_t action_id = 0;
    const bool by_id = arguments.Int("action_id", &action_id);
    if (!by_id && !arguments.String("action", &wanted)) {
      FillPayload(
          out_result,
          ErrorJson("custom_action requires action or action_id", generation));
      ihs_semantics_release_snapshot(snapshot);
      return IHS_MCP_ERR_INVALID;
    }

    // Resolved against this node's own declarations, not the snapshot's whole
    // table: a label another node declared is not this node's to run, and
    // dispatching it would be answered by whichever handler the framework
    // found rather than the one the caller named.
    bool found = false;
    for (size_t i = 0; i < node->custom_action_count && !found; i++) {
      const int32_t candidate = node->custom_action_ids[i];
      if (by_id) {
        found = candidate == action_id;
        continue;
      }
      const IhsSemanticsCustomAction* declared =
          ihs_semantics_find_custom_action(snapshot, candidate);
      if (declared != nullptr && wanted == declared->label) {
        action_id = candidate;
        found = true;
      }
    }
    if (!found) {
      FillPayload(out_result,
                  ErrorJson("node declares no such custom action", generation));
      ihs_semantics_release_snapshot(snapshot);
      return IHS_MCP_ERR_NOT_FOUND;
    }

    const int32_t target = node->id;
    ihs_semantics_release_snapshot(snapshot);

    std::vector<uint8_t> payload(sizeof(action_id));
    std::memcpy(payload.data(), &action_id, sizeof(action_id));

    Provider& provider = TheProvider();
    const int custom_status = ihs_semantics_dispatch(
        provider.consumer, 0, target, IHS_SEMANTICS_ACTION_CUSTOM_ACTION,
        payload.data(), payload.size(), nullptr, nullptr);
    if (custom_status != IHS_SEMANTICS_OK) {
      FillPayload(out_result, ErrorJson("dispatch refused", generation));
      return IHS_MCP_ERR_REFUSED;
    }

    rapidjson::StringBuffer custom_buffer;
    rapidjson::Writer<rapidjson::StringBuffer> custom_writer(custom_buffer);
    custom_writer.StartObject();
    custom_writer.Key("dispatched");
    custom_writer.Bool(true);
    custom_writer.Key("node_id");
    custom_writer.Int(target);
    custom_writer.Key("action_id");
    custom_writer.Int(action_id);
    WriteOutcome(custom_writer, "semantics", generation, target,
                 /*report_node=*/true);
    custom_writer.EndObject();
    FillPayload(out_result, custom_buffer.GetString());
    return IHS_MCP_OK;
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
    if (!arguments.String("text", &text)) {
      FillPayload(out_result, ErrorJson("set_text requires text", generation));
      return IHS_MCP_ERR_INVALID;
    }
    argument.assign(text.begin(), text.end());
  } else if (tool->action == IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET) {
    // Absent axes are zero rather than an error: scrolling a vertical list
    // means naming dy, and demanding dx as well would be noise.
    const double offset[2] = {arguments.NumberOr("dx", 0.0),
                              arguments.NumberOr("dy", 0.0)};
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
  return ihs_mcp_semantics_provider_start_with(nullptr);
}

int ihs_mcp_semantics_provider_start_with(const IhsMcpSemanticsConfig* config) {
  if (config != nullptr &&
      config->struct_size < sizeof(IhsMcpSemanticsConfig)) {
    return IHS_MCP_ERR_INVALID;
  }

  // Which tools this start offers. Resolved before anything is registered:
  // a policy that cannot be applied must not leave the surface running under
  // some other one.
  std::vector<bool> offered(kToolCount, true);
  if (config != nullptr && config->narrow_tools) {
    for (size_t i = 0; i < kToolCount; i++) {
      offered[i] = AlwaysOffered(kTools[i].name);
    }
    for (size_t i = 0; i < config->allowed_tool_count; i++) {
      const char* wanted =
          config->allowed_tools == nullptr ? nullptr : config->allowed_tools[i];
      if (wanted == nullptr) {
        return IHS_MCP_ERR_INVALID;
      }
      const size_t index = ToolIndexFor(wanted);
      if (index == kToolCount) {
        // Naming a tool that does not exist is refused rather than ignored.
        // A typo in a policy file must not quietly grant less than intended,
        // and least of all must it quietly grant more.
        Log(IHS_LEVEL_ERROR,
            std::string("mcp: no such tool in the allow list: ") + wanted);
        return IHS_MCP_ERR_INVALID;
      }
      offered[index] = true;
    }
  }

  Provider& p = TheProvider();
  std::lock_guard<std::mutex> lock(p.mutex);
  if (p.mcp != nullptr) {
    return IHS_MCP_OK;  // idempotent
  }

  p.names.clear();
  p.tools.clear();
  p.names.reserve(kToolCount);
  p.tools.reserve(kToolCount);
  // The hub mask carries only the tools that dispatch a semantics action, and
  // never more than the compiled default allows. ui_tap_at is deliberately
  // absent from it: it sends a pointer event rather than an action, so the
  // hub has nothing to enforce and the tool list is the only thing standing
  // in front of it. That the registry resolves a call against the list before
  // routing it is what makes the list a gate rather than a suggestion.
  uint64_t mask = 0;
  for (size_t i = 0; i < kToolCount; i++) {
    if (!offered[i]) {
      continue;
    }
    mask |= kTools[i].dispatches;
    p.names.emplace_back(kTools[i].name);
    IhsMcpToolDesc desc{};
    desc.description = kTools[i].description;
    desc.input_schema_json = kTools[i].schema;
    desc.capability = kTools[i].capability;
    p.tools.push_back(desc);
  }
  // Names are pointed at after the vector has stopped growing; a c_str() taken
  // during the loop above would dangle on the next reallocation.
  for (size_t i = 0; i < p.names.size(); i++) {
    p.tools[i].name = p.names[i].c_str();
  }
  mask &= kAllowMask;

  // What a policy actually permitted, once. Anything this grants should be
  // visible without running an experiment to find out -- a mask read from
  // configuration is exactly the kind of thing that is assumed rather than
  // checked, and a narrowed surface that silently did not narrow looks
  // identical to one that did until an agent uses it.
  if (p.names.size() < kToolCount) {
    std::string offered_names;
    for (const std::string& name : p.names) {
      offered_names += offered_names.empty() ? "" : ", ";
      offered_names += name;
    }
    Log(IHS_LEVEL_INFO, "mcp: semantics tools narrowed to " +
                            std::to_string(p.names.size()) + " of " +
                            std::to_string(kToolCount) + ": " + offered_names);
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
  consumer.action_allow_mask = mask;
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
