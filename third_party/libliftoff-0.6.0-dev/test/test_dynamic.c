#include <assert.h>
#include <drm_fourcc.h>
#include <unistd.h>
#include <libliftoff.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "libdrm_mock.h"

struct context {
	int drm_fd;
	struct liftoff_output *output;
	struct liftoff_mock_plane *mock_plane;
	struct liftoff_layer *layer, *other_layer;
	size_t commit_count;
};

struct test_case {
	const char *name;
	void (*run)(struct context *ctx);
};

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
first_commit(struct context *ctx)
{
	drmModeAtomicReq *req;
	int ret;

	assert(ctx->commit_count == 0);

	req = drmModeAtomicAlloc();
	ret = liftoff_output_apply(ctx->output, req, 0, NULL);
	assert(ret == 0);
	ret = drmModeAtomicCommit(ctx->drm_fd, req, 0, NULL);
	assert(ret == 0);
	drmModeAtomicFree(req);

	ctx->commit_count = liftoff_mock_commit_count;
	/* We need to check whether the library can re-use old configurations
	 * with a single atomic commit. If we don't have enough planes/layers,
	 * the library will find a plane allocation in a single commit and we
	 * won't be able to tell the difference between a re-use and a complete
	 * run. */
	assert(ctx->commit_count > 1);
}

static void
second_commit(struct context *ctx, bool want_reuse_prev_alloc)
{
	drmModeAtomicReq *req;
	int ret;

	req = drmModeAtomicAlloc();
	ret = liftoff_output_apply(ctx->output, req, 0, NULL);
	assert(ret == 0);
	if (want_reuse_prev_alloc) {
		/* The library should perform only one TEST_ONLY commit with the
		 * previous plane allocation. */
		assert(liftoff_mock_commit_count == ctx->commit_count + 1);
	} else {
		/* Since there are at least two planes, the library should
		 * perform more than one TEST_ONLY commit. */
		assert(liftoff_mock_commit_count > ctx->commit_count + 1);
	}
	ret = drmModeAtomicCommit(ctx->drm_fd, req, 0, NULL);
	assert(ret == 0);
	drmModeAtomicFree(req);
}

static void
run_same(struct context *ctx)
{
	first_commit(ctx);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);

	second_commit(ctx, true);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);
}

static void
run_change_fb(struct context *ctx)
{
	uint32_t fb_id;
	drmModeFB2 fb_info;

	fb_id = liftoff_mock_drm_create_fb(ctx->layer);
	fb_info = (drmModeFB2) {
		.fb_id = fb_id,
		.width = 1920,
		.height = 1080,
		.flags = DRM_MODE_FB_MODIFIERS,
		.pixel_format = DRM_FORMAT_ARGB8888,
		.modifier = DRM_FORMAT_MOD_LINEAR,
	};
	liftoff_mock_drm_set_fb_info(&fb_info);
	liftoff_layer_set_property(ctx->layer, "FB_ID", fb_id);

	first_commit(ctx);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);

	/* Create a new FB with the exact same FB info as the first one. */
	fb_id = liftoff_mock_drm_create_fb(ctx->layer);
	fb_info.fb_id = fb_id;
	liftoff_mock_drm_set_fb_info(&fb_info);
	liftoff_layer_set_property(ctx->layer, "FB_ID", fb_id);

	second_commit(ctx, true);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);
}

static void
run_change_fb_modifier(struct context *ctx)
{
	uint32_t fb_id;
	drmModeFB2 fb_info;

	fb_id = liftoff_mock_drm_create_fb(ctx->layer);
	fb_info = (drmModeFB2) {
		.fb_id = fb_id,
		.width = 1920,
		.height = 1080,
		.flags = DRM_MODE_FB_MODIFIERS,
		.pixel_format = DRM_FORMAT_ARGB8888,
		.modifier = I915_FORMAT_MOD_Y_TILED,
	};
	liftoff_mock_drm_set_fb_info(&fb_info);
	liftoff_layer_set_property(ctx->layer, "FB_ID", fb_id);

	first_commit(ctx);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);

	/* Simulate the situation where the previous FB gets removed, and a new
	 * one gets re-created with the same FB ID but a different modifier.
	 * This should prevent the first configuration from being re-used. */
	fb_info.modifier = I915_FORMAT_MOD_X_TILED;
	liftoff_mock_drm_set_fb_info(&fb_info);
	liftoff_layer_set_property(ctx->layer, "FB_ID", fb_id);

	second_commit(ctx, false);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);
}

