
#pragma once

#include <flutter/basic_message_channel.h>
#include <flutter/binary_messenger.h>
#include <flutter/encodable_value.h>
#include <flutter/standard_method_codec.h>

#include <map>
#include <optional>
#include <string>

namespace nav_render_view_plugin {

class FlutterError {
 public:
  explicit FlutterError(std::string code) : code_(std::move(code)) {}

  explicit FlutterError(std::string code, std::string message)
      : code_(std::move(code)), message_(std::move(message)) {}

  explicit FlutterError(std::string code,
                        std::string message,
                        flutter::EncodableValue details)
      : code_(std::move(code)),
        message_(std::move(message)),
        details_(std::move(details)) {}

  const std::string& code() const { return code_; }

  const std::string& message() const { return message_; }

  const flutter::EncodableValue& details() const { return details_; }

 private:
  std::string code_;
  std::string message_;
  flutter::EncodableValue details_;
};

template <class T>
class ErrorOr {
 public:
  ErrorOr(const T& rhs) : v_(rhs) {}

  ErrorOr(const T&& rhs) : v_(std::move(rhs)) {}

  ErrorOr(const FlutterError& rhs) : v_(rhs) {}

  ErrorOr(const FlutterError&& rhs) : v_(rhs) {}

  bool has_error() const { return std::holds_alternative<FlutterError>(v_); }

  const T& value() const { return std::get<T>(v_); };

  const FlutterError& error() const { return std::get<FlutterError>(v_); };

 private:
  ErrorOr() = default;

  T TakeValue() && { return std::get<T>(std::move(v_)); }

  std::variant<T, FlutterError> v_;
};

}  // namespace navi_render_api
