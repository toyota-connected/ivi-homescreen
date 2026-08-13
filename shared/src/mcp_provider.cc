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

// MCP provider registry: namespacing, capability masking, and routing between
// registered providers and the server above them. See
// include/ihs/ihs_mcp_provider.h for the provider contract and
// include/ihs/ihs_mcp_registry.h for the shell-facing routing surface.

#include "ihs/ihs_mcp_provider.h"
#include "ihs/ihs_mcp_registry.h"

#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace {

struct Provider {
  std::string name;
  std::string tool_prefix;
  std::string resource_scheme;  // without "://"
  std::string resource_prefix;  // with "://", precomputed for matching
  uint64_t capability_mask = 0;
  IhsMcpProviderCallbacks callbacks{};
  void* user_data = nullptr;
  int notify_fd = -1;
};

// One process-wide registry. A recursive mutex is not used and must not be:
// provider callbacks are documented as not calling back in, and a plain mutex
// turns a violation of that into a deadlock at the first test rather than into
// re-entrant corruption discovered much later.
struct Registry {
  std::mutex mutex;
  std::vector<std::unique_ptr<Provider>> providers;

  // The notification sink and the thread that feeds it. The watcher polls
  // every provider's notify_fd plus a self-pipe, so a registration or a
  // shutdown interrupts the poll rather than waiting for a provider to
  // signal -- otherwise adding a provider would not take effect until the
  // one before it happened to change.
  IhsMcpNotificationSink sink = nullptr;
  void* sink_user_data = nullptr;
  std::thread watcher;
  int wake_read_fd = -1;
  int wake_write_fd = -1;
  bool watching = false;

  // Bumped every time the watcher rebuilds its poll set. Unregister waits for
  // it to move so the watcher is not left polling an fd whose owner is about
  // to close it -- the descriptor number would be reused and the watcher
  // would then be waiting on something unrelated.
  uint64_t poll_generation = 0;
  std::condition_variable poll_rebuilt;
};

Registry& TheRegistry() {
  static auto* registry = new Registry();  // leaked; outlives statics
  return *registry;
}

// Nudges the watcher so it re-reads the provider list.
void WakeWatcher(Registry& registry) {
  if (registry.wake_write_fd < 0) {
    return;
  }
  const char byte = 1;
  ssize_t written;
  do {
    written = ::write(registry.wake_write_fd, &byte, 1);
  } while (written < 0 && errno == EINTR);
}

// Drains an fd a provider signalled. The count is irrelevant: the contract is
// that a signal means "something changed, go look", and coalescing several
// into one notification is the point rather than a loss.
//
// notify_fd must be non-blocking, which ihs_mcp_provider.h requires: a
// blocking one would stop the watcher dead here once it took the last byte,
// and take every other provider's notifications down with it.
void DrainSignal(const int fd) {
  uint64_t sink = 0;
  while (::read(fd, &sink, sizeof(sink)) > 0) {
  }
}

