// Copyright 2023, the Chromium project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
#include "firebase_storage_plugin.h"

#include "firebase/app.h"
#include "firebase/future.h"
#include "firebase/storage.h"
#include "firebase/storage/controller.h"
#include "firebase/storage/listener.h"
#include "firebase/storage/metadata.h"
#include "firebase/storage/storage_reference.h"
#include "firebase_storage/plugin_version.h"
#include "messages.g.h"

#include <flutter/event_channel.h>
#include <flutter/plugin_registrar_homescreen.h>
#include <flutter/standard_method_codec.h>

#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <uuid/uuid.h>

using ::firebase::App;
using ::firebase::Future;
using ::firebase::storage::Controller;
using ::firebase::storage::Listener;
using ::firebase::storage::Metadata;
using ::firebase::storage::Storage;
using ::firebase::storage::StorageReference;

using flutter::EncodableValue;

namespace firebase_storage_linux {

static std::string kLibraryName = "flutter-fire-gcs";
static std::string kStorageMethodChannelName =
    "plugins.flutter.io/firebase_storage";
static std::string kStorageTaskEventName = "taskEvent";
// static
void FirebaseStoragePlugin::RegisterWithRegistrar(
    flutter::PluginRegistrar* registrar) {
  auto plugin = std::make_unique<FirebaseStoragePlugin>();
  messenger_ = registrar->messenger();
  FirebaseStorageHostApi::SetUp(registrar->messenger(), plugin.get());
  registrar->AddPlugin(std::move(plugin));
  // Register for platform logging
  App::RegisterLibrary(kLibraryName.c_str(),
                       firebase_storage_linux::getPluginVersion().c_str(),
                       nullptr);
}

FirebaseStoragePlugin::FirebaseStoragePlugin() = default;

FirebaseStoragePlugin::~FirebaseStoragePlugin() = default;

Storage* GetCPPStorageFromPigeon(const PigeonStorageFirebaseApp& pigeonApp,
                                 const std::string& bucket_path) {
  std::string default_url = std::string("gs://") + bucket_path;
  App* app = App::GetInstance(pigeonApp.app_name().c_str());
  Storage* cpp_storage = Storage::GetInstance(app, default_url.c_str());

  return cpp_storage;
}

StorageReference GetCPPStorageReferenceFromPigeon(
    const PigeonStorageFirebaseApp& pigeonApp,
    const PigeonStorageReference& pigeonReference) {
  Storage* cpp_storage =
      GetCPPStorageFromPigeon(pigeonApp, pigeonReference.bucket());
  return cpp_storage->GetReference(pigeonReference.full_path());
}

flutter::BinaryMessenger*
    firebase_storage_linux::FirebaseStoragePlugin::messenger_ = nullptr;
std::map<std::string,
         std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>>
    event_channels_;
std::map<std::string, std::unique_ptr<flutter::StreamHandler<>>>
    stream_handlers_;

std::string RegisterEventChannelWithUUID(
    const std::string& prefix,
    std::string uuid,
    std::unique_ptr<flutter::StreamHandler<flutter::EncodableValue>> handler) {
  std::string channelName = prefix + "/" + uuid;

  event_channels_[channelName] =
      std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
          FirebaseStoragePlugin::messenger_, channelName,
          &flutter::StandardMethodCodec::GetInstance());
  stream_handlers_[channelName] = std::move(handler);

  event_channels_[channelName]->SetStreamHandler(
      std::move(stream_handlers_[channelName]));

  return uuid;
}

std::string RegisterEventChannel(
    const std::string& prefix,
    std::unique_ptr<flutter::StreamHandler<EncodableValue>> handler) {
  uuid_t uuid;
  uuid_generate_random(uuid);
  char str[200];
  uuid_unparse(uuid, str);

  std::string channelName = prefix + "/" + str;

  event_channels_[channelName] =
      std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
          FirebaseStoragePlugin::messenger_, channelName,
          &flutter::StandardMethodCodec::GetInstance());
  stream_handlers_[channelName] = std::move(handler);

  event_channels_[channelName]->SetStreamHandler(
      std::move(stream_handlers_[channelName]));

  return str;
}

