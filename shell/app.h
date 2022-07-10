/*
 * Copyright 2020 Toyota Connected North America
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

#include <EGL/egl.h>
#include <flutter_embedder.h>
#include <wayland-client.h>
#include <memory>

#include "constants.h"

#ifdef ENABLE_TEXTURE_TEST_EGL
#include "textures/test_egl/texture_test_egl.h"
#endif
#ifdef ENABLE_PLUGIN_TEXT_INPUT

#include "backend/backend.h"
#include "static_plugins/text_input/text_input.h"
#include "configuration/configuration.h"

#endif

class Display;

class WaylandWindow;

class Engine;

#if defined(BUILD_BACKEND_WAYLAND_EGL)
class WaylandEglBackend;
#elif defined(BUILD_BACKEND_WAYLAND_VULKAN)
class WaylandVulkanBackend;
#endif

class App {
private:
    std::shared_ptr<Display> m_wayland_display;
#if defined(BUILD_BACKEND_WAYLAND_EGL)
    std::shared_ptr<WaylandEglBackend> m_backend;
#elif defined(BUILD_BACKEND_WAYLAND_VULKAN)
    std::shared_ptr<WaylandVulkanBackend> m_backend;
#endif
    std::shared_ptr<WaylandWindow> m_wayland_window[kEngineInstanceCount];
    std::shared_ptr<Engine> m_flutter_engine[kEngineInstanceCount];
    struct {
        uint8_t output;
        uint32_t period;
        uint32_t counter;
        long long pretime;
    } m_fps{};
#ifdef ENABLE_TEXTURE_TEST_EGL
    std::unique_ptr<TextureTestEgl> m_texture_test_egl;
#endif
#ifdef ENABLE_PLUGIN_TEXT_INPUT
    std::shared_ptr<TextInput> m_text_input;
#endif

public:
    explicit App(const std::vector<Configuration::Config>& configs);

    App(const App &) = delete;

    const App &operator=(const App &) = delete;

    std::shared_ptr<WaylandWindow> GetEglWindow(size_t index) {
        return m_wayland_window[index];
    }

    Backend *GetBackend() { return reinterpret_cast<Backend *>(m_backend.get()); }

    int Loop();
};
