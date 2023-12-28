#include <fstream>
#include <iostream>

#include <camutils/Manipulator.h>
#include <filamat/MaterialBuilder.h>
#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/LightManager.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/View.h>
#include <filament/Viewport.h>

#include <utils/EntityManager.h>

#include <math/norm.h>

#include <gltfio/AssetLoader.h>
#include <gltfio/ResourceLoader.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL_video.h>

using CameraManipulator = ::filament::camutils::Manipulator<float>;

/// passed into Filament backend.  Requires static
static struct {
  struct wl_display* display;
  struct wl_surface* surface;
  uint32_t width;
  uint32_t height;
} native_window;

volatile uint32_t gWidth, gHeight;

/**
 * @brief Callback function for handling resize events.
 *
 * This function is called when an SDL_WINDOWEVENT_RESIZED event is triggered
 * for the specified window. It retrieves the new window size and updates
 * the global variables gWidth and gHeight accordingly.
 *
 * @param data A pointer to user-defined data (in this case, the SDL_Window*).
 * @param event A pointer to the SDL_Event structure containing the event data.
 * @return Returns 0 on success, or a negative value on failure.
 */
static int OnResize(void* data, SDL_Event* event) {
  if (event->type == SDL_WINDOWEVENT &&
      event->window.event == SDL_WINDOWEVENT_RESIZED) {
    SDL_Window* win = SDL_GetWindowFromID(event->window.windowID);
    if (win == (SDL_Window*)data) {
      SDL_GetWindowSizeInPixels(win, (int*)&gWidth, (int*)&gHeight);
    }
  }
  return 0;
}

/**
 * @brief Retrieve the native window handle associated with an SDL window.
 *
 * This function returns the native window handle for a given SDL window.
 * The native window handle is specific to the underlying windowing subsystem.
 *
 * @param sdlWindow A pointer to the SDL window for which to retrieve the native
 * window handle.
 * @return A void pointer to the native window handle. The type of the pointer
 * depends on the underlying windowing subsystem. If the native window handle
 * cannot be determined, nullptr is returned.
 */
void* getNativeWindow(SDL_Window* sdlWindow) {
  SDL_SysWMinfo wmi;
  SDL_VERSION(&wmi.version);
  SDL_GetWindowWMInfo(sdlWindow, &wmi);
  if (wmi.subsystem == SDL_SYSWM_X11) {
    auto win = (Window)wmi.info.x11.window;
    return (void*)win;
  } else if (wmi.subsystem == SDL_SYSWM_WAYLAND) {
    // The default Wayland swap chain extent is {0,0}
    // hence having to set the size
    SDL_GetWindowSizeInPixels(sdlWindow, (int*)&gWidth, (int*)&gHeight);
    native_window = {wmi.info.wl.display, wmi.info.wl.surface, gWidth, gHeight};
    return (void*)&native_window;
  } else {
    std::cout << "Unknown SDL subsystem";
    return nullptr;
  }
}

/**
 * @brief Creates an SDL window with specified properties.
 *
 * @return SDL_Window* The created window. Returns nullptr in case of failure.
 */
SDL_Window* createWindow() {
  const uint32_t windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN |
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
  SDL_Window* win =
      SDL_CreateWindow("Hello Filament!", 100, 100, 600, 400, windowFlags);
  if (win == nullptr) {
    std::cout << "SDL_CreateWindow Error: " << SDL_GetError() << std::endl;
    SDL_Quit();
    return nullptr;
  }
  SDL_AddEventWatch(OnResize, win);

  return win;
}

struct {
  filament::Engine* engine{};
  filament::SwapChain* swapChain{};
  filament::Renderer* renderer{};
  filament::Scene* scene{};
  filament::View* view{};
  filament::Skybox* skybox{};

  filament::Camera* camera{};
  CameraManipulator* cameraManipulator{};

  utils::Entity cameraEntity;
  utils::Entity light;

  struct {
    utils::Entity renderable;
    filament::IndexBuffer* indexBuffer{};
    filament::VertexBuffer* vertexBuffer{};
    filament::MaterialInstance* materialInstance{};
    filament::Material* material{};
  } quad;
} gContext;

/**
 * Adds a quad to the scene.
 * This function creates a quad mesh, assigns vertex and index buffers,
 * creates a material, sets default material parameters, and adds the
 * quad to the scene.
 */
