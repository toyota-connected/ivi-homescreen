#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "private.h"

static int
guess_plane_zpos_from_type(struct liftoff_device *device, uint32_t plane_id,
			   uint32_t type)
{
	struct liftoff_plane *primary;

	/* From far to close to the eye: primary, overlay, cursor. Unless
	 * the overlay ID < primary ID. */
	switch (type) {
	case DRM_PLANE_TYPE_PRIMARY:
		return 0;
	case DRM_PLANE_TYPE_CURSOR:
		return 2;
	case DRM_PLANE_TYPE_OVERLAY:
		if (liftoff_list_empty(&device->planes)) {
			return 0; /* No primary plane, shouldn't happen */
		}
		primary = liftoff_container_of(device->planes.next,
					       primary, link);
		if (plane_id < primary->id) {
			return -1;
		} else {
			return 1;
		}
	}
	return 0;
}

struct liftoff_plane *
liftoff_plane_create(struct liftoff_device *device, uint32_t id)
{
	struct liftoff_plane *plane, *cur;
	drmModePlane *drm_plane;
	drmModeObjectProperties *drm_props;
	uint32_t i;
	drmModePropertyRes *prop;
	uint64_t value;
	bool has_type = false, has_zpos = false;
	ssize_t core_prop_idx;

	liftoff_list_for_each(plane, &device->planes, link) {
		if (plane->id == id) {
			liftoff_log(LIFTOFF_ERROR, "tried to register plane "
				    "%"PRIu32" twice\n", id);
			errno = EEXIST;
			return NULL;
		}
	}

	plane = calloc(1, sizeof(*plane));
	if (plane == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "calloc");
		return NULL;
	}

	drm_plane = drmModeGetPlane(device->drm_fd, id);
	if (drm_plane == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "drmModeGetPlane");
		return NULL;
	}
	plane->id = drm_plane->plane_id;
	plane->possible_crtcs = drm_plane->possible_crtcs;
	drmModeFreePlane(drm_plane);

	drm_props = drmModeObjectGetProperties(device->drm_fd, id,
					       DRM_MODE_OBJECT_PLANE);
	if (drm_props == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "drmModeObjectGetProperties");
		return NULL;
	}
	plane->props = calloc(drm_props->count_props, sizeof(plane->props[0]));
	if (plane->props == NULL) {
		liftoff_log_errno(LIFTOFF_ERROR, "calloc");
		drmModeFreeObjectProperties(drm_props);
		return NULL;
	}
	for (i = 0; i < drm_props->count_props; i++) {
		prop = drmModeGetProperty(device->drm_fd, drm_props->props[i]);
		if (prop == NULL) {
			liftoff_log_errno(LIFTOFF_ERROR, "drmModeGetProperty");
			drmModeFreeObjectProperties(drm_props);
			return NULL;
		}
		plane->props[i] = prop;
		plane->props_len++;

		value = drm_props->prop_values[i];
		if (strcmp(prop->name, "type") == 0) {
			plane->type = value;
			has_type = true;
		} else if (strcmp(prop->name, "zpos") == 0) {
			plane->zpos = value;
			has_zpos = true;
		} else if (strcmp(prop->name, "IN_FORMATS") == 0) {
			plane->in_formats_blob = drmModeGetPropertyBlob(device->drm_fd,
									value);
			if (plane->in_formats_blob == NULL) {
				liftoff_log_errno(LIFTOFF_ERROR, "drmModeGetPropertyBlob");
				return NULL;
			}
		}

		core_prop_idx = core_property_index(prop->name);
		if (core_prop_idx >= 0) {
			plane->core_props[core_prop_idx] = prop;
		}
	}
	drmModeFreeObjectProperties(drm_props);

	if (!has_type) {
		liftoff_log(LIFTOFF_ERROR,
			    "plane %"PRIu32" is missing the 'type' property",
			    plane->id);
		free(plane);
		errno = EINVAL;
		return NULL;
	} else if (!has_zpos) {
		plane->zpos = guess_plane_zpos_from_type(device, plane->id,
							 plane->type);
	}

	/* During plane allocation, we will use the plane list order to fill
	 * planes with FBs. Primary planes need to be filled first, then planes
	 * far from the primary planes, then planes closer and closer to the
	 * primary plane. */
	if (plane->type == DRM_PLANE_TYPE_PRIMARY) {
		liftoff_list_insert(&device->planes, &plane->link);
	} else {
		liftoff_list_for_each(cur, &device->planes, link) {
			if (cur->type != DRM_PLANE_TYPE_PRIMARY &&
			    plane->zpos >= cur->zpos) {
				liftoff_list_insert(cur->link.prev, &plane->link);
				break;
			}
		}

		if (plane->link.next == NULL) { /* not inserted */
			liftoff_list_insert(device->planes.prev, &plane->link);
		}
	}

	return plane;
}

