#include <assert.h>
#include <unistd.h>
#include <libliftoff.h>
#include <stdio.h>
#include <string.h>
#include "libdrm_mock.h"

static struct liftoff_layer *
add_layer(struct liftoff_output *output, int x, int y, int width, int height)
{
	uint32_t fb_id;
	struct liftoff_layer *layer;

	layer = liftoff_layer_create(output);
	fb_id = liftoff_mock_drm_create_fb(layer);
	liftoff_layer_set_property(layer, "FB_ID", fb_id);
	liftoff_layer_set_property(layer, "CRTC_X", (uint64_t)x);
	liftoff_layer_set_property(layer, "CRTC_Y", (uint64_t)y);
	liftoff_layer_set_property(layer, "CRTC_W", (uint64_t)width);
	liftoff_layer_set_property(layer, "CRTC_H", (uint64_t)height);
	liftoff_layer_set_property(layer, "SRC_X", 0);
	liftoff_layer_set_property(layer, "SRC_Y", 0);
	liftoff_layer_set_property(layer, "SRC_W", (uint64_t)width << 16);
	liftoff_layer_set_property(layer, "SRC_H", (uint64_t)height << 16);

	return layer;
}

static void
test_basic(void)
{
	struct liftoff_mock_plane *mock_plane_ok, *mock_plane_ko;
	int drm_fd;
	struct liftoff_device *device;
	struct liftoff_output *output;
	struct liftoff_layer *layer;
	struct liftoff_plane *plane_ok, *plane_ko;
	drmModeAtomicReq *req;
	drmModePropertyRes prop = {0};
	int ret;

	mock_plane_ok = liftoff_mock_drm_create_plane(DRM_PLANE_TYPE_OVERLAY);
	mock_plane_ko = liftoff_mock_drm_create_plane(DRM_PLANE_TYPE_OVERLAY);

	/* Only add the COLOR_RANGE property to mock_plane_ok only. libliftoff
	 * should mark that one as a possible candidate, but not the other
	 * one. */
	strncpy(prop.name, "COLOR_RANGE", sizeof(prop.name) - 1);
	liftoff_mock_plane_add_property(mock_plane_ok, &prop, 0);

	drm_fd = liftoff_mock_drm_open();
	device = liftoff_device_create(drm_fd);
	assert(device != NULL);

	plane_ok = liftoff_plane_create(device, liftoff_mock_plane_get_id(mock_plane_ok));
	plane_ko = liftoff_plane_create(device, liftoff_mock_plane_get_id(mock_plane_ko));

	output = liftoff_output_create(device, liftoff_mock_drm_crtc_id);
	layer = add_layer(output, 0, 0, 1920, 1080);
	liftoff_layer_set_property(layer, "COLOR_RANGE", 0);

	req = drmModeAtomicAlloc();
	ret = liftoff_output_apply(output, req, 0, NULL);
	assert(ret == 0);
	ret = drmModeAtomicCommit(drm_fd, req, 0, NULL);
	assert(ret == 0);
	assert(liftoff_mock_plane_get_layer(mock_plane_ok) == NULL);
	assert(liftoff_mock_plane_get_layer(mock_plane_ko) == NULL);
	assert(liftoff_layer_is_candidate_plane(layer, plane_ok));
	assert(!liftoff_layer_is_candidate_plane(layer, plane_ko));
	drmModeAtomicFree(req);

	liftoff_device_destroy(device);
	close(drm_fd);
}

int
main(int argc, char *argv[])
{
	const char *test_name;

	liftoff_log_set_priority(LIFTOFF_DEBUG);

	if (argc != 2) {
		fprintf(stderr, "usage: %s <test-name>\n", argv[0]);
		return 1;
	}
	test_name = argv[1];

	if (strcmp(test_name, "basic") == 0) {
		test_basic();
	} else {
		fprintf(stderr, "no such test: %s\n", test_name);
		return 1;
	}

	return 0;
}
