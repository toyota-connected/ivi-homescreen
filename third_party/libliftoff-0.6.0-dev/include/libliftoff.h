#ifndef LIFTOFF_H
#define LIFTOFF_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <xf86drmMode.h>

struct liftoff_device;
struct liftoff_output;
struct liftoff_layer;
struct liftoff_plane;

/**
 * Initialize libliftoff for a DRM node.
 *
 * The node is expected to have DRM_CLIENT_CAP_ATOMIC enabled. libliftoff takes
 * ownership of the file descriptor.
 */
struct liftoff_device *
liftoff_device_create(int drm_fd);

/**
 * Destroy a libliftoff device.
 *
 * The caller is expected to destroy the outputs and layers explicitly.
 */
void
liftoff_device_destroy(struct liftoff_device *device);

/**
 * Register all available hardware planes to be managed by the libliftoff
 * device.
 *
 * Users should call this function if they don't manually set any plane property
 * and instead use libliftoff layers.
 *
 * Zero is returned on success, negative errno on error.
 */
int
liftoff_device_register_all_planes(struct liftoff_device *device);

/**
 * Register a hardware plane to be managed by the libliftoff device.
 *
 * Users should call this function for each plane they don't want to manually
 * manage. Registering the same plane twice is an error.
 */
struct liftoff_plane *
liftoff_plane_create(struct liftoff_device *device, uint32_t plane_id);

/**
 * Unregister a hardware plane.
 */
void
liftoff_plane_destroy(struct liftoff_plane *plane);

/**
 * Obtain the object ID of the plane.
 */
uint32_t
liftoff_plane_get_id(struct liftoff_plane *plane);

/**
 * Options for liftoff_output_apply().
 */
struct liftoff_output_apply_options {
	/* Timeout in nanoseconds. If zero, a default timeout is used. */
	int64_t timeout_ns;
};

/**
 * Build a layer to plane mapping and append the plane configuration to req.
 *
 * Callers are expected to commit req afterwards and can figure out which
 * layers need composition via liftoff_layer_needs_composition().
 *
 * flags is the atomic commit flags the caller intends to use. If options is
 * NULL, defaults are used.
 *
 * Zero is returned on success, negative errno on error.
 */
int
liftoff_output_apply(struct liftoff_output *output, drmModeAtomicReq *req,
		     uint32_t flags,
		     const struct liftoff_output_apply_options *options);

/**
 * Make the device manage a CRTC's planes.
 *
 * The returned output allows callers to attach layers.
 */
struct liftoff_output *
liftoff_output_create(struct liftoff_device *device, uint32_t crtc_id);

/**
 * Destroy a libliftoff output.
 *
 * The caller is expected to destroy the output's layers explicitly.
 */
void
liftoff_output_destroy(struct liftoff_output *output);

/**
 * Indicate on which layer composition can take place.
 *
 * Users should be able to blend layers that haven't been mapped to a plane to
 * this layer. The composition layer won't be used if all other layers have been
 * mapped to a plane. There is at most one composition layer per output.
 */
void
liftoff_output_set_composition_layer(struct liftoff_output *output,
				     struct liftoff_layer *layer);

/**
 * Check whether this output needs composition.
 *
 * An output doesn't need composition if all visible layers could be mapped to a
 * plane. In other words, if an output needs composition, at least one layer
 * will return true when liftoff_layer_needs_composition() is called.
 */
bool
liftoff_output_needs_composition(struct liftoff_output *output);

/**
 * Create a new layer on an output.
 *
 * A layer is a virtual plane. Users can create as many layers as they want and
 * set any KMS property on them, without any constraint. libliftoff will try
 * to map layers to hardware planes on a best-effort basis. The user will need
 * to manually handle layers that couldn't be mapped to a plane.
 */
struct liftoff_layer *
liftoff_layer_create(struct liftoff_output *output);

/**
 * Destroy a layer.
 */
void
liftoff_layer_destroy(struct liftoff_layer *layer);

/**
 * Set a property on the layer.
 *
 * Any plane property can be set (except CRTC_ID). If none of the planes support
 * the property, the layer won't be mapped to any plane.
 *
 * Setting a zero FB_ID disables the layer.
 *
 * Zero is returned on success, negative errno on error.
 */
int
liftoff_layer_set_property(struct liftoff_layer *layer, const char *name,
			   uint64_t value);

/**
 * Unset a property on the layer.
 */
void
liftoff_layer_unset_property(struct liftoff_layer *layer, const char *name);

/**
 * Force composition on this layer.
 *
 * This unsets any previous FB_ID value. To switch back to direct scan-out, set
 * FB_ID again.
 *
 * This can be used when no KMS FB ID is available for this layer but it still
 * needs to be displayed (e.g. the buffer cannot be imported in KMS).
 */
void
liftoff_layer_set_fb_composited(struct liftoff_layer *layer);

/**
 * Check whether this layer needs composition.
 *
 * A layer needs composition if it's visible and if it couldn't be mapped to a
 * plane.
 */
bool
liftoff_layer_needs_composition(struct liftoff_layer *layer);

/**
 * Retrieve the plane mapped to this layer.
 *
 * NULL is returned if no plane is mapped.
 */
struct liftoff_plane *
liftoff_layer_get_plane(struct liftoff_layer *layer);

/**
 * Check whether a plane is a candidate for this layer.
 *
 * A plane is a candidate if it could potentially be used for the layer with
 * a buffer with the same size. The buffer may need to be re-allocated with
 * formats and modifiers accepted by the plane.
 *
 * This can be used to implemented a feedback loop: if a layer isn't mapped to
 * a plane, loop over the candidate planes, and re-allocate the layer's FB
 * according to the IN_FORMATS property.
 */
bool
liftoff_layer_is_candidate_plane(struct liftoff_layer *layer,
				 struct liftoff_plane *plane);

enum liftoff_log_priority {
	LIFTOFF_SILENT,
	LIFTOFF_ERROR,
	LIFTOFF_DEBUG,
};

typedef void (*liftoff_log_handler)(enum liftoff_log_priority priority,
				    const char *fmt, va_list args);

/**
 * Set libliftoff's log priority.
 *
 * Only messages with a priority higher than the provided priority will be
 * logged. The default priority is LIFTOFF_ERROR.
 */
void
liftoff_log_set_priority(enum liftoff_log_priority priority);

/**
 * Set libliftoff's log handler.
 *
 * The default handler prints messages to stderr. NULL restores the default
 * handler.
 */
void
liftoff_log_set_handler(liftoff_log_handler handler);

#endif
