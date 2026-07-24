/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

// The measurement source / filter registry (see registry.hpp) plus the two
// public ihs_location_register_* entry points that populate it.

#include "registry.hpp"

#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace ihs::location {

namespace {

std::mutex g_mutex;
std::map<std::string, SourceEntry>& sources() {
  static std::map<std::string, SourceEntry> m;
  return m;
}
std::map<std::string, FilterEntry>& filters() {
  static std::map<std::string, FilterEntry> m;
  return m;
}

// Copy the caller's ops into a zeroed full-size struct, taking only the bytes
// the caller declared (min(struct_size, sizeof)). This is the ivi-homescreen
// #337 bounded-copy discipline applied to an inbound struct: a caller built
// against an older header passes a smaller struct_size, and any callback slot
// it does not carry stays NULL (so the manager null-checks before calling)
// rather than being read from past the caller's object.
template <typename T>
T CopyOps(const T* ops) {
  T full{};
  const size_t n = ops->struct_size < sizeof(T) ? ops->struct_size : sizeof(T);
  std::memcpy(&full, ops, n);
  full.struct_size = sizeof(T);  // normalize to what this build understands
  return full;
}

}  // namespace

bool RegisterSource(const std::string& key,
                    const IhsLocationSourceOps* ops,
                    void* user_data) {
  // start() is the mandatory callback — a source with none can never be
  // started, so reject it here rather than let a manager NULL-call it later.
  // struct_size must reach start() before it can be read (short-circuits so the
  // NULL check never touches an out-of-bounds slot). stop() stays optional (a
  // source with nothing to tear down is fine; the manager NULL-checks it).
  if (key.empty() || ops == nullptr ||
      ops->struct_size <
          offsetof(IhsLocationSourceOps, start) + sizeof(ops->start) ||
      ops->start == nullptr) {
    return false;
  }
  const std::lock_guard<std::mutex> lock(g_mutex);
  sources()[key] = SourceEntry{CopyOps(ops), user_data};
  return true;
}

bool RegisterFilter(const std::string& key,
                    const IhsLocationFilterOps* ops,
                    void* user_data) {
  // create()/update()/estimate() are the mandatory filter contract; reject an
  // ops missing any of them rather than register an unusable filter that fails
  // at bind/call time. struct_size must reach estimate() (the last callback,
  // so this also covers create()/update()) before those slots can be read.
  // destroy() stays optional (a filter with nothing to free is fine).
  if (key.empty() || ops == nullptr ||
      ops->struct_size <
          offsetof(IhsLocationFilterOps, estimate) + sizeof(ops->estimate) ||
      ops->create == nullptr || ops->update == nullptr ||
      ops->estimate == nullptr) {
    return false;
  }
  const std::lock_guard<std::mutex> lock(g_mutex);
  filters()[key] = FilterEntry{CopyOps(ops), user_data};
  return true;
}

bool LookupSource(const std::string& key, SourceEntry& out) {
  const std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = sources().find(key);
  if (it == sources().end()) {
    return false;
  }
  out = it->second;
  return true;
}

bool LookupFilter(const std::string& key, FilterEntry& out) {
  const std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = filters().find(key);
  if (it == filters().end()) {
    return false;
  }
  out = it->second;
  return true;
}

}  // namespace ihs::location

// --- public C ABI: registration ---------------------------------------------

extern "C" int ihs_location_register_source(const char* key,
                                            const IhsLocationSourceOps* ops,
                                            void* user_data) {
  if (key == nullptr) {
    return 0;
  }
  try {
    return ihs::location::RegisterSource(key, ops, user_data) ? 1 : 0;
  } catch (...) {
    return 0;  // no exception may cross the C ABI (std::map alloc can throw)
  }
}

extern "C" int ihs_location_register_filter(const char* key,
                                            const IhsLocationFilterOps* ops,
                                            void* user_data) {
  if (key == nullptr) {
    return 0;
  }
  try {
    return ihs::location::RegisterFilter(key, ops, user_data) ? 1 : 0;
  } catch (...) {
    return 0;
  }
}