void WatchProviders() {
  Registry& registry = TheRegistry();
  while (true) {
    // Snapshot what to poll, then release the lock: the poll blocks, and
    // holding the registry across it would stall every call into the host.
    std::vector<pollfd> fds;
    std::vector<std::string> resource_uris;  // parallel to fds, after index 0
    {
      std::unique_lock<std::mutex> lock(registry.mutex);
      if (!registry.watching) {
        registry.poll_generation++;
        lock.unlock();
        registry.poll_rebuilt.notify_all();
        return;
      }
      fds.push_back({registry.wake_read_fd, POLLIN, 0});
      for (const auto& provider : registry.providers) {
        if (provider->notify_fd < 0) {
          continue;
        }
        fds.push_back({provider->notify_fd, POLLIN, 0});
        resource_uris.push_back(provider->resource_prefix);
      }
      // Published before the poll, so a waiter that sees it knows the fds it
      // cared about are either in this set or already dropped.
      registry.poll_generation++;
      lock.unlock();
      registry.poll_rebuilt.notify_all();
    }

    if (::poll(fds.data(), fds.size(), -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }

    if ((fds[0].revents & POLLIN) != 0) {
      char byte = 0;
      while (::read(registry.wake_read_fd, &byte, 1) > 0) {
      }
      continue;  // the provider set changed; rebuild the poll list
    }

    // Copy the sink out under the lock and call it without: a sink is
    // documented as free to call back into the host, which a held lock would
    // turn into a deadlock.
    IhsMcpNotificationSink sink = nullptr;
    void* sink_user_data = nullptr;
    {
      const std::lock_guard<std::mutex> lock(registry.mutex);
      sink = registry.sink;
      sink_user_data = registry.sink_user_data;
    }
    if (sink == nullptr) {
      continue;
    }

    for (size_t i = 1; i < fds.size(); i++) {
      if ((fds[i].revents & POLLIN) == 0) {
        continue;
      }
      DrainSignal(fds[i].fd);
      // A provider signals for either reason and does not say which, so both
      // notifications go out. A client that re-reads a tool list which did
      // not change loses nothing; one that never hears about a change does.
      sink(IHS_MCP_NOTIFY_TOOLS_CHANGED, "", sink_user_data);
      sink(IHS_MCP_NOTIFY_RESOURCE_UPDATED, resource_uris[i - 1].c_str(),
           sink_user_data);
    }
  }
}

bool StartsWith(const char* s, const std::string& prefix) {
  return std::strncmp(s, prefix.c_str(), prefix.size()) == 0;
}

// Prefix collision is symmetric: neither may be a prefix of the other, not
// merely unequal. "ui_" and "ui_debug_" would both match a tool named
// "ui_debug_dump", and which provider got it would come down to registration
// order.
bool PrefixesOverlap(const std::string& a, const std::string& b) {
  const size_t n = a.size() < b.size() ? a.size() : b.size();
  return a.compare(0, n, b, 0, n) == 0;
}

Provider* FindByToolName(Registry& registry, const char* name) {
  for (const auto& provider : registry.providers) {
    if (StartsWith(name, provider->tool_prefix)) {
      return provider.get();
    }
  }
  return nullptr;
}

Provider* FindByUri(Registry& registry, const char* uri) {
  for (const auto& provider : registry.providers) {
    if (StartsWith(uri, provider->resource_prefix)) {
      return provider.get();
    }
  }
  return nullptr;
}

bool CapabilityAllowed(const Provider& provider, const uint64_t capability) {
  // A tool declaring no capability is not "unrestricted" -- it is
  // underspecified, and treating that as permission would make an omission the
  // most permissive choice available.
  return capability != 0 && (capability & ~provider.capability_mask) == 0;
}

}  // namespace