static void
run_unset_fb(struct context *ctx)
{
	first_commit(ctx);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);

	liftoff_layer_set_property(ctx->layer, "FB_ID", 0);

	second_commit(ctx, false);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == NULL);
}

static void
run_set_fb(struct context *ctx)
{
	liftoff_layer_set_property(ctx->layer, "FB_ID", 0);
	first_commit(ctx);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == NULL);

	liftoff_layer_set_property(ctx->layer, "FB_ID",
				   liftoff_mock_drm_create_fb(ctx->layer));

	second_commit(ctx, false);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);
}

static void
run_add_layer(struct context *ctx)
{
	first_commit(ctx);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);

	add_layer(ctx->output, 0, 0, 256, 256);

	second_commit(ctx, false);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);
}

static void
run_remove_layer(struct context *ctx)
{
	first_commit(ctx);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);

	liftoff_layer_destroy(ctx->other_layer);
	ctx->other_layer = NULL;

	second_commit(ctx, false);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);
}

static void
run_change_composition_layer(struct context *ctx)
{
	first_commit(ctx);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);

	liftoff_output_set_composition_layer(ctx->output, ctx->layer);

	second_commit(ctx, false);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);
}

static void
run_change_alpha(struct context *ctx)
{
	liftoff_layer_set_property(ctx->layer, "alpha", 42);

	first_commit(ctx);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);

	liftoff_layer_set_property(ctx->layer, "alpha", 43);

	second_commit(ctx, true);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);
}

static void
run_set_alpha_from_opaque(struct context *ctx)
{
	liftoff_layer_set_property(ctx->layer, "alpha", 0xFFFF); /* opaque */

	first_commit(ctx);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);

	liftoff_layer_set_property(ctx->layer, "alpha", 42);

	second_commit(ctx, false);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);
}

static void
run_set_alpha_from_transparent(struct context *ctx)
{
	liftoff_layer_set_property(ctx->layer, "alpha", 0); /* transparent */

	first_commit(ctx);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == NULL);

	liftoff_layer_set_property(ctx->layer, "alpha", 42);

	second_commit(ctx, false);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);
}

static void
run_unset_alpha_to_opaque(struct context *ctx)
{
	liftoff_layer_set_property(ctx->layer, "alpha", 42);

	first_commit(ctx);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);

	liftoff_layer_set_property(ctx->layer, "alpha", 0xFFFF); /* opaque */

	second_commit(ctx, false);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);
}

static void
run_unset_alpha_to_transparent(struct context *ctx)
{
	liftoff_layer_set_property(ctx->layer, "alpha", 42);

	first_commit(ctx);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);

	liftoff_layer_set_property(ctx->layer, "alpha", 0); /* transparent */

	second_commit(ctx, false);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == NULL);
}

static void
run_change_position_same_intersection(struct context *ctx)
{

	first_commit(ctx);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);

	liftoff_layer_set_property(ctx->other_layer, "CRTC_X", 1);

	second_commit(ctx, true);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);
}

static void
run_change_position_different_intersection(struct context *ctx)
{

	first_commit(ctx);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);

	liftoff_layer_set_property(ctx->other_layer, "CRTC_X", 2000);
	liftoff_layer_set_property(ctx->other_layer, "CRTC_Y", 2000);

	second_commit(ctx, false);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);
}

static void
run_change_in_fence_fd(struct context *ctx)
{
	liftoff_layer_set_property(ctx->layer, "IN_FENCE_FD", 42);

	first_commit(ctx);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);

	liftoff_layer_set_property(ctx->layer, "IN_FENCE_FD", 43);

	second_commit(ctx, true);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);
}

