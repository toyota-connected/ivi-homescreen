#include <assert.h>
#include <drm_fourcc.h>
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
commit(int drm_fd, struct liftoff_output *output)
{
	drmModeAtomicReq *req;
	int ret;

	req = drmModeAtomicAlloc();
	ret = liftoff_output_apply(output, req, 0, NULL);
	assert(ret == 0);
	ret = drmModeAtomicCommit(drm_fd, req, 0, NULL);
	assert(ret == 0);
	drmModeAtomicFree(req);
}

static int
test_prop_default(const char *prop_name)
{
	struct liftoff_mock_plane *mock_plane_with_prop,
				  *mock_plane_without_prop;
	int drm_fd;
	struct liftoff_device *device;
	struct liftoff_output *output;
	struct liftoff_layer *layer;
	uint64_t require_prop_value, default_value;
	drmModePropertyRes prop = {0};

	mock_plane_without_prop = liftoff_mock_drm_create_plane(DRM_PLANE_TYPE_OVERLAY);
	mock_plane_with_prop = liftoff_mock_drm_create_plane(DRM_PLANE_TYPE_OVERLAY);

	/* require_prop_value is a value that requires the prop for the plane
	 * allocation to succeed. default_value is a value that requires the
	 * prop for the plane allocation to succeed. */
	if (strcmp(prop_name, "alpha") == 0) {
		require_prop_value = (uint16_t)(0.5 * 0xFFFF);
		default_value = 0xFFFF; /* opaque */
	} else if (strcmp(prop_name, "rotation") == 0) {
		require_prop_value = DRM_MODE_ROTATE_180;
		default_value = DRM_MODE_ROTATE_0;
	} else {
		fprintf(stderr, "no such test: default-%s\n", prop_name);
		return 1;
	}

	/* We need to setup mock plane properties before creating the liftoff
	 * device */
	strncpy(prop.name, prop_name, sizeof(prop.name) - 1);
	liftoff_mock_plane_add_property(mock_plane_with_prop, &prop, 0);

	drm_fd = liftoff_mock_drm_open();
	device = liftoff_device_create(drm_fd);
	assert(device != NULL);

	liftoff_device_register_all_planes(device);

	output = liftoff_output_create(device, liftoff_mock_drm_crtc_id);
	layer = add_layer(output, 0, 0, 1920, 1080);

	liftoff_mock_plane_add_compatible_layer(mock_plane_without_prop, layer);

	/* First test that the layer doesn't get assigned to the plane without
	 * the prop when using a non-default value */
	liftoff_layer_set_property(layer, prop.name, require_prop_value);
	commit(drm_fd, output);
	assert(liftoff_layer_get_plane(layer) == NULL);

	/* The layer should get assigned to the plane without the prop when
	 * using the default value */
	liftoff_layer_set_property(layer, prop.name, default_value);
	commit(drm_fd, output);
	assert(liftoff_layer_get_plane(layer) != NULL);

	/* The layer should get assigned to the plane with the prop when using
	 * a non-default value */
	liftoff_mock_plane_add_compatible_layer(mock_plane_with_prop, layer);
	liftoff_layer_set_property(layer, prop.name, require_prop_value);
	commit(drm_fd, output);
	assert(liftoff_layer_get_plane(layer) != NULL);

	liftoff_device_destroy(device);
	close(drm_fd);

	return 0;
}

/* Checks that a fully transparent layer is ignored. */
static int
test_ignore_alpha(void)
{
	struct liftoff_mock_plane *mock_plane;
	drmModePropertyRes prop = {0};
	int drm_fd;
	struct liftoff_device *device;
	struct liftoff_output *output;
	struct liftoff_layer *layer;

	mock_plane = liftoff_mock_drm_create_plane(DRM_PLANE_TYPE_PRIMARY);

	strncpy(prop.name, "alpha", sizeof(prop.name) - 1);
	liftoff_mock_plane_add_property(mock_plane, &prop, 0);

	drm_fd = liftoff_mock_drm_open();
	device = liftoff_device_create(drm_fd);
	assert(device != NULL);

	liftoff_device_register_all_planes(device);

	output = liftoff_output_create(device, liftoff_mock_drm_crtc_id);
	layer = add_layer(output, 0, 0, 1920, 1080);
	liftoff_layer_set_property(layer, "alpha", 0); /* fully transparent */

	liftoff_mock_plane_add_compatible_layer(mock_plane, layer);

	commit(drm_fd, output);
	assert(liftoff_mock_plane_get_layer(mock_plane) == NULL);
	assert(!liftoff_layer_needs_composition(layer));

	liftoff_device_destroy(device);
	close(drm_fd);

	return 0;
}

