#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "log.h"
#include "private.h"

/* Plane allocation algorithm
 *
 * Goal: KMS exposes a set of hardware planes, user submitted a set of layers.
 * We want to map as many layers as possible to planes.
 *
 * However, all layers can't be mapped to any plane. There are constraints,
 * sometimes depending on driver-specific limitations or the configuration of
 * other planes.
 *
 * The only way to discover driver-specific limitations is via an atomic test
 * commit: we submit a plane configuration, and KMS replies whether it's
 * supported or not. Thus we need to incrementally build a valid configuration.
 *
 * Let's take an example with 2 planes and 3 layers. Plane 1 is only compatible
 * with layer 2 and plane 2 is only compatible with layer 3. Our algorithm will
 * discover the solution by building the mapping one plane at a time. It first
 * starts with plane 1: an atomic commit assigning layer 1 to plane 1 is
 * submitted. It fails, because this isn't supported by the driver. Then layer
 * 2 is assigned to plane 1 and the atomic test succeeds. We can go on and
 * repeat the operation with plane 2. After exploring the whole tree, we end up
 * with a valid allocation.
 *
 *
 *                    layer 1                 layer 1
 *                  +---------> failure     +---------> failure
 *                  |                       |
 *                  |                       |
 *                  |                       |
 *     +---------+  |          +---------+  |
 *     |         |  | layer 2  |         |  | layer 3   final allocation:
 *     | plane 1 +------------>+ plane 2 +--+---------> plane 1 → layer 2
 *     |         |  |          |         |              plane 2 → layer 3
 *     +---------+  |          +---------+
 *                  |
 *                  |
 *                  | layer 3
 *                  +---------> failure
 *
 *
 * Note how layer 2 isn't considered for plane 2: it's already mapped to plane
 * 1. Also note that branches are pruned as soon as an atomic test fails.
 *
 * In practice, the primary plane is treated separately. This is where layers
 * that can't be mapped to any plane (e.g. layer 1 in our example) will be
 * composited. The primary plane is the first that will be allocated, because
 * some drivers require it to be enabled in order to light up any other plane.
 * Then all other planes will be allocated, from the topmost one to the
 * bottommost one.
 *
 * The "zpos" property (which defines ordering between layers/planes) is handled
 * as a special case. If it's set on layers, it adds additional constraints on
 * their relative ordering. If two layers intersect, their relative zpos needs
 * to be preserved during plane allocation.
 *
 * Implementation-wise, the output_choose_layers function is called at each node
 * of the tree. It iterates over layers, check constraints, performs an atomic
 * test commit and calls itself recursively on the next plane.
 */

/* Global data for the allocation algorithm */
struct alloc_result {
	drmModeAtomicReq *req;
	uint32_t flags;
	size_t planes_len;

	struct liftoff_layer **best;
	int best_score;

	struct timespec started_at;
	int64_t timeout_ns;

	/* per-output */
	bool has_composition_layer;
	size_t non_composition_layers_len;
};

/* Transient data, arguments for each step */
struct alloc_step {
	struct liftoff_list *plane_link; /* liftoff_plane.link */
	size_t plane_idx;

	struct liftoff_layer **alloc; /* only items up to plane_idx are valid */
	int score; /* number of allocated layers */
	int last_layer_zpos;
	int primary_layer_zpos, primary_plane_zpos;

	bool composited; /* per-output */

	char log_prefix[64];
};

static const int64_t NSEC_PER_SEC = 1000 * 1000 * 1000;

static int64_t
timespec_to_nsec(struct timespec ts)
{
	return (int64_t)ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

static const int64_t DEFAULT_ALLOC_TIMEOUT_NSEC = 1000 * 1000; // 1ms

static bool
check_deadline(struct timespec start, int64_t timeout_ns)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		liftoff_log_errno(LIFTOFF_ERROR, "clock_gettime");
		return false;
	}

	return timespec_to_nsec(now) - timeout_ns < timespec_to_nsec(start);
}

static void
plane_step_init_next(struct alloc_step *step, struct alloc_step *prev,
		     struct liftoff_layer *layer)
{
	struct liftoff_plane *plane;
	struct liftoff_layer_property *zpos_prop;
	size_t len;

	plane = liftoff_container_of(prev->plane_link, plane, link);

