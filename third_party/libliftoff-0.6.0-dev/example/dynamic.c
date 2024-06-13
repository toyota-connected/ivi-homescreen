/* Dynamic: create a few layers, setup a rendering loop for one of them. The
 * result is a rectangle updating its color while all other layers that make it
 * into a plane are static. */

#define _POSIX_C_SOURCE 200809L
#include <drm_fourcc.h>
#include <fcntl.h>
#include <libliftoff.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "common.h"

#define LAYERS_LEN 4

struct example_layer {
	float color[3];
	int dec;
	int x, y;

	struct dumb_fb fbs[2];
	size_t front_fb;

	struct liftoff_layer *layer;
};

static int drm_fd = -1;
static struct liftoff_device *device = NULL;
static struct liftoff_output *output = NULL;
static struct example_layer layers[LAYERS_LEN] = {0};
static size_t active_layer_idx = 2;

static bool
init_layer(int drm_fd, struct example_layer *layer,
	   struct liftoff_output *output, uint32_t width, uint32_t height,
	   bool with_alpha)
{
	static size_t color_idx = 0;
	static float color_value = 1.0;
	uint32_t format;

	format = with_alpha ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
	if (!dumb_fb_init(&layer->fbs[0], drm_fd, format, width, height) ||
	    !dumb_fb_init(&layer->fbs[1], drm_fd, format, width, height)) {
		fprintf(stderr, "failed to create framebuffer\n");
		return false;
	}

	layer->layer = liftoff_layer_create(output);
	liftoff_layer_set_property(layer->layer, "CRTC_W", width);
	liftoff_layer_set_property(layer->layer, "CRTC_H", height);
	liftoff_layer_set_property(layer->layer, "SRC_X", 0);
	liftoff_layer_set_property(layer->layer, "SRC_Y", 0);
	liftoff_layer_set_property(layer->layer, "SRC_W", width << 16);
	liftoff_layer_set_property(layer->layer, "SRC_H", height << 16);

	layer->color[color_idx % 3] = color_value;
	color_idx++;
	if (color_idx % 3 == 0) {
		color_value -= 0.1f;
	}

	return true;
}

static void
draw_layer(int drm_fd, struct example_layer *layer)
{
	uint32_t color;
	struct dumb_fb *fb;

	layer->front_fb = (layer->front_fb + 1) % 2;
	fb = &layer->fbs[layer->front_fb];

	color = ((uint32_t)0xFF << 24) |
		((uint32_t)(layer->color[0] * 0xFF) << 16) |
		((uint32_t)(layer->color[1] * 0xFF) << 8) |
		(uint32_t)(layer->color[2] * 0xFF);

	dumb_fb_fill(fb, drm_fd, color);

	liftoff_layer_set_property(layer->layer, "FB_ID", fb->id);
	liftoff_layer_set_property(layer->layer, "CRTC_X", (uint64_t)layer->x);
	liftoff_layer_set_property(layer->layer, "CRTC_Y", (uint64_t)layer->y);
}

static bool
draw(void)
{
	struct example_layer *active_layer;
	drmModeAtomicReq *req;
	int ret, inc;
	size_t i;
	uint32_t flags;
	struct liftoff_plane *plane;

	active_layer = &layers[active_layer_idx];

	inc = (active_layer->dec + 1) % 3;

	active_layer->color[inc] += 0.05f;
	active_layer->color[active_layer->dec] -= 0.05f;

	if (active_layer->color[active_layer->dec] < 0.0f) {
		active_layer->color[inc] = 1.0f;
		active_layer->color[active_layer->dec] = 0.0f;
		active_layer->dec = inc;
	}

	draw_layer(drm_fd, active_layer);

	flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
	req = drmModeAtomicAlloc();
	ret = liftoff_output_apply(output, req, flags, NULL);
	if (ret != 0) {
		perror("liftoff_output_apply");
		return false;
	}

	ret = drmModeAtomicCommit(drm_fd, req, flags, NULL);
	if (ret < 0) {
		perror("drmModeAtomicCommit");
		return false;
	}

	drmModeAtomicFree(req);

	for (i = 0; i < sizeof(layers) / sizeof(layers[0]); i++) {
		plane = liftoff_layer_get_plane(layers[i].layer);
		if (plane != NULL) {
			printf("Layer %zu got assigned to plane %u\n", i,
			       liftoff_plane_get_id(plane));
		} else {
			printf("Layer %zu has no plane assigned\n", i);
		}
	}

	return true;
}

static void
page_flip_handler(int fd, unsigned seq, unsigned tv_sec, unsigned tv_usec,
		  unsigned crtc_id, void *data)
{
	draw();
}

int
main(int argc, char *argv[])
{
	drmModeRes *drm_res;
	drmModeCrtc *crtc;
	drmModeConnector *connector;
	size_t i;
	int ret;

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

	init_layer(drm_fd, &layers[0], output, crtc->mode.hdisplay,
		   crtc->mode.vdisplay, false);
	for (i = 1; i < LAYERS_LEN; i++) {
		init_layer(drm_fd, &layers[i], output, 100, 100, i % 2);
		layers[i].x = 100 * (int)i;
		layers[i].y = 100 * (int)i;
	}

	for (i = 0; i < LAYERS_LEN; i++) {
		liftoff_layer_set_property(layers[i].layer, "zpos", i);

		draw_layer(drm_fd, &layers[i]);
	}

	draw();

	for (i = 0; i < 120; i++) {
		drmEventContext drm_event = {
			.version = 3,
			.page_flip_handler2 = page_flip_handler,
		};
		struct pollfd pfd = {
			.fd = drm_fd,
			.events = POLLIN,
		};

		ret = poll(&pfd, 1, 1000);
		if (ret != 1) {
			perror("poll");
			return 1;
		}

		drmHandleEvent(drm_fd, &drm_event);
	}

	for (i = 0; i < sizeof(layers) / sizeof(layers[0]); i++) {
		liftoff_layer_destroy(layers[i].layer);
	}
	liftoff_output_destroy(output);
	drmModeFreeCrtc(crtc);
	drmModeFreeConnector(connector);
	liftoff_device_destroy(device);
	return 0;
}
