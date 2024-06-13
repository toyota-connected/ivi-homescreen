#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drm.h>
#include <sys/types.h>
#include "private.h"

struct liftoff_layer *
liftoff_layer_create(struct liftoff_output *output)
{
	struct liftoff_layer *layer;
	size_t i;

	layer = calloc(1, sizeof(*layer));
	if (layer == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "calloc");
		return NULL;
	}
	layer->output = output;
	layer->candidate_planes = calloc(output->device->planes_cap,
					 sizeof(layer->candidate_planes[0]));
	if (layer->candidate_planes == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "calloc");
		free(layer);
		return NULL;
	}
	for (i = 0; i < LIFTOFF_PROP_LAST; i++) {
		layer->core_props[i] = -1;
	}
	liftoff_list_insert(output->layers.prev, &layer->link);
	output->layers_changed = true;
	return layer;
}

void
liftoff_layer_destroy(struct liftoff_layer *layer)
{
	if (layer == NULL) {
		return;
	}

	layer->output->layers_changed = true;
	if (layer->plane != NULL) {
		layer->plane->layer = NULL;
	}
	if (layer->output->composition_layer == layer) {
		layer->output->composition_layer = NULL;
	}
	free(layer->props);
	free(layer->candidate_planes);
	liftoff_list_remove(&layer->link);
	free(layer);
}

struct liftoff_layer_property *
layer_get_core_property(struct liftoff_layer *layer, enum liftoff_core_property prop)
{
	ssize_t i;

	i = layer->core_props[prop];
	if (i < 0) {
		return NULL;
	}
	return &layer->props[i];
}

struct liftoff_layer_property *
layer_get_property(struct liftoff_layer *layer, const char *name)
{
	ssize_t core_prop_idx;
	size_t i;

	core_prop_idx = core_property_index(name);
	if (core_prop_idx >= 0) {
		return layer_get_core_property(layer, core_prop_idx);
	}

	for (i = 0; i < layer->props_len; i++) {
		if (strcmp(layer->props[i].name, name) == 0) {
			return &layer->props[i];
		}
	}
	return NULL;
}

int
liftoff_layer_set_property(struct liftoff_layer *layer, const char *name,
			   uint64_t value)
{
	struct liftoff_layer_property *props;
	struct liftoff_layer_property *prop;
	size_t i;

	if (strcmp(name, "CRTC_ID") == 0) {
		liftoff_log(LIFTOFF_ERROR,
			    "refusing to set a layer's CRTC_ID");
		return -EINVAL;
	}

	prop = layer_get_property(layer, name);
	if (prop == NULL) {
		props = realloc(layer->props,
				(layer->props_len + 1) * sizeof(props[0]));
		if (props == NULL) {
			liftoff_log_errno(LIFTOFF_ERROR, "realloc");
			return -ENOMEM;
		}
		layer->props = props;
		layer->props_len++;

		i = layer->props_len - 1;
		prop = &layer->props[i];
		*prop = (struct liftoff_layer_property){0};
		strncpy(prop->name, name, sizeof(prop->name) - 1);
		prop->core_index = core_property_index(name);

		layer->changed = true;

		if (prop->core_index >= 0) {
			layer->core_props[prop->core_index] = (ssize_t)i;
		}
	}

	prop->value = value;

	if (prop->core_index == LIFTOFF_PROP_FB_ID && layer->force_composition) {
		layer->force_composition = false;
		layer->changed = true;
	}

	return 0;
}

void
liftoff_layer_unset_property(struct liftoff_layer *layer, const char *name)
{
	struct liftoff_layer_property *prop, *last;

	prop = layer_get_property(layer, name);
	if (prop == NULL) {
		return;
	}

	if (prop->core_index >= 0) {
		layer->core_props[prop->core_index] = -1;
	}

	last = &layer->props[layer->props_len - 1];
	if (prop != last) {
		*prop = *last;
		if (last->core_index >= 0) {
			layer->core_props[last->core_index] = prop - layer->props;
		}
	}
	*last = (struct liftoff_layer_property){0};
	layer->props_len--;

	layer->changed = true;
}

