#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "libdrm_mock.h"

#define MAX_PLANES 64
#define MAX_LAYERS 512
#define MAX_PLANE_PROPS 64
#define MAX_REQ_PROPS 1024

#define OBJ_INDEX_MASK 0x00FFFFFF
#define OBJ_TYPE_MASK 0xFF000000

uint32_t liftoff_mock_drm_crtc_id = DRM_MODE_OBJECT_CRTC & OBJ_TYPE_MASK;
size_t liftoff_mock_commit_count = 0;
bool liftoff_mock_require_primary_plane = false;
bool liftoff_mock_verbose = true;

struct liftoff_mock_plane {
	uint32_t id;
	struct liftoff_layer *compatible_layers[MAX_LAYERS];
	bool enabled_props[MAX_PLANE_PROPS];
	uint64_t prop_values[MAX_PLANE_PROPS];
};

struct liftoff_mock_prop {
	uint32_t obj_id, prop_id;
	uint64_t value;
};

struct liftoff_mock_fb {
	struct liftoff_layer *layer;
	drmModeFB2 info;
};

struct _drmModeAtomicReq {
	struct liftoff_mock_prop props[MAX_REQ_PROPS];
	int cursor;
};

static int mock_pipe[2] = {-1, -1};
static struct liftoff_mock_plane mock_planes[MAX_PLANES];
static struct liftoff_mock_fb mock_fbs[MAX_LAYERS];
static drmModePropertyBlobRes mock_blobs[MAX_PLANES];

enum plane_prop {
	PLANE_TYPE,
	PLANE_FB_ID,
	PLANE_CRTC_ID,
};

static const char *basic_plane_props[] = {
	[PLANE_TYPE] = "type",
	[PLANE_FB_ID] = "FB_ID",
	[PLANE_CRTC_ID] = "CRTC_ID",
	"CRTC_X",
	"CRTC_Y",
	"CRTC_W",
	"CRTC_H",
	"SRC_X",
	"SRC_Y",
	"SRC_W",
	"SRC_H",
};

static const size_t basic_plane_props_len = sizeof(basic_plane_props) /
					    sizeof(basic_plane_props[0]);

static drmModePropertyRes plane_props[MAX_PLANE_PROPS] = {0};

static size_t plane_props_len = 0;

static uint32_t
object_id(size_t index, uint32_t type)
{
	assert((type & OBJ_TYPE_MASK) != 0);
	return index | (type & OBJ_TYPE_MASK);
}

static size_t
object_index(uint32_t id, uint32_t type)
{
	assert((id & OBJ_TYPE_MASK) == (type & OBJ_TYPE_MASK));
	return id & OBJ_INDEX_MASK;
}

static void
assert_drm_fd(int fd)
{
	int ret;
	struct stat stat_got, stat_want;

	ret = fstat(mock_pipe[0], &stat_want);
	assert(ret == 0);
	ret = fstat(fd, &stat_got);
	assert(ret == 0);

	assert(stat_got.st_dev == stat_want.st_dev &&
	       stat_got.st_ino == stat_want.st_ino);
}

static uint32_t
register_prop(const drmModePropertyRes *prop)
{
	drmModePropertyRes *dst;

	assert(plane_props_len < MAX_PLANE_PROPS);
	dst = &plane_props[plane_props_len];
	memcpy(dst, prop, sizeof(*dst));
	dst->prop_id = object_id(plane_props_len, DRM_MODE_OBJECT_PROPERTY);
	plane_props_len++;

	return dst->prop_id;
}

static void
init_basic_props(void)
{
	size_t i;

	if (plane_props_len > 0)
		return;

	for (i = 0; i < basic_plane_props_len; i++) {
		drmModePropertyRes prop = {0};
		strncpy(prop.name, basic_plane_props[i], sizeof(prop.name) - 1);
		/* TODO: fill flags */
		register_prop(&prop);
	}
}

int
liftoff_mock_drm_open(void)
{
	int ret;

	assert(mock_pipe[0] < 0);
	ret = pipe(mock_pipe);
	assert(ret == 0);

	init_basic_props();

	return mock_pipe[0];
}

struct liftoff_mock_plane *
liftoff_mock_drm_create_plane(uint64_t type)
{
	struct liftoff_mock_plane *plane;
	size_t i;

	assert(mock_pipe[0] < 0);

	init_basic_props();

	i = 0;
	plane = &mock_planes[0];
	while (plane->id != 0) {
		plane++;
		i++;
	}

	plane->id = object_id(i, DRM_MODE_OBJECT_PLANE);
	plane->prop_values[PLANE_TYPE] = type;

	for (size_t i = 0; i < basic_plane_props_len; i++) {
		plane->enabled_props[i] = true;
	}

	return plane;
}