	step->plane_link = prev->plane_link->next;
	step->plane_idx = prev->plane_idx + 1;
	step->alloc = prev->alloc;
	step->alloc[prev->plane_idx] = layer;

	if (layer != NULL && layer == layer->output->composition_layer) {
		assert(!prev->composited);
		step->composited = true;
	} else {
		step->composited = prev->composited;
	}

	if (layer != NULL && layer != layer->output->composition_layer) {
		step->score = prev->score + 1;
	} else {
		step->score = prev->score;
	}

	zpos_prop = NULL;
	if (layer != NULL) {
		zpos_prop = layer_get_core_property(layer, LIFTOFF_PROP_ZPOS);
	}
	if (zpos_prop != NULL && plane->type != DRM_PLANE_TYPE_PRIMARY) {
		step->last_layer_zpos = zpos_prop->value;
	} else {
		step->last_layer_zpos = prev->last_layer_zpos;
	}
	if (zpos_prop != NULL && plane->type == DRM_PLANE_TYPE_PRIMARY) {
		step->primary_layer_zpos = zpos_prop->value;
		step->primary_plane_zpos = plane->zpos;
	} else {
		step->primary_layer_zpos = prev->primary_layer_zpos;
		step->primary_plane_zpos = prev->primary_plane_zpos;
	}

	if (layer != NULL) {
		len = strlen(prev->log_prefix) + 2;
		if (len > sizeof(step->log_prefix) - 1) {
			len = sizeof(step->log_prefix) - 1;
		}
		memset(step->log_prefix, ' ', len);
		step->log_prefix[len] = '\0';
	} else {
		memcpy(step->log_prefix, prev->log_prefix,
		       sizeof(step->log_prefix));
	}
}

static bool
is_layer_allocated(struct alloc_step *step, struct liftoff_layer *layer)
{
	size_t i;

	/* TODO: speed this up with an array of bools indicating whether a layer
	 * has been allocated */
	for (i = 0; i < step->plane_idx; i++) {
		if (step->alloc[i] == layer) {
			return true;
		}
	}
	return false;
}

static bool
has_composited_layer_over(struct liftoff_output *output,
			  struct alloc_step *step, struct liftoff_layer *layer)
{
	struct liftoff_layer *other_layer;
	struct liftoff_layer_property *zpos_prop, *other_zpos_prop;

	zpos_prop = layer_get_core_property(layer, LIFTOFF_PROP_ZPOS);
	if (zpos_prop == NULL) {
		return false;
	}

	liftoff_list_for_each(other_layer, &output->layers, link) {
		if (is_layer_allocated(step, other_layer)) {
			continue;
		}

		other_zpos_prop = layer_get_core_property(other_layer,
							  LIFTOFF_PROP_ZPOS);
		if (other_zpos_prop == NULL) {
			continue;
		}

		if (layer_intersects(layer, other_layer) &&
		    other_zpos_prop->value > zpos_prop->value) {
			return true;
		}
	}

	return false;
}

static bool
has_allocated_layer_over(struct liftoff_output *output, struct alloc_step *step,
			 struct liftoff_layer *layer)
{
	ssize_t i;
	struct liftoff_plane *other_plane;
	struct liftoff_layer *other_layer;
	struct liftoff_layer_property *zpos_prop, *other_zpos_prop;

	zpos_prop = layer_get_core_property(layer, LIFTOFF_PROP_ZPOS);
	if (zpos_prop == NULL) {
		return false;
	}

	i = -1;
	liftoff_list_for_each(other_plane, &output->device->planes, link) {
		i++;
		if (i >= (ssize_t)step->plane_idx) {
			break;
		}
		if (other_plane->type == DRM_PLANE_TYPE_PRIMARY) {
			continue;
		}

		other_layer = step->alloc[i];
		if (other_layer == NULL) {
			continue;
		}

		other_zpos_prop = layer_get_core_property(other_layer,
							  LIFTOFF_PROP_ZPOS);
		if (other_zpos_prop == NULL) {
			continue;
		}

		/* Since plane zpos is descending, this means the other layer is
		 * supposed to be under but is mapped to a plane over the
		 * current one. */
		if (zpos_prop->value > other_zpos_prop->value &&
		    layer_intersects(layer, other_layer)) {
			return true;
		}
	}

