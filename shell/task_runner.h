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

#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"
#include "asio/io_context_strand.hpp"

#include "third_party/flutter/shell/platform/embedder/embedder.h"

#include "constants.h"
#include "handler_priority_queue.h"

class TaskRunner {
 public:
  explicit TaskRunner(std::string name,
                      FlutterEngineProcTable& proc_table,
                      FlutterEngine& engine);

  ~TaskRunner();

  static pthread_t GetThreadId() { return pthread_self(); };

  NODISCARD bool IsThreadEqual(pthread_t threadid) const {
    return pthread_equal(threadid, pthread_self_) != 0;
  };

  void QueueFlutterTask(size_t index,
                        uint64_t target_time,
                        FlutterTask task,
                        void* context);

  void QueuePlatformMessage(
      const char* channel,
      std::unique_ptr<std::vector<uint8_t>> message);

  std::string GetName() { return name_; }

 private:
  std::string name_;
  FlutterEngineProcTable& proc_table_;
  FlutterEngine& engine_;
  std::thread thread_;
  pthread_t pthread_self_;
  std::unique_ptr<asio::io_context> io_context_;
  asio::executor_work_guard<decltype(io_context_->get_executor())> work_;
  std::unique_ptr<asio::io_context::strand> strand_;
  std::unique_ptr<handler_priority_queue> pri_queue_;
};
