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

TaskRunner::TaskRunner(std::string name, FlutterEngine& engine)
    : io_context_(std::make_unique<asio::io_context>(ASIO_CONCURRENCY_HINT_1)),
      work_(io_context_->get_executor()),
      name_(std::move(name)),
      engine_(engine),
      pri_queue_(std::make_unique<handler_priority_queue>()),
      pthread_self_(pthread_self()),
      strand_(std::make_unique<asio::io_context::strand>(*io_context_)) {
  thread_ = std::thread([&]() {
    while (io_context_->run_one()) {
      // The custom invocation hook adds the handlers to the priority queue
      // rather than executing them from within the poll_one() call.
      while (io_context_->poll_one())
        ;

      pri_queue_->execute_all(engine_);
    }
  });

  asio::post(*strand_, [&]() {
    spdlog::debug("{} Task Runner, thread_id=0x{:x}", name_, pthread_self());
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
  auto current = LibFlutterEngine->GetCurrentTime();
  if (current >= target_time) {
    post(*strand_, [&, index, task]() {
#if defined(NDEBUG)
      (void)index;
#endif
      SPDLOG_TRACE("({}) [{}] Task Run {}", index, name_, task.task);
      LibFlutterEngine->RunTask(engine_, &task);
    });
  } else {
    asio::post(*strand_, pri_queue_->wrap(target_time, [&, index, task]() {
#if defined(NDEBUG)
      (void)index;
#endif
      SPDLOG_TRACE("({}) [{}] Task Run {}", index, name_, task.task);
      LibFlutterEngine->RunTask(engine_, &task);
    }));
  }
}

std::future<FlutterEngineResult> TaskRunner::QueuePlatformMessage(
    const char* channel,
    std::unique_ptr<std::vector<uint8_t>> message,
    FlutterPlatformMessageResponseHandle* handle) const {
  auto promise(std::make_unique<std::promise<FlutterEngineResult>>());
  auto future(promise->get_future());

  post(*strand_, [channel, message = std::move(message), handle,
                  promise = std::move(promise), engine = engine_]() {
    const FlutterPlatformMessage msg{
        sizeof(FlutterPlatformMessage),
        channel,
        message->data(),
        message->size(),
        handle,
    };

    promise->set_value(LibFlutterEngine->SendPlatformMessage(engine, &msg));
  });

  return future;
}

std::future<FlutterEngineResult> TaskRunner::QueueUpdateLocales(
    std::vector<const FlutterLocale*> locales) const {
  auto promise(std::make_unique<std::promise<FlutterEngineResult>>());
  auto future(promise->get_future());
  post(*strand_,
       [promise = std::move(promise), locales = std::move(locales),
        UpdateLocales = LibFlutterEngine->UpdateLocales, engine = engine_]() {
         std::vector l(locales.data(), locales.data() + locales.size());
         const FlutterEngineResult result =
             UpdateLocales(engine, l.data(), l.size());
         promise->set_value(result);
       });

  return future;
}