void addQuadToScene() {
  using namespace filament;
  using namespace math;

  static constexpr uint32_t indices[] = {0, 1, 2, 2, 3, 0};

  static constexpr math::float3 vertices[] = {
      {-10, 0, -10},
      {-10, 0, 10},
      {10, 0, 10},
      {10, 0, -10},
  };

  short4 tbn = math::packSnorm16(
      mat3f::packTangentFrame(math::mat3f{float3{1.0f, 0.0f, 0.0f},
                                          float3{0.0f, 0.0f, 1.0f},
                                          float3{0.0f, 1.0f, 0.0f}})
          .xyzw);

  const static math::short4 normals[]{tbn, tbn, tbn, tbn};

  auto vertexBuffer = gContext.quad.vertexBuffer =
      VertexBuffer::Builder()
          .vertexCount(4)
          .bufferCount(2)
          .attribute(VertexAttribute::POSITION, 0,
                     VertexBuffer::AttributeType::FLOAT3)
          .attribute(VertexAttribute::TANGENTS, 1,
                     VertexBuffer::AttributeType::SHORT4)
          .normalized(VertexAttribute::TANGENTS)
          .build(*gContext.engine);

  vertexBuffer->setBufferAt(
      *gContext.engine, 0,
      VertexBuffer::BufferDescriptor(
          vertices, vertexBuffer->getVertexCount() * sizeof(vertices[0])));
  vertexBuffer->setBufferAt(
      *gContext.engine, 1,
      VertexBuffer::BufferDescriptor(
          normals, vertexBuffer->getVertexCount() * sizeof(normals[0])));

  auto indexBuffer = gContext.quad.indexBuffer =
      IndexBuffer::Builder().indexCount(6).build(*gContext.engine);

  indexBuffer->setBuffer(
      *gContext.engine,
      IndexBuffer::BufferDescriptor(
          indices, indexBuffer->getIndexCount() * sizeof(uint32_t)));

  filamat::MaterialBuilder::init();
  filamat::MaterialBuilder builder;
  builder.name("Some material")
      .material(
          "    void material(inout MaterialInputs material) {\n"
          "        prepareMaterial(material);\n"
          "        material.baseColor.rgb = materialParams.baseColor;\n"
          "        material.metallic = materialParams.metallic;\n"
          "        material.roughness = materialParams.roughness;\n"
          "        material.reflectance = materialParams.reflectance;\n"
          "    }")
      .parameter("baseColor", filament::backend::UniformType::FLOAT3)
      .parameter("metallic", filament::backend::UniformType::FLOAT)
      .parameter("roughness", filament::backend::UniformType::FLOAT)
      .parameter("reflectance", filament::backend::UniformType::FLOAT)
      .shading(filamat::MaterialBuilder::Shading::LIT)
      .targetApi(filamat::MaterialBuilder::TargetApi::ALL)
      .platform(filamat::MaterialBuilder::Platform::ALL);

  filamat::Package package = builder.build(gContext.engine->getJobSystem());

  gContext.quad.material = Material::Builder()
                               .package(package.getData(), package.getSize())
                               .build(*gContext.engine);
  gContext.quad.material->setDefaultParameter("baseColor", RgbType::LINEAR,
                                              float3{0, 1, 0});
  gContext.quad.material->setDefaultParameter("metallic", 0.0f);
  gContext.quad.material->setDefaultParameter("roughness", 0.4f);
  gContext.quad.material->setDefaultParameter("reflectance", 0.5f);

  gContext.quad.materialInstance = gContext.quad.material->createInstance();

  gContext.quad.renderable = utils::EntityManager::get().create();
  RenderableManager::Builder(1)
      .boundingBox({{-1, -1, -1}, {1, 1, 1}})
      .material(0, gContext.quad.materialInstance)
      .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, vertexBuffer,
                indexBuffer, 0, 6)
      .culling(false)
      .build(*gContext.engine, gContext.quad.renderable);
  gContext.scene->addEntity(gContext.quad.renderable);
}

/**
 * @brief Set the default camera for the scene.
 *
 * This function creates and configures a default camera for the scene. It sets
 * the camera's exposure parameters, creates a camera manipulator, and sets the
 * camera's viewport, look-at position, projection, and assigns it to the view.
 */
void setDefaultCamera() {
  using namespace filament;
  using namespace math;

  static constexpr float kAperture = 16.0f;
  static constexpr float kShutterSpeed = 1.0f / 125;
  static constexpr float kSensitivity = 100.0f;

  auto viewport = gContext.view->getViewport();

  /// Create camera
  gContext.cameraEntity = utils::EntityManager::get().create();
  gContext.camera = gContext.engine->createCamera(gContext.cameraEntity);

  /// With the default parameters, the scene must contain at least one Light of
  /// intensity similar to the sun (e.g.: a 100,000 lux directional light).
  gContext.camera->setExposure(kAperture, kShutterSpeed, kSensitivity);

  /// Create camera manipulator
  gContext.cameraManipulator = CameraManipulator::Builder()
                                   .viewport(static_cast<int>(viewport.width),
                                             static_cast<int>(viewport.height))
                                   .orbitHomePosition(0, 50.5f, 0)  // eye
                                   .targetPosition(0, 0, 0)         // center
                                   .upVector(1.f, 0, 0)             // up
                                   .zoomSpeed(0.05f)
                                   .orbitSpeed(0.075f, 0.075f)
                                   .viewport(static_cast<int>(viewport.width),
                                             static_cast<int>(viewport.height))
                                   .build(::filament::camutils::Mode::ORBIT);

  filament::math::float3 eye, center, up;
  gContext.cameraManipulator->getLookAt(&eye, &center, &up);
  gContext.camera->lookAt(eye, center, up);
  gContext.camera->setProjection(
      60,
      static_cast<float>(viewport.width) / static_cast<float>(viewport.height),
      0.1, 10);

  gContext.view->setCamera(gContext.camera);
}

