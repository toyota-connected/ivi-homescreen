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

/*
 * Fuzzes the ui_* tool arguments against a published semantics tree.
 *
 * This is the second half of the surface the transport target covers. The
 * transport decides that a call is well-formed JSON-RPC naming a tool that
 * exists; everything after that is here -- the argument object is decoded, a
 * node is resolved from it, its state is checked, and an action is dispatched
 * with an encoded payload. An agent's arguments reach all of it.
 *
 * The first input byte selects the tool and the rest is the argument JSON.
 * Letting the fuzzer discover tool names from a corpus would spend nearly
 * every execution on "no such tool": the names are a closed set the caller
 * already knows, so choosing from it directly puts the mutation budget where
 * the decoders are.
 *
 * The host is a mock. A real engine is not needed to fuzz argument decoding,
 * and dispatch republishes the tree so the verify-after-act settle returns
 * promptly -- otherwise every action that changed nothing would wait out the
 * full bound and the target would run at single-digit executions per second.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ihs/ihs_mcp_provider.h"
#include "ihs/ihs_mcp_registry.h"
#include "ihs/ihs_mcp_semantics.h"
#include "ihs/ihs_semantics.h"
#include "ihs/ihs_semantics_host.h"

namespace {

// Unprefixed, matching the provider's own table. The prefix is added when the
// call is made, so a rename of one without the other shows up as every
// execution returning "not found" rather than as silence.
const char* const kToolNames[] = {
    "snapshot", "query",     "tap",           "long_press", "increase",
    "decrease", "dismiss",   "expand",        "collapse",   "show_on_screen",
    "set_text", "scroll_to", "custom_action", "tap_at",
};
constexpr size_t kToolCount = sizeof(kToolNames) / sizeof(kToolNames[0]);

// Whether a dispatch is in flight, so the republish below cannot recurse.
bool g_publishing = false;

void PublishTree();

void OnEnable(void*, bool) {}

int OnPointerTap(void*, int64_t, double, double) {
  return IHS_SEMANTICS_OK;
}

// Republishes from inside the dispatch, the way the framework would after
// handling an action. This is what keeps the verify-after-act wait short: it
// ends as soon as a newer generation appears, and without a republish every
// action pays the full settle bound.
int OnDispatch(void*, int64_t, int32_t, uint64_t, const uint8_t*, size_t) {
  if (!g_publishing) {
    PublishTree();
  }
  return IHS_SEMANTICS_OK;
}

// Storage the published tree points into. The hub copies what it needs, but
// the arrays have to outlive the publish call itself.
std::vector<IhsSemanticsPublishNode> g_nodes;
std::vector<IhsSemanticsPublishCustomAction> g_custom_actions;
std::vector<int32_t> g_custom_ids;
std::vector<int32_t> g_root_children;

// A tree shaped to make the decoders reachable rather than to resemble any
// real application: something for every selector query can filter on, a node
// for each state the enabled gate distinguishes, an obscured field for the
// paths that refuse one, and a scrollable carrying a numeric value.
void BuildTree() {
  g_custom_actions = {
      IhsSemanticsPublishCustomAction{7, "Set to maximum", ""},
      IhsSemanticsPublishCustomAction{8, "Reset", ""},
  };
  g_custom_ids = {7, 8};
  g_root_children = {2, 3, 4, 5, 6};

  g_nodes.clear();

  IhsSemanticsPublishNode root{};
  root.id = 1;
  root.label = "Root";
  root.role = IHS_SEMANTICS_ROLE_PANE;
  root.enabled = IHS_SEMANTICS_TRISTATE_NONE;
  root.child_ids = g_root_children.data();
  root.child_count = g_root_children.size();
  g_nodes.push_back(root);

  IhsSemanticsPublishNode button{};
  button.id = 2;
  button.identifier = "primary-button";
  button.label = "Increase temperature";
  button.hint = "Raises the cabin temperature";
  button.role = IHS_SEMANTICS_ROLE_BUTTON;
  button.enabled = IHS_SEMANTICS_TRISTATE_TRUE;
  button.focusable = true;
  button.actions = IHS_SEMANTICS_ACTION_TAP | IHS_SEMANTICS_ACTION_LONG_PRESS |
                   IHS_SEMANTICS_ACTION_INCREASE |
                   IHS_SEMANTICS_ACTION_DECREASE |
                   IHS_SEMANTICS_ACTION_CUSTOM_ACTION;
  button.custom_action_ids = g_custom_ids.data();
  button.custom_action_count = g_custom_ids.size();
  g_nodes.push_back(button);

  // Disabled, so the enabled gate has something to refuse. "Disabled" and
  // "enablement does not apply" are different answers and only the first
  // refuses, which is why the root above is NONE rather than FALSE.
  IhsSemanticsPublishNode disabled{};
  disabled.id = 3;
  disabled.label = "Unavailable";
  disabled.role = IHS_SEMANTICS_ROLE_BUTTON;
  disabled.enabled = IHS_SEMANTICS_TRISTATE_FALSE;
  disabled.actions = IHS_SEMANTICS_ACTION_TAP;
  g_nodes.push_back(disabled);

  IhsSemanticsPublishNode field{};
  field.id = 4;
  field.identifier = "destination";
  field.label = "Destination";
  field.value = "San Jose";
  field.role = IHS_SEMANTICS_ROLE_TEXT_INPUT;
  field.enabled = IHS_SEMANTICS_TRISTATE_TRUE;
  field.focusable = true;
  field.actions = IHS_SEMANTICS_ACTION_TAP | IHS_SEMANTICS_ACTION_SET_TEXT |
                  IHS_SEMANTICS_ACTION_FOCUS |
                  IHS_SEMANTICS_ACTION_SET_SELECTION;
  g_nodes.push_back(field);

  // Obscured: the read path must not emit its value and set_text must refuse
  // it outright.
  IhsSemanticsPublishNode secret{};
  secret.id = 5;
  secret.label = "Passcode";
  secret.value = "hunter2";
  secret.role = IHS_SEMANTICS_ROLE_PASSWORD_INPUT;
  secret.obscured = true;
  secret.enabled = IHS_SEMANTICS_TRISTATE_TRUE;
  secret.actions = IHS_SEMANTICS_ACTION_TAP | IHS_SEMANTICS_ACTION_SET_TEXT;
  g_nodes.push_back(secret);

  IhsSemanticsPublishNode scroller{};
  scroller.id = 6;
  scroller.label = "Track list";
  scroller.role = IHS_SEMANTICS_ROLE_SCROLL_VIEW;
  scroller.enabled = IHS_SEMANTICS_TRISTATE_NONE;
  scroller.actions = IHS_SEMANTICS_ACTION_SCROLL_UP |
                     IHS_SEMANTICS_ACTION_SCROLL_DOWN |
                     IHS_SEMANTICS_ACTION_SCROLL_TO_OFFSET |
                     IHS_SEMANTICS_ACTION_SHOW_ON_SCREEN;
  scroller.has_numeric_value = true;
  scroller.numeric_value = 40.0;
  scroller.numeric_value_min = 0.0;
  scroller.numeric_value_max = 400.0;
  g_nodes.push_back(scroller);
}

void PublishTree() {
  g_publishing = true;
  IhsSemanticsPublishInfo info{};
  info.struct_size = sizeof(IhsSemanticsPublishInfo);
  info.nodes = g_nodes.data();
  info.node_count = g_nodes.size();
  info.custom_actions = g_custom_actions.data();
  info.custom_action_count = g_custom_actions.size();
  ihs_semantics_publish(&info);
  g_publishing = false;
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
  IhsSemanticsHost host{};
  host.struct_size = sizeof(IhsSemanticsHost);
  host.set_semantics_enabled = OnEnable;
  host.dispatch = OnDispatch;
  host.send_pointer_tap = OnPointerTap;
  ihs_semantics_set_host(&host);

  if (ihs_mcp_semantics_provider_start() != IHS_MCP_OK) {
    std::fprintf(stderr, "fuzz_mcp_semantics_tools: provider did not start\n");
    ::abort();  // fuzzing a surface that is not there would prove nothing
  }
  BuildTree();
  PublishTree();
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, const size_t size) {
  if (size < 2) {
    return 0;  // one selector byte plus at least one byte of arguments
  }

  const std::string tool =
      std::string("ui_") + kToolNames[data[0] % kToolCount];

  // The arguments are not NUL-terminated in the input, and the callee takes a
  // length -- but it is a C ABI taking a const char*, and a provider that
  // reaches for the terminator rather than the length is exactly the kind of
  // thing worth finding. A copy would hide it by always supplying one, so the
  // bytes are passed through unterminated and ASan decides.
  const char* arguments = reinterpret_cast<const char*>(data + 1);
  const size_t arguments_length = size - 1;

  IhsMcpPayload payload{};
  ihs_mcp_registry_call_tool(tool.c_str(), arguments, arguments_length,
                             &payload);
  ihs_mcp_registry_release_payload(&payload);
  return 0;
}