void
liftoff_plane_destroy(struct liftoff_plane *plane)
{
	size_t i;

	if (plane == NULL) {
		return;
	}

	if (plane->layer != NULL) {
		plane->layer->plane = NULL;
	}

	for (i = 0; i < plane->props_len; i++) {
		drmModeFreeProperty(plane->props[i]);
	}

	liftoff_list_remove(&plane->link);
	free(plane->props);
	drmModeFreePropertyBlob(plane->in_formats_blob);
	free(plane);
}

uint32_t
liftoff_plane_get_id(struct liftoff_plane *plane)
{
	return plane->id;
}

static const drmModePropertyRes *
plane_get_property(struct liftoff_plane *plane,
		   const struct liftoff_layer_property *layer_prop)
{
	size_t i;

	if (layer_prop->core_index >= 0)
		return plane->core_props[layer_prop->core_index];

	for (i = 0; i < plane->props_len; i++) {
		if (strcmp(plane->props[i]->name, layer_prop->name) == 0) {
			return plane->props[i];
		}
	}
	return NULL;
}

static int
check_range_prop(const drmModePropertyRes *prop, uint64_t value)
{
	if (value < prop->values[0] || value > prop->values[1]) {
		return -EINVAL;
	}
	return 0;
}

static int
check_enum_prop(const drmModePropertyRes *prop, uint64_t value)
{
	int i;

	for (i = 0; i < prop->count_enums; i++) {
		if (prop->enums[i].value == value) {
			return 0;
		}
	}
	return -EINVAL;
}

static int
check_bitmask_prop(const drmModePropertyRes *prop, uint64_t value)
{
	int i;
	uint64_t mask;

	mask = 0;
	for (i = 0; i < prop->count_enums; i++) {
		mask |= (uint64_t)1 << prop->enums[i].value;
	}

	if ((value & ~mask) != 0) {
		return -EINVAL;
	}
	return 0;
}

static int
check_signed_range_prop(const drmModePropertyRes *prop, uint64_t value)
{
	if ((int64_t) value < (int64_t) prop->values[0] ||
	    (int64_t) value > (int64_t) prop->values[1]) {
		return -EINVAL;
	}
	return 0;
}

static int
plane_set_prop(struct liftoff_plane *plane, drmModeAtomicReq *req,
	       const drmModePropertyRes *prop, uint64_t value)
{
	int ret;

	if (prop->flags & DRM_MODE_PROP_IMMUTABLE) {
		return -EINVAL;
	}

	/* Manually check the property value if we can: this may avoid
	 * unnecessary test commits */
	ret = 0;
	switch (drmModeGetPropertyType(prop)) {
	case DRM_MODE_PROP_RANGE:
		ret = check_range_prop(prop, value);
		break;
	case DRM_MODE_PROP_ENUM:
		ret = check_enum_prop(prop, value);
		break;
	case DRM_MODE_PROP_BITMASK:
		ret = check_bitmask_prop(prop, value);
		break;
	case DRM_MODE_PROP_SIGNED_RANGE:
		ret = check_signed_range_prop(prop, value);
		break;
	}
	if (ret != 0) {
		return ret;
	}

	ret = drmModeAtomicAddProperty(req, plane->id, prop->prop_id, value);
	if (ret < 0) {
		liftoff_log(LIFTOFF_ERROR, "drmModeAtomicAddProperty: %s",
			    strerror(-ret));
		return ret;
	}

	return 0;
}

static int
set_plane_core_prop(struct liftoff_plane *plane, drmModeAtomicReq *req,
		    enum liftoff_core_property core_prop, uint64_t value)
{
	const drmModePropertyRes *prop;

