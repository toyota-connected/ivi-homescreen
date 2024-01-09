/*
 * Copyright 2023-2024 Toyota Connected North America
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

#include "curl_client.h"

#include "../logging.h"

namespace plugin_common {

CurlClient::CurlClient() : mCode(CURLE_OK) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  std::make_unique<char>(CURL_ERROR_SIZE);
}

CurlClient::~CurlClient() {
  curl_easy_cleanup(mConn);
  mErrorBuffer.reset();
}

int CurlClient::StringWriter(char* data,
                             size_t size,
                             size_t num_mem_block,
                             std::string* writerData) {
  writerData->append(data, size * num_mem_block);
  return static_cast<int>(size * num_mem_block);
}

int CurlClient::VectorWriter(char* data,
                             size_t size,
                             size_t num_mem_block,
                             std::vector<uint8_t>* writerData) {
  auto* u_data = reinterpret_cast<uint8_t*>(data);
  writerData->insert(writerData->end(), u_data,
                     u_data + (size * num_mem_block));
  return static_cast<int>(size * num_mem_block);
}

bool CurlClient::Init(
    const std::string& url,
    const std::vector<std::string>& headers,
    const std::vector<std::pair<std::string, std::string>>& url_form,
    const bool follow_location) {
  mCode = CURLE_OK;

  mConn = curl_easy_init();
  if (mConn == nullptr) {
    spdlog::error("[CurlClient] Failed to create CURL connection");
    return false;
  }

  mCode = curl_easy_setopt(mConn, CURLOPT_ERRORBUFFER, mErrorBuffer.get());
  if (mCode != CURLE_OK) {
    spdlog::error("[CurlClient] Failed to set error buffer [{}]",
                  static_cast<int>(mCode));
    return false;
  }

  mUrl = url;
  spdlog::trace("[CurlClient] URL: {}", mUrl);

  mCode = curl_easy_setopt(mConn, CURLOPT_URL, mUrl.c_str());
  if (mCode != CURLE_OK) {
    spdlog::error("[CurlClient] Failed to set URL [{}]", mErrorBuffer.get());
    return false;
  }

  if (!url_form.empty()) {
    mPostFields.clear();
    int count = 0;
    for (auto& item : url_form) {
      if (count) {
        mPostFields += "&";
      }
      mPostFields.append(item.first + "=" + item.second);
      count++;
    }
    spdlog::trace("[CurlClient] PostFields: {}", mPostFields);
    curl_easy_setopt(mConn, CURLOPT_POSTFIELDSIZE, mPostFields.length());
    // libcurl does not copy
    curl_easy_setopt(mConn, CURLOPT_POSTFIELDS, mPostFields.c_str());
  }

  if (!headers.empty()) {
    struct curl_slist* curl_headers{};
    for (auto& header : headers) {
      spdlog::trace("[CurlClient] Header: {}", header);
      curl_headers = curl_slist_append(curl_headers, header.c_str());
    }
    mCode = curl_easy_setopt(mConn, CURLOPT_HTTPHEADER, curl_headers);
    if (mCode != CURLE_OK) {
      spdlog::error("[CurlClient] Failed to set headers option [{}]",
                    mErrorBuffer.get());
      return false;
    }
  }

  mCode = curl_easy_setopt(mConn, CURLOPT_FOLLOWLOCATION,
                           follow_location ? 1L : 0L);
  if (mCode != CURLE_OK) {
    spdlog::error("[CurlClient] Failed to set redirect option [{}]",
                  mErrorBuffer.get());
    return false;
  }

  return true;
}

std::string CurlClient::RetrieveContentAsString(bool verbose) {
  curl_easy_setopt(mConn, CURLOPT_VERBOSE, verbose ? 1L : 0L);
  if (mCode != CURLE_OK) {
    spdlog::error("[CurlClient] Failed to set 'CURLOPT_VERBOSE' [{}]\n",
                  mErrorBuffer.get());
    return {};
  }

  mCode = curl_easy_setopt(mConn, CURLOPT_WRITEFUNCTION, StringWriter);
  if (mCode != CURLE_OK) {
    spdlog::error("[CurlClient] Failed to set writer [{}]", mErrorBuffer.get());
    return {};
  }

  mCode = curl_easy_setopt(mConn, CURLOPT_WRITEDATA, &mStringBuffer);
  if (mCode != CURLE_OK) {
    spdlog::error("[CurlClient] Failed to set write data [{}]",
                  mErrorBuffer.get());
    return {};
  }

  mStringBuffer.clear();

  mCode = curl_easy_perform(mConn);
  if (mCode != CURLE_OK) {
    spdlog::error("[CurlClient] Failed to get '{}' [{}]\n", mUrl,
                  mErrorBuffer.get());
    return {};
  }
  return mStringBuffer;
}

const std::vector<uint8_t>& CurlClient::RetrieveContentAsVector(bool verbose) {
  curl_easy_setopt(mConn, CURLOPT_VERBOSE, verbose ? 1L : 0L);
  if (mCode != CURLE_OK) {
    spdlog::error("[CurlClient] Failed to set 'CURLOPT_VERBOSE' [{}]\n",
                  mErrorBuffer.get());
    mVectorBuffer.clear();
    return mVectorBuffer;
  }

  mCode = curl_easy_setopt(mConn, CURLOPT_WRITEFUNCTION, VectorWriter);
  if (mCode != CURLE_OK) {
    spdlog::error("[CurlClient] Failed to set writer [{}]", mErrorBuffer.get());
    mVectorBuffer.clear();
    return mVectorBuffer;
  }

  mCode = curl_easy_setopt(mConn, CURLOPT_WRITEDATA, &mVectorBuffer);
  if (mCode != CURLE_OK) {
    spdlog::error("[CurlClient] Failed to set write data [{}]",
                  mErrorBuffer.get());
    mVectorBuffer.clear();
    return mVectorBuffer;
  }

  std::vector<uint8_t>().swap(mVectorBuffer);

  mCode = curl_easy_perform(mConn);
  if (mCode != CURLE_OK) {
    spdlog::error("[CurlClient] Failed to get '{}' [{}]\n", mUrl,
                  mErrorBuffer.get());
    mVectorBuffer.clear();
    return mVectorBuffer;
  }
  return mVectorBuffer;
}
}  // namespace plugin_common