static int
test_immutable_zpos(void)
{
	struct liftoff_mock_plane *mock_plane1, *mock_plane2;
	drmModePropertyRes prop = {0};
	int drm_fd;
	struct liftoff_device *device;
	struct liftoff_output *output;
	struct liftoff_layer *layer1, *layer2;

	mock_plane1 = liftoff_mock_drm_create_plane(DRM_PLANE_TYPE_OVERLAY);
	mock_plane2 = liftoff_mock_drm_create_plane(DRM_PLANE_TYPE_OVERLAY);

	strncpy(prop.name, "zpos", sizeof(prop.name) - 1);
	prop.flags = DRM_MODE_PROP_IMMUTABLE;

	/* Plane 2 is always on top of plane 1, and this is immutable */
	liftoff_mock_plane_add_property(mock_plane1, &prop, 1);
	liftoff_mock_plane_add_property(mock_plane2, &prop, 2);

	drm_fd = liftoff_mock_drm_open();
	device = liftoff_device_create(drm_fd);
	assert(device != NULL);

	liftoff_device_register_all_planes(device);

	output = liftoff_output_create(device, liftoff_mock_drm_crtc_id);
	layer1 = add_layer(output, 0, 0, 256, 256);
	layer2 = add_layer(output, 128, 128, 256, 256);

	/* All layers are compatible with all planes */
	liftoff_mock_plane_add_compatible_layer(mock_plane1, layer1);
	liftoff_mock_plane_add_compatible_layer(mock_plane1, layer2);
	liftoff_mock_plane_add_compatible_layer(mock_plane2, layer1);
	liftoff_mock_plane_add_compatible_layer(mock_plane2, layer2);

	/* Layer 2 on top of layer 1 */
	liftoff_layer_set_property(layer1, "zpos", 42);
	liftoff_layer_set_property(layer2, "zpos", 43);

	commit(drm_fd, output);
	assert(liftoff_mock_plane_get_layer(mock_plane1) == layer1);
	assert(liftoff_mock_plane_get_layer(mock_plane2) == layer2);

	/* Layer 1 on top of layer 2 */
	liftoff_layer_set_property(layer1, "zpos", 43);
	liftoff_layer_set_property(layer2, "zpos", 42);

	commit(drm_fd, output);
	assert(liftoff_mock_plane_get_layer(mock_plane1) == layer2);
	assert(liftoff_mock_plane_get_layer(mock_plane2) == layer1);

	liftoff_device_destroy(device);
	close(drm_fd);

	return 0;
}

static int
test_unmatched_prop(void)
{
	struct liftoff_mock_plane *mock_plane;
	int drm_fd;
	struct liftoff_device *device;
	struct liftoff_output *output;
	struct liftoff_layer *layer;

	mock_plane = liftoff_mock_drm_create_plane(DRM_PLANE_TYPE_PRIMARY);

	drm_fd = liftoff_mock_drm_open();
	device = liftoff_device_create(drm_fd);
	assert(device != NULL);

	liftoff_device_register_all_planes(device);

	output = liftoff_output_create(device, liftoff_mock_drm_crtc_id);
	layer = add_layer(output, 0, 0, 1920, 1080);
	liftoff_layer_set_property(layer, "asdf", 0); /* doesn't exist */

	liftoff_mock_plane_add_compatible_layer(mock_plane, layer);

	commit(drm_fd, output);
	assert(liftoff_mock_plane_get_layer(mock_plane) == NULL);

	liftoff_device_destroy(device);
	close(drm_fd);

	return 0;
}

static int
test_unset_prop(void)
{
	struct liftoff_mock_plane *mock_plane;
	int drm_fd;
	struct liftoff_device *device;
	struct liftoff_output *output;
	struct liftoff_layer *layer;

	mock_plane = liftoff_mock_drm_create_plane(DRM_PLANE_TYPE_PRIMARY);

	drm_fd = liftoff_mock_drm_open();
	device = liftoff_device_create(drm_fd);
	assert(device != NULL);

	liftoff_device_register_all_planes(device);

	output = liftoff_output_create(device, liftoff_mock_drm_crtc_id);
	layer = add_layer(output, 0, 0, 1920, 1080);
	liftoff_layer_set_property(layer, "asdf", 0); /* doesn't exist */
	liftoff_layer_set_property(layer, "alpha", 0xFFFF);

	liftoff_mock_plane_add_compatible_layer(mock_plane, layer);

	commit(drm_fd, output);
	assert(liftoff_mock_plane_get_layer(mock_plane) == NULL);

	liftoff_layer_unset_property(layer, "asdf");

	commit(drm_fd, output);
	assert(liftoff_mock_plane_get_layer(mock_plane) == layer);

	liftoff_device_destroy(device);
	close(drm_fd);

	return 0;
}