extern "C" {

int ihs_mcp_provider_register(const IhsMcpProviderDesc* desc,
                              IhsMcpProvider** out_provider) {
  if (desc == nullptr || out_provider == nullptr ||
      desc->struct_size < sizeof(IhsMcpProviderDesc) || desc->name == nullptr ||
      desc->tool_prefix == nullptr || desc->resource_scheme == nullptr) {
    return IHS_MCP_ERR_INVALID;
  }
  // An empty prefix would claim the entire namespace and collide with every
  // provider that follows, so it is rejected rather than being allowed to
  // become a first-registration-wins land grab.
  if (desc->tool_prefix[0] == '\0' || desc->resource_scheme[0] == '\0') {
    return IHS_MCP_ERR_INVALID;
  }

  auto provider = std::make_unique<Provider>();
  provider->name = desc->name;
  provider->tool_prefix = desc->tool_prefix;
  provider->resource_scheme = desc->resource_scheme;
  provider->resource_prefix = provider->resource_scheme + "://";
  provider->capability_mask = desc->capability_mask;
  provider->callbacks = desc->callbacks;
  provider->user_data = desc->user_data;
  provider->notify_fd = desc->notify_fd;

  Provider* raw = provider.get();
  {
    Registry& registry = TheRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    for (const auto& existing : registry.providers) {
      if (PrefixesOverlap(existing->tool_prefix, provider->tool_prefix) ||
          PrefixesOverlap(existing->resource_scheme,
                          provider->resource_scheme)) {
        // Rejected whole: a provider is never half-registered, so a caller
        // that ignores the status does not end up with resources served and
        // tools missing.
        return IHS_MCP_ERR_PREFIX_TAKEN;
      }
    }
    registry.providers.push_back(std::move(provider));
    // The watcher polls a snapshot of the fds, so it has to be told the set
    // changed or a new provider's signals go unheard until an older one
    // happens to fire.
    WakeWatcher(registry);
  }

  *out_provider = reinterpret_cast<IhsMcpProvider*>(raw);
  return IHS_MCP_OK;
}

void ihs_mcp_provider_unregister(IhsMcpProvider* provider) {
  if (provider == nullptr) {
    return;
  }
  auto* raw = reinterpret_cast<Provider*>(provider);

  std::unique_ptr<Provider> owned;
  {
    Registry& registry = TheRegistry();
    std::unique_lock<std::mutex> lock(registry.mutex);
    for (auto it = registry.providers.begin(); it != registry.providers.end();
         ++it) {
      if (it->get() == raw) {
        owned = std::move(*it);
        registry.providers.erase(it);
        break;
      }
    }
    if (registry.watching && owned != nullptr && owned->notify_fd >= 0) {
      // Wait for the watcher to rebuild its poll set without this fd:
      // otherwise the caller closes notify_fd next while the watcher is still
      // polling it, and the descriptor number is immediately reusable.
      //
      // The provider is already out of the list, so any rebuild from here on
      // excludes it -- one is enough, and waiting for more would hang.
      const uint64_t seen = registry.poll_generation;
      WakeWatcher(registry);
      registry.poll_rebuilt.wait(
          lock, [&] { return registry.poll_generation > seen; });
    }
  }
  // Removal happens under the lock, and every route into a provider is taken
  // under that same lock, so once we are here no call can be in flight and
  // none can start. That is the drain the contract promises; the provider's
  // state is safe to free the moment this returns.
}

size_t ihs_mcp_provider_count(void) {
  Registry& registry = TheRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  return registry.providers.size();
}

int ihs_mcp_registry_for_each_tool(IhsMcpToolVisitor fn, void* user_data) {
  if (fn == nullptr) {
    return IHS_MCP_ERR_INVALID;
  }
  Registry& registry = TheRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  for (const auto& provider : registry.providers) {
    if (provider->callbacks.list_tools == nullptr) {
      continue;  // resources-only provider; an empty list, not an error
    }
    const IhsMcpToolDesc* tools = nullptr;
    size_t count = 0;
    if (provider->callbacks.list_tools(provider->user_data, &tools, &count) !=
            IHS_MCP_OK ||
        tools == nullptr) {
      continue;
    }
    for (size_t i = 0; i < count; i++) {
      if (!CapabilityAllowed(*provider, tools[i].capability)) {
        continue;  // masked out: never advertised, not merely refused on call
      }
      const std::string prefixed =
          provider->tool_prefix +
          (tools[i].name != nullptr ? tools[i].name : "");
      IhsMcpRegistryTool out;
      out.name = prefixed.c_str();
      out.description =
          tools[i].description != nullptr ? tools[i].description : "";
      out.input_schema_json = tools[i].input_schema_json != nullptr
                                  ? tools[i].input_schema_json
                                  : "{}";
      out.provider = provider->name.c_str();
      out.capability = tools[i].capability;
      fn(&out, user_data);
    }
  }
  return IHS_MCP_OK;
}

int ihs_mcp_registry_for_each_resource(IhsMcpResourceVisitor fn,
                                       void* user_data) {
  if (fn == nullptr) {
    return IHS_MCP_ERR_INVALID;
  }
  Registry& registry = TheRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  for (const auto& provider : registry.providers) {
    if (provider->callbacks.list_resources == nullptr) {
      continue;
    }
    const IhsMcpResourceDesc* resources = nullptr;
    size_t count = 0;
    if (provider->callbacks.list_resources(provider->user_data, &resources,
                                           &count) != IHS_MCP_OK ||
        resources == nullptr) {
      continue;
    }
    for (size_t i = 0; i < count; i++) {
      IhsMcpRegistryResource out;
      out.uri = resources[i].uri != nullptr ? resources[i].uri : "";
      out.name = resources[i].name != nullptr ? resources[i].name : "";
      out.description =
          resources[i].description != nullptr ? resources[i].description : "";
      out.mime_type =
          resources[i].mime_type != nullptr ? resources[i].mime_type : "";
      out.provider = provider->name.c_str();
      fn(&out, user_data);
    }
  }
  return IHS_MCP_OK;
}

int ihs_mcp_registry_set_notification_sink(const IhsMcpNotificationSink sink,
                                           void* user_data) {
  Registry& registry = TheRegistry();

  if (sink == nullptr) {
    std::thread watcher;
    {
      const std::lock_guard<std::mutex> lock(registry.mutex);
      if (!registry.watching) {
        return IHS_MCP_OK;  // idempotent
      }
      registry.watching = false;
      registry.sink = nullptr;
      registry.sink_user_data = nullptr;
      WakeWatcher(registry);
      watcher = std::move(registry.watcher);
    }
    // Joined outside the lock: the watcher takes it on every pass, so holding
    // it here would deadlock against the thread being waited for.
    if (watcher.joinable()) {
      watcher.join();
    }
    const std::lock_guard<std::mutex> lock(registry.mutex);
    ::close(registry.wake_read_fd);
    ::close(registry.wake_write_fd);
    registry.wake_read_fd = -1;
    registry.wake_write_fd = -1;
    return IHS_MCP_OK;
  }

  const std::lock_guard<std::mutex> lock(registry.mutex);
  if (registry.watching) {
    return IHS_MCP_ERR_REFUSED;
  }
  int wake[2] = {-1, -1};
  // Non-blocking: the watcher drains the pipe until it is empty, and a
  // blocking read end turns that drain into a hang once the last byte is
  // taken.
  if (::pipe2(wake, O_NONBLOCK | O_CLOEXEC) != 0) {
    return IHS_MCP_ERR_UNAVAILABLE;
  }
  registry.wake_read_fd = wake[0];
  registry.wake_write_fd = wake[1];
  registry.sink = sink;
  registry.sink_user_data = user_data;
  registry.watching = true;
  registry.watcher = std::thread(WatchProviders);
  return IHS_MCP_OK;
}

int ihs_mcp_registry_call_tool(const char* name,
                               const char* arguments_json,
                               size_t arguments_length,
                               IhsMcpPayload* out_result) {
  if (name == nullptr || out_result == nullptr) {
    return IHS_MCP_ERR_INVALID;
  }

  Registry& registry = TheRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  Provider* provider = FindByToolName(registry, name);
  if (provider == nullptr || provider->callbacks.call_tool == nullptr) {
    return IHS_MCP_ERR_NOT_FOUND;
  }

  // Resolve the tool before calling so the capability can be checked. A
  // provider that offers no listing cannot have its tools masked, so it is
  // refused rather than trusted -- the mask must not be bypassable by
  // declining to enumerate.
  if (provider->callbacks.list_tools == nullptr) {
    return IHS_MCP_ERR_CAPABILITY_DENIED;
  }

  const char* unprefixed = name + provider->tool_prefix.size();
  const IhsMcpToolDesc* tools = nullptr;
  size_t count = 0;
  if (provider->callbacks.list_tools(provider->user_data, &tools, &count) !=
          IHS_MCP_OK ||
      tools == nullptr) {
    return IHS_MCP_ERR_NOT_FOUND;
  }

  const IhsMcpToolDesc* match = nullptr;
  for (size_t i = 0; i < count; i++) {
    if (tools[i].name != nullptr &&
        std::strcmp(tools[i].name, unprefixed) == 0) {
      match = &tools[i];
      break;
    }
  }
  if (match == nullptr) {
    return IHS_MCP_ERR_NOT_FOUND;
  }
  if (!CapabilityAllowed(*provider, match->capability)) {
    return IHS_MCP_ERR_CAPABILITY_DENIED;
  }

  // Providers are promised a non-null argument object, so an absent one
  // becomes "{}" here rather than every provider needing the same check.
  static const char kEmptyArguments[] = "{}";
  const char* args =
      arguments_json != nullptr ? arguments_json : kEmptyArguments;
  const size_t args_length = arguments_json != nullptr
                                 ? arguments_length
                                 : sizeof(kEmptyArguments) - 1;

  return provider->callbacks.call_tool(provider->user_data, unprefixed, args,
                                       args_length, out_result);
}

int ihs_mcp_registry_read_resource(const char* uri,
                                   IhsMcpPayload* out_content) {
  if (uri == nullptr || out_content == nullptr) {
    return IHS_MCP_ERR_INVALID;
  }
  Registry& registry = TheRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  Provider* provider = FindByUri(registry, uri);
  if (provider == nullptr || provider->callbacks.read_resource == nullptr) {
    return IHS_MCP_ERR_NOT_FOUND;
  }
  return provider->callbacks.read_resource(provider->user_data, uri,
                                           out_content);
}

int ihs_mcp_registry_subscribe(const char* uri, bool subscribed) {
  if (uri == nullptr) {
    return IHS_MCP_ERR_INVALID;
  }
  Registry& registry = TheRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  Provider* provider = FindByUri(registry, uri);
  if (provider == nullptr) {
    return IHS_MCP_ERR_NOT_FOUND;
  }
  if (provider->callbacks.subscribe == nullptr) {
    // The registry tracks subscriptions itself; the callback only exists so a
    // provider can start or stop work it would otherwise not do.
    return IHS_MCP_OK;
  }
  return provider->callbacks.subscribe(provider->user_data, uri, subscribed);
}

void ihs_mcp_registry_release_payload(IhsMcpPayload* payload) {
  if (payload == nullptr || payload->release == nullptr) {
    return;
  }
  payload->release(payload->release_ctx);
  payload->release = nullptr;  // idempotent: a double release is a no-op
  payload->release_ctx = nullptr;
  payload->data = nullptr;
  payload->length = 0;
}

}  // extern "C"
