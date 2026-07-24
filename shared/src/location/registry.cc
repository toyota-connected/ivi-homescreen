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

// Inbound ops are copied with the ivi-homescreen #337 bounded-copy discipline:
// a caller built against an older header passes a smaller struct_size, and any
// callback it does not carry stays NULL (the manager null-checks before
// calling) rather than being read from past the caller's object.

// The clamped number of bytes the caller actually declared.
template <typename T>
size_t CopiedSize(const T* ops) {
  return ops->struct_size < sizeof(T) ? ops->struct_size : sizeof(T);
}

// True iff the field at [offset, offset+size) is FULLY within the caller's
// declared bytes. A per-field copy is guarded by this so that (a) a field the
// caller did not carry is never read past their object, and (b) a struct_size
// that ends mid-field never yields a truncated, non-NULL garbage pointer that a
// manager would later call — the omitted/partial field stays NULL/zero.
constexpr bool Covers(size_t copied, size_t offset, size_t size) {
  return offset + size <= copied;
}

// COPY_FIELD(full, ops, copied, name): copy one field only when fully covered.
#define COPY_FIELD(full, ops, copied, name)              \
  do {                                                   \
    if (Covers((copied), offsetof(decltype(full), name), \
               sizeof((full).name))) {                   \
      (full).name = (ops)->name;                         \
    }                                                    \
  } while (0)

// Copy the caller's ops field-by-field into a zeroed full-size struct. Unlike a
// byte-wise memcpy of struct_size bytes (which can split a pointer field), this
// copies each callback only when the caller's struct_size fully covers it, so
// an omitted or straddled callback reads back NULL — the bounded-copy contract.
// The stored struct_size keeps the caller's clamped size (the ABI signal for
// which fields were provided), matching platform_view.cc's zero_out discipline.
IhsLocationSourceOps CopySourceOps(const IhsLocationSourceOps* ops) {
  const size_t n = CopiedSize(ops);
  IhsLocationSourceOps full{};
  full.struct_size = static_cast<uint32_t>(n);
  COPY_FIELD(full, ops, n, start);
  COPY_FIELD(full, ops, n, stop);
  return full;
}

IhsLocationFilterOps CopyFilterOps(const IhsLocationFilterOps* ops) {
  const size_t n = CopiedSize(ops);
  IhsLocationFilterOps full{};
  full.struct_size = static_cast<uint32_t>(n);
  COPY_FIELD(full, ops, n, create);
  COPY_FIELD(full, ops, n, destroy);
  COPY_FIELD(full, ops, n, update);
  COPY_FIELD(full, ops, n, estimate);
  return full;
}

#undef COPY_FIELD

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
  sources()[key] = SourceEntry{CopySourceOps(ops), user_data};
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
  filters()[key] = FilterEntry{CopyFilterOps(ops), user_data};
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
