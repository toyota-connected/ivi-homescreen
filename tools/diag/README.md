# Diagnostic harnesses

Standalone C programs that exist to produce evidence about a driver. Nothing in
the product links them; they are built by hand and run by hand. CI builds them
without linking, because a harness that no longer compiles is not evidence of
anything.

## `dmabuf_import_probe.c` — will this driver import a dma-buf its own GBM allocated?

Allocates through GBM with an explicit modifier, exports the dma-buf, builds a
`VkImage` over that exact layout, and imports it at the only `allocationSize` a
dedicated allocation may legally use — `VkMemoryRequirements::size`. When that is
refused it retries a few bytes short, which is what distinguishes a driver that
charges the import for internal scratch padding from a producer that simply
under-allocated.

Headless: no surface, no display, no DRM master, so it runs over plain SSH on a
render node.

```sh
make
./dmabuf_import_probe --width 1280 --from 1438 --heights 1442
```

Read the `spare` column — the dma-buf's size minus the image's requirement. A
driver charging N bytes of scratch refuses exactly the extents whose spare falls
below N, and those same extents import when asked N short.

| option | meaning |
| --- | --- |
| `--device` | render node, default `/dev/dri/renderD128` |
| `--width`, `--from`, `--heights` | sweep heights `--from`..`--heights` at one width |
| `--mod-index` | which of the driver's importable modifiers to use, default the first |
| `--modifier` | force a modifier instead, e.g. `0` for LINEAR |
| `--usage` | image usage bits: `sampled,transfer-src,transfer-dst,color-attachment,storage` |
| `--readahead` | bytes to try short on a refusal, default 64 |

Exits non-zero when any extent was refused, so it can gate a scripted run.

### Why it is here

It reproduces ivi-homescreen#723 in isolation, without the shell, a compositor
or a producer plugin. On V3D 7.1.7 with Mesa 25.0.7, `1280x1440` XR24 LINEAR
allocates a 7372800-byte bo for an image needing exactly 7372800, and the import
is refused — while 1439 and 1441 import. 125 of the heights from 1 to 2000 are
refused, at every multiple of 16. On the tiled modifier the tile rounding makes
several consecutive extents share one allocation, so the failure covers a band,
which is why it first looked like a property of particular surfaces. radv
imports the same zero-spare buffer without complaint.

**Fixed in Mesa 26.2.2**, measured on the same board: 0 of 4600 extents refused,
across both modifiers, two widths and both usage sets. The fix is on the
exporter side, which is the part worth knowing — v3d now leaves four pages of
headroom on the buffers it exports, so `spare` is never 0:

```
                        25.0.7      26.2.2
gbm bo                  7372800     7389184
req.size (TRANSFER_SRC) 7372800     7372864
spare                   0           16320
```

`--usage` exists because 26.1 also narrowed the padding to images declaring
`TRANSFER_SRC`, which is what the shell's importer asks for. That narrowing is
real and still charges those images 64 bytes, but the exporter headroom makes it
moot. Keep the flag: it is how you tell the two halves apart on a driver where
only one of them has landed.

### Cross-building for a board

Ask `emb` for the toolchain rather than writing a path down — the profile
directory carries a hash that moves with the target configuration:

```sh
out=$(emb cross . --target rpi5-trixie)
make CC="$(sed -n 's/^ *cc *: *//p' <<<"$out" | head -1)" \
     SYSROOT="$(sed -n 's/^ *target sysroot *: *//p' <<<"$out" | head -1)" \
     CFLAGS="-O2 -Wall -Wextra -mcpu=cortex-a76"
```

Build on the host and copy the binary over. A board may well have
`vulkan/vulkan.h` and no `gbm.h` or `libdrm` headers, and installing them needs
root it may not give you.
