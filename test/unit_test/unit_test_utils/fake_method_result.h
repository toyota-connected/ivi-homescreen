/*
 * Copyright 2026 Toyota Connected North America
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
#include <optional>
#include <string>

#include "flutter/shell/platform/common/client_wrapper/include/flutter/method_result.h"

// A minimal hand-rolled flutter::MethodResult<T> that records the outcome of
// a HandleMethodCall invocation so unit tests can assert on it without
// bringing up a real messenger round-trip.
//
// Usage:
//   auto result =
//   std::make_unique<FakeMethodResult<flutter::EncodableValue>>(); auto* raw =
//   result.get(); handler.HandleMethodCall(call, std::move(result));
//   EXPECT_TRUE(raw->succeeded());
template <typename T>
class FakeMethodResult : public flutter::MethodResult<T> {
 public:
  enum class Outcome { kNone, kSuccess, kError, kNotImplemented };

  [[nodiscard]] Outcome outcome() const { return outcome_; }
  [[nodiscard]] bool succeeded() const { return outcome_ == Outcome::kSuccess; }
  [[nodiscard]] bool errored() const { return outcome_ == Outcome::kError; }
  [[nodiscard]] bool not_implemented() const {
    return outcome_ == Outcome::kNotImplemented;
  }

  [[nodiscard]] const std::string& error_code() const { return error_code_; }
  [[nodiscard]] const std::string& error_message() const {
    return error_message_;
  }
  [[nodiscard]] const std::optional<T>& success_value() const {
    return success_value_;
  }

 protected:
  void SuccessInternal(const T* result) override {
    outcome_ = Outcome::kSuccess;
    if (result) {
      success_value_ = *result;
    }
  }

  void ErrorInternal(const std::string& error_code,
                     const std::string& error_message,
                     const T* /* error_details */) override {
    outcome_ = Outcome::kError;
    error_code_ = error_code;
    error_message_ = error_message;
  }

  void NotImplementedInternal() override {
    outcome_ = Outcome::kNotImplemented;
  }

 private:
  Outcome outcome_{Outcome::kNone};
  std::string error_code_;
  std::string error_message_;
  std::optional<T> success_value_;
};