	return false;
}

static bool
has_allocated_plane_under(struct liftoff_output *output,
			  struct alloc_step *step, struct liftoff_layer *layer)
{
	struct liftoff_plane *plane, *other_plane;
	ssize_t i;

	plane = liftoff_container_of(step->plane_link, plane, link);

	i = -1;
	liftoff_list_for_each(other_plane, &output->device->planes, link) {
		i++;
		if (i >= (ssize_t)step->plane_idx) {
			break;
		}
		if (other_plane->type == DRM_PLANE_TYPE_PRIMARY) {
			continue;
		}
		if (step->alloc[i] == NULL) {
			continue;
		}

		if (plane->zpos >= other_plane->zpos &&
		    layer_intersects(layer, step->alloc[i])) {
			return true;
		}
	}

	return false;
}

static bool
check_layer_plane_compatible(struct alloc_step *step,
			     struct liftoff_layer *layer,
			     struct liftoff_plane *plane)
{
	struct liftoff_output *output;
	struct liftoff_layer_property *zpos_prop;

	output = layer->output;

	/* Skip this layer if already allocated */
	if (is_layer_allocated(step, layer)) {
		return false;
	}

	zpos_prop = layer_get_core_property(layer, LIFTOFF_PROP_ZPOS);
	if (zpos_prop != NULL) {
		if ((int)zpos_prop->value > step->last_layer_zpos &&
		    has_allocated_layer_over(output, step, layer)) {
			/* This layer needs to be on top of the last
			 * allocated one */
			liftoff_log(LIFTOFF_DEBUG,
				    "%s Layer %p -> plane %"PRIu32": "
				    "layer zpos invalid",
				    step->log_prefix, (void *)layer, plane->id);
			return false;
		}
		if ((int)zpos_prop->value < step->last_layer_zpos &&
		    has_allocated_plane_under(output, step, layer)) {
			/* This layer needs to be under the last
			 * allocated one, but this plane isn't under the
			 * last one (in practice, since planes are
			 * sorted by zpos it means it has the same zpos,
			 * ie. undefined ordering). */
			liftoff_log(LIFTOFF_DEBUG,
				    "%s Layer %p -> plane %"PRIu32": "
				    "plane zpos invalid",
				    step->log_prefix, (void *)layer, plane->id);
			return false;
		}
		if (plane->type != DRM_PLANE_TYPE_PRIMARY &&
		    (int)zpos_prop->value < step->primary_layer_zpos &&
		    plane->zpos > step->primary_plane_zpos) {
			/* Primary planes are handled up front, because some
			 * drivers fail all atomic commits when it's missing.
			 * However that messes up with our zpos checks. In
			 * particular, we need to make sure we don't put a layer
			 * configured to be over the primary plane under it.
			 * TODO: revisit this once we add underlay support. */
			liftoff_log(LIFTOFF_DEBUG,
				    "%s Layer %p -> plane %"PRIu32": "
				    "layer zpos under primary",
				    step->log_prefix, (void *)layer, plane->id);
			return false;
		}
	}

	if (plane->type != DRM_PLANE_TYPE_PRIMARY &&
	    has_composited_layer_over(output, step, layer)) {
		liftoff_log(LIFTOFF_DEBUG,
			    "%s Layer %p -> plane %"PRIu32": "
			    "has composited layer on top",
			    step->log_prefix, (void *)layer, plane->id);
		return false;
	}

	if (plane->type != DRM_PLANE_TYPE_PRIMARY &&
	    layer == layer->output->composition_layer) {
		liftoff_log(LIFTOFF_DEBUG,
			    "%s Layer %p -> plane %"PRIu32": "
			    "cannot put composition layer on "
			    "non-primary plane",
			    step->log_prefix, (void *)layer, plane->id);
		return false;
	}

	return true;
}

