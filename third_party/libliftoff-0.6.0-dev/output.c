#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "private.h"

struct liftoff_output *
liftoff_output_create(struct liftoff_device *device, uint32_t crtc_id)
{
	struct liftoff_output *output;
	ssize_t crtc_index;
	size_t i;

	crtc_index = -1;
	for (i = 0; i < device->crtcs_len; i++) {
		if (device->crtcs[i] == crtc_id) {
			crtc_index = (ssize_t)i;
			break;
		}
	}
	if (crtc_index < 0) {
		return NULL;
	}

	output = calloc(1, sizeof(*output));
	if (output == NULL) {
		return NULL;
	}
	output->device = device;
	output->crtc_id = crtc_id;
	output->crtc_index = (size_t)crtc_index;
	liftoff_list_init(&output->layers);
	liftoff_list_insert(&device->outputs, &output->link);
	return output;
}

void
liftoff_output_destroy(struct liftoff_output *output)
{
	if (output == NULL) {
		return;
	}

	liftoff_list_remove(&output->link);
	free(output);
}

void
liftoff_output_set_composition_layer(struct liftoff_output *output,
				     struct liftoff_layer *layer)
{
	assert(layer->output == output);
	if (layer != output->composition_layer) {
		output->layers_changed = true;
	}
	output->composition_layer = layer;
}

bool
liftoff_output_needs_composition(struct liftoff_output *output)
{
	struct liftoff_layer *layer;

	liftoff_list_for_each(layer, &output->layers, link) {
		if (liftoff_layer_needs_composition(layer)) {
			return true;
		}
	}

	return false;
}

static double
fp16_to_double(uint64_t val)
{
	return (double)(val >> 16) + (double)(val & 0xFFFF) / 0xFFFF;
}

void
output_log_layers(struct liftoff_output *output)
{
	struct liftoff_layer *layer;
	size_t i;
	bool is_composition_layer;

	if (!log_has(LIFTOFF_DEBUG)) {
		return;
	}

	liftoff_log(LIFTOFF_DEBUG, "Layers on CRTC %"PRIu32" (%zu total):",
		    output->crtc_id, liftoff_list_length(&output->layers));
	liftoff_list_for_each(layer, &output->layers, link) {
		if (layer->force_composition) {
			liftoff_log(LIFTOFF_DEBUG, "  Layer %p "
				    "(forced composition):", (void *)layer);
		} else {
			if (!layer_has_fb(layer)) {
				continue;
			}
			is_composition_layer = output->composition_layer == layer;
			liftoff_log(LIFTOFF_DEBUG, "  Layer %p%s:",
				    (void *)layer, is_composition_layer ?
						   " (composition layer)" : "");
		}

		liftoff_log(LIFTOFF_DEBUG, "    Priority = %"PRIi32,
			    layer->current_priority);

		for (i = 0; i < layer->props_len; i++) {
			char *name = layer->props[i].name;
			uint64_t value = layer->props[i].value;

			if (strcmp(name, "CRTC_X") == 0 ||
			    strcmp(name, "CRTC_Y") == 0) {
				liftoff_log(LIFTOFF_DEBUG, "    %s = %+"PRIi32,
					    name, (int32_t)value);
			} else if (strcmp(name, "SRC_X") == 0 ||
				   strcmp(name, "SRC_Y") == 0 ||
				   strcmp(name, "SRC_W") == 0 ||
				   strcmp(name, "SRC_H") == 0) {
				liftoff_log(LIFTOFF_DEBUG, "    %s = %f",
					    name, fp16_to_double(value));
			} else {
				liftoff_log(LIFTOFF_DEBUG, "    %s = %"PRIu64,
					    name, value);
			}
		}
	}
}
