#ifndef EXAMPLE_COMMON_H
#define EXAMPLE_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <xf86drmMode.h>

struct dumb_fb {
	uint32_t format;
	uint32_t width, height, stride, size;
	uint32_t handle;
	uint32_t id;
};

drmModeConnector *
pick_connector(int drm_fd, drmModeRes *drm_res);

drmModeCrtc *
pick_crtc(int drm_fd, drmModeRes *drm_res, drmModeConnector *connector);

void
disable_all_crtcs_except(int drm_fd, drmModeRes *drm_res, uint32_t crtc_id);

bool
dumb_fb_init(struct dumb_fb *fb, int drm_fd, uint32_t format,
	     uint32_t width, uint32_t height);

void *
dumb_fb_map(struct dumb_fb *fb, int drm_fd);

void
dumb_fb_fill(struct dumb_fb *fb, int drm_fd, uint32_t color);

#endif
