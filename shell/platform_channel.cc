// Copyright 2020 Toyota Connected North America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "platform_channel.h"

#ifdef ENABLE_PLUGIN_ACCESSIBILITY
#include "static_plugins/accessibility/accessibility.h"
#endif
#ifdef ENABLE_PLUGIN_CONNECTIVITY
#include "static_plugins/connectivity/connectivity.h"
#endif
#ifdef ENABLE_PLUGIN_ISOLATE
#include "static_plugins/isolate/isolate.h"
#endif
#ifdef ENABLE_PLUGIN_RESTORATION
#include "static_plugins/restoration/restoration.h"
#endif
#ifdef ENABLE_PLUGIN_MOUSE_CURSOR
#include "static_plugins/mouse_cursor/mouse_cursor.h"
#endif
#ifdef ENABLE_PLUGIN_NAVIGATION
#include "static_plugins/navigation/navigation.h"
#endif
#ifdef ENABLE_PLUGIN_OPENGL_TEXTURE
#include "static_plugins/opengl_texture/opengl_texture.h"
#endif
#ifdef ENABLE_PLUGIN_PACKAGE_INFO
#include "static_plugins/package_info/package_info.h"
#endif
#ifdef ENABLE_PLUGIN_PLATFORM
#include "static_plugins/platform/platform.h"
#endif
#ifdef ENABLE_PLUGIN_PLATFORM_VIEWS
#include "static_plugins/platform_views/platform_views.h"
#endif
#ifdef ENABLE_PLUGIN_TEXT_INPUT
#include "static_plugins/text_input/text_input.h"
#endif
#ifdef ENABLE_PLUGIN_URL_LAUNCHER
#include "static_plugins/url_launcher/url_launcher.h"
#endif
#ifdef ENABLE_PLUGIN_NAVIGATION_SEARCH
#include "static_plugins/navigation_search/navigation_search.h"
#endif

PlatformChannel* PlatformChannel::singleton = nullptr;

PlatformChannel::PlatformChannel() {
#ifdef ENABLE_PLUGIN_ACCESSIBILITY
  RegisterCallback(kChannelAccessibility, &Accessibility::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_CONNECTIVITY
  RegisterCallback(kChannelConnectivity, &Connectivity::OnPlatformMessage);
  RegisterCallback(kChannelConnectivityStatus,
                   &Connectivity::OnPlatformMessageStatus);
#endif
#ifdef ENABLE_PLUGIN_ISOLATE
  RegisterCallback(kChannelIsolate, &Isolate::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_RESTORATION
  RegisterCallback(kChannelRestoration, &Restoration::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_MOUSE_CURSOR
  RegisterCallback(kChannelMouseCursor, &MouseCursor::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_NAVIGATION
  RegisterCallback(kChannelNavigation, &Navigation::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_OPENGL_TEXTURE
  RegisterCallback(kChannelOpenGlTexture, OpenGlTexture::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_PACKAGE_INFO
  RegisterCallback(kChannelPackageInfo, &PackageInfo::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_PLATFORM
  RegisterCallback(kChannelPlatform, &Platform::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_PLATFORM_VIEWS
  RegisterCallback(kChannelPlatformViews, &PlatformViews::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_TEXT_INPUT
  RegisterCallback(kChannelTextInput, &TextInput::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_URL_LAUNCHER
  RegisterCallback(kChannelUrlLauncher, &UrlLauncher::OnPlatformMessage);
#endif
#ifdef ENABLE_PLUGIN_NAVIGATION_SEARCH
  RegisterCallback(kChannelNavigationSearch,
                   &NavigationSearch::OnPlatformMessage);
#endif
}