static bool
check_alloc_valid(struct liftoff_output *output, struct alloc_result *result,
		  struct alloc_step *step)
{
	/* If composition isn't used, we need to have allocated all
	 * layers. */
	/* TODO: find a way to fail earlier, e.g. when the number of
	 * layers exceeds the number of planes. */
	if (result->has_composition_layer && !step->composited &&
	    step->score != (int)result->non_composition_layers_len) {
		liftoff_log(LIFTOFF_DEBUG,
			    "%sCannot skip composition: some layers "
			    "are missing a plane", step->log_prefix);
		return false;
	}
	/* On the other hand, if we manage to allocate all layers, we
	 * don't want to use composition. We don't want to use the
	 * composition layer at all. */
	if (step->composited &&
	    step->score == (int)result->non_composition_layers_len) {
		liftoff_log(LIFTOFF_DEBUG,
			    "%sRefusing to use composition: all layers "
			    "have been put in a plane", step->log_prefix);
		return false;
	}

	/* TODO: check allocation isn't empty */

	return true;
}

static bool
check_plane_output_compatible(struct liftoff_plane *plane, struct liftoff_output *output)
{
	return (plane->possible_crtcs & (1 << output->crtc_index)) != 0;
}

static int
count_remaining_compatible_planes(struct liftoff_output *output,
				  struct alloc_step *step)
{
	struct liftoff_list *link;
	struct liftoff_plane *plane;
	int remaining = 0;

	for (link = step->plane_link; link != &output->device->planes; link = link->next) {
		plane = liftoff_container_of(link, plane, link);
		if (plane->layer == NULL &&
		    check_plane_output_compatible(plane, output)) {
			remaining++;
		}
	}

	return remaining;
}

static int
output_choose_layers(struct liftoff_output *output, struct alloc_result *result,
		     struct alloc_step *step)
{
	struct liftoff_device *device;
	struct liftoff_plane *plane;
	struct liftoff_layer *layer;
	int cursor, ret;
	int remaining_planes;
	struct alloc_step next_step = {0};

	device = output->device;

	if (step->plane_link == &device->planes) { /* Allocation finished */
		if (step->score > result->best_score &&
		    check_alloc_valid(output, result, step)) {
			/* We found a better allocation */
			liftoff_log(LIFTOFF_DEBUG,
				    "%sFound a better allocation with score=%d",
				    step->log_prefix, step->score);
			result->best_score = step->score;
			memcpy(result->best, step->alloc,
			       result->planes_len * sizeof(struct liftoff_layer *));
		}
		return 0;
	}

	plane = liftoff_container_of(step->plane_link, plane, link);

	remaining_planes = count_remaining_compatible_planes(output, step);
	if (result->best_score >= step->score + remaining_planes) {
		/* Even if we find a layer for all remaining planes, we won't
		 * find a better allocation. Give up. */
		return 0;
	}

	cursor = drmModeAtomicGetCursor(result->req);

	if (plane->layer != NULL || !check_plane_output_compatible(plane, output)) {
		goto skip;
	}

	liftoff_log(LIFTOFF_DEBUG,
		    "%sPerforming allocation for plane %"PRIu32" (%zu/%zu)",
		    step->log_prefix, plane->id, step->plane_idx + 1, result->planes_len);

	liftoff_list_for_each(layer, &output->layers, link) {
		if (layer->plane != NULL) {
			continue;
		}
		if (!layer_is_visible(layer)) {
			continue;
		}
		if (!check_layer_plane_compatible(step, layer, plane)) {
			continue;
		}

		if (!check_deadline(result->started_at, result->timeout_ns)) {
			liftoff_log(LIFTOFF_DEBUG, "%s Deadline exceeded",
				    step->log_prefix);
			break;
		}

		/* Try to use this layer for the current plane */
		ret = plane_apply(plane, layer, result->req);
		if (ret == -EINVAL) {
			liftoff_log(LIFTOFF_DEBUG,
				    "%s Layer %p -> plane %"PRIu32": "
				    "incompatible properties",
				    step->log_prefix, (void *)layer, plane->id);
			continue;
		} else if (ret != 0) {
			return ret;
		}

		layer_add_candidate_plane(layer, plane);

		/* If composition is forced, wait until after the
		 * layer_add_candidate_plane() call to reject the plane: we want
		 * to return a meaningful list of candidate planes so that the
		 * API user has the opportunity to re-allocate its buffers with
		 * scanout-capable ones. Same deal for the FB check. */
		if (layer->force_composition || !plane_check_layer_fb(plane, layer)) {
			drmModeAtomicSetCursor(result->req, cursor);
			continue;
		}

		ret = device_test_commit(device, result->req, result->flags);
		if (ret == 0) {
			liftoff_log(LIFTOFF_DEBUG,
				    "%s Layer %p -> plane %"PRIu32": success",
				    step->log_prefix, (void *)layer, plane->id);
			/* Continue with the next plane */
			plane_step_init_next(&next_step, step, layer);
			ret = output_choose_layers(output, result, &next_step);
			if (ret != 0) {
				return ret;
			}
		} else if (ret != -EINVAL && ret != -ERANGE && ret != -ENOSPC) {
			return ret;
		} else {
			liftoff_log(LIFTOFF_DEBUG,
				    "%s Layer %p -> plane %"PRIu32": "
				    "test-only commit failed (%s)",
				    step->log_prefix, (void *)layer, plane->id,
				    strerror(-ret));
		}

		drmModeAtomicSetCursor(result->req, cursor);
	}

skip:
	/* Try not to use the current plane */
	plane_step_init_next(&next_step, step, NULL);
	ret = output_choose_layers(output, result, &next_step);
	if (ret != 0) {
		return ret;
	}
	drmModeAtomicSetCursor(result->req, cursor);

