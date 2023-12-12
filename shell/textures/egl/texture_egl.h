
#pragma once

#include <map>
#include <memory>

#include <flutter/encodable_value.h>

#include "../../view/flutter_view.h"
#include "../texture.h"

class App;

class Engine;

class FlutterView;

class WaylandEglBackend;

class TextureEgl {
 public:
  ~TextureEgl();

  /**
   * @brief Set FlutterView
   * @param[in] view pointer to FlutterView
   * @return void
   * @relation
   * flutter
   */
  void SetView(FlutterView* view);

  /**
   * @brief Set Engine
   * @param[in] engine pointer to Engine
   * @return void
   * @relation
   * flutter
   */
  void SetEngine(std::shared_ptr<Engine> engine);

  /**
   * @brief Get flutter OpenGL texture
   * @param[in] engine pointer to flutter engine
   * @param[in] texture_id texture id
   * @param[in] width Width
   * @param[in] height Height
   * @param[in] args from Dart
   * @return flutter::EncodableValue
   * @retval Callback to m_create_callback
   * @retval Error callback is not set
   * @relation
   * wayland, flutter
   */
  flutter::EncodableValue Create(
      Engine* engine,
      int64_t texture_id,
      int32_t width,
      int32_t height,
      const std::map<flutter::EncodableValue, flutter::EncodableValue>* args);

  /**
   * @brief Draw a new texture
   * @return void
   * @relation
   * wayland, flutter
   */
  void Draw() const;

  /**
   * @brief Run Task
   * @return void
   * @relation
   * wayland, flutter
   */
  void RunTask() const;

  /**
   * @brief Dispose Navigation Instance
   * @return void
   * @relation
   * wayland, flutter
   */
  void Dispose() const;

  /**
   * @brief Get instance of EglProcessResolver class
   * @return EglProcessResolver&
   * @retval Instance of the EglProcessResolver class
   * @relation
   * internal
   */
  static TextureEgl& GetInstance() {
    if (!sInstance) {
      sInstance = std::make_shared<TextureEgl>();
      sInstance->Initialize();
    }
    return *sInstance;
  }

 private:
#ifdef ENABLE_TEXTURE_TEST_EGL
  static constexpr int64_t kTextureEgl_ObjectId_Test = 5150;
#endif
#ifdef ENABLE_TEXTURE_NAVI_RENDER_EGL
  static constexpr uint32_t kTextureEgl_ObjectId_Navigation = 98765;
#endif

  std::unique_ptr<std::vector<std::unique_ptr<Texture>>> m_textures;

  FlutterView* m_flutter_view{};

  std::shared_ptr<Engine> m_engine;

  /**
   * @brief Initialize
   * @return void
   * @relation
   * internal
   */
  void Initialize();

 protected:
  static std::shared_ptr<TextureEgl> sInstance;
};
