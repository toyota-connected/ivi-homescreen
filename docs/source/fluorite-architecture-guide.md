# Fluorite / Filament Architecture Guide

> **ASIL-QM Notice**: This document is provided as quality management (QM) level
> guidance only. It has not been validated against ISO 26262 for safety-critical
> automotive applications rated ASIL-A through ASIL-D.

## Overview

Fluorite is the 3D rendering engine inside `ivi-homescreen`, built on Google
Filament (v1.69.1) for physically based rendering (PBR). It uses an
Entity-Component-System (ECS) architecture implemented in C++, driven by
scene configuration from Flutter/Dart via platform channels.

## Quick Start for Maintainers

This document is a **docs-only architecture guide** for the historical
`filament_view` implementation.

- **What this is**: a concise map of ECS structure, component roles, variant
  flow, and contribution entry points based on commit `51ba753` analysis.
- **What this is not**: a proposal to restore built-in `filament_view`, change
  runtime behavior, or bypass the external plugin direction in [PR #160](https://github.com/toyota-connected/ivi-homescreen/pull/160).
- **Who should use it**: maintainers reviewing docs PRs, contributors trying to
  understand Fluorite internals, and developers preparing low-risk docs/build
  improvements before code-level changes.

## Claim Classification (Fact / Inference / Guidance)

Legend:
- **Fact** = verifiable from repository history, code layout, or cited PR/commit.
- **Inference** = derived interpretation from available evidence.
- **Guidance** = recommendation for contributor behavior.

| Strong Claim | Type | Evidence / Basis |
|--------------|------|------------------|
| Fluorite is the 3D rendering engine inside `ivi-homescreen`. | **Fact** | Repository architecture and plugin integration context. |
| Fluorite uses Google Filament v1.69.1. | **Fact** | Build/dependency setup used in Sprint 2 execution artifacts. |
| Fluorite uses an ECS architecture implemented in C++. | **Fact** | `filament_view` source structure at commit `51ba753`. |
| Flutter/Dart scene config drives native rendering via platform channels. | **Fact** | Plugin architecture pattern and document data-flow model. |
| This is a docs-only guide for historical `filament_view`. | **Fact** | Scope statement in this document. |
| The guide is based on commit `51ba753`. | **Fact** | Explicit recovery reference and source baseline. |
| The document is not proposing restoration/behavior change/bypass of [PR #160](https://github.com/toyota-connected/ivi-homescreen/pull/160) direction. | **Guidance** | Explicit scope guard for this PR and contributor behavior. |
| Plugin model is transitioning to external loading. | **Fact** | [PR #160](https://github.com/toyota-connected/ivi-homescreen/pull/160) topic and referenced plugin direction. |
| ECS is suitable for automotive IVI due to performance/composability/testability. | **Inference** | Architecture reasoning derived from ECS properties. |
| ECS can support ~60 fps on embedded GPUs. | **Inference** | Performance expectation derived from design goals and prior context. |
| The listed component catalog reflects Fluorite structure. | **Fact** | Derived from `filament_view` code analysis at `51ba753`. |
| Variant configurations define runtime entity composition. | **Fact** | ECS + scene-config mapping model in analyzed implementation. |
| Fluorite differs from `flutter_scene` mainly by ECS + Filament stack choices. | **Inference** | Comparative synthesis across two approaches. |
| "Production (shipping 2026)" indicates maturity posture. | **Inference** | Project signal interpretation, not guaranteed release evidence. |
| `filament_view` was removed in [PR #212](https://github.com/toyota-connected/ivi-homescreen-plugins/pull/212). | **Fact** | Verifiable in `ivi-homescreen-plugins` PR history. |
| Removal was part of transition to external plugin loading ([PR #160](https://github.com/toyota-connected/ivi-homescreen/pull/160)). | **Fact** | [PR #212](https://github.com/toyota-connected/ivi-homescreen-plugins/pull/212) + [PR #160](https://github.com/toyota-connected/ivi-homescreen/pull/160) relationship in upstream history. |
| Plugin is being extracted, not abandoned. | **Inference** | Interpreted from external-plugin migration signals. |
| Contributors should not restore built-in `filament_view` directly. | **Guidance** | Aligns with external-plugin roadmap and avoids architectural conflict. |
| Contributors should reference commit `51ba753` for study. | **Guidance** | Lowest-risk path for architecture understanding. |
| Contributors should wait for [PR #160](https://github.com/toyota-connected/ivi-homescreen/pull/160) merge before filament_view code contributions. | **Guidance** | Sequencing recommendation to align with upstream direction. |
| Documentation/build fixes are low-risk contribution entry points; C++ systems are high-risk. | **Guidance** | Safety/process policy from Sprint 2 guardrails (A1/A2). |

## System Architecture

```
Flutter Application
├── Dart Widgets (FilamentViewWidget, SceneConfig, Camera Controls)
│   └── Platform Channel (Method Channel)
│
├── filament_view Plugin (C++)
│   ├── Entity-Component-System (ECS)
│   │   ├── Entities: Camera, Light, Model, Fog, Skybox, ...
│   │   ├── Components: Transform, Renderable, Animation, ...
│   │   └── Systems: RenderSystem, InputSystem, AnimationSystem
│   │
│   └── Google Filament v1.69.1 (PBR)
│       ├── Vulkan backend
│       └── OpenGL ES backend
│
├── ivi-homescreen (Flutter Embedder)
│   ├── Wayland surface
│   ├── EGL/GLES context
│   └── Plugin host (built-in / external)
│
└── Linux / Yocto
    └── Wayland compositor · GPU driver · SDL3
```

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Rendering engine | Google Filament | PBR-quality, BSD-3, battle-tested on Android |
| Architecture | ECS | Cache-friendly, composable, testable |
| Language split | Dart + C++ | Flutter for UI; C++ for GPU performance |
| I/O layer | SDL3 | Cross-platform input, Wayland-native |
| Plugin model | Transitioning to external | [PR #160](https://github.com/toyota-connected/ivi-homescreen/pull/160) adds dynamic loading |

## ECS Pattern

```
Entity     = ID representing a scene object (camera, light, model)
Component  = Pure data attached to an entity (position, color, mesh)
System     = Logic that processes entities with matching components
```

Data flows from Flutter to C++ via method channels:

```
Dart (SceneConfig)  ──►  EntityManager (C++)  ──►  Systems  ──►  Filament  ──►  GPU
```

**Why ECS for automotive IVI?**
- **Performance** — cache-friendly data layout for 60 fps on embedded GPUs
- **Composability** — new features (fog, particles) added by composing components
- **Testability** — systems tested in isolation with mock components
- **Separation** — Dart manages *what* exists; C++ manages *how* it renders

## Component Catalog

### Core Components

| Component | Key Fields | Purpose |
|-----------|-----------|---------|
| TransformComponent | position, rotation, scale | Spatial placement of any entity |
| CameraComponent | fov, near, far, projection, exposure | View configuration |
| LightComponent | type, color, intensity, direction, radius | Scene illumination |
| RenderableComponent | mesh, material, bounding box, visibility | Visible geometry |
| AnimationComponent | index, state, speed, loop | Skeletal / property animation |
| FogComponent | density, color, height, start, inscattering | Atmospheric fog |
| SkyboxComponent | cubemap, intensity, color | Environment background |
| IndirectLightComponent | IBL texture, intensity, rotation | Image-based lighting |

### Input Components

| Component | Key Fields | Purpose |
|-----------|-----------|---------|
| GestureComponent | type, delta, velocity | Touch / pointer input |
| OrbitComponent | target, radius, yaw, pitch | Orbital camera control |

### Scene Management Components

| Component | Key Fields | Purpose |
|-----------|-----------|---------|
| SceneComponent | background color, clear flags | Global scene settings |
| MaterialComponent | URL, parameters | Filament material (.filamat) |
| ShapeComponent | type, dimensions | Procedural geometry |

## Variant Pattern

Variants are named scene configurations that define *which* entities and
components exist. Each variant is a complete scene definition in Dart:

```dart
// Example: snow_scene variant
final snowScene = SceneConfig(
  camera: Camera(fov: 45, near: 0.1, far: 1000, position: [0, 5, 20]),
  lights: [DirectionalLight(direction: [-1, -1, 0], intensity: 50000)],
  models: [Model(url: 'assets/ground_plane.glb')],
  fog: Fog(density: 0.15, color: Color(0xFFE8E8E8), height: 2.0),
  skybox: Skybox(cubemap: 'assets/overcast_sky.ktx'),
);
```

This creates the following entity tree at runtime:

```
Scene
├── Entity: Camera    [CameraComponent, TransformComponent]
├── Entity: Sun       [LightComponent, TransformComponent]
├── Entity: Ground    [RenderableComponent, TransformComponent, MaterialComponent]
├── Entity: Fog       [FogComponent]
└── Entity: Skybox    [SkyboxComponent, IndirectLightComponent]
```

## flutter_scene Comparison

| Aspect | flutter_scene | Fluorite (filament_view) |
|--------|--------------|--------------------------|
| Rendering engine | Impeller (Flutter) | Google Filament (external) |
| PBR quality | Basic (evolving) | Production (Android default) |
| Architecture | Scene graph (OOP) | ECS (data-oriented) |
| Platform scope | All Flutter platforms | Linux IVI (ivi-homescreen) |
| Maturity | Preview | Production (shipping 2026) |
| Lighting | Directional + point | Full IBL, area lights, shadows, fog |
| Model format | glTF 2.0 | glTF 2.0 + Filament materials |
| Material system | Impeller shaders | Filament material system (matc) |
| ECS support | No | Yes (core architecture) |

If flutter_scene matures to production PBR quality, Fluorite's advantage
shifts to its ECS architecture for complex IVI scenes.

## Git History Recovery — filament_view

The `filament_view` plugin was removed from `ivi-homescreen-plugins` in
[PR #212](https://github.com/toyota-connected/ivi-homescreen-plugins/pull/212) as part of the transition to external plugin loading ([PR #160](https://github.com/toyota-connected/ivi-homescreen/pull/160)).

**This deletion was intentional** — the plugin is being extracted, not
abandoned.

```
Recovery reference:
  Repository: toyota-connected/ivi-homescreen-plugins
  Last known commit: 51ba753
  Command: git checkout 51ba753 -- plugins/filament_view/
```

**For contributors**:
This document does not propose code restoration or behavior changes.

1. Do NOT submit PRs to restore filament_view as a built-in plugin
2. DO reference commit `51ba753` when studying the architecture
3. Wait for [PR #160](https://github.com/toyota-connected/ivi-homescreen/pull/160) (external plugin support) to merge before
   contributing filament_view code

## Contribution Entry Points

| Entry Point | Risk | Notes |
|-------------|------|-------|
| Documentation (this guide) | Minimal | Docs-only, no runtime impact |
| Component catalog expansion | Minimal | Docs-only |
| Build system fixes (CMake) | Low | Test with CI before submitting |
| New ECS component (C++) | High | Requires safety review |
| New ECS system (C++) | High | Requires safety review |
| Filament version upgrade | Very High | Requires maintainer coordination |

---

*Architecture based on commit `51ba753` analysis. See FOSDEM 2026 talk
"Fluorite — Scalable 3D Rendering on Embedded Flutter" for context.*
