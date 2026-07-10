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

#include "ihs/config.h"

#include "ihs_internal.hpp"

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace {

struct Value {
  enum class Type { kStr, kInt, kBool, kDouble };
  Type type = Type::kStr;
  std::string str;
  int64_t i = 0;
  double d = 0.0;
  bool b = false;
};

using Key = std::pair<int32_t, std::string>;
using ValueMap = std::map<Key, Value>;

}  // namespace

// Opaque to consumers. Reference-counted: the published "current" pointer holds
// one reference; every ihs_config_acquire adds one; ihs_config_release drops
// one and frees at zero, so a snapshot outlives a config replacement as long as
// a holder keeps it.
struct IhsConfigSnapshot {
  mutable std::atomic<int> refcount{1};
  uint64_t generation = 0;
  size_t view_count = 0;
  ValueMap values;
};

struct IhsConfigBuilder {
  size_t view_count = 0;
  ValueMap values;
};

namespace {

std::mutex g_mutex;
const IhsConfigSnapshot* g_current = nullptr;  // holds one reference
std::atomic<uint64_t> g_generation{0};

void drop_ref(const IhsConfigSnapshot* snapshot) {
  if (snapshot != nullptr &&
      snapshot->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    delete snapshot;
  }
}

const Value* find(const IhsConfigSnapshot* snapshot,
                  int32_t view,
                  const char* key) {
  if (snapshot == nullptr || key == nullptr) {
    return nullptr;
  }
  const auto it = snapshot->values.find(Key{view, std::string(key)});
  return it != snapshot->values.end() ? &it->second : nullptr;
}

void builder_set(IhsConfigBuilder* builder,
                 int32_t view,
                 const char* key,
                 Value value) {
  if (builder == nullptr || key == nullptr) {
    return;
  }
  builder->values[Key{view, std::string(key)}] = std::move(value);
}

}  // namespace

extern "C" const IhsConfigSnapshot* ihs_config_acquire(void) {
  const std::lock_guard<std::mutex> lock(g_mutex);
  if (g_current != nullptr) {
    g_current->refcount.fetch_add(1, std::memory_order_relaxed);
  }
  return g_current;
}

extern "C" void ihs_config_release(const IhsConfigSnapshot* snapshot) {
  drop_ref(snapshot);
}

extern "C" uint64_t ihs_config_generation(const IhsConfigSnapshot* snapshot) {
  return snapshot != nullptr ? snapshot->generation : 0;
}

extern "C" size_t ihs_config_view_count(const IhsConfigSnapshot* snapshot) {
  return snapshot != nullptr ? snapshot->view_count : 0;
}

extern "C" int ihs_config_get_str(const IhsConfigSnapshot* snapshot,
                                  int32_t view,
                                  const char* key,
                                  char* out,
                                  size_t cap,
                                  size_t* needed) {
  const Value* v = find(snapshot, view, key);
  if (v == nullptr || v->type != Value::Type::kStr) {
    return 0;
  }
  if (needed != nullptr) {
    *needed = v->str.size();
  }
  if (out != nullptr && cap > 0) {
    const size_t n = v->str.size() < cap - 1 ? v->str.size() : cap - 1;
    std::memcpy(out, v->str.data(), n);
    out[n] = '\0';
  }
  return 1;
}

extern "C" int ihs_config_get_int(const IhsConfigSnapshot* snapshot,
                                  int32_t view,
                                  const char* key,
                                  int64_t* out) {
  const Value* v = find(snapshot, view, key);
  if (v == nullptr || v->type != Value::Type::kInt) {
    return 0;
  }
  if (out != nullptr) {
    *out = v->i;
  }
  return 1;
}

extern "C" int ihs_config_get_bool(const IhsConfigSnapshot* snapshot,
                                   int32_t view,
                                   const char* key,
                                   int* out) {
  const Value* v = find(snapshot, view, key);
  if (v == nullptr || v->type != Value::Type::kBool) {
    return 0;
  }
  if (out != nullptr) {
    *out = v->b ? 1 : 0;
  }
  return 1;
}

extern "C" int ihs_config_get_double(const IhsConfigSnapshot* snapshot,
                                     int32_t view,
                                     const char* key,
                                     double* out) {
  const Value* v = find(snapshot, view, key);
  if (v == nullptr || v->type != Value::Type::kDouble) {
    return 0;
  }
  if (out != nullptr) {
    *out = v->d;
  }
  return 1;
}

extern "C" IhsConfigBuilder* ihs_config_builder_new(size_t view_count) {
  auto* b = new IhsConfigBuilder();
  b->view_count = view_count;
  return b;
}

extern "C" void ihs_config_builder_set_str(IhsConfigBuilder* builder,
                                           int32_t view,
                                           const char* key,
                                           const char* value) {
  Value v;
  v.type = Value::Type::kStr;
  v.str = value != nullptr ? value : "";
  builder_set(builder, view, key, std::move(v));
}

extern "C" void ihs_config_builder_set_int(IhsConfigBuilder* builder,
                                           int32_t view,
                                           const char* key,
                                           int64_t value) {
  Value v;
  v.type = Value::Type::kInt;
  v.i = value;
  builder_set(builder, view, key, std::move(v));
}

extern "C" void ihs_config_builder_set_bool(IhsConfigBuilder* builder,
                                            int32_t view,
                                            const char* key,
                                            int value) {
  Value v;
  v.type = Value::Type::kBool;
  v.b = value != 0;
  builder_set(builder, view, key, std::move(v));
}

extern "C" void ihs_config_builder_set_double(IhsConfigBuilder* builder,
                                              int32_t view,
                                              const char* key,
                                              double value) {
  Value v;
  v.type = Value::Type::kDouble;
  v.d = value;
  builder_set(builder, view, key, std::move(v));
}

extern "C" void ihs_config_builder_free(IhsConfigBuilder* builder) {
  delete builder;
}

extern "C" uint64_t ihs_config_publish(IhsConfigBuilder* builder) {
  if (builder == nullptr) {
    return 0;
  }
  auto* snapshot = new IhsConfigSnapshot();
  snapshot->generation =
      g_generation.fetch_add(1, std::memory_order_relaxed) + 1;
  snapshot->view_count = builder->view_count;
  snapshot->values = std::move(builder->values);
  delete builder;

  const IhsConfigSnapshot* previous = nullptr;
  {
    const std::lock_guard<std::mutex> lock(g_mutex);
    previous = g_current;
    g_current = snapshot;
  }
  drop_ref(previous);  // release the reference the old current held
  return snapshot->generation;
}

namespace ihs::config {

const IhsConfigApi* config_api() noexcept {
  static const IhsConfigApi api = {
      sizeof(IhsConfigApi),   &ihs_config_acquire,    &ihs_config_release,
      &ihs_config_generation, &ihs_config_view_count, &ihs_config_get_str,
      &ihs_config_get_int,    &ihs_config_get_bool,   &ihs_config_get_double,
  };
  return &api;
}

}  // namespace ihs::config