static void
run_change_fb_damage_clips(struct context *ctx)
{
	liftoff_layer_set_property(ctx->layer, "FB_DAMAGE_CLIPS", 42);

	first_commit(ctx);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);

	liftoff_layer_set_property(ctx->layer, "FB_DAMAGE_CLIPS", 43);

	second_commit(ctx, true);
	assert(liftoff_mock_plane_get_layer(ctx->mock_plane) == ctx->layer);
}

static const struct test_case tests[] = {
	{ .name = "same", .run = run_same },
	{ .name = "change-fb", .run = run_change_fb },
	{ .name = "change-fb-modifier", .run = run_change_fb_modifier },
	{ .name = "unset-fb", .run = run_unset_fb },
	{ .name = "set-fb", .run = run_set_fb },
	{ .name = "add-layer", .run = run_add_layer },
	{ .name = "remove-layer", .run = run_remove_layer },
	{ .name = "change-composition-layer", .run = run_change_composition_layer },
	{ .name = "change-alpha", .run = run_change_alpha },
	{ .name = "set-alpha-from-opaque", .run = run_set_alpha_from_opaque },
	{ .name = "set-alpha-from-transparent", .run = run_set_alpha_from_transparent },
	{ .name = "change-position-same-intersection", .run = run_change_position_same_intersection },
	{ .name = "change-position-different-intersection", .run = run_change_position_different_intersection },
	{ .name = "unset-alpha-to-opaque", .run = run_unset_alpha_to_opaque },
	{ .name = "unset-alpha-to-transparent", .run = run_unset_alpha_to_transparent },
	{ .name = "change-in-fence-fd", .run = run_change_in_fence_fd },
	{ .name = "change-fb-damage-clips", .run = run_change_fb_damage_clips },
};

static void
run(const struct test_case *test)
{
	struct context ctx = {0};
	struct liftoff_device *device;
	const char *prop_name;
	drmModePropertyRes prop;

	/* Always create two planes: a primary plane only compatible with
	 * `layer`, and a cursor plane incompatible with any layer. Always
	 * create 3 layers: `layer`, `other_layer`, and an unnamed third layer.
	 */

	ctx.mock_plane = liftoff_mock_drm_create_plane(DRM_PLANE_TYPE_PRIMARY);
	/* Plane incompatible with all layers */
	liftoff_mock_drm_create_plane(DRM_PLANE_TYPE_CURSOR);

	prop_name = "alpha";
	prop = (drmModePropertyRes){0};
	strncpy(prop.name, prop_name, sizeof(prop.name) - 1);
	liftoff_mock_plane_add_property(ctx.mock_plane, &prop, 0);

	prop_name = "IN_FENCE_FD";
	prop = (drmModePropertyRes){0};
	strncpy(prop.name, prop_name, sizeof(prop.name) - 1);
	liftoff_mock_plane_add_property(ctx.mock_plane, &prop, (uint64_t)-1);

	prop_name = "FB_DAMAGE_CLIPS";
	prop = (drmModePropertyRes){0};
	strncpy(prop.name, prop_name, sizeof(prop.name) - 1);
	liftoff_mock_plane_add_property(ctx.mock_plane, &prop, 0);

	ctx.drm_fd = liftoff_mock_drm_open();
	device = liftoff_device_create(ctx.drm_fd);
	assert(device != NULL);

	liftoff_device_register_all_planes(device);

	ctx.output = liftoff_output_create(device, liftoff_mock_drm_crtc_id);
	ctx.layer = add_layer(ctx.output, 0, 0, 1920, 1080);
	/* Layers incompatible with all planes */
	ctx.other_layer = add_layer(ctx.output, 0, 0, 256, 256);
	add_layer(ctx.output, 0, 0, 256, 256);

	liftoff_mock_plane_add_compatible_layer(ctx.mock_plane, ctx.layer);

	test->run(&ctx);

	liftoff_device_destroy(device);
	close(ctx.drm_fd);
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

	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		if (strcmp(test_name, tests[i].name) == 0) {
			run(&tests[i]);
			return 0;
		}
	}

	fprintf(stderr, "no such test: %s\n", test_name);
	return 1;
}
