#ifndef PRIVATE_H
#define PRIVATE_H

#include <libliftoff.h>
#include <sys/types.h>
#include "list.h"
#include "log.h"

/* Layer priority is assigned depending on the number of updates during a
 * given number of page-flips */
#define LIFTOFF_PRIORITY_PERIOD 60

/**
 * List of well-known KMS properties.
 *
 * Keep core_property_index() in sync.
 */
enum liftoff_core_property {
	LIFTOFF_PROP_FB_ID,
	LIFTOFF_PROP_CRTC_ID,
	LIFTOFF_PROP_CRTC_X,
	LIFTOFF_PROP_CRTC_Y,
	LIFTOFF_PROP_CRTC_W,
	LIFTOFF_PROP_CRTC_H,
	LIFTOFF_PROP_SRC_X,
	LIFTOFF_PROP_SRC_Y,
	LIFTOFF_PROP_SRC_W,
	LIFTOFF_PROP_SRC_H,
	LIFTOFF_PROP_ZPOS,
	LIFTOFF_PROP_ALPHA,
	LIFTOFF_PROP_ROTATION,
	LIFTOFF_PROP_LAST, /* keep last */
};

struct liftoff_device {
	int drm_fd;

	struct liftoff_list planes; /* liftoff_plane.link */
	struct liftoff_list outputs; /* liftoff_output.link */

	uint32_t *crtcs;
	size_t crtcs_len;

	size_t planes_cap; /* max number of planes */

	int page_flip_counter;
	int test_commit_counter;
};

struct liftoff_output {
	struct liftoff_device *device;
	uint32_t crtc_id;
	size_t crtc_index;
	struct liftoff_list link; /* liftoff_device.outputs */

	struct liftoff_layer *composition_layer;

	struct liftoff_list layers; /* liftoff_layer.link */
	/* layer added or removed, or composition layer changed */
	bool layers_changed;

	int alloc_reused_counter;
};

struct liftoff_layer {
	struct liftoff_output *output;
	struct liftoff_list link; /* liftoff_output.layers */

	struct liftoff_layer_property *props;
	size_t props_len;
	ssize_t core_props[LIFTOFF_PROP_LAST]; /* indices into the props array */

	bool force_composition; /* FB needs to be composited */

	struct liftoff_plane *plane;

	/* Array of plane IDs with a length of liftoff_device.planes_cap */
	uint32_t *candidate_planes;

	int current_priority, pending_priority;
	/* prop added or force_composition changed */
	bool changed;
	drmModeFB2 fb_info, prev_fb_info; /* cached FB info */
};

struct liftoff_layer_property {
	char name[DRM_PROP_NAME_LEN];
	uint64_t value, prev_value;
	ssize_t core_index;
};

struct liftoff_plane {
	uint32_t id;
	uint32_t possible_crtcs;
	uint32_t type;
	int zpos; /* greater values mean closer to the eye */
	struct liftoff_list link; /* liftoff_device.planes */

	drmModePropertyRes **props;
	size_t props_len;
	drmModePropertyBlobRes *in_formats_blob;
	const drmModePropertyRes *core_props[LIFTOFF_PROP_LAST];

	struct liftoff_layer *layer;
};

struct liftoff_rect {
	int x, y;
	int width, height;
};

int
device_test_commit(struct liftoff_device *device, drmModeAtomicReq *req,
		   uint32_t flags);

struct liftoff_layer_property *
layer_get_property(struct liftoff_layer *layer, const char *name);

struct liftoff_layer_property *
layer_get_core_property(struct liftoff_layer *layer, enum liftoff_core_property prop);

void
layer_get_rect(struct liftoff_layer *layer, struct liftoff_rect *rect);

void
layer_get_prev_rect(struct liftoff_layer *layer, struct liftoff_rect *rect);

bool
rect_intersects(struct liftoff_rect *a, struct liftoff_rect *b);

bool
layer_intersects(struct liftoff_layer *a, struct liftoff_layer *b);

void
layer_mark_clean(struct liftoff_layer *layer);

void
layer_update_priority(struct liftoff_layer *layer, bool make_current);

bool
layer_has_fb(struct liftoff_layer *layer);

void
layer_add_candidate_plane(struct liftoff_layer *layer,
			  struct liftoff_plane *plane);

void
layer_reset_candidate_planes(struct liftoff_layer *layer);

bool
layer_is_visible(struct liftoff_layer *layer);

int
layer_cache_fb_info(struct liftoff_layer *layer);

int
plane_apply(struct liftoff_plane *plane, struct liftoff_layer *layer,
	    drmModeAtomicReq *req);

bool
plane_check_layer_fb(struct liftoff_plane *plane, struct liftoff_layer *layer);

void
output_log_layers(struct liftoff_output *output);

ssize_t
core_property_index(const char *name);

#endif
