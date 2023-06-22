/*
 * Copyright 2023 Toyota Connected North America
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

#include "dlt/libdlt.h"
#include "flutter/fml/macros.h"

class Dlt {
 public:
  /**
   * @brief Check if DLT is supported on the current platform.
   * @return boot
   * @retval true if DLT is supported, false otherwise.
   * @relation
   * dlt
   */
  static bool IsSupported();

  /**
   * @brief Register DLT context.
   * @return boot
   * @retval true if DLT is registered, false otherwise.
   * @relation
   * dlt
   */
  static bool Register();

  /**
   * @brief Unregister DLT context.
   * @return boot
   * @retval true if DLT is unregistered, false otherwise.
   * @relation
   * dlt
   */
  static bool Unregister();

  /**
   * @brief Convert a specified string to DLT log string.
   * @param[in] log_level DLT log level.
   * @param[in] buff String to be converted.
   * @return void
   * @relation
   * dlt
   */
  static void LogString(DltLogLevelType log_level, const char* buff);

 private:
  FML_DISALLOW_COPY_AND_ASSIGN(Dlt);
};