static int
test_ignored_prop(const char *name)
{
	struct liftoff_mock_plane *mock_plane;
	int drm_fd;
	struct liftoff_device *device;
	struct liftoff_output *output;
	struct liftoff_layer *layer;

	mock_plane = liftoff_mock_drm_create_plane(DRM_PLANE_TYPE_PRIMARY);

	drm_fd = liftoff_mock_drm_open();
	device = liftoff_device_create(drm_fd);
	assert(device != NULL);

	liftoff_device_register_all_planes(device);

	output = liftoff_output_create(device, liftoff_mock_drm_crtc_id);
	layer = add_layer(output, 0, 0, 1920, 1080);
	liftoff_layer_set_property(layer, name, 0);

	liftoff_mock_plane_add_compatible_layer(mock_plane, layer);

	commit(drm_fd, output);
	assert(liftoff_mock_plane_get_layer(mock_plane) == layer);

	liftoff_device_destroy(device);
	close(drm_fd);

	return 0;
}

struct single_format_modifier_blob {
	struct drm_format_modifier_blob base;
	uint32_t formats[1];
	struct drm_format_modifier modifiers[1];
};

static int
test_in_formats(void)
{
	struct liftoff_mock_plane *mock_plane;
	int drm_fd;
	struct liftoff_device *device;
	struct liftoff_output *output;
	struct liftoff_layer *layer;
	struct single_format_modifier_blob in_formats;
	uint32_t fb_id;
	drmModeFB2 fb_info;

	/* Create an IN_FORMATS property which only supports
	 * DRM_FORMAT_ARGB8888 + DRM_FORMAT_MOD_LINEAR */
	in_formats = (struct single_format_modifier_blob) {
		.base = {
			.version = 1,
			.count_formats = 1,
			.formats_offset = offsetof(struct single_format_modifier_blob, formats),
			.count_modifiers = 1,
			.modifiers_offset = offsetof(struct single_format_modifier_blob, modifiers),
		},
		.formats = { DRM_FORMAT_ARGB8888 },
		.modifiers = {
			{ .formats = 0x01, .modifier = DRM_FORMAT_MOD_LINEAR },
		},
	};

	mock_plane = liftoff_mock_drm_create_plane(DRM_PLANE_TYPE_PRIMARY);
	liftoff_mock_plane_add_in_formats(mock_plane, &in_formats.base, sizeof(in_formats));

	drm_fd = liftoff_mock_drm_open();
	device = liftoff_device_create(drm_fd);
	assert(device != NULL);

	liftoff_device_register_all_planes(device);

	output = liftoff_output_create(device, liftoff_mock_drm_crtc_id);
	layer = add_layer(output, 0, 0, 1920, 1080);
	fb_id = liftoff_mock_drm_create_fb(layer);
	fb_info = (drmModeFB2) {
		.fb_id = fb_id,
		.width = 1920,
		.height = 1080,
		.flags = DRM_MODE_FB_MODIFIERS,
		.pixel_format = DRM_FORMAT_ARGB8888,
		.modifier = I915_FORMAT_MOD_X_TILED,
	};
	liftoff_mock_drm_set_fb_info(&fb_info);
	liftoff_layer_set_property(layer, "FB_ID", fb_id);

	liftoff_mock_plane_add_compatible_layer(mock_plane, layer);

	/* First commit: even if the layer is compatible with the plane,
	 * libliftoff shouldn't try to use the plane because the FB modifier
	 * isn't in IN_FORMATS */
	commit(drm_fd, output);
	assert(liftoff_mock_plane_get_layer(mock_plane) == NULL);

	fb_id = liftoff_mock_drm_create_fb(layer);
	fb_info.fb_id = fb_id;
	fb_info.modifier = DRM_FORMAT_MOD_LINEAR;
	liftoff_mock_drm_set_fb_info(&fb_info);
	liftoff_layer_set_property(layer, "FB_ID", fb_id);

	/* Second commit: the new FB modifier is in IN_FORMATS */
	commit(drm_fd, output);
	assert(liftoff_mock_plane_get_layer(mock_plane) == layer);

	liftoff_device_destroy(device);
	close(drm_fd);

	return 0;
}