struct liftoff_mock_plane *
liftoff_mock_drm_get_plane(uint32_t id)
{
	struct liftoff_mock_plane *plane;

	plane = &mock_planes[0];
	while (plane->id != 0) {
		if (plane->id == id) {
			return plane;
		}
		plane++;
	}

	abort(); // unreachable
}

void
liftoff_mock_plane_add_compatible_layer(struct liftoff_mock_plane *plane,
					struct liftoff_layer *layer)
{
	size_t i;

	for (i = 0; i < MAX_LAYERS; i++) {
		if (plane->compatible_layers[i] == NULL) {
			plane->compatible_layers[i] = layer;
			return;
		}
	}

	abort(); // unreachable
}

uint32_t
liftoff_mock_plane_get_id(struct liftoff_mock_plane *plane)
{
	return plane->id;
}

uint32_t
liftoff_mock_drm_create_fb(struct liftoff_layer *layer)
{
	size_t i;
	uint32_t fb_id;

	i = 0;
	while (mock_fbs[i].layer != NULL) {
		i++;
	}

	fb_id = object_id(i, DRM_MODE_OBJECT_FB);

	mock_fbs[i] = (struct liftoff_mock_fb) {
		.layer = layer,
	};

	return fb_id;
}

void
liftoff_mock_drm_set_fb_info(const drmModeFB2 *fb_info)
{
	size_t i;

	i = object_index(fb_info->fb_id, DRM_MODE_OBJECT_FB);
	mock_fbs[i].info = *fb_info;
}

static bool
mock_atomic_req_get_property(drmModeAtomicReq *req, uint32_t obj_id,
			     enum plane_prop prop, uint64_t *value)
{
	ssize_t i;
	uint32_t prop_id;

	prop_id = object_id(prop, DRM_MODE_OBJECT_PROPERTY);
	for (i = req->cursor - 1; i >= 0; i--) {
		if (req->props[i].obj_id == obj_id &&
		    req->props[i].prop_id == prop_id) {
			*value = req->props[i].value;
			return true;
		}
	}

	return false;
}

static struct liftoff_layer *
mock_fb_get_layer(uint32_t fb_id)
{
	size_t i;

	if (fb_id == 0) {
		return NULL;
	}

	i = object_index(fb_id, DRM_MODE_OBJECT_FB);
	assert(i < MAX_LAYERS);

	return mock_fbs[i].layer;
}

struct liftoff_layer *
liftoff_mock_plane_get_layer(struct liftoff_mock_plane *plane)
{
	return mock_fb_get_layer(plane->prop_values[PLANE_FB_ID]);
}

static size_t
get_prop_index(uint32_t id)
{
	size_t i;

	i = object_index(id, DRM_MODE_OBJECT_PROPERTY);
	assert(i < plane_props_len);

	return i;
}

uint32_t
liftoff_mock_plane_add_property(struct liftoff_mock_plane *plane,
				const drmModePropertyRes *prop,
				uint64_t value)
{
	uint32_t prop_id;

	prop_id = register_prop(prop);
	plane->enabled_props[get_prop_index(prop_id)] = true;
	plane->prop_values[get_prop_index(prop_id)] = value;
	return prop_id;
}

static uint32_t
register_blob(size_t size, const void *data)
{
	size_t i;
	uint32_t blob_id;
	void *copy;

	i = 0;
	while (mock_blobs[i].id != 0) {
		i++;
		assert(i < sizeof(mock_blobs) / sizeof(mock_blobs[0]));
	}

	copy = malloc(size);
	memcpy(copy, data, size);

	blob_id = object_id(i, DRM_MODE_OBJECT_BLOB);
	mock_blobs[i] = (drmModePropertyBlobRes) {
		.id = blob_id,
		.length = size,
		.data = copy,
	};
	return blob_id;
}

void
liftoff_mock_plane_add_in_formats(struct liftoff_mock_plane *plane,
				  const struct drm_format_modifier_blob *data,
				  size_t size)
{
	uint32_t blob_id;
	drmModePropertyRes prop = {0};

	blob_id = register_blob(size, data);

	strncpy(prop.name, "IN_FORMATS", sizeof(prop.name) - 1);
	/* TODO: fill flags */
	liftoff_mock_plane_add_property(plane, &prop, blob_id);
}

