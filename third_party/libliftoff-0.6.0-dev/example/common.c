#include <assert.h>
#include <drm_fourcc.h>
#include <stddef.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include "common.h"

drmModeConnector *
pick_connector(int drm_fd, drmModeRes *drm_res)
{
	int i;
	drmModeConnector *connector;

	for (i = 0; i < drm_res->count_connectors; i++) {
		connector = drmModeGetConnector(drm_fd, drm_res->connectors[i]);
		if (connector->connection == DRM_MODE_CONNECTED) {
			return connector;
		}
		drmModeFreeConnector(connector);
	}

	return NULL;
}

drmModeCrtc *
pick_crtc(int drm_fd, drmModeRes *drm_res, drmModeConnector *connector)
{
	drmModeEncoder *enc;
	uint32_t crtc_id;
	int i;
	int j;
	bool found;

	enc = drmModeGetEncoder(drm_fd, connector->encoder_id);

	if (enc) {
		/* Current CRTC happens to be usable on the selected connector */
		crtc_id = enc->crtc_id;
		drmModeFreeEncoder(enc);
		return drmModeGetCrtc(drm_fd, crtc_id);
	} else {
		/* Current CRTC used by this encoder can't drive the selected connector.
		 * Search all of them for a valid combination. */
		for (i = 0, found = false; !found && i < connector->count_encoders; i++) {
			enc = drmModeGetEncoder(drm_fd, connector->encoders[i]);

			if (!enc) {
				continue;
			}

			for (j = 0; !found && j < drm_res->count_crtcs; j++) {
				/* Can the CRTC drive the connector? */
				if (enc->possible_crtcs & (1 << j)) {
					crtc_id = drm_res->crtcs[j];
					found = true;
				}
			}
			drmModeFreeEncoder(enc);
		}

		if (found) {
			return drmModeGetCrtc(drm_fd, crtc_id);
		} else {
			return NULL;
		}
	}
}

void
disable_all_crtcs_except(int drm_fd, drmModeRes *drm_res, uint32_t crtc_id)
{
	int i;

	for (i = 0; i < drm_res->count_crtcs; i++) {
		if (drm_res->crtcs[i] == crtc_id) {
			continue;
		}
		drmModeSetCrtc(drm_fd, drm_res->crtcs[i],
			0, 0, 0, NULL, 0, NULL);
	}
}

bool
dumb_fb_init(struct dumb_fb *fb, int drm_fd, uint32_t format, uint32_t width,
	     uint32_t height)
{
	int ret;
	uint32_t fb_id;
	struct drm_mode_create_dumb create;
	uint32_t handles[4] = {0};
	uint32_t strides[4] = {0};
	uint32_t offsets[4] = { 0 };

	assert(format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888);

	create = (struct drm_mode_create_dumb) {
		.width = width,
		.height = height,
		.bpp = 32,
		.flags = 0,
	};
	ret = drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
	if (ret < 0) {
		return false;
	}

	handles[0] = create.handle;
	strides[0] = create.pitch;
	ret = drmModeAddFB2(drm_fd, width, height, format, handles, strides,
			    offsets, &fb_id, 0);
	if (ret < 0) {
		return false;
	}

	fb->width = width;
	fb->height = height;
	fb->stride = create.pitch;
	fb->size = create.size;
	fb->handle = create.handle;
	fb->id = fb_id;
	return true;
}

void *
dumb_fb_map(struct dumb_fb *fb, int drm_fd)
{
	int ret;

	struct drm_mode_map_dumb map = { .handle = fb->handle };
	ret = drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
	if (ret < 0) {
		return MAP_FAILED;
	}

	return mmap(0, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd,
		    (off_t)map.offset);
}

void
dumb_fb_fill(struct dumb_fb *fb, int drm_fd, uint32_t color)
{
	uint32_t *data;
	size_t i;

	data = dumb_fb_map(fb, drm_fd);
	if (data == MAP_FAILED) {
		return;
	}

	for (i = 0; i < fb->size / sizeof(uint32_t); i++) {
		data[i] = color;
	}

	munmap(data, fb->size);
}
