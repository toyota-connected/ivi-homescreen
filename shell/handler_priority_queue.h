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

#include <queue>

#include "asio/bind_executor.hpp"

#include "flutter/shell/platform/embedder/embedder.h"

#include "libflutter_engine.h"
#include "logging/logging.h"

class handler_priority_queue : public asio::execution_context {
 public:
  template <typename Function>
  void add(uint64_t priority, Function function) {
    std::unique_ptr<queued_handler_base> handler(
        new queued_handler<Function>(priority, std::move(function)));

    handlers_.push(std::move(handler));
  }

  void execute_all(FlutterEngine& /* engine */) {
    while (!handlers_.empty()) {
      const auto current = LibFlutterEngine->GetCurrentTime();
      const auto target_time = handlers_.top()->GetTimestamp();
      if (current >= target_time) {
        handlers_.top()->execute();
        handlers_.pop();
      } else {
        spdlog::debug("Task Pending Delta: {}", target_time - current);
        break;
      }
    }
  }

  class executor {
   public:
    executor(handler_priority_queue& q, uint64_t t)
        : context_(q), timestamp_(t) {}

    NODISCARD handler_priority_queue& context() const noexcept {
      return context_;
    }

    template <typename Function, typename Allocator>
    void dispatch(Function f, const Allocator&) const {
      context_.add(timestamp_, std::move(f));
    }

    template <typename Function, typename Allocator>
    void post(Function f, const Allocator&) const {
      context_.add(timestamp_, std::move(f));
    }

    template <typename Function, typename Allocator>
    void defer(Function f, const Allocator&) const {
      context_.add(timestamp_, std::move(f));
    }

    void on_work_started() const noexcept {}
    void on_work_finished() const noexcept {}

    bool operator==(const executor& other) const noexcept {
      return &context_ == &other.context_ && timestamp_ == other.timestamp_;
    }

    bool operator!=(const executor& other) const noexcept {
      return !operator==(other);
    }

   private:
    handler_priority_queue& context_;
    uint64_t timestamp_;
  };

  template <typename Handler>
  asio::executor_binder<Handler, executor> wrap(uint64_t timestamp,
                                                Handler handler) {
    return asio::bind_executor(executor(*this, timestamp), std::move(handler));
  }

 private:
  class queued_handler_base {
   public:
    explicit queued_handler_base(uint64_t t) : timestamp_(t) {}

    virtual ~queued_handler_base() = default;

    virtual void execute() = 0;

    NODISCARD uint64_t GetTimestamp() const { return timestamp_; }

    friend bool operator<(
        const std::unique_ptr<queued_handler_base>& a,
        const std::unique_ptr<queued_handler_base>& b) noexcept {
      return a->timestamp_ >= b->timestamp_;
    }

   private:
    uint64_t timestamp_;
  };

  template <typename Function>
  class queued_handler : public queued_handler_base {
   public:
    queued_handler(uint64_t t, Function f)
        : queued_handler_base(t), function_(std::move(f)) {}

    void execute() override { function_(); }

   private:
    Function function_;
  };

  std::priority_queue<std::unique_ptr<queued_handler_base>> handlers_;
};
