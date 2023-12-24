# Table of Contents

1. [Overview of the Platform Views Handler](#overview-of-the-platform-views-handler)
2. [Pros of Using AndroidView Interface](#pros-of-using-androidview-interface)
3. [Backing Implementation Approaches](#backing-implementation-approaches)
    - [Rendering to a Texture](#rendering-to-a-texture)
    - [Rendering to a Compositor Sub-Surface](#rendering-to-a-compositor-sub-surface)
4. [Existing Implementations](#existing-implementations)

## Overview of the Platform Views Handler

This handler is designed to handle seamless integration with any Dart code in Platform View using the `AndroidView`
interface.

## Pros of Using AndroidView Interface

- The `AndroidView` interface is pre-defined and well-supported within Flutter SDK, reducing compatibility issues.
- The interface's parameters are adjustable in Dart, enhancing adaptability to different use cases.
- The support for transfer of touch to another process/library without the requirement of additional code.

## Backing Implementation Approaches

The following approaches can be used for backing implementation in platform views.

### Rendering to a Texture

This approach involves rendering a `PlatformView` implementation to a texture.

### Rendering to a Compositor Sub-Surface

This method involves rendering a `PlatformView` implementation to a compositor sub-surface.

Both approaches can effectively be implemented using either an OpenGL texture or a compositor sub-surface and are
feasible through the `Platform Views` interface.

## Existing Implementations

There are currently two example plugin implementations that render to a compositor Sub-Surface:

- `LayerPlaygroundView`
- `FilamentView`