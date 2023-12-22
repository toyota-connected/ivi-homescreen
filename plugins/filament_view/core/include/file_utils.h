/*
 * Copyright 2020-2023 Toyota Connected North America
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

#include <filesystem>
#include <fstream>
#include <vector>
#include "logging/logging.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>
#include "logging/logging.h"

namespace plugin_filament_view {

inline std::filesystem::path getAbsolutePath(const std::string& dependent_path,
                                             const std::string& main_path) {
  std::filesystem::path path(main_path);
  path /= dependent_path;
  return path;
}

inline bool isValidFilePath(const std::filesystem::path& path) {
  if (path.empty() || !std::filesystem::exists(path)) {
    spdlog::error("[readAsset] invalid path: {}", path.c_str());
    return false;
  }
  return true;
}

inline std::vector<uint8_t> createBuffer(std::ifstream& file,
                                         const std::size_t size) {
  std::vector<uint8_t> buffer(size);
  if (!file.read(reinterpret_cast<char*>(buffer.data()),
                 static_cast<long>(buffer.size()))) {
    buffer.clear();
  }
  return buffer;
}

inline std::optional<std::ifstream> readFileContent(
    const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    spdlog::error("[{}] Failed to open", path.c_str());
    return {};
  }
  return file;
}

inline std::vector<uint8_t> returnErrorMessageAndBuffer(
    const std::filesystem::path& path,
    const std::string& message) {
  spdlog::error("[{}] {}", path.c_str(), message);
  return {};
}

inline std::vector<uint8_t> readBinaryFile(const std::string& dependent_path,
                                           const std::string& main_path) {
  const std::filesystem::path filePath =
      getAbsolutePath(dependent_path, main_path);

  if (!isValidFilePath(filePath)) {
    return returnErrorMessageAndBuffer(filePath, "Invalid path");
  }

  std::optional<std::ifstream> optionalFile = readFileContent(filePath);
  if (!optionalFile.has_value()) {
    return returnErrorMessageAndBuffer(filePath, "Failed to open");
  }

  std::ifstream& file = optionalFile.value();
  const auto end = file.tellg();
  file.seekg(0, std::ios::beg);
  const auto size = static_cast<std::size_t>(end - file.tellg());

  if (size == 0) {
    return returnErrorMessageAndBuffer(filePath, "Empty file");
  }

  std::vector<uint8_t> buffer = createBuffer(file, size);
  if (buffer.empty()) {
    return returnErrorMessageAndBuffer(filePath, "Failed to read");
  }

  return buffer;
}

}  // namespace plugin_filament_view