std::string FirebaseStoragePlugin::GetStorageErrorCode(Error storageError) {
  switch (storageError) {
    case firebase::storage::kErrorNone:
    case firebase::storage::kErrorUnknown:
      return "unknown";
    case firebase::storage::kErrorObjectNotFound:
      return "object-not-found";
    case firebase::storage::kErrorBucketNotFound:
      return "bucket-not-found";
    case firebase::storage::kErrorProjectNotFound:
      return "project-not-found";
    case firebase::storage::kErrorQuotaExceeded:
      return "quota-exceeded";
    case firebase::storage::kErrorUnauthenticated:
      return "unauthenticated";
    case firebase::storage::kErrorUnauthorized:
      return "unauthorized";
    case firebase::storage::kErrorRetryLimitExceeded:
      return "retry-limit-exceeded";
    case firebase::storage::kErrorNonMatchingChecksum:
      return "invalid-checksum";
    case firebase::storage::kErrorDownloadSizeExceeded:
      return "download-size-exceeded";
    case firebase::storage::kErrorCancelled:
      return "canceled";

    default:
      return "unknown";
  }
}

std::string FirebaseStoragePlugin::GetStorageErrorMessage(Error storageError) {
  switch (storageError) {
    case firebase::storage::kErrorNone:
    case firebase::storage::kErrorUnknown:
      return "An unknown error occurred";
    case firebase::storage::kErrorObjectNotFound:
      return "No object exists at the desired reference.";
    case firebase::storage::kErrorBucketNotFound:
      return "No bucket is configured for Firebase Storage.";
    case firebase::storage::kErrorProjectNotFound:
      return "No project is configured for Firebase Storage.";
    case firebase::storage::kErrorQuotaExceeded:
      return "Quota on your Firebase Storage bucket has been exceeded.";
    case firebase::storage::kErrorUnauthenticated:
      return "User is unauthenticated. Authenticate and try again.";
    case firebase::storage::kErrorUnauthorized:
      return "User is not authorized to perform the desired action.";
    case firebase::storage::kErrorRetryLimitExceeded:
      return "The maximum time limit on an operation (upload, download, "
             "delete, etc.) has been exceeded.";
    case firebase::storage::kErrorNonMatchingChecksum:
      return "File on the client does not match the checksum of the file "
             "received by the server.";
    case firebase::storage::kErrorDownloadSizeExceeded:
      return "Size of the downloaded file exceeds the amount of memory "
             "allocated for the download.";
    case firebase::storage::kErrorCancelled:
      return "User cancelled the operation.";

    default:
      return "An unknown error occurred";
  }
}

FlutterError FirebaseStoragePlugin::ParseError(
    const firebase::FutureBase& completed_future) {
  const auto errorCode = static_cast<const Error>(completed_future.error());

  return FlutterError(FirebaseStoragePlugin::GetStorageErrorCode(errorCode),
                      FirebaseStoragePlugin::GetStorageErrorMessage(errorCode));
}

void FirebaseStoragePlugin::GetReferencebyPath(
    const PigeonStorageFirebaseApp& app,
    const std::string& path,
    const std::string* bucket,
    std::function<void(ErrorOr<PigeonStorageReference> reply)> result) {
  Storage* cpp_storage = GetCPPStorageFromPigeon(app, *bucket);
  StorageReference cpp_reference = cpp_storage->GetReference(path);
  auto* value_ptr = new PigeonStorageReference(
      cpp_reference.bucket(), cpp_reference.full_path(), cpp_reference.name());
  result(ErrorOr<PigeonStorageReference>(*value_ptr));
}

