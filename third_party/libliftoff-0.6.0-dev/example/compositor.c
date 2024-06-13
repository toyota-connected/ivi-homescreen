/* Compositor: create a few layers, display as many of them as possible on a
 * plane. Iterate over layers that didn't make it into a plane, and fallback to
 * composition if necessary. */

#define _POSIX_C_SOURCE 200809L
#include <drm_fourcc.h>
#include <fcntl.h>
#include <libliftoff.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "common.h"

#define MAX_LAYERS_LEN 16

/* ARGB 8:8:8:8 */
static const uint32_t colors[] = {
	0xFFFF0000, /* red */
	0xFF00FF00, /* green */
	0xFF0000FF, /* blue */
	0xFFFFFF00, /* yellow */
};

static struct liftoff_layer *
add_layer(int drm_fd, struct liftoff_output *output, int x, int y, uint32_t width,
	  uint32_t height, bool with_alpha, bool white, struct dumb_fb *fb)
{
	static size_t color_idx = 0;
	uint32_t color;
	struct liftoff_layer *layer;

	uint32_t format = with_alpha ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
	if (!dumb_fb_init(fb, drm_fd, format, width, height)) {
		fprintf(stderr, "failed to create framebuffer\n");
		return NULL;
	}
	printf("Created FB %d with size %dx%d\n", fb->id, width, height);

	if (white) {
		color = 0xFFFFFFFF;
	} else {
		color = colors[color_idx];
		color_idx = (color_idx + 1) % (sizeof(colors) / sizeof(colors[0]));
	}

	dumb_fb_fill(fb, drm_fd, color);

	layer = liftoff_layer_create(output);
	liftoff_layer_set_property(layer, "FB_ID", fb->id);
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

/* Naive compositor for opaque buffers */
static void
composite(int drm_fd, struct dumb_fb *dst_fb, struct dumb_fb *src_fb, int dst_x,
	  int dst_y)
{
	uint8_t *dst, *src;
	int i, y, src_width;

	dst = dumb_fb_map(dst_fb, drm_fd);
	src = dumb_fb_map(src_fb, drm_fd);

	src_width = (int)src_fb->width;
	if (dst_x < 0) {
		dst_x = 0;
	}
	if (dst_x + src_width > (int)dst_fb->width) {
		src_width = (int)dst_fb->width - dst_x;
	}

	for (i = 0; i < (int)src_fb->height; i++) {
		y = dst_y + i;
		if (y < 0 || y >= (int)dst_fb->height) {
			continue;
		}
		memcpy(dst + dst_fb->stride * (size_t)y +
			     (size_t)dst_x * sizeof(uint32_t),
		       src + src_fb->stride * (size_t)i,
		       (size_t)src_width * sizeof(uint32_t));
	}

	munmap(dst, dst_fb->size);
	munmap(src, src_fb->size);
}

int
main(int argc, char *argv[])
{
	int opt;
	size_t layers_len;
	int drm_fd;
	struct liftoff_device *device;
	drmModeRes *drm_res;
	drmModeCrtc *crtc;
	drmModeConnector *connector;
	struct liftoff_output *output;
	struct dumb_fb composition_fb = {0};
	struct liftoff_layer *composition_layer;
	struct dumb_fb fbs[MAX_LAYERS_LEN] = {0};
	struct liftoff_layer *layers[MAX_LAYERS_LEN];
	struct liftoff_plane *plane;
	drmModeAtomicReq *req;
	int ret;
	size_t i;
	uint32_t flags;

	layers_len = 6;
	while ((opt = getopt(argc, argv, "l:h")) != -1) {
		switch (opt) {
		case 'l':
			layers_len = (size_t)atoi(optarg);
			break;
		default:
			fprintf(stderr,
				"usage: %s [options...]\n"
				"  -h        Display help message\n"
				"  -l <num>  Number of layers (default: 6)\n",
				argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}
	if (layers_len <= 0 || layers_len > MAX_LAYERS_LEN) {
		fprintf(stderr, "invalid -l value\n");
		return 1;
	}

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

	composition_layer = add_layer(drm_fd, output, 0, 0, crtc->mode.hdisplay,
				      crtc->mode.vdisplay, false, true,
				      &composition_fb);
	layers[0] = add_layer(drm_fd, output, 0, 0, crtc->mode.hdisplay,
			      crtc->mode.vdisplay, false, true, &fbs[0]);
	for (i = 1; i < layers_len; i++) {
		layers[i] = add_layer(drm_fd, output, 100 * (int)i, 100 * (int)i,
				      256, 256, i % 2, false, &fbs[i]);
	}

	liftoff_layer_set_property(composition_layer, "zpos", 0);
	for (i = 0; i < layers_len; i++) {
		liftoff_layer_set_property(layers[i], "zpos", i);
	}

	liftoff_output_set_composition_layer(output, composition_layer);

	flags = DRM_MODE_ATOMIC_NONBLOCK;
	req = drmModeAtomicAlloc();
	ret = liftoff_output_apply(output, req, flags, NULL);
	if (ret != 0) {
		perror("liftoff_output_apply");
		return 1;
	}

	/* Composite layers that didn't make it into a plane */
	for (i = 1; i < layers_len; i++) {
		if (liftoff_layer_needs_composition(layers[i])) {
			composite(drm_fd, &composition_fb, &fbs[i],
				  (int)i * 100, (int)i * 100);
		}
	}

	ret = drmModeAtomicCommit(drm_fd, req, flags, NULL);
	if (ret < 0) {
		perror("drmModeAtomicCommit");
		return 1;
	}

	plane = liftoff_layer_get_plane(composition_layer);
	printf("Composition layer got assigned to plane %u\n",
	       plane ? liftoff_plane_get_id(plane) : 0);
	for (i = 0; i < layers_len; i++) {
		plane = liftoff_layer_get_plane(layers[i]);
		printf("Layer %zu got assigned to plane %u\n", i,
		       plane ? liftoff_plane_get_id(plane) : 0);
	}

	sleep(1);

	drmModeAtomicFree(req);
	liftoff_layer_destroy(composition_layer);
	for (i = 0; i < layers_len; i++) {
		liftoff_layer_destroy(layers[i]);
	}
	liftoff_output_destroy(output);
	drmModeFreeCrtc(crtc);
	drmModeFreeConnector(connector);
	liftoff_device_destroy(device);
	return 0;
}
