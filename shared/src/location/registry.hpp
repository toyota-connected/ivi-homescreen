/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef IHS_LOC_REGISTRY_H_
#define IHS_LOC_REGISTRY_H_

#include <string>

#include "ihs/location.h"

// Process-wide table of measurement sources and filters, keyed by string, the
// same shape as ihs_pv's factory registry. Unlike ihs_pv (whose views live in
// the shell, so its registry is a host indirection), the location providers
// live in libihs_shared itself, so this is a plain in-process table.
//
// The manager (built next) looks entries up by key to compose a service; the
// public ihs_location_register_source/_filter functions populate it. Thread-
// safe: registration and lookup take an internal mutex, so a source thread can
// register while the manager binds.
namespace ihs::location {

// A registered source/filter: the caller's ops (copied bounded by struct_size
// into a zeroed full-size struct, so a shorter caller ops is never over-read
// and any callback it did not provide reads back NULL) plus its user_data.
struct SourceEntry {
  IhsLocationSourceOps ops{};
  void* user_data = nullptr;
};
struct FilterEntry {
  IhsLocationFilterOps ops{};
  void* user_data = nullptr;
};

// Register under @key, replacing any prior entry. Returns false on a NULL/empty
// key, NULL @ops, or an @ops whose struct_size is too small to carry the
// callbacks the entry requires. Thread-safe.
bool RegisterSource(const std::string& key,
                    const IhsLocationSourceOps* ops,
                    void* user_data);
bool RegisterFilter(const std::string& key,
                    const IhsLocationFilterOps* ops,
                    void* user_data);

// Copy the entry for @key into @out. Returns false if @key is not registered.
// Thread-safe; @out is a value copy, so it stays valid across later
// (re-)registrations.
bool LookupSource(const std::string& key, SourceEntry& out);
bool LookupFilter(const std::string& key, FilterEntry& out);

}  // namespace ihs::location

#endif  // IHS_LOC_REGISTRY_H_