void FirebaseStoragePlugin::SetMaxOperationRetryTime(
    const PigeonStorageFirebaseApp& app,
    int64_t time,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {
  Storage* cpp_storage = GetCPPStorageFromPigeon(app, "");
  cpp_storage->set_max_operation_retry_time((double)time);
}

void FirebaseStoragePlugin::SetMaxUploadRetryTime(
    const PigeonStorageFirebaseApp& app,
    int64_t time,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {
  Storage* cpp_storage = GetCPPStorageFromPigeon(app, "");
  cpp_storage->set_max_upload_retry_time((double)time);
}

void FirebaseStoragePlugin::SetMaxDownloadRetryTime(
    const PigeonStorageFirebaseApp& app,
    int64_t time,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {
  Storage* cpp_storage = GetCPPStorageFromPigeon(app, "");
  cpp_storage->set_max_download_retry_time((double)time);
}

void FirebaseStoragePlugin::UseStorageEmulator(
    const PigeonStorageFirebaseApp& /* app */,
    const std::string& /* host */,
    int64_t /* port */,
    std::function<void(std::optional<FlutterError> reply)> /* result */) {
  // C++ doesn't support emulator on desktop for now. Do nothing.
}

void FirebaseStoragePlugin::ReferenceDelete(
    const PigeonStorageFirebaseApp& app,
    const PigeonStorageReference& reference,
    std::function<void(std::optional<FlutterError> reply)> result) {
  StorageReference cpp_reference =
      GetCPPStorageReferenceFromPigeon(app, reference);
  Future<void> future_result = cpp_reference.Delete();
  std::this_thread::sleep_for(
      std::chrono::seconds(1));  // timing for c++ sdk grabbing a mutex
  future_result.OnCompletion([result](const Future<void>& void_result) {
    if (void_result.error() == firebase::storage::kErrorNone) {
      result(std::nullopt);
    } else {
      result(FirebaseStoragePlugin::ParseError(void_result));
    }
  });
}
void FirebaseStoragePlugin::ReferenceGetDownloadURL(
    const PigeonStorageFirebaseApp& app,
    const PigeonStorageReference& reference,
    std::function<void(ErrorOr<std::string> reply)> result) {
  StorageReference cpp_reference =
      GetCPPStorageReferenceFromPigeon(app, reference);
  Future<std::string> future_result = cpp_reference.GetDownloadUrl();
  std::this_thread::sleep_for(
      std::chrono::seconds(1));  // timing for c++ sdk grabbing a mutex
  future_result.OnCompletion(
      [result](const Future<std::string>& string_result) {
        if (string_result.error() == firebase::storage::kErrorNone) {
          result(ErrorOr<std::string>(*string_result.result()));
        } else {
          result(ErrorOr<std::string>(
              FirebaseStoragePlugin::ParseError(string_result)));
        }
      });
}

std::string kCacheControlName = "cacheControl";
std::string kContentDispositionName = "contentDisposition";
std::string kContentEncodingName = "contentEncoding";
std::string kContentLanguageName = "contentLanguage";
std::string kContentTypeName = "contentType";
std::string kCustomMetadataName = "metadata";
std::string kSizeName = "size";

flutter::EncodableMap ConvertMedadataToPigeon(const Metadata* meta) {
  flutter::EncodableMap meta_map = flutter::EncodableMap();
  if (meta->cache_control() != nullptr) {
    meta_map[flutter::EncodableValue(kCacheControlName)] =
        flutter::EncodableValue(meta->cache_control());
  }
  if (meta->content_disposition() != nullptr) {
    meta_map[flutter::EncodableValue(kContentDispositionName)] =
        flutter::EncodableValue(meta->content_disposition());
  }
  if (meta->content_encoding() != nullptr) {
    meta_map[flutter::EncodableValue(kContentEncodingName)] =
        flutter::EncodableValue(meta->content_encoding());
  }
  if (meta->content_language() != nullptr) {
    meta_map[flutter::EncodableValue(kContentLanguageName)] =
        flutter::EncodableValue(meta->content_language());
  }
  if (meta->content_type() != nullptr) {
    meta_map[flutter::EncodableValue(kContentTypeName)] =
        flutter::EncodableValue(meta->content_type());
  }
  meta_map[flutter::EncodableValue(kSizeName)] =
      flutter::EncodableValue(meta->size_bytes());
  if (meta->custom_metadata() != nullptr) {
    flutter::EncodableMap custom_meta_map = flutter::EncodableMap();
    for (const auto& kv : *meta->custom_metadata()) {
      custom_meta_map[flutter::EncodableValue(kv.first)] =
          flutter::EncodableValue(kv.second);
    }
    meta_map[flutter::EncodableValue(kCustomMetadataName)] = custom_meta_map;
  }
  return meta_map;
}

void GetMetadataFromPigeon(const PigeonSettableMetadata& pigeonMetadata,
                           Metadata* out_metadata) {
  if (pigeonMetadata.cache_control() != nullptr) {
    out_metadata->set_cache_control(pigeonMetadata.cache_control()->c_str());
  }
  if (pigeonMetadata.content_disposition() != nullptr) {
    out_metadata->set_content_disposition(
        pigeonMetadata.content_disposition()->c_str());
  }
  if (pigeonMetadata.content_encoding() != nullptr) {
    out_metadata->set_content_encoding(
        pigeonMetadata.content_encoding()->c_str());
  }
  if (pigeonMetadata.content_language() != nullptr) {
    out_metadata->set_content_language(
        pigeonMetadata.content_language()->c_str());
  }
  if (pigeonMetadata.content_type() != nullptr) {
    out_metadata->set_content_type(pigeonMetadata.content_type()->c_str());
  }
  if (pigeonMetadata.custom_metadata() != nullptr) {
    for (const auto& kv : *pigeonMetadata.custom_metadata()) {
      const std::string* key_name = std::get_if<std::string>(&kv.first);
      const std::string* value = std::get_if<std::string>(&kv.second);
      (*out_metadata->custom_metadata())[*key_name] = *value;
    }
  }
}

void FirebaseStoragePlugin::ReferenceGetMetaData(
    const PigeonStorageFirebaseApp& app,
    const PigeonStorageReference& reference,
    std::function<void(ErrorOr<PigeonFullMetaData> reply)> result) {
  StorageReference cpp_reference =
      GetCPPStorageReferenceFromPigeon(app, reference);
  Future<Metadata> future_result = cpp_reference.GetMetadata();
  std::this_thread::sleep_for(
      std::chrono::seconds(1));  // timing for c++ sdk grabbing a mutex
  future_result.OnCompletion([result](const Future<Metadata>& metadata_result) {
    if (metadata_result.error() == firebase::storage::kErrorNone) {
      PigeonFullMetaData pigeon_meta = PigeonFullMetaData();
      pigeon_meta.set_metadata(
          ConvertMedadataToPigeon(metadata_result.result()));

      result(ErrorOr<PigeonFullMetaData>(pigeon_meta));
    } else {
      result(ErrorOr<PigeonFullMetaData>(
          FirebaseStoragePlugin::ParseError(metadata_result)));
    }
  });
}

void FirebaseStoragePlugin::ReferenceList(
    const PigeonStorageFirebaseApp& /* app */,
    const PigeonStorageReference& /* reference */,
    const PigeonListOptions& /* options */,
    std::function<void(ErrorOr<PigeonListResult> reply)> result) {
  // C++ doesn't support list yet
  flutter::EncodableList items = flutter::EncodableList();
  flutter::EncodableList prefixs = flutter::EncodableList();
  PigeonListResult pigeon_result = PigeonListResult(items, prefixs);
  result(ErrorOr<PigeonListResult>(pigeon_result));
}

void FirebaseStoragePlugin::ReferenceListAll(
    const PigeonStorageFirebaseApp& /* app */,
    const PigeonStorageReference& /* reference */,
    std::function<void(ErrorOr<PigeonListResult> reply)> result) {
  // C++ doesn't support listAll yet
  flutter::EncodableList items = flutter::EncodableList();
  flutter::EncodableList prefixs = flutter::EncodableList();
  PigeonListResult pigeon_result = PigeonListResult(items, prefixs);
  result(ErrorOr<PigeonListResult>(pigeon_result));
}

void FirebaseStoragePlugin::ReferenceGetData(
    const PigeonStorageFirebaseApp& app,
    const PigeonStorageReference& reference,
    int64_t max_size,
    std::function<void(ErrorOr<std::optional<std::vector<uint8_t>>> reply)>
        result) {
  StorageReference cpp_reference =
      GetCPPStorageReferenceFromPigeon(app, reference);
  const size_t kMaxAllowedSize = 1 * 1024 * 1024;

  // Use a shared pointer for automatic memory management and copy ability
  auto byte_buffer = std::make_shared<std::vector<uint8_t>>(kMaxAllowedSize);

  Future<size_t> future_result = cpp_reference.GetBytes(
      byte_buffer->data(), static_cast<size_t>(max_size));
  std::this_thread::sleep_for(
      std::chrono::seconds(1));  // timing for c++ sdk grabbing a mutex
  future_result.OnCompletion(
      [result, byte_buffer](const Future<size_t>& data_result) {
        if (data_result.error() == firebase::storage::kErrorNone) {
          size_t vector_size = *data_result.result();
          std::optional<std::vector<uint8_t>> vector_buffer;
          vector_buffer = std::vector<uint8_t>(
              byte_buffer->begin(), byte_buffer->begin() + vector_size);
          result(ErrorOr<std::optional<std::vector<uint8_t>>>(vector_buffer));
        } else {
          result(ErrorOr<std::optional<std::vector<uint8_t>>>(
              FirebaseStoragePlugin::ParseError(data_result)));
        }
      });
}

std::string kTaskStateName = "taskState";
std::string kTaskAppName = "appName";
std::string kTaskSnapshotName = "snapshot";
std::string kTaskSnapshotPath = "path";
std::string kTaskSnapshotBytesTransferred = "bytesTransferred";
std::string kTaskSnapshotTotalBytes = "totalBytes";

class TaskStateListener : public Listener {
 public:
  explicit TaskStateListener(
      flutter::EventSink<flutter::EncodableValue>* events) {
    events_ = events;
  }
  void OnProgress(firebase::storage::Controller* controller) override {
    // A progress event occurred
    // TODO error handling

    flutter::EncodableMap event = flutter::EncodableMap();
    event[EncodableValue(kTaskStateName)] =
        static_cast<int>(PigeonStorageTaskState::running);
    event[EncodableValue(kTaskAppName)] =
        controller->GetReference().storage()->app()->name();
    flutter::EncodableMap snapshot = flutter::EncodableMap();
    snapshot[EncodableValue(kTaskSnapshotPath)] =
        controller->GetReference().full_path();
    snapshot[EncodableValue(kTaskSnapshotTotalBytes)] =
        controller->total_byte_count();
    snapshot[EncodableValue(kTaskSnapshotBytesTransferred)] =
        controller->bytes_transferred();
    event[EncodableValue(kTaskSnapshotName)] = snapshot;

    events_->Success(EncodableValue(event));
  }

  void OnPaused(firebase::storage::Controller* controller) override {
    // A progress event occurred
    // TODO error handling
    flutter::EncodableMap event = flutter::EncodableMap();
    event[EncodableValue(kTaskStateName)] =
        static_cast<int>(PigeonStorageTaskState::paused);
    event[EncodableValue(kTaskAppName)] =
        controller->GetReference().storage()->app()->name();
    flutter::EncodableMap snapshot = flutter::EncodableMap();
    snapshot[EncodableValue(kTaskSnapshotPath)] =
        controller->GetReference().full_path();
    snapshot[EncodableValue(kTaskSnapshotTotalBytes)] =
        controller->total_byte_count();
    snapshot[EncodableValue(kTaskSnapshotBytesTransferred)] =
        controller->bytes_transferred();
    event[EncodableValue(kTaskSnapshotName)] = snapshot;

    events_->Success(EncodableValue(event));
  }

  flutter::EventSink<flutter::EncodableValue>* events_;
};

class PutDataStreamHandler
    : public flutter::StreamHandler<flutter::EncodableValue> {
 public:
  PutDataStreamHandler(Storage* storage,
                       std::string reference_path,
                       const void* data,
                       size_t buffer_size,
                       Controller* controller) {
    storage_ = storage;
    reference_path_ = std::move(reference_path);
    data_ = data;
    buffer_size_ = buffer_size;
    controller_ = controller;
  }

  std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>>
  OnListenInternal(
      const flutter::EncodableValue* /* arguments */,
      std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&& events)
      override {
    events_ = std::move(events);

    TaskStateListener putStringListener = TaskStateListener(events_.get());
    StorageReference reference = storage_->GetReference(reference_path_);
    Future<Metadata> future_result = reference.PutBytes(
        data_, buffer_size_, &putStringListener, controller_);
    std::this_thread::sleep_for(
        std::chrono::seconds(1));  // timing for c++ sdk grabbing a mutex

    future_result.OnCompletion([this](const Future<Metadata>& data_result) {
      if (data_result.error() == firebase::storage::kErrorNone) {
        flutter::EncodableMap event = flutter::EncodableMap();
        event[EncodableValue(kTaskStateName)] =
            static_cast<int>(PigeonStorageTaskState::success);
        event[EncodableValue(kTaskAppName)] =
            std::string(storage_->app()->name());
        flutter::EncodableMap snapshot = flutter::EncodableMap();
        snapshot[EncodableValue(kTaskSnapshotPath)] =
            data_result.result()->path();
        snapshot[EncodableValue(kTaskSnapshotTotalBytes)] =
            data_result.result()->size_bytes();
        snapshot[EncodableValue(kTaskSnapshotBytesTransferred)] =
            data_result.result()->size_bytes();
        snapshot[EncodableValue(kCustomMetadataName)] =
            ConvertMedadataToPigeon(data_result.result());
        event[EncodableValue(kTaskSnapshotName)] = snapshot;

        events_->Success(EncodableValue(event));
      } else {
        const auto errorCode = static_cast<const Error>(data_result.error());
        events_->Error(
            FirebaseStoragePlugin::GetStorageErrorCode(errorCode),
            FirebaseStoragePlugin::GetStorageErrorMessage(errorCode));
      }
    });
    return nullptr;
  }

  std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>>
  OnCancelInternal(const flutter::EncodableValue* /* arguments */) override {
    return nullptr;
  }

 private:
  Storage* storage_;
  std::string reference_path_;
  const void* data_;
  size_t buffer_size_;
  Controller* controller_;
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> events_{};
};

class PutFileStreamHandler
    : public flutter::StreamHandler<flutter::EncodableValue> {
 public:
  PutFileStreamHandler(Storage* storage,
                       std::string reference_path,
                       std::string file_path,
                       Controller* controller) {
    storage_ = storage;
    reference_path_ = std::move(reference_path);
    file_path_ = std::move(file_path);
    controller_ = controller;
  }

  std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>>
  OnListenInternal(
      const flutter::EncodableValue* /* arguments */,
      std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&& events)
      override {
    events_ = std::move(events);

    TaskStateListener putFileListener = TaskStateListener(events_.get());
    StorageReference reference = storage_->GetReference(reference_path_);
    Future<Metadata> future_result =
        reference.PutFile(file_path_.c_str(), &putFileListener, controller_);

    std::this_thread::sleep_for(
        std::chrono::seconds(1));  // timing for c++ sdk grabbing a mutex
    future_result.OnCompletion([this](const Future<Metadata>& data_result) {
      if (data_result.error() == firebase::storage::kErrorNone) {
        flutter::EncodableMap event = flutter::EncodableMap();
        event[EncodableValue(kTaskStateName)] =
            static_cast<int>(PigeonStorageTaskState::success);
        event[EncodableValue(kTaskAppName)] =
            std::string(storage_->app()->name());
        flutter::EncodableMap snapshot = flutter::EncodableMap();
        snapshot[EncodableValue(kTaskSnapshotPath)] =
            data_result.result()->path();
        snapshot[EncodableValue(kTaskSnapshotTotalBytes)] =
            data_result.result()->size_bytes();
        snapshot[EncodableValue(kTaskSnapshotBytesTransferred)] =
            data_result.result()->size_bytes();
        snapshot[EncodableValue(kCustomMetadataName)] =
            ConvertMedadataToPigeon(data_result.result());
        event[EncodableValue(kTaskSnapshotName)] = snapshot;

        events_->Success(EncodableValue(event));
      } else {
        const auto errorCode = static_cast<const Error>(data_result.error());
        events_->Error(
            FirebaseStoragePlugin::GetStorageErrorCode(errorCode),
            FirebaseStoragePlugin::GetStorageErrorMessage(errorCode));
      }
    });
    return nullptr;
  }

  std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>>
  OnCancelInternal(const flutter::EncodableValue* /* arguments */) override {
    return nullptr;
  }

 private:
  Storage* storage_;
  std::string reference_path_;
  std::string file_path_;
  Controller* controller_;
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> events_{};
};

class GetFileStreamHandler
    : public flutter::StreamHandler<flutter::EncodableValue> {
 public:
  GetFileStreamHandler(Storage* storage,
                       std::string reference_path,
                       std::string file_path,
                       Controller* controller) {
    storage_ = storage;
    reference_path_ = std::move(reference_path);
    file_path_ = std::move(file_path);
    controller_ = controller;
  }

  std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>>
  OnListenInternal(
      const flutter::EncodableValue* /* arguments */,
      std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&& events)
      override {
    events_ = std::move(events);
    std::unique_lock<std::mutex> lock(mtx_);

    TaskStateListener getFileListener = TaskStateListener(events_.get());
    StorageReference reference = storage_->GetReference(reference_path_);
    Future<size_t> future_result =
        reference.GetFile(file_path_.c_str(), &getFileListener, controller_);

    std::this_thread::sleep_for(
        std::chrono::seconds(1));  // timing for c++ sdk grabbing a mutex
    future_result.OnCompletion([this](const Future<size_t>& data_result) {
      if (data_result.error() == firebase::storage::kErrorNone) {
        flutter::EncodableMap event = flutter::EncodableMap();
        event[EncodableValue(kTaskStateName)] =
            static_cast<int>(PigeonStorageTaskState::success);
        event[EncodableValue(kTaskAppName)] =
            std::string(storage_->app()->name());
        flutter::EncodableMap snapshot = flutter::EncodableMap();
        size_t data_size = *data_result.result();
        snapshot[EncodableValue(kTaskSnapshotTotalBytes)] =
            flutter::EncodableValue(static_cast<int64_t>(data_size));
        snapshot[EncodableValue(kTaskSnapshotBytesTransferred)] =
            flutter::EncodableValue(static_cast<int64_t>(data_size));
        event[EncodableValue(kTaskSnapshotName)] = snapshot;

        events_->Success(EncodableValue(event));
      } else {
        const auto errorCode = static_cast<const Error>(data_result.error());
        events_->Error(
            FirebaseStoragePlugin::GetStorageErrorCode(errorCode),
            FirebaseStoragePlugin::GetStorageErrorMessage(errorCode));
      }
    });
    return nullptr;
  }

  std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>>
  OnCancelInternal(const flutter::EncodableValue* /* arguments */) override {
    std::unique_lock<std::mutex> lock(mtx_);

    return nullptr;
  }

 public:
  Storage* storage_;
  std::string reference_path_;
  std::string file_path_;
  Controller* controller_;
  std::mutex mtx_;
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> events_ =
      nullptr;
};

void FirebaseStoragePlugin::ReferencePutData(
    const PigeonStorageFirebaseApp& pigeon_app,
    const PigeonStorageReference& pigeon_reference,
    const std::vector<uint8_t>& data,
    const PigeonSettableMetadata& /* pigeon_meta_data */,
    uint64_t handle,
    std::function<void(ErrorOr<std::string> reply)> result) {
  Storage* cpp_storage =
      GetCPPStorageFromPigeon(pigeon_app, pigeon_reference.bucket());
  controllers_[handle] = std::make_unique<Controller>();

  auto handler = std::make_unique<PutDataStreamHandler>(
      cpp_storage, pigeon_reference.full_path(), &data, data.size(),
      controllers_[handle].get());

  std::string channelName = RegisterEventChannel(
      kStorageMethodChannelName + "/" + kStorageTaskEventName,
      std::move(handler));

  result(ErrorOr<std::string>(channelName));
}

void FirebaseStoragePlugin::ReferencePutString(
    const PigeonStorageFirebaseApp& pigeon_app,
    const PigeonStorageReference& pigeon_reference,
    const std::string& data,
    int64_t /* format */,
    const PigeonSettableMetadata& /* settable_meta_data */,
    uint64_t handle,
    std::function<void(ErrorOr<std::string> reply)> result) {
  Storage* cpp_storage =
      GetCPPStorageFromPigeon(pigeon_app, pigeon_reference.bucket());
  controllers_[handle] = std::make_unique<Controller>();

  auto handler = std::make_unique<PutDataStreamHandler>(
      cpp_storage, pigeon_reference.full_path(), &data, data.size(),
      controllers_[handle].get());

  std::string channelName = RegisterEventChannel(
      kStorageMethodChannelName + "/" + kStorageTaskEventName,
      std::move(handler));

  result(ErrorOr<std::string>(channelName));
}

void FirebaseStoragePlugin::ReferencePutFile(
    const PigeonStorageFirebaseApp& pigeon_app,
    const PigeonStorageReference& pigeon_reference,
    const std::string& file_path,
    const PigeonSettableMetadata& /* settable_meta_data */,
    uint64_t handle,
    std::function<void(ErrorOr<std::string> reply)> result) {
  Storage* cpp_storage =
      GetCPPStorageFromPigeon(pigeon_app, pigeon_reference.bucket());
  controllers_[handle] = std::make_unique<Controller>();

  auto handler = std::make_unique<PutFileStreamHandler>(
      cpp_storage, pigeon_reference.full_path(), file_path,
      controllers_[handle].get());

  std::string channelName = RegisterEventChannel(
      kStorageMethodChannelName + "/" + kStorageTaskEventName,
      std::move(handler));

  result(ErrorOr<std::string>(channelName));
}

void FirebaseStoragePlugin::ReferenceDownloadFile(
    const PigeonStorageFirebaseApp& pigeon_app,
    const PigeonStorageReference& pigeon_reference,
    const std::string& file_path,
    uint64_t handle,
    std::function<void(ErrorOr<std::string> reply)> result) {
  Storage* cpp_storage =
      GetCPPStorageFromPigeon(pigeon_app, pigeon_reference.bucket());
  controllers_[handle] = std::make_unique<Controller>();

  auto handler = std::make_unique<GetFileStreamHandler>(
      cpp_storage, pigeon_reference.full_path(), file_path,
      controllers_[handle].get());

  std::string channelName = RegisterEventChannel(
      kStorageMethodChannelName + "/" + kStorageTaskEventName,
      std::move(handler));

  result(ErrorOr<std::string>(channelName));
}

void FirebaseStoragePlugin::ReferenceUpdateMetadata(
    const PigeonStorageFirebaseApp& app,
    const PigeonStorageReference& reference,
    const PigeonSettableMetadata& metadata,
    std::function<void(ErrorOr<PigeonFullMetaData> reply)> result) {
  StorageReference cpp_reference =
      GetCPPStorageReferenceFromPigeon(app, reference);
  Metadata cpp_meta;
  GetMetadataFromPigeon(metadata, &cpp_meta);

  const Future<Metadata> future_result = cpp_reference.UpdateMetadata(cpp_meta);
  std::this_thread::sleep_for(
      std::chrono::seconds(1));  // timing for c++ sdk grabbing a mutex
  future_result.OnCompletion([result](const Future<Metadata>& data_result) {
    if (data_result.error() == firebase::storage::kErrorNone) {
      const Metadata* result_meta = data_result.result();
      PigeonFullMetaData pigeonData;
      pigeonData.set_metadata(ConvertMedadataToPigeon(result_meta));

      result(ErrorOr<PigeonFullMetaData>(pigeonData));
    } else {
      result(ErrorOr<PigeonFullMetaData>(
          FirebaseStoragePlugin::ParseError(data_result)));
    }
  });
}

void FirebaseStoragePlugin::TaskPause(
    const PigeonStorageFirebaseApp& /* app */,
    uint64_t handle,
    std::function<void(ErrorOr<flutter::EncodableMap> reply)> result) {
  bool status = controllers_[handle]->Pause();
  flutter::EncodableMap task_result = flutter::EncodableMap();
  flutter::EncodableMap task_data = flutter::EncodableMap();
  task_result[EncodableValue("status")] = status;
  task_data[EncodableValue("bytesTransferred")] =
      controllers_[handle]->bytes_transferred();
  task_data[EncodableValue("totalBytes")] =
      controllers_[handle]->total_byte_count();
  task_result[EncodableValue("snapshot")] = task_data;
  result(ErrorOr<flutter::EncodableMap>(task_result));
}

void FirebaseStoragePlugin::TaskResume(
    const PigeonStorageFirebaseApp& /* app */,
    uint64_t handle,
    std::function<void(ErrorOr<flutter::EncodableMap> reply)> result) {
  bool status = controllers_[handle]->Resume();
  flutter::EncodableMap task_result = flutter::EncodableMap();
  flutter::EncodableMap task_data = flutter::EncodableMap();
  task_result[EncodableValue("status")] = status;
  task_data[EncodableValue("bytesTransferred")] =
      controllers_[handle]->bytes_transferred();
  task_data[EncodableValue("totalBytes")] =
      controllers_[handle]->total_byte_count();
  task_result[EncodableValue("snapshot")] = task_data;
  result(ErrorOr<flutter::EncodableMap>(task_result));
}

void FirebaseStoragePlugin::TaskCancel(
    const PigeonStorageFirebaseApp& /* app */,
    uint64_t handle,
    std::function<void(ErrorOr<flutter::EncodableMap> reply)> result) {
  bool status = controllers_[handle]->Cancel();
  flutter::EncodableMap task_result = flutter::EncodableMap();
  flutter::EncodableMap task_data = flutter::EncodableMap();
  task_result[EncodableValue("status")] = status;
  task_data[EncodableValue("bytesTransferred")] =
      controllers_[handle]->bytes_transferred();
  task_data[EncodableValue("totalBytes")] =
      controllers_[handle]->total_byte_count();
  task_result[EncodableValue("snapshot")] = task_data;
  result(ErrorOr<flutter::EncodableMap>(task_result));
}

}  // namespace firebase_storage_linux
