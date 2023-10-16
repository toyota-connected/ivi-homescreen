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

#include "json_utils.h"

#include <filesystem>
#include <fstream>

#include <rapidjson/filewritestream.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include "logging.h"

rapidjson::Document JsonUtils::GetJsonDocumentFromFile(std::string& path,
                                                       bool missing_is_error) {
  rapidjson::Document d{};
  if (std::filesystem::exists(path)) {
    std::ifstream ifs{path};
    if (!ifs.is_open()) {
      if (missing_is_error) {
        d.Parse("{}");
        spdlog::error("Failed to open file for reading: {}", path);
      }
      return std::move(d);
    }
    rapidjson::IStreamWrapper isw{ifs};
    d.ParseStream(isw);
  } else {
    d.SetObject();
    if (missing_is_error) {
      spdlog::error("File missing: {}", path);
    }
  }
  return std::move(d);
}

bool JsonUtils::WriteJsonDocumentToFile(std::string& path,
                                        rapidjson::Document& doc) {
  if (path.empty()) {
    spdlog::error("Missing File Path: {}", path);
    return false;
  }

  if (!std::filesystem::exists(path)) {
    std::filesystem::path p(path);
    std::filesystem::create_directories(p.parent_path());
  }

  FILE* fp = fopen(path.c_str(), "w+");
  if (fp == nullptr) {
    spdlog::error("Failed to open file: {}", path);
    return false;
  }

  const auto bufSize = 1024;
  auto buffer = std::make_unique<char[]>(bufSize);
  rapidjson::FileWriteStream os(fp, buffer.get(), bufSize);
  rapidjson::PrettyWriter<rapidjson::FileWriteStream> writer(os);
  doc.Accept(writer);

  fclose(fp);
  buffer.reset();

  return true;
}

bool JsonUtils::AddEmptyKeyToFile(std::string& path, const char* key) {
  auto d = JsonUtils::GetJsonDocumentFromFile(path, false);
  auto& allocator = d.GetAllocator();
  auto obj = d.GetObject();
  if (obj.HasMember(key)) {
    obj[key] = "";
  } else {
    rapidjson::Value k(key, allocator);
    rapidjson::Value v("", allocator);
    obj.AddMember(k, v, allocator);
  }

  // flush to disk
  return WriteJsonDocumentToFile(path, d);
}