void
liftoff_layer_set_fb_composited(struct liftoff_layer *layer)
{
	if (layer->force_composition) {
		return;
	}

	liftoff_layer_set_property(layer, "FB_ID", 0);

	layer->force_composition = true;
	layer->changed = true;
}

struct liftoff_plane *
liftoff_layer_get_plane(struct liftoff_layer *layer)
{
	return layer->plane;
}

bool
liftoff_layer_needs_composition(struct liftoff_layer *layer)
{
	if (!layer_is_visible(layer)) {
		return false;
	}
	return layer->plane == NULL;
}

void
layer_get_rect(struct liftoff_layer *layer, struct liftoff_rect *rect)
{
	struct liftoff_layer_property *x_prop, *y_prop, *w_prop, *h_prop;

	x_prop = layer_get_core_property(layer, LIFTOFF_PROP_CRTC_X);
	y_prop = layer_get_core_property(layer, LIFTOFF_PROP_CRTC_Y);
	w_prop = layer_get_core_property(layer, LIFTOFF_PROP_CRTC_W);
	h_prop = layer_get_core_property(layer, LIFTOFF_PROP_CRTC_H);

	rect->x = x_prop != NULL ? x_prop->value : 0;
	rect->y = y_prop != NULL ? y_prop->value : 0;
	rect->width = w_prop != NULL ? w_prop->value : 0;
	rect->height = h_prop != NULL ? h_prop->value : 0;
}

void
layer_get_prev_rect(struct liftoff_layer *layer, struct liftoff_rect *rect)
{
	struct liftoff_layer_property *x_prop, *y_prop, *w_prop, *h_prop;

	x_prop = layer_get_core_property(layer, LIFTOFF_PROP_CRTC_X);
	y_prop = layer_get_core_property(layer, LIFTOFF_PROP_CRTC_Y);
	w_prop = layer_get_core_property(layer, LIFTOFF_PROP_CRTC_W);
	h_prop = layer_get_core_property(layer, LIFTOFF_PROP_CRTC_H);

	rect->x = x_prop != NULL ? x_prop->prev_value : 0;
	rect->y = y_prop != NULL ? y_prop->prev_value : 0;
	rect->width = w_prop != NULL ? w_prop->prev_value : 0;
	rect->height = h_prop != NULL ? h_prop -> prev_value : 0;
}

bool
rect_intersects(struct liftoff_rect *ra, struct liftoff_rect *rb)
{
	return ra->x < rb->x + rb->width && ra->y < rb->y + rb->height &&
	       ra->x + ra->width > rb->x && ra->y + ra->height > rb->y;
}

bool
layer_intersects(struct liftoff_layer *a, struct liftoff_layer *b)
{
	struct liftoff_rect ra, rb;

	if (!layer_is_visible(a) || !layer_is_visible(b)) {
		return false;
	}

	layer_get_rect(a, &ra);
	layer_get_rect(b, &rb);

	return rect_intersects(&ra, &rb);
}

void
layer_mark_clean(struct liftoff_layer *layer)
{
	size_t i;

	layer->changed = false;
	layer->prev_fb_info = layer->fb_info;

	for (i = 0; i < layer->props_len; i++) {
		layer->props[i].prev_value = layer->props[i].value;
	}
}

static void
log_priority(struct liftoff_layer *layer)
{
	if (layer->current_priority == layer->pending_priority) {
		return;
	}

	liftoff_log(LIFTOFF_DEBUG, "Layer %p priority change: %d -> %d",
		    (void *)layer, layer->current_priority,
		    layer->pending_priority);
}

