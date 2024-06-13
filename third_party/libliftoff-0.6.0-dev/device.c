#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "log.h"
#include "private.h"

struct liftoff_device *
liftoff_device_create(int drm_fd)
{
	struct liftoff_device *device;
	drmModeRes *drm_res;
	drmModePlaneRes *drm_plane_res;

	device = calloc(1, sizeof(*device));
	if (device == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "calloc");
		return NULL;
	}

	liftoff_list_init(&device->planes);
	liftoff_list_init(&device->outputs);

	device->drm_fd = dup(drm_fd);
	if (device->drm_fd < 0) {
		liftoff_log_errno(LIFTOFF_ERROR, "dup");
		liftoff_device_destroy(device);
		return NULL;
	}

	drm_res = drmModeGetResources(drm_fd);
	if (drm_res == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "drmModeGetResources");
		liftoff_device_destroy(device);
		return NULL;
	}

	device->crtcs_len = (size_t)drm_res->count_crtcs;
	device->crtcs = malloc(device->crtcs_len * sizeof(device->crtcs[0]));
	if (device->crtcs == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "malloc");
		drmModeFreeResources(drm_res);
		liftoff_device_destroy(device);
		return NULL;
	}
	memcpy(device->crtcs, drm_res->crtcs, device->crtcs_len * sizeof(device->crtcs[0]));

	drmModeFreeResources(drm_res);

	drm_plane_res = drmModeGetPlaneResources(device->drm_fd);
	if (drm_plane_res == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "drmModeGetPlaneResources");
		liftoff_device_destroy(device);
		return NULL;
	}
	device->planes_cap = drm_plane_res->count_planes;
	drmModeFreePlaneResources(drm_plane_res);

	return device;
}

void
liftoff_device_destroy(struct liftoff_device *device)
{
	struct liftoff_plane *plane, *tmp;

	if (device == NULL) {
		return;
	}

	close(device->drm_fd);
	liftoff_list_for_each_safe(plane, tmp, &device->planes, link) {
		liftoff_plane_destroy(plane);
	}
	free(device->crtcs);
	free(device);
}

int
liftoff_device_register_all_planes(struct liftoff_device *device)
{
	drmModePlaneRes *drm_plane_res;
	uint32_t i;

	drm_plane_res = drmModeGetPlaneResources(device->drm_fd);
	if (drm_plane_res == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "drmModeGetPlaneResources");
		return -errno;
	}

	for (i = 0; i < drm_plane_res->count_planes; i++) {
		if (liftoff_plane_create(device, drm_plane_res->planes[i]) == NULL) {
			return -errno;
		}
	}
	drmModeFreePlaneResources(drm_plane_res);

	return 0;
}

int
device_test_commit(struct liftoff_device *device, drmModeAtomicReq *req,
		   uint32_t flags)
{
	int ret;

	device->test_commit_counter++;

	flags &= ~(uint32_t)DRM_MODE_PAGE_FLIP_EVENT;
	do {
		ret = drmModeAtomicCommit(device->drm_fd, req,
					  DRM_MODE_ATOMIC_TEST_ONLY | flags,
					  NULL);
	} while (ret == -EINTR || ret == -EAGAIN);

	/* The kernel will return -EINVAL for invalid configuration, -ERANGE for
	 * CRTC coords overflow, and -ENOSPC for invalid SRC coords. */
	if (ret != 0 && ret != -EINVAL && ret != -ERANGE && ret != -ENOSPC) {
		liftoff_log(LIFTOFF_ERROR, "drmModeAtomicCommit: %s",
			    strerror(-ret));
	}

	return ret;
}

ssize_t
core_property_index(const char *name)
{
	if (strcmp(name, "FB_ID") == 0) {
		return LIFTOFF_PROP_FB_ID;
	} else if (strcmp(name, "CRTC_ID") == 0) {
		return LIFTOFF_PROP_CRTC_ID;
	} else if (strcmp(name, "CRTC_X") == 0) {
		return LIFTOFF_PROP_CRTC_X;
	} else if (strcmp(name, "CRTC_Y") == 0) {
		return LIFTOFF_PROP_CRTC_Y;
	} else if (strcmp(name, "CRTC_W") == 0) {
		return LIFTOFF_PROP_CRTC_W;
	} else if (strcmp(name, "CRTC_H") == 0) {
		return LIFTOFF_PROP_CRTC_H;
	} else if (strcmp(name, "SRC_X") == 0) {
		return LIFTOFF_PROP_SRC_X;
	} else if (strcmp(name, "SRC_Y") == 0) {
		return LIFTOFF_PROP_SRC_Y;
	} else if (strcmp(name, "SRC_W") == 0) {
		return LIFTOFF_PROP_SRC_W;
	} else if (strcmp(name, "SRC_H") == 0) {
		return LIFTOFF_PROP_SRC_H;
	} else if (strcmp(name, "zpos") == 0) {
		return LIFTOFF_PROP_ZPOS;
	} else if (strcmp(name, "alpha") == 0) {
		return LIFTOFF_PROP_ALPHA;
	} else if (strcmp(name, "rotation") == 0) {
		return LIFTOFF_PROP_ROTATION;
	}
	return -1;
}
