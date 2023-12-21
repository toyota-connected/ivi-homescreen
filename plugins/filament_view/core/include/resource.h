
#pragma once

#include <optional>
#include <string>
#include <utility>

enum class Status {
  Success,
  Error,
  Loading
};

template <typename T>
class Resource {
 private:
  Status status;
  std::string message;
  std::optional<T> data;

 public:
  Resource(Status status, std::string message, std::optional<T> data = std::nullopt)
      : status(status), message(std::move(message)), data(data) {}

  static Resource Success(T data) {
    return Resource(Status::Success, "", data);
  }

  static Resource Error(std::string message) {
    return Resource(Status::Error, message);
  }

  [[nodiscard]] Status getStatus() const {
    return status;
  }

  [[nodiscard]] std::string getMessage() const {
    return message;
  }

  std::optional<T> getData() const {
    return data;
  }
};