	return 0;
}

static int
apply_current(struct liftoff_device *device, drmModeAtomicReq *req)
{
	struct liftoff_plane *plane;
	int cursor, ret;

	cursor = drmModeAtomicGetCursor(req);

	liftoff_list_for_each(plane, &device->planes, link) {
		ret = plane_apply(plane, plane->layer, req);
		if (ret != 0) {
			drmModeAtomicSetCursor(req, cursor);
			return ret;
		}
	}

	return 0;
}

static bool
fb_info_needs_realloc(const drmModeFB2 *a, const drmModeFB2 *b)
{
	if (a->width != b->width || a->height != b->height ||
	    a->pixel_format != b->pixel_format || a->modifier != b->modifier) {
		return true;
	}

	/* TODO: consider checking pitch and offset? */

	return false;
}

static bool
layer_intersection_changed(struct liftoff_layer *this,
			   struct liftoff_output *output)
{
	struct liftoff_layer *other;
	struct liftoff_rect this_cur, this_prev, other_cur, other_prev;

	layer_get_rect(this, &this_cur);
	layer_get_prev_rect(this, &this_prev);
	liftoff_list_for_each(other, &output->layers, link) {
		if (this == other) {
			continue;
		}

		layer_get_rect(other, &other_cur);
		layer_get_prev_rect(other, &other_prev);

		if (rect_intersects(&this_cur, &other_cur) !=
		    rect_intersects(&this_prev, &other_prev)) {
			return true;
		}
	}

	return false;
}

