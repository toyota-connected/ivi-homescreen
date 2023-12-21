
#pragma once

#include <future>

#include <asio/io_context_strand.hpp>

#include "viewer/custom_model_viewer.h"

namespace plugin_filament_view {

class CustomModelViewer;

class MaterialLoader {
 public:
  MaterialLoader(CustomModelViewer* modelViewer, const std::string& assetPath);
  ~MaterialLoader() = default;

  ::filament::Material* loadMaterialFromAsset(const std::string& path);

  ::filament::Material* loadMaterialFromUrl(const std::string& url);

  // Disallow copy and assign.
  MaterialLoader(const MaterialLoader&) = delete;
  MaterialLoader& operator=(const MaterialLoader&) = delete;

 private:
  CustomModelViewer* modelViewer_;
  const std::string& assetPath_;
  ::filament::Engine* engine_;
  const asio::io_context::strand& strand_;
};
}  // namespace plugin_filament_view