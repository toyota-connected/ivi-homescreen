/*
 * Copyright 2020 Toyota Connected North America
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

#pragma once

#include <libsecret/secret.h>

namespace secret {

class FHashTable {
  GHashTable* m_hashTable;

 public:
  FHashTable() { m_hashTable = g_hash_table_new(g_str_hash, nullptr); }

  /**
  * @brief Get GHashTable
  * @return GHashTable*
  * @retval Pointer to GHashTable
  * @relation
  * flutter
  */
  GHashTable* getGHashTable() { return m_hashTable; }

  /**
  * @brief Insert a new key and value into GHashTable
  * @param[in] key A key to insert
  * @param[in] value The value to associate with the key
  * @return bool
  * @retval true If the key did not exist yet
  * @retval false If the key already exist
  * @relation
  * flutter
  */
  bool insert(const char* key, const char* value) {
    return g_hash_table_insert(m_hashTable, (void*)key, (void*)value);
  }

  /**
  * @brief Look up a key in GHashTable
  * @param[in] key A key to look up
  * @return char*
  * @retval The associated value, or NULL if the key is not found
  * @relation
  * flutter
  */
  const char* get(const char* key) {
    return (const char*)g_hash_table_lookup(m_hashTable, (void*)key);
  }

  /**
  * @brief Check if key is in GHashTable
  * @param[in] key A key to check
  * @return bool
  * @retval true If key is in GHashTable
  * @retval false Otherwise
  * @relation
  * flutter
  */
  bool contains(const char* key) {
    return g_hash_table_contains(m_hashTable, (void*)key);
  }

  ~FHashTable() { g_hash_table_destroy(m_hashTable); }
};

}  // namespace secret
