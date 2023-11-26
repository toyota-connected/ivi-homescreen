
#include "texture_egl.h"

#include <memory>
#include <utility>

std::shared_ptr<TextureEgl> TextureEgl::sInstance = nullptr;

TextureEgl::~TextureEgl() {
  SPDLOG_DEBUG("~TextureEgl");
}

void TextureEgl::Initialize() {
  m_textures = std::make_unique<std::vector<std::unique_ptr<Texture>>>();
}

void TextureEgl::SetView(FlutterView* view) {
  m_flutter_view = view;
}

void TextureEgl::SetEngine(std::shared_ptr<Engine> engine) {
  m_engine = std::move(engine);
};

flutter::EncodableValue TextureEgl::Create(
    Engine* engine,
    int64_t texture_id,
    int32_t width,
    int32_t height,
    const std::map<flutter::EncodableValue, flutter::EncodableValue>* args) {
  switch (texture_id) {
#ifdef ENABLE_TEXTURE_TEST_EGL
    case kTextureEgl_ObjectId_Test: {
      m_textures->push_back(std::make_unique<TextureTestEgl>(m_flutter_view));
      m_textures->back()->SetEngine(engine);
      return m_textures->back()->Create(width, height, args);
    }
#endif
#ifdef ENABLE_TEXTURE_NAVI_RENDER_EGL
    case kTextureEgl_ObjectId_Navigation: {
      m_textures->push_back(
          std::make_unique<TextureNaviRender>(m_flutter_view));
      m_textures->back()->SetEngine(engine);
      return m_textures->back()->Create(width, height, args);
    }
#endif
#if !defined(ENABLE_TEXTURE_TEST_EGL) || !defined(ENABLE_TEXTURE_NAVI_RENDER_EGL)
      (void)engine;
      (void)texture_id;
      (void)width;
      (void)height;
      (void)args;
#endif
    default: {
      return flutter::EncodableValue(flutter::EncodableMap{
          {flutter::EncodableValue("result"), flutter::EncodableValue(-1)},
          {flutter::EncodableValue("error"),
           flutter::EncodableValue("Not implemented")}});
    }
  }
}

void TextureEgl::Dispose() const {
  for (auto& item : *m_textures) {
    switch (item->GetId()) {
#ifdef ENABLE_TEXTURE_TEST_EGL
      case kTextureEgl_ObjectId_Test: {
        TextureTestEgl::Dispose(item.get(), static_cast<GLuint>(item->GetId()));
        break;
      }
#endif
#ifdef ENABLE_TEXTURE_NAVI_RENDER_EGL
      case kTextureEgl_ObjectId_Navigation: {
        TextureNaviRender::Dispose(item.get(),
                                   static_cast<GLuint>(item->GetId()));
        break;
      }
#endif
      default:
        break;
    }
  }
}

void TextureEgl::Draw() const {
  for (auto& item : *m_textures) {
    switch (item->GetId()) {
#ifdef ENABLE_TEXTURE_TEST_EGL
      case kTextureEgl_ObjectId_Test: {
        TextureTestEgl::Draw(item.get());
        break;
      }
#endif
#ifdef ENABLE_TEXTURE_NAVI_RENDER_EGL
      case kTextureEgl_ObjectId_Navigation: {
        TextureNaviRender::Draw(item.get());
        break;
      }
#endif
      default: {
        break;
      }
    }
  }
}

void TextureEgl::RunTask() const {
#ifdef ENABLE_TEXTURE_NAVI_RENDER_EGL
  for (auto& item : *m_textures) {
    if (item->GetId() == kTextureEgl_ObjectId_Navigation) {
      TextureNaviRender::RunTask(item.get());
    }
  }
#endif
}