/**
 * @brief Initializes the SDL video subsystem and creates a window.
 *
 * It creates a SDL window using SDL_CreateWindow function and sets the window
 * flags. The window is resizable and supports Vulkan rendering.
 *
 * @return A pointer to the created SDL window, or nullptr on error.
 */
int main() {
  SDL_Init(SDL_INIT_VIDEO);

  using namespace filament;
  using namespace math;
  using namespace utils;

  SDL_Window* window = createWindow();

  /// Core
  gContext.engine = Engine::create(filament::backend::Backend::VULKAN);
  gContext.swapChain =
      gContext.engine->createSwapChain(getNativeWindow(window));
  gContext.renderer = gContext.engine->createRenderer();

  /// Scene
  gContext.scene = gContext.engine->createScene();

  /// View
  gContext.view = gContext.engine->createView();
  gContext.view->setViewport({0, 0, gWidth, gHeight});
  gContext.view->setScene(gContext.scene);

  /// Camera - requires valid view
  setDefaultCamera();

  /// Skybox
  gContext.skybox =
      Skybox::Builder().color({0.5, 0.5, 0.5, 1.0}).build(*gContext.engine);
  gContext.scene->setSkybox(gContext.skybox);

  /// Direct Lighting
  gContext.light = EntityManager::get().create();
  LightManager::Builder(LightManager::Type::SUN)
      .color(Color::toLinear<ACCURATE>(sRGBColor(0.98f, 0.92f, 0.89f)))
      .intensity(110000)
      .direction({0.7, -1, -0.8})
      .sunAngularRadius(1.9f)
      .castShadows(true)
      .build(*gContext.engine, gContext.light);

  gContext.scene->addEntity(gContext.light);

  /// Build a quad
  // addQuadToScene();

  /// Display loop
  volatile uint32_t prev_width = gWidth;
  volatile uint32_t prev_height = gHeight;
  bool keep_window_open = true;
  while (keep_window_open) {
    SDL_Event e;
    while (SDL_PollEvent(&e) > 0) {
      switch (e.type) {
        case SDL_QUIT:
          keep_window_open = false;
          break;
        case SDL_MOUSEBUTTONDOWN:
          gContext.cameraManipulator->grabBegin(e.button.x, e.button.y, false);
          break;
        case SDL_MOUSEMOTION:
          gContext.cameraManipulator->grabUpdate(e.motion.x, e.motion.y);
          break;
        case SDL_MOUSEBUTTONUP:
          gContext.cameraManipulator->grabEnd();
          break;
        case SDL_MOUSEWHEEL:
          gContext.cameraManipulator->scroll(e.wheel.mouseX, e.wheel.mouseY,
                                             e.wheel.preciseY * 20.0f);
          break;
      }

      if (prev_width != gWidth || prev_height != gHeight) {
        prev_width = gWidth;
        prev_height = gHeight;
        gContext.engine->flushAndWait();
        gContext.engine->destroy(gContext.swapChain);
        gContext.swapChain =
            gContext.engine->createSwapChain(getNativeWindow(window));
        gContext.view->setViewport({0, 0, gWidth, gHeight});
        gContext.cameraManipulator->setViewport((int)gWidth, (int)gHeight);
        gContext.camera->setProjection(
            60, static_cast<float>(gWidth) / static_cast<float>(gHeight), 0.1,
            10);
      }

      float3 eye, center, up;
      gContext.cameraManipulator->getLookAt(&eye, &center, &up);
      gContext.camera->lookAt(eye, center, up);

      // beginFrame() returns false if we need to skip a frame
      if (gContext.renderer->beginFrame(gContext.swapChain)) {
        // for each View
        gContext.renderer->render(gContext.view);
        gContext.renderer->endFrame();
      }
      SDL_UpdateWindowSurface(window);
      SDL_Delay(16);
    }
  }

  gContext.engine->destroy(gContext.scene);
  gContext.engine->destroy(gContext.view);
  gContext.engine->destroyCameraComponent(gContext.cameraEntity);
  delete gContext.cameraManipulator;
  gContext.engine->destroy(gContext.light);
  gContext.engine->destroy(gContext.skybox);
  gContext.engine->destroy(gContext.quad.renderable);
  gContext.engine->destroy(gContext.quad.indexBuffer);
  gContext.engine->destroy(gContext.quad.vertexBuffer);
  gContext.engine->destroy(gContext.quad.materialInstance);
  gContext.engine->destroy(gContext.quad.material);
  gContext.engine->destroy(gContext.renderer);
  gContext.engine->destroy(gContext.swapChain);
  Engine::destroy(gContext.engine);

  SDL_Quit();
  return 0;
}