void
layer_update_priority(struct liftoff_layer *layer, bool make_current)
{
	struct liftoff_layer_property *prop;

	/* TODO: also bump priority when updating other properties */
	prop = layer_get_core_property(layer, LIFTOFF_PROP_FB_ID);
	if (prop != NULL && prop->prev_value != prop->value) {
		layer->pending_priority++;
	}

	if (make_current) {
		log_priority(layer);
		layer->current_priority = layer->pending_priority;
		layer->pending_priority = 0;
	}
}

bool
layer_has_fb(struct liftoff_layer *layer)
{
	struct liftoff_layer_property *fb_id_prop;

	fb_id_prop = layer_get_core_property(layer, LIFTOFF_PROP_FB_ID);
	return fb_id_prop != NULL && fb_id_prop->value != 0;
}

bool
layer_is_visible(struct liftoff_layer *layer)
{
	struct liftoff_layer_property *alpha_prop;

	alpha_prop = layer_get_core_property(layer, LIFTOFF_PROP_ALPHA);
	if (alpha_prop != NULL && alpha_prop->value == 0) {
		return false; /* fully transparent */
	}

	if (layer->force_composition) {
		return true;
	} else {
		return layer_has_fb(layer);
	}
}

int
layer_cache_fb_info(struct liftoff_layer *layer)
{
	struct liftoff_layer_property *fb_id_prop;
	drmModeFB2 *fb_info;
	size_t i, j, num_planes;
	int ret;

	fb_id_prop = layer_get_core_property(layer, LIFTOFF_PROP_FB_ID);
	if (fb_id_prop == NULL || fb_id_prop->value == 0) {
		layer->fb_info = (drmModeFB2){0};
		return 0;
	}

	if (layer->fb_info.fb_id == fb_id_prop->value) {
		return 0;
	}

	fb_info = drmModeGetFB2(layer->output->device->drm_fd, fb_id_prop->value);
	if (fb_info == NULL) {
		if (errno == EINVAL) {
			return 0; /* old kernel */
		}
		return -errno;
	}

	/* drmModeGetFB2() always creates new GEM handles -- close these, we
	 * won't use them and we don't want to leak them */
	num_planes = sizeof(fb_info->handles) / sizeof(fb_info->handles[0]);
	for (i = 0; i < num_planes; i++) {
		if (fb_info->handles[i] == 0) {
			continue;
		}

		ret = drmCloseBufferHandle(layer->output->device->drm_fd,
					   fb_info->handles[i]);
		if (ret != 0) {
			liftoff_log_errno(LIFTOFF_ERROR, "drmCloseBufferHandle");
			continue;
		}

		/* Make sure we don't double-close a handle */
		for (j = i + 1; j < num_planes; j++) {
			if (fb_info->handles[j] == fb_info->handles[i]) {
				fb_info->handles[j] = 0;
			}
		}
		fb_info->handles[i] = 0;
	}

	layer->fb_info = *fb_info;
	drmModeFreeFB2(fb_info);
	return 0;
}

bool
liftoff_layer_is_candidate_plane(struct liftoff_layer *layer,
				 struct liftoff_plane *plane)
{
	size_t i;

	for (i = 0; i < layer->output->device->planes_cap; i++) {
		if (layer->candidate_planes[i] == plane->id) {
			return true;
		}
	}

	return false;
}

void
layer_add_candidate_plane(struct liftoff_layer *layer,
			  struct liftoff_plane *plane)
{
	size_t i;
	ssize_t empty_slot = -1;

	for (i = 0; i < layer->output->device->planes_cap; i++) {
		if (layer->candidate_planes[i] == plane->id) {
			return;
		}
		if (empty_slot < 0 && layer->candidate_planes[i] == 0) {
			empty_slot = (ssize_t)i;
		}
	}

	assert(empty_slot >= 0);
	layer->candidate_planes[empty_slot] = plane->id;
}

void
layer_reset_candidate_planes(struct liftoff_layer *layer)
{
	memset(layer->candidate_planes, 0,
	       sizeof(layer->candidate_planes[0]) * layer->output->device->planes_cap);
}
