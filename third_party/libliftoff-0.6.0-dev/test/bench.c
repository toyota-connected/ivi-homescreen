#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <libliftoff.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "libdrm_mock.h"
#include "log.h"

#define MAX_PLANES 128
#define MAX_LAYERS 128

static struct liftoff_layer *
add_layer(struct liftoff_output *output, int x, int y, uint32_t width, uint32_t height)
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

int
main(int argc, char *argv[])
{
	int opt;
	size_t planes_len, layers_len;
	struct timespec start, end;
	struct liftoff_mock_plane *mock_planes[MAX_PLANES];
	size_t i, j;
	uint64_t plane_type;
	int drm_fd;
	struct liftoff_device *device;
	struct liftoff_output *output;
	struct liftoff_layer *layers[MAX_LAYERS];
	drmModeAtomicReq *req;
	int ret;
	double dur_ms;

	planes_len = 5;
	layers_len = 10;
	while ((opt = getopt(argc, argv, "p:l:")) != -1) {
		switch (opt) {
		case 'p':
			planes_len = (size_t)atoi(optarg);
			break;
		case 'l':
			layers_len = (size_t)atoi(optarg);
			break;
		default:
			fprintf(stderr, "usage: %s [-p planes] [-l layers]\n",
				argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	liftoff_log_set_priority(LIFTOFF_SILENT);
	liftoff_mock_verbose = false;

	for (i = 0; i < planes_len; i++) {
		plane_type = i == 0 ? DRM_PLANE_TYPE_PRIMARY :
				      DRM_PLANE_TYPE_OVERLAY;
		mock_planes[i] = liftoff_mock_drm_create_plane(plane_type);
	}

	drm_fd = liftoff_mock_drm_open();
	device = liftoff_device_create(drm_fd);
	assert(device != NULL);

	liftoff_device_register_all_planes(device);

	output = liftoff_output_create(device, liftoff_mock_drm_crtc_id);

	for (i = 0; i < layers_len; i++) {
		/* Planes don't intersect, so the library can arrange them in
		 * any order. Testing all combinations takes more time. */
		layers[i] = add_layer(output, (int)i * 100, (int)i * 100, 100, 100);
		for (j = 0; j < planes_len; j++) {
			if (j == 1) {
				/* Make the lowest plane above the primary plane
				 * incompatible with all layers. A solution
				 * using all planes won't be reached, so the
				 * library will keep trying more combinations.
				 */
				continue;
			}
			liftoff_mock_plane_add_compatible_layer(mock_planes[j],
								layers[i]);
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &start);

	req = drmModeAtomicAlloc();
	ret = liftoff_output_apply(output, req, 0, &(struct liftoff_output_apply_options){
		.timeout_ns = INT64_MAX,
	});
	assert(ret == 0);
	drmModeAtomicFree(req);

	clock_gettime(CLOCK_MONOTONIC, &end);

	dur_ms = ((double)end.tv_sec - (double)start.tv_sec) * 1000 +
		 ((double)end.tv_nsec - (double)start.tv_nsec) / 1000000;
	printf("Plane allocation took %fms\n", dur_ms);
	printf("Plane allocation performed %zu atomic test commits\n",
	       liftoff_mock_commit_count);
	/* TODO: the mock libdrm library takes time to check atomic requests.
	 * This benchmark doesn't account for time spent in the mock library. */
	printf("With 20µs per atomic test commit, plane allocation would take "
	       "%fms\n", dur_ms + liftoff_mock_commit_count * 0.02);

	liftoff_device_destroy(device);
	close(drm_fd);
}
