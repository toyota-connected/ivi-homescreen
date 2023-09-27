/*
 * Copyright 2021-2022 Toyota Connected North America
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

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "constants.h"


class Egl {
 public:
  Egl(void* native_display, EGLenum platform, int buffer_size, bool debug);

  ~Egl();

  /**
   * @brief Clear an EGL rendering context
   * @return bool
   * @retval true Normal end
   * @retval false Abnormal end
   * @relation
   * wayland
   */
  bool ClearCurrent();

  /**
   * @brief Attach an EGL rendering context to EGL surface
   * @return bool
   * @retval true Normal end
   * @retval false Abnormal end
   * @relation
   * wayland
   */
  bool MakeCurrent();

  /**
   * @brief Post EGL surface color buffer to a native window
   * @return bool
   * @retval true Normal end
   * @retval false Abnormal end
   * @relation
   * wayland
   */
  bool SwapBuffers();

  /**
   * @brief Attach an EGL rendering context to EGL surface by specifying
   * Resource content
   * @return bool
   * @retval true Normal end
   * @retval false Abnormal end
   * @relation
   * wayland
   */
  bool MakeResourceCurrent();

  /**
   * @brief Attach an EGL rendering context to EGL surface by specifying Texture
   * content
   * @return bool
   * @retval true Normal end
   * @retval false Abnormal end
   * @relation
   * wayland
   */
  bool MakeTextureCurrent();

  /**
   * @brief Get an EGL display connection
   * @param[in] platform Platform
   * @param[in] native_display The native display
   * @param[in] attrib_list Display attributes
   * @return EGLDisplay
   * @retval An EGL display connection
   * @relation
   * wayland
   */
  static EGLDisplay get_egl_display(EGLenum platform,
                                    void* native_display,
                                    const EGLint* attrib_list);

  /**
   * @brief Create a new EGL window surface
   * @param[in] native_window The native window
   * @param[in] attrib_list Window surface attributes
   * @return EGLSurface
   * @retval An EGL window surface
   * @relation
   * wayland
   */
  EGLSurface create_egl_surface(void* native_window, const EGLint* attrib_list);

  /**
   * @brief Function that returns a function pointer to
   * PFNEGLSETDAMAGEREGIONKHRPROC
   * @return PFNEGLSETDAMAGEREGIONKHRPROC
   * @retval nullptr if extensions are not supported
   * @relation
   * EGL
   */
  PFNEGLSETDAMAGEREGIONKHRPROC GetSetDamageRegion() {
    return m_pfSetDamageRegion;
  }

  /**
   * @brief Function that returns a function pointer to
   * PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC
   * @return PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC
   * @retval nullptr if extensions are not supported
   * @relation
   * EGL
   */
  PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC GetSwapBuffersWithDamage() {
    return m_pfSwapBufferWithDamage;
  }

  /**
   * @brief Function that returns if GL Ext Buffer Age is supported
   * @return bool
   * @retval true if extension is available
   * @relation
   * EGL
   */
  bool HasExtBufferAge() const { return m_has_egl_ext_buffer_age; }

  /**
   * @brief Auxiliary function used to transform a FlutterRect into the format
   * that is expected by the EGL functions
   * @param[in] rect FlutterRect
   * @return array of EGLint
   * @retval if extension is present
   * @relation
   * EGL
   */
  std::array<EGLint, 4> RectToInts(FlutterRect rect) const;

  EGLDisplay GetDisplay() { return m_dpy; }

  EGLContext GetTextureContext() { return m_texture_context; }

 protected:
  EGLSurface m_egl_surface{};

 private:
  EGLConfig m_config{};
  EGLContext m_texture_context{};

  int m_buffer_size{};

  EGLDisplay m_dpy{};
  EGLContext m_context{};
  EGLContext m_resource_context{};

  EGLint m_major{};
  EGLint m_minor{};

  PFNEGLDEBUGMESSAGECONTROLKHRPROC m_pfDebugMessageControl{};
  PFNEGLQUERYDEBUGKHRPROC m_pfQueryDebug{};
  PFNEGLLABELOBJECTKHRPROC m_pfLabelObject{};

  PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC m_pfSwapBufferWithDamage{};
  PFNEGLSETDAMAGEREGIONKHRPROC m_pfSetDamageRegion{};
  bool m_has_egl_ext_buffer_age{};

  /**
   * @brief Auxiliary function used to check if the given list of extensions
   * contains the requested extension name.
   * @param[in] dpy EGL display
   * @param[in] name name of extension
   * @return bool
   * @retval if extension is present
   * @relation
   * EGL
   */
  static bool HasEGLExtension(
      std::unordered_map<EGLDisplay, const char*>& extensions,
      EGLDisplay dpy,
      const char* name);

  /**
   * @brief Auxiliary function used to check if the GL extension
   * is available.
   * @param[in] name name of extension
   * @return bool
   * @retval if extension is present
   * @relation
   * EGL
   */
  static bool HasGLExtension(const char* name);

  /**
   * @brief Report the contents of EGL attributes and frame buffer
   * configurations
   * @param[in] configs EGL Config
   * @param[in] count Count of configs
   * @return void
   * @relation
   * wayland
   */
  void ReportGlesAttributes(EGLConfig* configs, EGLint count);

  /**
   * @brief Debug callback function to output error details
   * @param[in] error Error code
   * @param[in] command Commands in case of error
   * @param[in] messageType Type of message when an error occurs
   * @param[in] threadLabel Thread label when error occurs
   * @param[in] objectLabel Object label when an error occurs
   * @param[in] message Error message
   * @return void
   * @relation
   * internal
   */
  static void sDebugCallback(EGLenum error,
                             const char* command,
                             EGLint messageType,
                             EGLLabelKHR threadLabel,
                             EGLLabelKHR objectLabel,
                             const char* message);

  /**
   * @brief Initialize of EGL KHR_debug
   * @param[in] extensions unordered_map of EGL extensions
   * @return void
   * @relation
   * wayland
   */
  void EGL_KHR_debug_init(
      std::unordered_map<EGLDisplay, const char*>& extensions);

  /**
   * @brief Print a list of extensions, with word-wrapping
   * @param[in] ext List of extensions
   * @return void
   * @relation
   * internal
   */
  static void print_extension_list(EGLDisplay dpy);
};
