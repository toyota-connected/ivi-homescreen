#include <fstream>
#include <iostream>

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

static struct {
  struct wl_display* display;
  struct wl_surface* surface;
  uint32_t width;
  uint32_t height;
} native_window;

volatile uint32_t gWidth, gHeight;

/**
 * @brief Event handler for window resize events
 *
 * @param data The user data associated with the window
 * @param event The SDL event being handled
 * @return int Returns 0 on success, a negative error code on failure
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
 * @brief Retrieves the native window handle from an SDL window object.
 *
 * This function returns the native window handle of an SDL window object,
 * depending on the underlying window system.
 *
 * @param sdlWindow Pointer to the SDL window object.
 * @return A void pointer to the native window handle.
 *     - For X11 window system, the native window handle is a `Window` object.
 *     - For Wayland window system, the native window handle is stored in a
 * `native_window` struct which contains the Wayland display, surface, width,
 * and height.
 *     - For other window systems, `nullptr` is returned and an error message is
 * printed to the console.
 *
 * @note The `gWidth` and `gHeight` variables are defined elsewhere and are used
 * to store the window size when using the Wayland window system.
 * @note The `native_window` struct is defined elsewhere and is used to store
 * the native window handle for the Wayland window system.
 * @note It is the caller's responsibility to ensure that the return value is
 * properly cast to the appropriate type based on the window system being used.
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
    // requires static
    native_window = {wmi.info.wl.display, wmi.info.wl.surface, gWidth, gHeight};
    return (void*)&native_window;
  } else {
    std::cout << "Unknown SDL subsystem";
    return nullptr;
  }
}

/**
 * @brief Creates an SDL window with specified parameters.
 *
 * The function creates an SDL window with the given title, position, size, and
 * window flags. The window is shown, resizable, and allows high DPI.
 *
 * @return The created SDL window on success, or nullptr if an error occurred.
 *
 * Example usage:
 * @code{cpp}
 * SDL_Window* window = createWindow();
 * @endcode
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

/**
 * @brief Initializes and runs a Filament application.
 *
 * This function initializes the SDL library, creates a window using the
 * createWindow() function, and sets up the Filament engine, renderer, camera,
 * scene, and other necessary components. It then enters a main loop, rendering
 * the scene and handling input events until the application is closed.
 *
 * @return 0 if the application exits successfully.
 */
int main() {
  SDL_Init(SDL_INIT_VIDEO);

  using namespace filament;
  using namespace math;
  using namespace utils;

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
  SDL_Window* window = createWindow();

  Engine* engine = Engine::create(filament::backend::Backend::VULKAN);
  SwapChain* swapChain = engine->createSwapChain(getNativeWindow(window));
  Renderer* renderer = engine->createRenderer();

  auto cameraEntity = EntityManager::get().create();
  Camera* camera = engine->createCamera(cameraEntity);
  camera->lookAt(float3(0, 50.5f, 0), float3(0, 0, 0), float3(1.f, 0, 0));
  camera->setProjection(45.0, double(gWidth) / gHeight, 0.1, 50,
                        Camera::Fov::VERTICAL);

  Scene* scene = engine->createScene();

  View* view = engine->createView();
  view->setCamera(camera);
  view->setViewport({0, 0, gWidth, gHeight});
  view->setScene(scene);

  VertexBuffer* vertexBuffer =
      VertexBuffer::Builder()
          .vertexCount(4)
          .bufferCount(2)
          .attribute(VertexAttribute::POSITION, 0,
                     VertexBuffer::AttributeType::FLOAT3)
          .attribute(VertexAttribute::TANGENTS, 1,
                     VertexBuffer::AttributeType::SHORT4)
          .normalized(VertexAttribute::TANGENTS)
          .build(*engine);

  vertexBuffer->setBufferAt(
      *engine, 0,
      VertexBuffer::BufferDescriptor(
          vertices, vertexBuffer->getVertexCount() * sizeof(vertices[0])));
  vertexBuffer->setBufferAt(
      *engine, 1,
      VertexBuffer::BufferDescriptor(
          normals, vertexBuffer->getVertexCount() * sizeof(normals[0])));

  IndexBuffer* indexBuffer =
      IndexBuffer::Builder().indexCount(6).build(*engine);

  indexBuffer->setBuffer(
      *engine, IndexBuffer::BufferDescriptor(
                   indices, indexBuffer->getIndexCount() * sizeof(uint32_t)));

  auto skybox = Skybox::Builder().color({0.5, 0.5, 0.5, 1.0}).build(*engine);
  scene->setSkybox(skybox);

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

  filamat::Package package = builder.build(engine->getJobSystem());

  Material* material = Material::Builder()
                           .package(package.getData(), package.getSize())
                           .build(*engine);
  material->setDefaultParameter("baseColor", RgbType::LINEAR, float3{0, 1, 0});
  material->setDefaultParameter("metallic", 0.0f);
  material->setDefaultParameter("roughness", 0.4f);
  material->setDefaultParameter("reflectance", 0.5f);

  MaterialInstance* materialInstance = material->createInstance();

  Entity renderable = EntityManager::get().create();

  Entity light = EntityManager::get().create();

  LightManager::Builder(LightManager::Type::SUN)
      .color(Color::toLinear<ACCURATE>(sRGBColor(0.98f, 0.92f, 0.89f)))
      .intensity(110000)
      .direction({0.7, -1, -0.8})
      .sunAngularRadius(1.9f)
      .castShadows(true)
      .build(*engine, light);

  scene->addEntity(light);

  // build a quad
  RenderableManager::Builder(1)
      .boundingBox({{-1, -1, -1}, {1, 1, 1}})
      .material(0, materialInstance)
      .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, vertexBuffer,
                indexBuffer, 0, 6)
      .culling(false)
      .build(*engine, renderable);
  scene->addEntity(renderable);

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
      }

      if (prev_width != gWidth || prev_height != gHeight) {
        prev_width = gWidth;
        prev_height = gHeight;
        engine->destroy(swapChain);
        swapChain = engine->createSwapChain(getNativeWindow(window));
        view->setViewport({0, 0, gWidth, gHeight});
        camera->setProjection(45.0, double(gWidth) / gHeight, 0.1, 50,
                              Camera::Fov::VERTICAL);
      }

      // beginFrame() returns false if we need to skip a frame
      if (renderer->beginFrame(swapChain)) {
        // for each View
        renderer->render(view);
        renderer->endFrame();
      }
      SDL_UpdateWindowSurface(window);
      SDL_Delay(16);
    }
  }

  engine->destroy(scene);
  engine->destroy(view);
  engine->destroy(cameraEntity);
  engine->destroy(light);
  engine->destroy(skybox);
  engine->destroy(renderable);
  engine->destroy(indexBuffer);
  engine->destroy(vertexBuffer);
  engine->destroy(materialInstance);
  engine->destroy(material);
  engine->destroy(renderer);
  engine->destroy(swapChain);
  Engine::destroy(engine);

  SDL_Quit();
  return 0;
}
