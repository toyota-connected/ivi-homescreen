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
#include <array>

// Screen Size
constexpr int32_t kDefaultViewWidth = 1920;
constexpr int32_t kDefaultViewHeight = 720;
constexpr double kDefaultPixelRatio = 1.0;
constexpr int kEglBufferSize = 24;

// Cursor
constexpr int kCursorSize = 24;
constexpr char kCursorKindBasic[] = "left_ptr";
constexpr char kCursorKindClick[] = "hand";
constexpr char kCursorKindText[] = "left_ptr";
constexpr char kCursorKindForbidden[] = "pirate";

// Touch
constexpr int kMaxTouchPoints = 10;

// Locale
constexpr char kDefaultLocaleLanguageCode[] = "en";
constexpr char kDefaultLocaleCountryCode[] = "US";
constexpr char kDefaultLocaleScriptCode[] = "";

// Path prefix comes from build
constexpr char kPathPrefix[] = PATH_PREFIX;

// Bundle folder paths
constexpr char kBundleFlutterAssets[] = "data/flutter_assets";
constexpr char kBundleAot[] = "lib/libapp.so";
// Bundle folder override paths
constexpr char kBundleIcudtl[] = "data/icudtl.dat";
constexpr char kBundleEngine[] = "lib/libflutter_engine.so";
// System paths
constexpr char kSystemEngine[] = "libflutter_engine.so";
constexpr char kSystemIcudtl[] = "share/flutter/icudtl.dat";

// Install path constants
static constexpr char kApplicationName[] = "homescreen";
static constexpr char kXdgApplicationDir[] = ".homescreen";
static constexpr char kXdgConfigDir[] = ".config";

// Compositor Surface
constexpr unsigned int kCompSurfExpectedInterfaceVersion = 0x00010000;

static constexpr std::array<EGLint, 5> kEglContextAttribs = {{
    // clang-format off
    EGL_CONTEXT_MAJOR_VERSION, 3,
    EGL_CONTEXT_MAJOR_VERSION, 2,
    EGL_NONE
    // clang-format on
}};

static constexpr std::array<EGLint, 15> kEglConfigAttribs = {{
    // clang-format off
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
#if defined(BUILD_EGL_ENABLE_TRANSPARENCY)
    EGL_ALPHA_SIZE, 8,
#else
    EGL_ALPHA_SIZE, 0,
#endif
    EGL_NONE // termination sentinel
    // clang-format on
}};

// All vkCreate* functions take an optional allocator. For now, we select the
// default allocator by passing in a null pointer, and we highlight the argument
// by using the VKALLOC constant.
constexpr struct VkAllocationCallbacks* VKALLOC = nullptr;

#if defined(__GNUC__)
#ifndef MAYBE_UNUSED
#define MAYBE_UNUSED
#endif
#ifndef NODISCARD
#define NODISCARD
#endif
#endif

#if defined(__clang__)
#ifndef MAYBE_UNUSED
#define MAYBE_UNUSED [[maybe_unused]]
#endif
#ifndef NODISCARD
#define NODISCARD [[nodiscard]]
#endif
#endif
