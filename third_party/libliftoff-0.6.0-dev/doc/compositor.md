# Using libliftoff for a compositor

This document explains how to use libliftoff for a full-blown compositor.

A compositor receives buffers coming from clients and displays them on screen.
Client buffers are attached to surfaces. For instance a Wayland compositor
receives shared-memory buffers via `wl_shm` and DMA-BUFs via `wp_linux_dmabuf`.
Shared memory buffers are stored in main memory, DMA-BUFs are stored directly
on the GPU.

Compositors which can't take advantage of hardware planes will always perform
composition for all client buffers. In other words, such compositors will copy
client buffers to their own buffer, then display their own buffer on screen.
Typically the copying is done via OpenGL.

Compositors which can take advantage of hardware planes will sometimes avoid
composition (because the hardware can directly scan-out client buffers), but
sometimes they'll need to fallback to composition (because of hardware
limitations). libliftoff can help with this decision-making.

## Setting up layers

A compositor will want to create one layer per surface. That way, in the
best-case scenario, all client buffers will make it into a hardware plane and
composition won't be necessary. The layers effectively describe the compositor's
scene-graph.

The compositor will need to set standard plane properties on the buffer, in
particular the `CRTC_*`, `SRC_*` and `zpos` properties. These properties define
the position and size of the layer.

### DMA-BUFs

Each time a client submits a new DMA-BUF for a surface, the compositor should
import the buffer and update the layer's `FB_ID`. libliftoff will try to put
this layer in a plane.

### Buffers that can't be directly scanned out

Buffers using shared memory can't be directly scanned out by the hardware.

These will need to be composited, the compositor should call
`liftoff_layer_set_fb_composited`. libliftoff will never try to put this layer
in a plane.

### Elements drawn by the compositor

Sometimes compositors want to draw UI elements themselves, for instance window
decorations. The compositor could allocate buffers for these UI elements, but
sometimes it may want not to. libliftoff needs to know about this to avoid
putting a layer which is underneath in a hardware plane.

For each of those elements, the compositor can create a layer, set the `CRTC_*`
and `zpos` properties to indicate the geometry of the element, and call
`liftoff_layer_set_fb_composited` to indicate that this region needs to be
composited.

## Composition layer

A compositor must always be prepared to fallback to composition. There are a lot
of potential reasons why composition is needed, for instance:

- The client uses a buffer which can't be directly scanned out, or the
  compositor wants to draw a UI element itself
- The format or modifier used by the client isn't compatible with any of the
  hardware planes
- The client buffer isn't compatible with any of the planes, e.g. because of a
  format or modifier mismatch
- Putting a client buffer into a hardware plane would exceed the hardware
  bandwidth limits

The compositor needs to prepare a special buffer where client buffers will be
copied to if composition is necessary. Because it's not possible to know in
advance whether composition will be necessary, the compositor needs to setup
this "composition buffer" before libliftoff does its job.

The composition buffer should be allocated with a format and modifier compatible
with the primary plane.

The compositor should create a special layer, the "composition layer". The layer
should cover the whole CRTC. `liftoff_output_set_composition_layer` should be
used to tell libliftoff that composition will happen on this special layer.

If all regular layers can be put into a plane, the composition layer won't be
used. Otherwise, the compositor needs to perform the composition prior to
performing a page-flip. Each layer that didn't make it into a hardware plane
(ie. `liftoff_layer_needs_composition` returns true) needs to be composited.

## Disabling layers

When the compositor needs to hide a surface, it's not very handy to destroy the
layer (and re-create it when the surface is visible again). Instead, the
compositor can disable a layer by setting `FB_ID` to zero.

The layer will be ignored by libliftoff.
