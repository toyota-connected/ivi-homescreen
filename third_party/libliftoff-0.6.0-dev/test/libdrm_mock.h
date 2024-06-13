#ifndef LIFTOFF_LIBDRM_MOCK_H
#define LIFTOFF_LIBDRM_MOCK_H

#include <stddef.h>
#include <stdint.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

extern uint32_t liftoff_mock_drm_crtc_id;
extern size_t liftoff_mock_commit_count;
extern bool liftoff_mock_verbose;

/**
 * Some drivers require the primary plane to be enabled in order to light up a
 * CRTC (e.g. i915). If this variable is set to true, this behavior is mimicked.
 */
extern bool liftoff_mock_require_primary_plane;

struct liftoff_layer;

int
liftoff_mock_drm_open(void);

uint32_t
liftoff_mock_drm_create_fb(struct liftoff_layer *layer);

void
liftoff_mock_drm_set_fb_info(const drmModeFB2 *fb_info);

struct liftoff_mock_plane *
liftoff_mock_drm_create_plane(uint64_t type);

struct liftoff_mock_plane *
liftoff_mock_drm_get_plane(uint32_t id);

void
liftoff_mock_plane_add_compatible_layer(struct liftoff_mock_plane *plane,
					struct liftoff_layer *layer);
struct liftoff_layer *
liftoff_mock_plane_get_layer(struct liftoff_mock_plane *plane);
uint32_t
liftoff_mock_plane_get_id(struct liftoff_mock_plane *plane);

uint32_t
liftoff_mock_plane_add_property(struct liftoff_mock_plane *plane,
				const drmModePropertyRes *prop,
				uint64_t value);
void
liftoff_mock_plane_add_in_formats(struct liftoff_mock_plane *plane,
				  const struct drm_format_modifier_blob *data,
				  size_t size);

#endif