static void
debug(const char *fmt, ...)
{
	va_list args;

	if (!liftoff_mock_verbose) {
		return;
	}

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

static void
apply_atomic_req(drmModeAtomicReq *req)
{
	int i;
	size_t prop_index;
	struct liftoff_mock_prop *prop;
	struct liftoff_mock_plane *plane;

	for (i = 0; i < req->cursor; i++) {
		prop = &req->props[i];
		plane = liftoff_mock_drm_get_plane(prop->obj_id);
		prop_index = get_prop_index(prop->prop_id);
		plane->prop_values[prop_index] = prop->value;
		debug("libdrm_mock: plane %"PRIu32": setting %s = %"PRIu64"\n",
		      plane->id, plane_props[prop_index].name, prop->value);
	}
}

int
drmModeAtomicCommit(int fd, drmModeAtomicReq *req, uint32_t flags,
		    void *user_data)
{
	size_t i, j;
	struct liftoff_mock_plane *plane;
	uint64_t type, fb_id, crtc_id;
	bool has_fb, has_crtc, found;
	bool any_plane_enabled, primary_plane_enabled;
	struct liftoff_layer *layer;

	assert_drm_fd(fd);
	assert(flags == DRM_MODE_ATOMIC_TEST_ONLY || flags == 0);

	liftoff_mock_commit_count++;

	any_plane_enabled = false;
	primary_plane_enabled = false;
	for (i = 0; i < MAX_PLANES; i++) {
		plane = &mock_planes[i];
		if (plane->id == 0) {
			break;
		}

		type = plane->prop_values[PLANE_TYPE];
		fb_id = plane->prop_values[PLANE_FB_ID];
		crtc_id = plane->prop_values[PLANE_CRTC_ID];
		mock_atomic_req_get_property(req, plane->id, PLANE_FB_ID,
					     &fb_id);
		mock_atomic_req_get_property(req, plane->id, PLANE_CRTC_ID,
					     &crtc_id);

		has_fb = fb_id != 0;
		has_crtc = crtc_id != 0;

		if (has_fb != has_crtc) {
			debug("libdrm_mock: plane %u: both FB_ID and CRTC_ID "
			      "must be set or unset together "
			      "(FB_ID = %"PRIu64", CRTC_ID = %"PRIu64")\n",
			      plane->id, fb_id, crtc_id);
			return -EINVAL;
		}

		if (has_fb) {
			if (crtc_id != liftoff_mock_drm_crtc_id) {
				debug("libdrm_mock: plane %u: invalid CRTC_ID\n",
				      plane->id);
				return -EINVAL;
			}
			layer = mock_fb_get_layer(fb_id);
			if (layer == NULL) {
				debug("libdrm_mock: plane %u: invalid FB_ID\n",
				      plane->id);
				return -EINVAL;
			}
			found = false;
			for (j = 0; j < MAX_LAYERS; j++) {
				if (plane->compatible_layers[j] == layer) {
					found = true;
					break;
				}
			}
			if (!found) {
				debug("libdrm_mock: plane %u: "
				      "layer %p is not compatible\n",
				      plane->id, (void *)layer);
				return -EINVAL;
			}

			any_plane_enabled = true;
			if (type == DRM_PLANE_TYPE_PRIMARY) {
				primary_plane_enabled = true;
			}
		}
	}

	if (liftoff_mock_require_primary_plane && any_plane_enabled &&
	    !primary_plane_enabled) {
		debug("libdrm_mock: cannot light up CRTC without enabling the "
		      "primary plane\n");
		return -EINVAL;
	}

	if (!(flags & DRM_MODE_ATOMIC_TEST_ONLY)) {
		apply_atomic_req(req);
	}

	return 0;
}

drmModeRes *
drmModeGetResources(int fd)
{
	drmModeRes *res;

	assert_drm_fd(fd);

	res = calloc(1, sizeof(*res));
	res->count_crtcs = 1;
	res->crtcs = &liftoff_mock_drm_crtc_id;
	return res;
}

void
drmModeFreeResources(drmModeRes *res)
{
	free(res);
}

drmModePlaneRes *
drmModeGetPlaneResources(int fd)
{
	static uint32_t plane_ids[MAX_PLANES];
	drmModePlaneRes *res;
	size_t i;

	assert_drm_fd(fd);

	for (i = 0; i < MAX_PLANES; i++) {
		if (mock_planes[i].id == 0) {
			break;
		}
		plane_ids[i] = mock_planes[i].id;
	}

	res = calloc(1, sizeof(*res));
	res->count_planes = i;
	res->planes = plane_ids;
	return res;
}

void
drmModeFreePlaneResources(drmModePlaneRes *res)
{
	free(res);
}

drmModePlane *
drmModeGetPlane(int fd, uint32_t id)
{
	drmModePlane *plane;

	assert_drm_fd(fd);

	plane = calloc(1, sizeof(*plane));
	plane->plane_id = id;
	plane->possible_crtcs = 1 << 0;
	return plane;
}

void
drmModeFreePlane(drmModePlane *plane) {
	free(plane);
}

drmModeObjectProperties *
drmModeObjectGetProperties(int fd, uint32_t obj_id, uint32_t obj_type)
{
	struct liftoff_mock_plane *plane;
	drmModeObjectProperties *props;
	size_t i;

	assert_drm_fd(fd);
	assert(obj_type == DRM_MODE_OBJECT_PLANE);

	plane = NULL;
	for (i = 0; i < MAX_PLANES; i++) {
		if (mock_planes[i].id == obj_id) {
			plane = &mock_planes[i];
			break;
		}
	}
	assert(plane != NULL);

	props = calloc(1, sizeof(*props));
	props->props = calloc(plane_props_len, sizeof(uint32_t));
	props->prop_values = calloc(plane_props_len, sizeof(uint64_t));
	for (i = 0; i < plane_props_len; i++) {
		if (!plane->enabled_props[i])
			continue;
		props->props[props->count_props] = plane_props[i].prop_id;
		props->prop_values[props->count_props] = plane->prop_values[i];
		props->count_props++;
	}
	return props;
}

void
drmModeFreeObjectProperties(drmModeObjectProperties *props) {
	free(props->props);
	free(props->prop_values);
	free(props);
}

drmModePropertyRes *
drmModeGetProperty(int fd, uint32_t id)
{
	assert_drm_fd(fd);

	return &plane_props[get_prop_index(id)];
}

void
drmModeFreeProperty(drmModePropertyRes *prop) {
	/* Owned by plane_props */
}

drmModeAtomicReq *
drmModeAtomicAlloc(void)
{
	return calloc(1, sizeof(drmModeAtomicReq));
}

void
drmModeAtomicFree(drmModeAtomicReq *req)
{
	free(req);
}

int
drmModeAtomicAddProperty(drmModeAtomicReq *req, uint32_t obj_id,
			 uint32_t prop_id, uint64_t value)
{
	assert((size_t)req->cursor < sizeof(req->props) / sizeof(req->props[0]));
	req->props[req->cursor].obj_id = obj_id;
	req->props[req->cursor].prop_id = prop_id;
	req->props[req->cursor].value = value;
	req->cursor++;
	return req->cursor;
}

int
drmModeAtomicGetCursor(drmModeAtomicReq *req)
{
	return req->cursor;
}

void
drmModeAtomicSetCursor(drmModeAtomicReq *req, int cursor)
{
	req->cursor = cursor;
}

drmModeFB2 *
drmModeGetFB2(int fd, uint32_t fb_id)
{
	size_t i;
	drmModeFB2 *fb2;

	i = object_index(fb_id, DRM_MODE_OBJECT_FB);
	assert(i < MAX_LAYERS);

	if (mock_fbs[i].info.fb_id == 0) {
		errno = EINVAL;
		return NULL;
	}

	fb2 = malloc(sizeof(*fb2));
	if (fb2 == NULL) {
		return NULL;
	}
	*fb2 = mock_fbs[i].info;
	return fb2;
}

void
drmModeFreeFB2(drmModeFB2 *fb2)
{
	free(fb2);
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	errno = EINVAL;
	return -EINVAL;
}

drmModePropertyBlobRes *
drmModeGetPropertyBlob(int fd, uint32_t blob_id)
{
	size_t i;
	drmModePropertyBlobRes *blob;
	void *data;

	i = object_index(blob_id, DRM_MODE_OBJECT_BLOB);
	assert(i < sizeof(mock_blobs) / sizeof(mock_blobs[0]));

	if (mock_blobs[i].id == 0) {
		errno = EINVAL;
		return NULL;
	}

	blob = malloc(sizeof(*blob));
	if (blob == NULL) {
		return NULL;
	}

	data = malloc(mock_blobs[i].length);
	if (data == NULL) {
		free(blob);
		return NULL;
	}
	memcpy(data, mock_blobs[i].data, mock_blobs[i].length);

	*blob = (drmModePropertyBlobRes) {
		.id = blob_id,
		.length = mock_blobs[i].length,
		.data = data,
	};
	return blob;
}

void
drmModeFreePropertyBlob(drmModePropertyBlobRes *blob)
{
	if (blob == NULL) {
		return;
	}
	free(blob->data);
	free(blob);
}
