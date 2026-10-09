// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_COMMON_PUBLIC_FLUTTER_TEXTURE_REGISTRAR_H_
#define FLUTTER_SHELL_PLATFORM_COMMON_PUBLIC_FLUTTER_TEXTURE_REGISTRAR_H_

#include <stddef.h>
#include <stdint.h>

#include "flutter_export.h"

#if defined(__cplusplus)
extern "C" {
#endif

struct FlutterDesktopTextureRegistrar;
// Opaque reference to a texture registrar.
typedef struct FlutterDesktopTextureRegistrar*
    FlutterDesktopTextureRegistrarRef;

// Possible values for the type specified in FlutterDesktopTextureInfo.
// Additional types may be added in the future.
typedef enum {
  // A Pixel buffer-based texture.
  kFlutterDesktopPixelBufferTexture,
  // A platform-specific GPU surface-backed texture.
  kFlutterDesktopGpuSurfaceTexture
} FlutterDesktopTextureType;

// Supported GPU surface types.
typedef enum {
  // Uninitialized.
  kFlutterDesktopGpuSurfaceTypeNone,
  // A DXGI shared texture handle (Windows only).
  // See
  // https://docs.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiresource-getsharedhandle
  kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle,
  // A |ID3D11Texture2D| (Windows only).
  kFlutterDesktopGpuSurfaceTypeD3d11Texture2D,
  // A GL texture name (ivi-homescreen EGL backends).
  kFlutterDesktopGpuSurfaceTypeGlTexture2D,
  // A |VkImage| (ivi-homescreen Vulkan backends). |handle| points to a
  // |FlutterDesktopVulkanImage|. See that type for the sampling contract.
  kFlutterDesktopGpuSurfaceTypeVkImage
} FlutterDesktopGpuSurfaceType;

// The image a |kFlutterDesktopGpuSurfaceTypeVkImage| texture hands to the
// engine. |FlutterDesktopGpuSurfaceDescriptor::handle| points to one of these.
// The descriptor and this struct are read after the GPU-surface callback
// returns, so keep both unchanged until the next callback invocation (or
// until the texture is unregistered).
//
// Unlike |kFlutterDesktopGpuSurfaceTypeGlTexture2D|, whose descriptor is read
// once at registration, the GPU-surface callback of a VkImage texture runs on
// the raster thread each time the engine resolves the texture, i.e. once after
// each |FlutterDesktopTextureRegistrarMarkExternalTextureFrameAvailable|. A
// producer cycling through several images returns the current one each time.
// No registrar lock is held during the callback.
//
// Contract, per the Flutter embedder API (FlutterVulkanExternalTexture):
//  - |image| must be created on the VkDevice the engine renders with (see
//    ihs_pv_vulkan_context in <ihs/platform_view.h>), with
//    VK_IMAGE_USAGE_SAMPLED_BIT, and owned by the queue family reported there.
//  - It must be in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL when the callback
//    returns.
//  - All writes to it must have completed before the callback returns; the
//    engine samples it with no further synchronization. The callback blocks
//    the raster thread, so wait the producer's fence before calling
//    MarkExternalTextureFrameAvailable and keep the callback itself cheap.
//  - It must stay alive and unmodified until the descriptor's
//    |release_callback| runs. The engine keeps the current image until the
//    next frame is marked available and the GPU work sampling it has retired,
//    so a producer needs at least two images. |release_callback| runs once
//    per frame, on an engine thread; when the embedder rejects a descriptor
//    (see below) it runs right away, on the raster thread.
//  - |FlutterDesktopGpuSurfaceDescriptor::width|/|height| give the image's
//    physical size, at most maxImageDimension2D; 0 lets the engine use the
//    size it requested.
//  - |format| must be one the engine samples on both of its renderers:
//    R8G8B8A8_UNORM, R8G8B8A8_SRGB, B8G8R8A8_UNORM, R16G16B16A16_SFLOAT,
//    R32G32B32A32_SFLOAT, R8_UNORM or R8G8_UNORM; or a multi-planar YCbCr
//    format (e.g. G8_B8R8_2PLANE_420_UNORM for NV12) when the backend enabled
//    samplerYcbcrConversion on the device. Other formats are rejected.
//
// Unregistering is asynchronous: the completion callback passed to
// |FlutterDesktopTextureRegistrarUnregisterExternalTexture| runs once the
// engine has released every frame of the texture, possibly later and on an
// engine thread. Keep the images and the release context alive until then.
// Always pass a completion callback for a VkImage texture (with the C++
// wrapper, TextureRegistrar::UnregisterTexture(id, callback)): without one
// there is no point at which freeing them is safe.
//
// Requires a Flutter engine that includes the Vulkan external texture API
// (flutter/flutter#188855). On an older engine, registration succeeds but the
// texture never resolves.
typedef struct {
  // The size of this struct. Must be sizeof(FlutterDesktopVulkanImage).
  size_t struct_size;
  // The VkImage handle.
  uint64_t image;
  // The VkFormat of |image| (e.g. VK_FORMAT_R8G8B8A8_UNORM).
  uint32_t format;
} FlutterDesktopVulkanImage;

// Supported pixel formats.
typedef enum {
  // Uninitialized.
  kFlutterDesktopPixelFormatNone,
  // Represents a 32-bit RGBA color format with 8 bits each for red, green, blue
  // and alpha.
  kFlutterDesktopPixelFormatRGBA8888,
  // Represents a 32-bit BGRA color format with 8 bits each for blue, green, red
  // and alpha.
  kFlutterDesktopPixelFormatBGRA8888
} FlutterDesktopPixelFormat;

// An image buffer object.
typedef struct {
  // The pixel data buffer.
  const uint8_t* buffer;
  // Width of the pixel buffer.
  size_t width;
  // Height of the pixel buffer.
  size_t height;
  // An optional callback that gets invoked when the |buffer| can be released.
  void (*release_callback)(void* release_context);
  // Opaque data passed to |release_callback|.
  void* release_context;
} FlutterDesktopPixelBuffer;

// A GPU surface descriptor.
typedef struct {
  // The size of this struct. Must be
  // sizeof(FlutterDesktopGpuSurfaceDescriptor).
  size_t struct_size;
  // The surface handle. The expected type depends on the
  // |FlutterDesktopGpuSurfaceType|.
  //
  // Provide a |ID3D11Texture2D*| when using
  // |kFlutterDesktopGpuSurfaceTypeD3d11Texture2D| or a |HANDLE| when using
  // |kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle|.
  //
  // The referenced resource needs to stay valid until it has been opened by
  // Flutter. Consider incrementing the resource's reference count in the
  // |FlutterDesktopGpuSurfaceTextureCallback| and registering a
  // |release_callback| for decrementing the reference count once it has been
  // opened.
  void* handle;
  // The physical width.
  size_t width;
  // The physical height.
  size_t height;
  // The visible width.
  // It might be less or equal to the physical |width|.
  size_t visible_width;
  // The visible height.
  // It might be less or equal to the physical |height|.
  size_t visible_height;
  // The pixel format which might be optional depending on the surface type.
  FlutterDesktopPixelFormat format;
  // An optional callback that gets invoked when the |handle| has been opened.
  void (*release_callback)(void* release_context);
  // Opaque data passed to |release_callback|.
  void* release_context;
} FlutterDesktopGpuSurfaceDescriptor;

// The pixel buffer copy callback definition provided to
// the Flutter engine to copy the texture.
// It is invoked with the intended surface size specified by |width| and
// |height| and the |user_data| held by
// |FlutterDesktopPixelBufferTextureConfig|.
//
// As this is usually called from the render thread, the callee must take
// care of proper synchronization. It also needs to be ensured that the
// returned |FlutterDesktopPixelBuffer| isn't released prior to unregistering
// the corresponding texture.
typedef const FlutterDesktopPixelBuffer* (
    *FlutterDesktopPixelBufferTextureCallback)(size_t width,
                                               size_t height,
                                               void* user_data);

// The GPU surface callback definition provided to the Flutter engine to obtain
// the surface. It is invoked with the intended surface size specified by
// |width| and |height| and the |user_data| held by
// |FlutterDesktopGpuSurfaceTextureConfig|.
typedef const FlutterDesktopGpuSurfaceDescriptor* (
    *FlutterDesktopGpuSurfaceTextureCallback)(size_t width,
                                              size_t height,
                                              void* user_data);

// An object used to configure pixel buffer textures.
typedef struct {
  // The callback used by the engine to copy the pixel buffer object.
  FlutterDesktopPixelBufferTextureCallback callback;
  // Opaque data that will get passed to the provided |callback|.
  void* user_data;
} FlutterDesktopPixelBufferTextureConfig;

// An object used to configure GPU-surface textures.
typedef struct {
  // The size of this struct. Must be
  // sizeof(FlutterDesktopGpuSurfaceTextureConfig).
  size_t struct_size;
  // The concrete surface type (e.g.
  // |kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle|)
  FlutterDesktopGpuSurfaceType type;
  // The callback used by the engine to obtain the surface descriptor.
  FlutterDesktopGpuSurfaceTextureCallback callback;
  // Opaque data that will get passed to the provided |callback|.
  void* user_data;
} FlutterDesktopGpuSurfaceTextureConfig;

typedef struct {
  FlutterDesktopTextureType type;
  union {
    FlutterDesktopPixelBufferTextureConfig pixel_buffer_config;
    FlutterDesktopGpuSurfaceTextureConfig gpu_surface_config;
  };
} FlutterDesktopTextureInfo;

// Registers a new texture with the Flutter engine and returns the texture ID.
// This function can be called from any thread.
FLUTTER_EXPORT int64_t FlutterDesktopTextureRegistrarRegisterExternalTexture(
    FlutterDesktopTextureRegistrarRef texture_registrar,
    const FlutterDesktopTextureInfo* info);

// Asynchronously unregisters the texture identified by |texture_id| from the
// Flutter engine.
// An optional |callback| gets invoked upon completion.
// This function can be called from any thread.
FLUTTER_EXPORT void FlutterDesktopTextureRegistrarUnregisterExternalTexture(
    FlutterDesktopTextureRegistrarRef texture_registrar,
    int64_t texture_id,
    void (*callback)(void* user_data),
    void* user_data);

// Marks that a new texture frame is available for a given |texture_id|.
// Returns true on success or false if the specified texture doesn't exist.
// This function can be called from any thread.
FLUTTER_EXPORT bool
FlutterDesktopTextureRegistrarMarkExternalTextureFrameAvailable(
    FlutterDesktopTextureRegistrarRef texture_registrar,
    int64_t texture_id);

FLUTTER_EXPORT bool FlutterDesktopTextureMakeCurrent(
    FlutterDesktopTextureRegistrarRef texture_registrar);

FLUTTER_EXPORT bool FlutterDesktopTextureClearCurrent(
    FlutterDesktopTextureRegistrarRef texture_registrar);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // FLUTTER_SHELL_PLATFORM_COMMON_PUBLIC_FLUTTER_TEXTURE_REGISTRAR_H_
