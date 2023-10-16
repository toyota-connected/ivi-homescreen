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

#include <memory>
#include <sstream>
#include <vector>

#include <curl/curl.h>

class CurlClient {
 public:
  CurlClient();
  ~CurlClient();

  /**
   * @brief Initialization Function
   * @param headers vector of headers to use
   * @param url_form url form key/values
   * @param follow_location follows redirects from server.  Defaults to true
   * @return bool
   * @retval true if initialized, false if failed
   * @relation
   * google_sign_in
   */
  bool Init(std::string& url,
            std::vector<std::string>& headers,
            std::vector<std::pair<std::string, std::string>>& url_form,
            bool follow_location = true);

  /**
   * @brief Function to execute http client
   * @param verbose flag to enable stderr output of curl dialog
   * @return std::string
   * @retval body response of client request
   * @relation
   * google_sign_in
   */
  std::string RetrieveContent(bool verbose = false);

  /**
   * @brief Function to return last curl response code
   * @return CURLcode
   * @retval the last response code.  Used to check response state
   * @relation
   * google_sign_in
   */
  CURLcode GetCode() { return mCode; }

 private:
  CURL* mConn;
  CURLcode mCode;
  std::string mUrl;
  std::string mPostFields;
  std::unique_ptr<char> mErrorBuffer;
  std::string mBuffer;

  /**
   * @brief Callback function for curl client
   * @param data buffer of response
   * @param size length of buffer
   * @param num_mem_block number of memory blocks
   * @param writerData user pointer
   * @return int
   * @retval returns back to curl size of write
   * @relation
   * google_sign_in
   */
  static int Writer(char* data,
                    size_t size,
                    size_t num_mem_block,
                    std::string* writerData);
};
