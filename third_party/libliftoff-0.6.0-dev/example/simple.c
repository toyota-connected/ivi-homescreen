/* Minimalistic example: create a few layers and display as many of them as
 * possible. Layers that don't make it into a plane won't be displayed. */

#define _POSIX_C_SOURCE 200809L
#include <drm_fourcc.h>
#include <fcntl.h>
#include <libliftoff.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "common.h"

#define LAYERS_LEN 4

/* ARGB 8:8:8:8 */
static const uint32_t colors[] = {
	0xFFFF0000, /* red */
	0xFF00FF00, /* green */
	0xFF0000FF, /* blue */
	0xFFFFFF00, /* yellow */
};

static struct liftoff_layer *
add_layer(int drm_fd, struct liftoff_output *output, int x, int y, uint32_t width,
	  uint32_t height, bool with_alpha)
{
	static bool first = true;
	static size_t color_idx = 0;
	struct dumb_fb fb = {0};
	uint32_t color;
	struct liftoff_layer *layer;

	uint32_t format = with_alpha ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
	if (!dumb_fb_init(&fb, drm_fd, format, width, height)) {
		fprintf(stderr, "failed to create framebuffer\n");
		return NULL;
	}
	printf("Created FB %d with size %dx%d\n", fb.id, width, height);

	if (first) {
		color = 0xFFFFFFFF;
		first = false;
	} else {
		color = colors[color_idx];
		color_idx = (color_idx + 1) % (sizeof(colors) / sizeof(colors[0]));
	}

	dumb_fb_fill(&fb, drm_fd, color);

	layer = liftoff_layer_create(output);
	liftoff_layer_set_property(layer, "FB_ID", fb.id);
	liftoff_layer_set_property(layer, "CRTC_X", (uint64_t)x);
	liftoff_layer_set_property(layer, "CRTC_Y", (uint64_t)y);
	liftoff_layer_set_property(layer, "CRTC_W", width);
	liftoff_layer_set_property(layer, "CRTC_H", height);
	liftoff_layer_set_property(layer, "SRC_X", 0);
	liftoff_layer_set_property(layer, "SRC_Y", 0);
	liftoff_layer_set_property(layer, "SRC_W", width << 16);
	liftoff_layer_set_property(layer, "SRC_H", height << 16);

	return layer;
}

int
main(int argc, char *argv[])
{
	int drm_fd;
	struct liftoff_device *device;
	drmModeRes *drm_res;
	drmModeCrtc *crtc;
	drmModeConnector *connector;
	struct liftoff_output *output;
	struct liftoff_layer *layers[LAYERS_LEN];
	struct liftoff_plane *plane;
	drmModeAtomicReq *req;
	int ret;
	size_t i;
	uint32_t flags;

	drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		perror("open");
		return 1;
	}

	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0) {
		perror("drmSetClientCap(ATOMIC)");
		return 1;
	}

	device = liftoff_device_create(drm_fd);
	if (device == NULL) {
		perror("liftoff_device_create");
		return 1;
	}

	liftoff_device_register_all_planes(device);

	drm_res = drmModeGetResources(drm_fd);
	connector = pick_connector(drm_fd, drm_res);
	crtc = pick_crtc(drm_fd, drm_res, connector);
	disable_all_crtcs_except(drm_fd, drm_res, crtc->crtc_id);
	output = liftoff_output_create(device, crtc->crtc_id);
	drmModeFreeResources(drm_res);

	if (connector == NULL) {
		fprintf(stderr, "no connector found\n");
		return 1;
	}
	if (crtc == NULL || !crtc->mode_valid) {
		fprintf(stderr, "no CRTC found\n");
		return 1;
	}

	printf("Using connector %d, CRTC %d\n", connector->connector_id,
	       crtc->crtc_id);

	layers[0] = add_layer(drm_fd, output, 0, 0, crtc->mode.hdisplay,
			      crtc->mode.vdisplay, false);
	for (i = 1; i < LAYERS_LEN; i++) {
		layers[i] = add_layer(drm_fd, output, 100 * (int)i, 100 * (int)i,
				      256, 256, i % 2);
	}

	for (i = 0; i < LAYERS_LEN; i++) {
		liftoff_layer_set_property(layers[i], "zpos", i);
	}

	flags = DRM_MODE_ATOMIC_NONBLOCK;
	req = drmModeAtomicAlloc();
	ret = liftoff_output_apply(output, req, flags, NULL);
	if (ret != 0) {
		perror("liftoff_output_apply");
		return 1;
	}

	ret = drmModeAtomicCommit(drm_fd, req, flags, NULL);
	if (ret < 0) {
		perror("drmModeAtomicCommit");
		return 1;
	}

	for (i = 0; i < sizeof(layers) / sizeof(layers[0]); i++) {
		plane = liftoff_layer_get_plane(layers[i]);
		if (plane != NULL) {
			printf("Layer %zu got assigned to plane %u\n", i,
			       liftoff_plane_get_id(plane));
		} else {
			printf("Layer %zu has no plane assigned\n", i);
		}
	}

	sleep(1);

	drmModeAtomicFree(req);
	for (i = 0; i < sizeof(layers) / sizeof(layers[0]); i++) {
		liftoff_layer_destroy(layers[i]);
	}
	liftoff_output_destroy(output);
	drmModeFreeCrtc(crtc);
	drmModeFreeConnector(connector);
	liftoff_device_destroy(device);
	return 0;
}
