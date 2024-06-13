# libliftoff

[![builds.sr.ht status](https://builds.sr.ht/~emersion/libliftoff/commits/master.svg)](https://builds.sr.ht/~emersion/libliftoff/commits/master)

Lightweight KMS plane library.

libliftoff eases the use of KMS planes from userspace without standing in your
way. Users create "virtual planes" called layers, set KMS properties on them,
and libliftoff will pick hardware planes for these layers if possible.

Resources:

* [Blog post introducing the project][intro-post]
* [FOSDEM 2020 talk][fosdem-2020]

libliftoff is used in production by [gamescope] on Steam Deck devices, and work
is underway to integrate it with [wlroots].

## Building

Depends on libdrm. Requires universal planes and atomic.

    meson setup build/
    ninja -C build/

## Usage

See [`liftoff.h`][liftoff.h] and the [`example/`][example] directory. See
[`doc/compositor.md`][doc/compositor] for compositor guidelines.

Here's the general idea:

```c
struct liftoff_device *device;
struct liftoff_output *output;
struct liftoff_layer *layer;
drmModeAtomicReq *req;
int ret;

device = liftoff_device_create(drm_fd);
output = liftoff_output_create(device, crtc_id);

liftoff_device_register_all_planes(device);

layer = liftoff_layer_create(output);
liftoff_layer_set_property(layer, "FB_ID", fb_id);
/* Probably setup more properties and more layers */

req = drmModeAtomicAlloc();
ret = liftoff_output_apply(output, req);
if (ret < 0) {
	perror("liftoff_output_apply");
	exit(1);
}

ret = drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
if (ret < 0) {
	perror("drmModeAtomicCommit");
	exit(1);
}
drmModeAtomicFree(req);
```

## Contributing

Report bugs and send pull requests on [GitLab][gitlab].

We use the Wayland/Weston style and contribution guidelines, see [Weston's
contributing document][weston-contributing].

## License

MIT

[liftoff.h]: https://gitlab.freedesktop.org/emersion/libliftoff/-/blob/master/include/libliftoff.h
[example]: https://gitlab.freedesktop.org/emersion/libliftoff/-/tree/master/example
[doc/compositor]: https://gitlab.freedesktop.org/emersion/libliftoff/-/blob/master/doc/compositor.md
[intro-post]: https://emersion.fr/blog/2019/xdc2019-wrap-up/#libliftoff
[fosdem-2020]: https://fosdem.org/2020/schedule/event/kms_planes/
[gitlab]: https://gitlab.freedesktop.org/emersion/libliftoff
[weston-contributing]: https://gitlab.freedesktop.org/wayland/weston/-/blob/master/CONTRIBUTING.md
[gamescope]: https://github.com/Plagman/gamescope
[wlroots]: https://gitlab.freedesktop.org/wlroots/wlroots