static bool
layer_needs_realloc(struct liftoff_layer *layer, struct liftoff_output *output)
{
	struct liftoff_layer_property *prop;
	bool check_crtc_intersect = false;
	size_t i;

	if (layer->changed) {
		liftoff_log(LIFTOFF_DEBUG, "Cannot re-use previous allocation: "
			    "layer property added or force composition changed");
		return true;
	}

	for (i = 0; i < layer->props_len; i++) {
		prop = &layer->props[i];

		/* If FB_ID changes from non-zero to zero, we don't need to
		 * display this layer anymore, so we may be able to re-use its
		 * plane for another layer. If FB_ID changes from zero to
		 * non-zero, we might be able to find a plane for this layer.
		 * If FB_ID changes from non-zero to non-zero and the FB
		 * attributes didn't change, we can try to re-use the previous
		 * allocation. */
		if (prop->core_index == LIFTOFF_PROP_FB_ID) {
			if (prop->value == 0 && prop->prev_value == 0) {
				continue;
			}

			if (prop->value == 0 || prop->prev_value == 0) {
				liftoff_log(LIFTOFF_DEBUG, "Cannot re-use previous allocation: "
					    "layer enabled or disabled");
				return true;
			}

			if (fb_info_needs_realloc(&layer->fb_info,
						  &layer->prev_fb_info)) {
				liftoff_log(LIFTOFF_DEBUG, "Cannot re-use previous allocation: "
					    "FB info changed");
				return true;
			}

			continue;
		}

		/* For all properties except FB_ID, we can skip realloc if the
		 * value didn't change. */
		if (prop->value == prop->prev_value) {
			continue;
		}

		/* If the layer was or becomes completely transparent or
		 * completely opaque, we might be able to find a better
		 * allocation. Otherwise, we can keep the current one. */
		if (prop->core_index == LIFTOFF_PROP_ALPHA) {
			if (prop->value == 0 || prop->prev_value == 0 ||
			    prop->value == 0xFFFF || prop->prev_value == 0xFFFF) {
				liftoff_log(LIFTOFF_DEBUG, "Cannot re-use previous allocation: "
					    "alpha changed");
				return true;
			}
			continue;
		}

		/* We should never need a re-alloc when IN_FENCE_FD or
		 * FB_DAMAGE_CLIPS changes. */
		if (strcmp(prop->name, "IN_FENCE_FD") == 0 ||
		    strcmp(prop->name, "FB_DAMAGE_CLIPS") == 0) {
			continue;
		}

		/* If CRTC_* changed, check for intersection later */
		if (strcmp(prop->name, "CRTC_X") == 0 ||
		    strcmp(prop->name, "CRTC_Y") == 0 ||
		    strcmp(prop->name, "CRTC_W") == 0 ||
		    strcmp(prop->name, "CRTC_H") == 0) {
			check_crtc_intersect = true;
			continue;
		}

		liftoff_log(LIFTOFF_DEBUG, "Cannot re-use previous allocation: "
			    "property \"%s\" changed", prop->name);
		return true;
	}

	if (check_crtc_intersect &&
	    layer_intersection_changed(layer, output)) {
		liftoff_log(LIFTOFF_DEBUG, "Cannot re-use previous allocation: "
			    "intersection with other layer(s) changed");
		return true;
	}

	return false;
}

static bool
layer_is_higher_priority(struct liftoff_layer *this, struct liftoff_layer *other)
{
	struct liftoff_layer_property *this_zpos, *other_zpos;
	bool this_visible, other_visible, intersects;

	// The composition layer should be highest priority.
	if (this->output->composition_layer == this) {
		return true;
	} else if (this->output->composition_layer == other) {
		return false;
	}

	// Invisible layers are given lowest priority. Pass-thru if both have
	// same visibility
	this_visible = layer_is_visible(this);
	other_visible = layer_is_visible(other);
	if (this_visible != other_visible) {
		return this_visible;
	}

	// A layer's overall priority is determined by a combination of it's
	// current_priority, it's zpos, and whether it intersects with others.
	//
	// Consider two layers. If they do not intersect, the layer with higher
	// priority is given overall priority. However if both layers have
	// identical priority, then the layer with higher zpos is given overall
	// priority.
	//
	// If the layers intersect, their zpos determines the overall priority.
	// If their zpos are identical, then simply fallback to looking at
	// current_priority. Otherwise, the layer with higher zpos is given
	// overall priority, since the top layer needs to be offloaded in order
	// to offload the bottom layer.

	this_zpos = layer_get_core_property(this, LIFTOFF_PROP_ZPOS);
	other_zpos = layer_get_core_property(other, LIFTOFF_PROP_ZPOS);
	intersects = layer_intersects(this, other);

	if (this_zpos != NULL && other_zpos != NULL) {
		if (intersects) {
			return this_zpos->value == other_zpos->value ?
			       this->current_priority > other->current_priority :
			       this_zpos->value > other_zpos->value;
		} else {
			return this->current_priority == other->current_priority ?
			       this_zpos->value > other_zpos->value :
			       this->current_priority > other->current_priority;
		}
	} else if (this_zpos == NULL && other_zpos == NULL) {
		return this->current_priority > other->current_priority;
	} else {
		// Either this or other zpos is null
		return this_zpos != NULL;
	}
}

static bool
update_layers_order(struct liftoff_output *output)
{
	struct liftoff_list *search, *max, *cur, *head;
	struct liftoff_layer *this_layer, *other_layer;
	bool order_changed = false;

	head = &output->layers;
	cur = head;

	// Run a insertion sort to order layers by priority.
	while (cur->next != head) {
		cur = cur->next;

		max = cur;
		search = cur;
		while (search->next != head) {
			search = search->next;
			this_layer = liftoff_container_of(search, this_layer, link);
			other_layer = liftoff_container_of(max, other_layer, link);
			if (layer_is_higher_priority(this_layer, other_layer)) {
				max = search;
			}
		}

		if (cur != max) {
			liftoff_list_swap(cur, max);
			// max is now where iterator cur was, relocate to continue
			cur = max;
			order_changed = true;
		}
	}

	return order_changed;
}

