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

// Tools an application declares for itself. See ihs/ihs_mcp_app_tools.h for
// the contract; this is a provider whose tool list is whatever was registered
// and whose call_tool hands the call to the application and waits for it.

#include "ihs/ihs_mcp_app_tools.h"

#include "ihs/ihs_mcp_provider.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ihs/logging.h"

namespace {

int32_t TraceContext() {
  static const int32_t ctx = ihs_log_context_open("MCPA", nullptr);
  return ctx;
}

void Log(const int32_t level, const std::string& text) {
  const int32_t ctx = TraceContext();
  if (ctx < 0 || ihs_log_enabled(ctx, level) == 0) {
    return;
  }
  ihs_log(ctx, level, text.c_str(), text.size());
}

constexpr uint32_t kDefaultCallTimeoutMs = 5000;

// One tool, with its strings owned here. The descriptor a caller passes is
// borrowed for the length of the register call only, and a Dart caller's
// strings are unlikely to outlive it.
struct OwnedTool {
  std::string name;
  std::string description;
  std::string schema;
  uint64_t capability = 0;
};

// A call handed to the application and not yet answered.
struct PendingCall {
  std::mutex mutex;
  std::condition_variable cv;
  bool answered = false;
  bool ok = false;
  std::string result;
};

struct Registration {
  std::string prefix;
  std::vector<OwnedTool> tools;
  // Rebuilt whenever `tools` changes: the registry borrows this array and
  // reads the pointers out of it, so it has to outlive the list_tools call.
  std::vector<IhsMcpToolDesc> advertised;
  IhsMcpAppToolInvoke invoke = nullptr;
  void* user_data = nullptr;
  uint32_t timeout_ms = kDefaultCallTimeoutMs;
  IhsMcpProvider* provider = nullptr;
  int notify_fd = -1;

  // Held across an invoke so unregister cannot pull the callback out from
  // under a call already on its way to the application.
  std::mutex mutex;
  bool live = false;
};

struct State {
  std::mutex mutex;
  std::map<uint64_t, std::shared_ptr<PendingCall>> calls;
  uint64_t next_call_id = 1;
};

State& TheState() {
  static auto* state = new State();  // leaked: outlives static teardown
  return *state;
}

// ---------------------------------------------------------------------------
// Provider callbacks
// ---------------------------------------------------------------------------

int ListTools(void* user_data,
              const IhsMcpToolDesc** out_tools,
              size_t* out_count) {
  auto* reg = static_cast<Registration*>(user_data);
  const std::lock_guard<std::mutex> lock(reg->mutex);
  *out_tools = reg->advertised.empty() ? nullptr : reg->advertised.data();
  *out_count = reg->advertised.size();
  return IHS_MCP_OK;
}

int ListResources(void* /* user_data */,
                  const IhsMcpResourceDesc** out_resources,
                  size_t* out_count) {
  // Tools only. An application that wants to publish readable state has the
  // semantics tree for it already.
  *out_resources = nullptr;
  *out_count = 0;
  return IHS_MCP_OK;
}

int ReadResource(void* /* user_data */,
                 const char* /* uri */,
                 IhsMcpPayload* /* out_content */) {
  return IHS_MCP_ERR_NOT_FOUND;
}

void ReleasePayload(void* ctx) {
  delete[] static_cast<char*>(ctx);
}

void FillPayload(IhsMcpPayload* out, const std::string& text) {
  char* copy = new char[text.size()];
  std::memcpy(copy, text.data(), text.size());
  out->struct_size = sizeof(IhsMcpPayload);
  out->data = copy;
  out->length = text.size();
  out->release = ReleasePayload;
  out->release_ctx = copy;
}

int CallTool(void* user_data,
             const char* name,
             const char* arguments_json,
             size_t arguments_length,
             IhsMcpPayload* out_result) {
  auto* reg = static_cast<Registration*>(user_data);

  IhsMcpAppToolInvoke invoke = nullptr;
  void* invoke_user_data = nullptr;
  uint32_t timeout_ms = kDefaultCallTimeoutMs;
  {
    const std::lock_guard<std::mutex> lock(reg->mutex);
    if (!reg->live) {
      return IHS_MCP_ERR_UNAVAILABLE;
    }
    bool known = false;
    for (const OwnedTool& tool : reg->tools) {
      if (tool.name == name) {
        known = true;
        break;
      }
    }
    if (!known) {
      return IHS_MCP_ERR_NOT_FOUND;
    }
    invoke = reg->invoke;
    invoke_user_data = reg->user_data;
    timeout_ms = reg->timeout_ms;
  }

  auto call = std::make_shared<PendingCall>();
  uint64_t call_id = 0;
  {
    State& state = TheState();
    const std::lock_guard<std::mutex> lock(state.mutex);
    call_id = state.next_call_id++;
    state.calls[call_id] = call;
  }

  // Invoked with no lock held: the application is free to complete the call
  // from inside this callback, on this thread, and holding anything here would
  // deadlock the simplest implementation there is.
  invoke(invoke_user_data, call_id, name,
         arguments_json != nullptr ? arguments_json : "", arguments_length);

  bool answered = false;
  bool ok = false;
  std::string result;
  {
    std::unique_lock<std::mutex> lock(call->mutex);
    answered = call->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                 [&call]() { return call->answered; });
    ok = call->ok;
    result = call->result;
  }

