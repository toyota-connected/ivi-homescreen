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

#include "task_runner.h"

#include "asio/post.hpp"

#include "logging/logging.h"

TaskRunner::TaskRunner(std::string name,
                       FlutterEngineProcTable& proc_table,
                       FlutterEngine& engine)
    : io_context_(std::make_unique<asio::io_context>(ASIO_CONCURRENCY_HINT_1)),
      work_(io_context_->get_executor()),
      name_(std::move(name)),
      engine_(engine),
      proc_table_(proc_table),
      pri_queue_(std::make_unique<handler_priority_queue>()),
      pthread_self_(pthread_self()),
      strand_(std::make_unique<asio::io_context::strand>(*io_context_)) {
  thread_ = std::thread([&]() {
    while (io_context_->run_one()) {
      // The custom invocation hook adds the handlers to the priority queue
      // rather than executing them from within the poll_one() call.
      while (io_context_->poll_one())
        ;

      pri_queue_->execute_all(proc_table_, engine_);
    }
  });

  asio::post(*strand_, [&]() {
    spdlog::debug("[0x{:x}] {} Task Runner", pthread_self(), name_);
  });
}

TaskRunner::~TaskRunner() {
  work_.reset();
  thread_.join();
  spdlog::debug("[0x{:x}] {} ~Task Runner", pthread_self(), name_);
}

void TaskRunner::QueueFlutterTask(size_t index,
                                  uint64_t target_time,
                                  FlutterTask task,
                                  void* /* context */) {
  SPDLOG_TRACE("({}) [{}] Task Queue {}", index, name_, task.task);
  auto current = proc_table_.GetCurrentTime();
  if (current >= target_time) {
    asio::post(*strand_, [&, index, task]() {
      SPDLOG_TRACE("({}) [{}] Task Run {}", index, name_, task.task);
      proc_table_.RunTask(engine_, &task);
    });
  } else {
    asio::post(*strand_, pri_queue_->wrap(target_time, [&, index, task]() {
      SPDLOG_TRACE("({}) [{}] Task Run {}", index, name_, task.task);
      proc_table_.RunTask(engine_, &task);
    }));
  }
}