static int
reuse_previous_alloc(struct liftoff_output *output, drmModeAtomicReq *req,
		     uint32_t flags)
{
	struct liftoff_device *device;
	struct liftoff_layer *layer;
	int cursor, ret;
	bool layer_order_changed;

	device = output->device;

	layer_order_changed = update_layers_order(output);

	if (output->layers_changed) {
		liftoff_log(LIFTOFF_DEBUG, "Cannot re-use previous allocation: "
			    "a layer has been added or removed");
		return -EINVAL;
	}

	liftoff_list_for_each(layer, &output->layers, link) {
		if (layer_needs_realloc(layer, output)) {
			return -EINVAL;
		}
	}

	if (layer_order_changed) {
		liftoff_log(LIFTOFF_DEBUG, "Cannot re-use previous allocation: "
			    "layer priority order changed.");
		return -EINVAL;
	}

	cursor = drmModeAtomicGetCursor(req);

	ret = apply_current(device, req);
	if (ret != 0) {
		return ret;
	}

	ret = device_test_commit(device, req, flags);
	if (ret != 0) {
		drmModeAtomicSetCursor(req, cursor);
	}
	return ret;
}

static void
mark_layers_clean(struct liftoff_output *output)
{
	struct liftoff_layer *layer;

	output->layers_changed = false;

	liftoff_list_for_each(layer, &output->layers, link) {
		layer_mark_clean(layer);
	}
}

static void
update_layers_priority(struct liftoff_device *device)
{
	struct liftoff_output *output;
	struct liftoff_layer *layer;
	bool period_elapsed;

	device->page_flip_counter++;
	period_elapsed = device->page_flip_counter >= LIFTOFF_PRIORITY_PERIOD;
	if (period_elapsed) {
		device->page_flip_counter = 0;
	}

	liftoff_list_for_each(output, &device->outputs, link) {
		liftoff_list_for_each(layer, &output->layers, link) {
			layer_update_priority(layer, period_elapsed);
		}
	}
}

static void
update_layers_fb_info(struct liftoff_output *output)
{
	struct liftoff_layer *layer;

	/* We don't know what the library user did in-between
	 * liftoff_output_apply() calls. They might've removed the FB and
	 * re-created a completely different one which happens to have the same
	 * FB ID. */
	liftoff_list_for_each(layer, &output->layers, link) {
		layer->fb_info = (drmModeFB2){0};

		layer_cache_fb_info(layer);
		/* TODO: propagate error? */
	}
}

static void
log_reuse(struct liftoff_output *output)
{
	if (output->alloc_reused_counter == 0) {
		liftoff_log(LIFTOFF_DEBUG,
			    "Reusing previous plane allocation on output %"PRIu32,
			    output->crtc_id);
	}
	output->alloc_reused_counter++;
}

static void
log_no_reuse(struct liftoff_output *output)
{
	liftoff_log(LIFTOFF_DEBUG, "Computing plane allocation on output %"PRIu32,
		    output->crtc_id);

	if (output->alloc_reused_counter != 0) {
		liftoff_log(LIFTOFF_DEBUG,
			    "Stopped reusing previous plane allocation on "
			    "output %"PRIu32" (had reused it %d times)",
			    output->crtc_id, output->alloc_reused_counter);
		output->alloc_reused_counter = 0;
	}
}

static size_t
non_composition_layers_length(struct liftoff_output *output)
{
	struct liftoff_layer *layer;
	size_t n;

	n = 0;
	liftoff_list_for_each(layer, &output->layers, link) {
		if (layer_is_visible(layer) &&
		    output->composition_layer != layer) {
			n++;
		}
	}

	return n;
}

