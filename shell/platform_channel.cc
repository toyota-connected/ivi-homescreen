/*
 * Copyright 2020-2023 Toyota Connected North America
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

#include "platform_channel.h"

#ifdef ENABLE_PLUGIN_ACCESSIBILITY
#include "plugins/accessibility/accessibility.h"
#endif
#ifdef ENABLE_PLUGIN_RESTORATION
#include "plugins/restoration/restoration.h"
#endif
#ifdef ENABLE_PLUGIN_MOUSE_CURSOR
#include "plugins/mouse_cursor/mouse_cursor.h"
#endif
#ifdef ENABLE_PLUGIN_GSTREAMER_EGL
#include "plugins/gstreamer_egl/gstreamer_egl.h"
#endif
#ifdef ENABLE_PLUGIN_COMP_SURF
#include "plugins/comp_surf/comp_surf.h"
#endif
#ifdef ENABLE_PLUGIN_COMP_REGION
#include "plugins/comp_region/comp_region.h"
#endif
#ifdef ENABLE_PLUGIN_OPENGL_TEXTURE
#include "plugins/opengl_texture/opengl_texture.h"
#endif
#ifdef ENABLE_PLUGIN_PACKAGE_INFO
#include "plugins/package_info/package_info.h"
#endif
#ifdef ENABLE_PLUGIN_PLATFORM_VIEWS
#include "plugins/platform_views/platform_views.h"
#endif
#ifdef ENABLE_PLUGIN_DESKTOP_WINDOW
#include "plugins/desktop_window/desktop_window.h"
#endif
#ifdef ENABLE_PLUGIN_SECURE_STORAGE
#include "plugins/secure_storage/secure_storage.h"
#endif
#ifdef ENABLE_PLUGIN_INTEGRATION_TEST
#include "plugins/integration_test/integration_test.h"
#endif
#ifdef ENABLE_PLUGIN_LOGGING
#include "plugins/logging/logging.h"
#endif
#ifdef ENABLE_PLUGIN_GOOGLE_SIGN_IN
#include "plugins/google_sign_in/google_sign_in.h"
#endif
#ifdef ENABLE_PLUGIN_FILE_SELECTOR
#include "plugins/file_selector/file_selector.h"
#endif

PlatformChannel* PlatformChannel::singleton = nullptr;

PlatformChannel::PlatformChannel() {
#ifdef ENABLE_PLUGIN_ACCESSIBILITY
  RegisterCallback(Accessibility::kChannelName,
                   &Accessibility::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_RESTORATION
  RegisterCallback(Restoration::kChannelName, &Restoration::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_MOUSE_CURSOR
  RegisterCallback(MouseCursor::kChannelName, &MouseCursor::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_GSTREAMER_EGL
  RegisterCallback(GstreamerEgl::kChannelGstreamerInitialize,
                   &GstreamerEgl::OnInitialize);
#endif
#ifdef ENABLE_PLUGIN_COMP_SURF
  RegisterCallback(CompositorSurfacePlugin::kChannelName,
                   &CompositorSurfacePlugin::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_COMP_REGION
  RegisterCallback(CompositorRegionPlugin::kChannelName,
                   &CompositorRegionPlugin::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_OPENGL_TEXTURE
  RegisterCallback(OpenGlTexture::kChannelName,
                   OpenGlTexture::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_PACKAGE_INFO
  RegisterCallback(PackageInfo::kChannelName, &PackageInfo::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_PLATFORM_VIEWS
  RegisterCallback(PlatformViews::kChannelName,
                   &PlatformViews::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_DESKTOP_WINDOW
  RegisterCallback(DesktopWindow::kChannelName,
                   &DesktopWindow::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_SECURE_STORAGE
  RegisterCallback(SecureStorage::kChannelName,
                   &SecureStorage::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_INTEGRATION_TEST
  RegisterCallback(IntegrationTestPlugin::kChannelName,
                   &IntegrationTestPlugin::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_LOGGING
  RegisterCallback(LoggingPlugin::kChannelName,
                   &LoggingPlugin::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_GOOGLE_SIGN_IN
  RegisterCallback(GoogleSignIn::kChannelName,
                   &GoogleSignIn::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_FILE_SELECTOR
  RegisterCallback(FileSelector::kChannelName,
                   &FileSelector::OnPlatformMessage);
#endif
}