  // Removed whether or not it was answered, so a late completion is reported
  // to the application as unknown rather than writing into a call nobody is
  // waiting on.
  {
    State& state = TheState();
    const std::lock_guard<std::mutex> lock(state.mutex);
    state.calls.erase(call_id);
  }

  if (!answered) {
    Log(IHS_LEVEL_WARN, std::string("app tool '") + name +
                            "' did not answer within " +
                            std::to_string(timeout_ms) + "ms");
    return IHS_MCP_ERR_UNAVAILABLE;
  }
  if (!result.empty()) {
    FillPayload(out_result, result);
  }
  return ok ? IHS_MCP_OK : IHS_MCP_ERR_REFUSED;
}

// Tells clients the tool list changed. The registry re-lists and emits
// notifications/tools/list_changed, so an agent that already listed does not
// have to poll to discover a route's tools.
void SignalListChanged(const Registration& reg) {
  if (reg.notify_fd < 0) {
    return;
  }
  const uint64_t one = 1;
  ssize_t written;
  do {
    written = ::write(reg.notify_fd, &one, sizeof(one));
  } while (written < 0 && errno == EINTR);
}

}  // namespace

struct IhsMcpAppTools {
  Registration reg;
};

extern "C" {

int ihs_mcp_app_tools_register(const IhsMcpAppToolsDesc* desc,
                               IhsMcpAppTools** out_handle) {
  if (desc == nullptr || desc->struct_size == 0 || out_handle == nullptr ||
      desc->invoke == nullptr || desc->tool_prefix == nullptr ||
      desc->tool_prefix[0] == '\0') {
    return IHS_MCP_ERR_INVALID;
  }
  if (desc->tool_count > 0 && desc->tools == nullptr) {
    return IHS_MCP_ERR_INVALID;
  }

  auto handle = std::make_unique<IhsMcpAppTools>();
  Registration& reg = handle->reg;
  reg.prefix = desc->tool_prefix;
  reg.invoke = desc->invoke;
  reg.user_data = desc->user_data;
  reg.timeout_ms = desc->call_timeout_ms != 0 ? desc->call_timeout_ms
                                              : kDefaultCallTimeoutMs;

  for (size_t i = 0; i < desc->tool_count; i++) {
    const IhsMcpAppTool& in = desc->tools[i];
    if (in.name == nullptr || in.name[0] == '\0') {
      return IHS_MCP_ERR_INVALID;
    }
    OwnedTool tool;
    tool.name = in.name;
    tool.description = in.description != nullptr ? in.description : "";
    tool.schema = in.input_schema_json != nullptr
                      ? in.input_schema_json
                      : R"({"type":"object","properties":{}})";
    tool.capability = in.capability != 0 ? in.capability : IHS_MCP_CAP_INTERACT;
    reg.tools.push_back(std::move(tool));
  }

  reg.advertised.reserve(reg.tools.size());
  for (const OwnedTool& tool : reg.tools) {
    IhsMcpToolDesc out{};
    out.name = tool.name.c_str();
    out.description = tool.description.c_str();
    out.input_schema_json = tool.schema.c_str();
    out.capability = tool.capability;
    reg.advertised.push_back(out);
  }

  // Its own descriptor, so a tool list change is a signal the registry already
  // knows how to turn into notifications/tools/list_changed.
  reg.notify_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

  IhsMcpProviderDesc provider{};
  provider.struct_size = sizeof(provider);
  provider.name = "app";
  provider.tool_prefix = reg.prefix.c_str();
  // No resource scheme: this provider serves tools only, and claiming one
  // would collide with a provider that actually publishes resources.
  provider.resource_scheme = nullptr;
  provider.capability_mask = IHS_MCP_CAP_ALL;
  provider.notify_fd = reg.notify_fd;
  provider.user_data = &reg;
  provider.callbacks.list_tools = ListTools;
  provider.callbacks.list_resources = ListResources;
  provider.callbacks.call_tool = CallTool;
  provider.callbacks.read_resource = ReadResource;

  const int status = ihs_mcp_provider_register(&provider, &reg.provider);
  if (status != IHS_MCP_OK) {
    if (reg.notify_fd >= 0) {
      ::close(reg.notify_fd);
      reg.notify_fd = -1;
    }
    return status;
  }

  {
    const std::lock_guard<std::mutex> lock(reg.mutex);
    reg.live = true;
  }
  SignalListChanged(reg);

  Log(IHS_LEVEL_INFO, "registered " + std::to_string(reg.tools.size()) +
                          " application tool(s) under '" + reg.prefix + "'");
  *out_handle = handle.release();
  return IHS_MCP_OK;
}

int ihs_mcp_app_tools_complete(const uint64_t call_id,
                               const bool ok,
                               const char* result_json,
                               const size_t result_length) {
  std::shared_ptr<PendingCall> call;
  {
    State& state = TheState();
    const std::lock_guard<std::mutex> lock(state.mutex);
    const auto it = state.calls.find(call_id);
    if (it == state.calls.end()) {
      return IHS_MCP_ERR_NOT_FOUND;
    }
    call = it->second;
  }

  {
    const std::lock_guard<std::mutex> lock(call->mutex);
    if (call->answered) {
      return IHS_MCP_ERR_NOT_FOUND;  // answered once already
    }
    call->answered = true;
    call->ok = ok;
    if (result_json != nullptr && result_length > 0) {
      call->result.assign(result_json, result_length);
    }
  }
  call->cv.notify_one();
  return IHS_MCP_OK;
}

void ihs_mcp_app_tools_unregister(IhsMcpAppTools* handle) {
  if (handle == nullptr) {
    return;
  }
  Registration& reg = handle->reg;

  // Marked dead first: a call that has not yet reached CallTool is refused
  // rather than dispatched into a callback about to be torn down.
  {
    const std::lock_guard<std::mutex> lock(reg.mutex);
    reg.live = false;
  }

  // Fail everything outstanding before unregistering. A caller waiting on the
  // application would otherwise wait out its whole timeout for an answer that
  // can no longer come.
  {
    State& state = TheState();
    std::vector<std::shared_ptr<PendingCall>> orphaned;
    {
      const std::lock_guard<std::mutex> lock(state.mutex);
      for (auto& entry : state.calls) {
        orphaned.push_back(entry.second);
      }
    }
    for (const std::shared_ptr<PendingCall>& call : orphaned) {
      {
        const std::lock_guard<std::mutex> lock(call->mutex);
        if (call->answered) {
          continue;
        }
        call->answered = true;
        call->ok = false;
      }
      call->cv.notify_one();
    }
  }

  // Drains any dispatch already in flight before returning, so no invoke can
  // fire after this.
  if (reg.provider != nullptr) {
    ihs_mcp_provider_unregister(reg.provider);
    reg.provider = nullptr;
  }
  if (reg.notify_fd >= 0) {
    ::close(reg.notify_fd);
    reg.notify_fd = -1;
  }
  delete handle;
}

}  // extern "C"