static int
test_type(const drmModePropertyRes *prop, uint64_t valid_value,
	  uint64_t invalid_value)
{
	struct liftoff_mock_plane *mock_plane;
	int drm_fd;
	struct liftoff_device *device;
	struct liftoff_output *output;
	struct liftoff_layer *layer;

	mock_plane = liftoff_mock_drm_create_plane(DRM_PLANE_TYPE_PRIMARY);
	liftoff_mock_plane_add_property(mock_plane, prop, 0);

	drm_fd = liftoff_mock_drm_open();
	device = liftoff_device_create(drm_fd);
	assert(device != NULL);

	liftoff_device_register_all_planes(device);

	output = liftoff_output_create(device, liftoff_mock_drm_crtc_id);
	layer = add_layer(output, 0, 0, 1920, 1080);

	liftoff_mock_plane_add_compatible_layer(mock_plane, layer);

	/* First commit with a valid prop value */
	liftoff_layer_set_property(layer, prop->name, valid_value);
	commit(drm_fd, output);
	assert(liftoff_mock_plane_get_layer(mock_plane) != NULL);

	/* Second commit with an invalid prop value: even if the plane is
	 * compatible with the layer, the invalid value should be checked by
	 * libliftoff and rejected before any test commit happens */
	liftoff_layer_set_property(layer, prop->name, invalid_value);
	commit(drm_fd, output);
	assert(liftoff_mock_plane_get_layer(mock_plane) == NULL);

	liftoff_device_destroy(device);
	close(drm_fd);

	return 0;
}

static int
test_range(void)
{
	drmModePropertyRes prop = {0};
	uint64_t range[2];

	range[0] = 0;
	range[1] = 0xFFFF;
	strncpy(prop.name, "alpha", sizeof(prop.name) - 1);
	prop.flags = DRM_MODE_PROP_RANGE;
	prop.count_values = 2;
	prop.values = range;

	return test_type(&prop, 42, 0xFFFF + 42);
}

static int
test_signed_range(void)
{
	drmModePropertyRes prop = {0};
	uint64_t range[2];

	range[0] = (uint64_t)-1;
	range[1] = INT64_MAX;
	strncpy(prop.name, "IN_FENCE_FD", sizeof(prop.name) - 1);
	prop.flags = DRM_MODE_PROP_SIGNED_RANGE;
	prop.count_values = 2;
	prop.values = range;

	return test_type(&prop, (uint64_t)-1, (uint64_t)-2);
}

static int
test_enum(void)
{
	drmModePropertyRes prop = {0};
	struct drm_mode_property_enum enums[3] = {0};

	enums[0].value = 0;
	strncpy(enums[0].name, "rotate-0", sizeof(enums[0].name) - 1);
	enums[1].value = 1;
	strncpy(enums[1].name, "rotate-90", sizeof(enums[1].name) - 1);
	enums[2].value = 2;
	strncpy(enums[2].name, "rotate-180", sizeof(enums[2].name) - 1);
	strncpy(prop.name, "rotation", sizeof(prop.name) - 1);
	prop.flags = DRM_MODE_PROP_BITMASK;
	prop.enums = enums;
	prop.count_enums = 3;

	return test_type(&prop, DRM_MODE_ROTATE_90, DRM_MODE_REFLECT_X);
}

static int
test_bitmask(void)
{
	drmModePropertyRes prop = {0};
	struct drm_mode_property_enum enums[2] = {0};

	enums[0].value = 0;
	strncpy(enums[0].name, "YCbCr limited range", sizeof(enums[0].name) - 1);
	enums[1].value = 1;
	strncpy(enums[1].name, "YCbCr full range", sizeof(enums[1].name) - 1);
	strncpy(prop.name, "COLOR_RANGE", sizeof(prop.name) - 1);
	prop.flags = DRM_MODE_PROP_ENUM;
	prop.enums = enums;
	prop.count_enums = 2;

	return test_type(&prop, 1, 2);
}

int
main(int argc, char *argv[])
{
	const char *test_name, *default_test_prefix;

	liftoff_log_set_priority(LIFTOFF_DEBUG);

	if (argc != 2) {
		fprintf(stderr, "usage: %s <test-name>\n", argv[0]);
		return 1;
	}
	test_name = argv[1];

	default_test_prefix = "default-";
	if (strncmp(test_name, default_test_prefix,
	    strlen(default_test_prefix)) == 0) {
		return test_prop_default(test_name + strlen(default_test_prefix));
	} else if (strcmp(test_name, "ignore-alpha") == 0) {
		return test_ignore_alpha();
	} else if (strcmp(test_name, "immutable-zpos") == 0) {
		return test_immutable_zpos();
	} else if (strcmp(test_name, "unmatched") == 0) {
		return test_unmatched_prop();
	} else if (strcmp(test_name, "unset") == 0) {
		return test_unset_prop();
	} else if (strcmp(test_name, "in-formats") == 0) {
		return test_in_formats();
	} else if (strcmp(test_name, "fb-damage-clips") == 0) {
		return test_ignored_prop("FB_DAMAGE_CLIPS");
	} else if (strcmp(test_name, "range") == 0) {
		return test_range();
	} else if (strcmp(test_name, "signed-range") == 0) {
		return test_signed_range();
	} else if (strcmp(test_name, "enum") == 0) {
		return test_enum();
	} else if (strcmp(test_name, "bitmask") == 0) {
		return test_bitmask();
	} else {
		fprintf(stderr, "no such test: %s\n", test_name);
		return 1;
	}
}