int
liftoff_output_apply(struct liftoff_output *output, drmModeAtomicReq *req,
		     uint32_t flags,
		     const struct liftoff_output_apply_options *options)
{
	struct liftoff_device *device;
	struct liftoff_plane *plane;
	struct liftoff_layer *layer;
	struct alloc_result result = {0};
	struct alloc_step step = {0};
	const struct liftoff_output_apply_options default_options = {0};
	size_t i, candidate_planes;
	int ret;
	bool found_layer;

	if (options == NULL) {
		options = &default_options;
	}

	device = output->device;

	update_layers_priority(device);
	update_layers_fb_info(output);

	ret = reuse_previous_alloc(output, req, flags);
	if (ret == 0) {
		log_reuse(output);
		mark_layers_clean(output);
		return 0;
	}
	log_no_reuse(output);

	/* Reset layers' candidate planes */
	liftoff_list_for_each(layer, &output->layers, link) {
		layer_reset_candidate_planes(layer);
	}

	device->test_commit_counter = 0;
	output_log_layers(output);

	/* Unset all existing plane and layer mappings. */
	liftoff_list_for_each(plane, &device->planes, link) {
		if (plane->layer != NULL && plane->layer->output == output) {
			plane->layer->plane = NULL;
			plane->layer = NULL;
		}
	}

	/* Disable all planes we might use. Do it before building mappings to
	 * make sure not to hit bandwidth limits because too many planes are
	 * enabled. */
	candidate_planes = 0;
	liftoff_list_for_each(plane, &device->planes, link) {
		if (plane->layer == NULL) {
			candidate_planes++;
			liftoff_log(LIFTOFF_DEBUG,
				    "Disabling plane %"PRIu32, plane->id);
			ret = plane_apply(plane, NULL, req);
			assert(ret != -EINVAL);
			if (ret != 0) {
				return ret;
			}
		}
	}

	result.req = req;
	result.flags = flags;
	result.planes_len = liftoff_list_length(&device->planes);

	step.alloc = malloc(result.planes_len * sizeof(step.alloc[0]));
	result.best = malloc(result.planes_len * sizeof(result.best[0]));
	if (step.alloc == NULL || result.best == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "malloc");
		return -ENOMEM;
	}

	if (clock_gettime(CLOCK_MONOTONIC, &result.started_at) != 0) {
		liftoff_log_errno(LIFTOFF_ERROR, "clock_gettime");
		return -errno;
	}

	result.timeout_ns = options->timeout_ns;
	if (result.timeout_ns == 0) {
		result.timeout_ns = DEFAULT_ALLOC_TIMEOUT_NSEC;
	}

	/* For each plane, try to find a layer. Don't do it the other
	 * way around (ie. for each layer, try to find a plane) because
	 * some drivers want user-space to enable the primary plane
	 * before any other plane. */

	result.best_score = -1;
	memset(result.best, 0, result.planes_len * sizeof(result.best[0]));
	result.has_composition_layer = output->composition_layer != NULL;
	result.non_composition_layers_len =
		non_composition_layers_length(output);
	step.plane_link = device->planes.next;
	step.plane_idx = 0;
	step.score = 0;
	step.last_layer_zpos = INT_MAX;
	step.primary_layer_zpos = INT_MIN;
	step.primary_plane_zpos = INT_MAX;
	step.composited = false;
	ret = output_choose_layers(output, &result, &step);
	if (ret != 0) {
		return ret;
	}

	liftoff_log(LIFTOFF_DEBUG,
		    "Found plane allocation for output %"PRIu32" "
		    "(score: %d, candidate planes: %zu, tests: %d):",
		    output->crtc_id, result.best_score, candidate_planes,
		    device->test_commit_counter);

	/* Apply the best allocation */
	i = 0;
	found_layer = false;
	liftoff_list_for_each(plane, &device->planes, link) {
		layer = result.best[i];
		i++;
		if (layer == NULL) {
			continue;
		}

		liftoff_log(LIFTOFF_DEBUG, "  Layer %p -> plane %"PRIu32,
			    (void *)layer, plane->id);

		assert(plane->layer == NULL);
		assert(layer->plane == NULL);
		plane->layer = layer;
		layer->plane = plane;

		found_layer = true;
	}
	if (!found_layer) {
		liftoff_log(LIFTOFF_DEBUG, "  (No layer has a plane)");
	}

	ret = apply_current(device, req);
	if (ret != 0) {
		return ret;
	}

	free(step.alloc);
	free(result.best);

	mark_layers_clean(output);

	return 0;
}