	prop = plane->core_props[core_prop];
	if (prop == NULL) {
		liftoff_log(LIFTOFF_DEBUG,
			    "plane %"PRIu32" is missing core property %d",
			    plane->id, core_prop);
		return -EINVAL;
	}

	return plane_set_prop(plane, req, prop, value);
}

bool
plane_check_layer_fb(struct liftoff_plane *plane, struct liftoff_layer *layer)
{
	const struct drm_format_modifier_blob *set;
	const uint32_t *formats;
	const struct drm_format_modifier *modifiers;
	size_t i;
	ssize_t format_index, modifier_index;
	int format_shift;

	/* TODO: add support for legacy format list with implicit modifier */
	if (layer->fb_info.fb_id == 0 ||
	    !(layer->fb_info.flags & DRM_MODE_FB_MODIFIERS) ||
	    plane->in_formats_blob == NULL) {
		return true; /* not enough information to reject */
	}

	set = plane->in_formats_blob->data;

	formats = (void *)((char *)set + set->formats_offset);
	format_index = -1;
	for (i = 0; i < set->count_formats; ++i) {
		if (formats[i] == layer->fb_info.pixel_format) {
			format_index = (ssize_t)i;
			break;
		}
	}
	if (format_index < 0) {
		return false;
	}

	modifiers = (void *)((char *)set + set->modifiers_offset);
	modifier_index = -1;
	for (i = 0; i < set->count_modifiers; i++) {
		if (modifiers[i].modifier == layer->fb_info.modifier) {
			modifier_index = (ssize_t)i;
			break;
		}
	}
	if (modifier_index < 0) {
		return false;
	}

	if ((size_t)format_index < modifiers[modifier_index].offset ||
	    (size_t)format_index >= modifiers[modifier_index].offset + 64) {
		return false;
	}
	format_shift = format_index - (int)modifiers[modifier_index].offset;
	return (modifiers[modifier_index].formats & ((uint64_t)1 << format_shift)) != 0;
}

int
plane_apply(struct liftoff_plane *plane, struct liftoff_layer *layer,
	    drmModeAtomicReq *req)
{
	int cursor, ret;
	size_t i;
	struct liftoff_layer_property *layer_prop;
	const drmModePropertyRes *plane_prop;

	cursor = drmModeAtomicGetCursor(req);

	if (layer == NULL) {
		ret = set_plane_core_prop(plane, req, LIFTOFF_PROP_FB_ID, 0);
		if (ret != 0) {
			return ret;
		}
		return set_plane_core_prop(plane, req, LIFTOFF_PROP_CRTC_ID, 0);
	}

	ret = set_plane_core_prop(plane, req, LIFTOFF_PROP_CRTC_ID,
				  layer->output->crtc_id);
	if (ret != 0) {
		return ret;
	}

	for (i = 0; i < layer->props_len; i++) {
		layer_prop = &layer->props[i];
		if (layer_prop->core_index == LIFTOFF_PROP_ZPOS) {
			/* We don't yet support setting the zpos property. We
			 * only use it (read-only) during plane allocation. */
			continue;
		}

		plane_prop = plane_get_property(plane, layer_prop);
		if (plane_prop == NULL) {
			if (layer_prop->core_index == LIFTOFF_PROP_ALPHA &&
			    layer_prop->value == 0xFFFF) {
				continue; /* Layer is completely opaque */
			}
			if (layer_prop->core_index == LIFTOFF_PROP_ROTATION &&
			    layer_prop->value == DRM_MODE_ROTATE_0) {
				continue; /* Layer isn't rotated */
			}
			if (strcmp(layer_prop->name, "SCALING_FILTER") == 0 &&
			    layer_prop->value == 0) {
				continue; /* Layer uses default scaling filter */
			}
			if (strcmp(layer_prop->name, "pixel blend mode") == 0 &&
			    layer_prop->value == 0) {
				continue; /* Layer uses pre-multiplied alpha */
			}
			if (strcmp(layer_prop->name, "FB_DAMAGE_CLIPS") == 0) {
				continue; /* Damage can be omitted */
			}
			drmModeAtomicSetCursor(req, cursor);
			return -EINVAL;
		}

		ret = plane_set_prop(plane, req, plane_prop, layer_prop->value);
		if (ret != 0) {
			drmModeAtomicSetCursor(req, cursor);
			return ret;
		}
	}

	return 0;
}
