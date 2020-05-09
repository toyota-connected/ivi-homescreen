/*
 * Copyright (c) 2017 TOYOTA MOTOR CORPORATION
 * Copyright © 2011 Benjamin Franzke
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <mutex>
#include <chrono>

#include <iostream>
#include <string>
#include <stdarg.h>
#include <sys/types.h>
#include <thread>
#include <exception>
#include <vector>
#include <sstream>

#include <assert.h>
#include <signal.h>

#include <linux/input.h>

#include <wayland-client.h>
#include <wayland-egl.h>

#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#include <libwindowmanager.h>
#include <libhomescreen.hpp>

#include <ilm/ivi-application-client-protocol.h>
#include "hmi-debug.h"

using namespace std;

const char* log_prefix = "simple-egl";
uint32_t g_id_ivisurf = 9009;
long port = 1700;
string token = string("wm");

string app_name = string("Homescreen");
const char* main_role = "homescreen";

LibHomeScreen* hs;
LibWindowmanager *wm;

static const struct wl_interface *types[] = {
        NULL,
        NULL,
        NULL,
        &wl_surface_interface,
        &ivi_surface_interface,
};

static const struct wl_message ivi_surface_requests[] = {
        { "destroy", "", types + 0 },
};

static const struct wl_message ivi_surface_events[] = {
        { "configure", "ii", types + 0 },
};

const struct wl_interface ivi_surface_interface = {
        "ivi_surface", 1,
        1, ivi_surface_requests,
        1, ivi_surface_events,
};

static const struct wl_message ivi_application_requests[] = {
        { "surface_create", "uon", types + 2 },
};

const struct wl_interface ivi_application_interface = {
	"ivi_application", 1,
	1, ivi_application_requests,
	0, NULL,
};

#include "platform.h"

#ifndef EGL_EXT_swap_buffers_with_damage
#define EGL_EXT_swap_buffers_with_damage 1
typedef EGLBoolean (EGLAPIENTRYP PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC)(EGLDisplay dpy, EGLSurface surface, EGLint *rects, EGLint n_rects);
#endif

#ifndef EGL_EXT_buffer_age
#define EGL_EXT_buffer_age 1
#define EGL_BUFFER_AGE_EXT			0x313D
#endif

struct window;
struct seat;

/*
struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct xdg_wm_base *wm_base;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_touch *touch;
	struct wl_keyboard *keyboard;
	struct wl_shm *shm;
	struct wl_cursor_theme *cursor_theme;
	struct wl_cursor *default_cursor;
	struct wl_surface *cursor_surface;
	struct {
		EGLDisplay dpy;
		EGLContext ctx;
		EGLConfig conf;
	} egl;
	struct window *window;

	PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC swap_buffers_with_damage;
};
*/

struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_touch *touch;
	struct wl_keyboard *keyboard;
	struct {
		EGLDisplay dpy;
		EGLContext ctx;
		EGLConfig conf;
	} egl;
	struct window *window;
	struct ivi_application *ivi_application;

	PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC swap_buffers_with_damage;
};

struct geometry {
	int width, height;
};

/*
struct window {
	struct display *display;
	struct geometry geometry, window_size;
	struct {
		GLuint rotation_uniform;
		GLuint pos;
		GLuint col;
	} gl;

	uint32_t benchmark_time, frames;
	struct wl_egl_window *native;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	EGLSurface egl_surface;
	struct wl_callback *callback;
	int fullscreen, maximized, opaque, buffer_size, frame_sync, delay;
	bool wait_for_configure;
};
*/

struct window {
	struct display *display;
	struct geometry geometry, window_size;
	struct {
		GLuint rotation_uniform;
		GLuint pos;
		GLuint col;
	} gl;

	uint32_t benchmark_time, frames;
	struct wl_egl_window *native;
	struct wl_surface *surface;
	struct ivi_surface *ivi_surface;
	EGLSurface egl_surface;
	struct wl_callback *callback;
	int fullscreen, opaque, buffer_size, frame_sync;
};

static const char *vert_shader_text =
	"uniform mat4 rotation;\n"
	"attribute vec4 pos;\n"
	"attribute vec4 color;\n"
	"varying vec4 v_color;\n"
	"void main() {\n"
	"  gl_Position = rotation * pos;\n"
	"  v_color = color;\n"
	"}\n";

static const char *frag_shader_text =
	"precision mediump float;\n"
	"varying vec4 v_color;\n"
	"void main() {\n"
	"  gl_FragColor = v_color;\n"
	"}\n";

static int running = 1;

static void
init_egl(struct display *display, struct window *window)
{
	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	const char *extensions;

	EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 1,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};

	EGLint major, minor, n, count, i, size;
	EGLConfig *configs;
	EGLBoolean ret;

	if (window->opaque || window->buffer_size == 16)
		config_attribs[9] = 0;

	display->egl.dpy = weston_platform_get_egl_display(EGL_PLATFORM_WAYLAND_KHR, display->display, NULL);
	assert(display->egl.dpy);

	ret = eglInitialize(display->egl.dpy, &major, &minor);
	assert(ret == EGL_TRUE);
	ret = eglBindAPI(EGL_OPENGL_ES_API);
	assert(ret == EGL_TRUE);

	if (!eglGetConfigs(display->egl.dpy, NULL, 0, &count) || count < 1)
		assert(0);

	configs = calloc(count, sizeof *configs);
	assert(configs);

	ret = eglChooseConfig(display->egl.dpy, config_attribs,
			      configs, count, &n);
	assert(ret && n >= 1);

	for (i = 0; i < n; i++) {
		eglGetConfigAttrib(display->egl.dpy,
				   configs[i], EGL_BUFFER_SIZE, &size);
		if (window->buffer_size == size) {
			display->egl.conf = configs[i];
			break;
		}
	}
	free(configs);
	if (display->egl.conf == NULL) {
		HMI_ERROR(log_prefix,"did not find config with buffer size %d",
			window->buffer_size);
		exit(EXIT_FAILURE);
	}

	display->egl.ctx = eglCreateContext(display->egl.dpy,
					    display->egl.conf,
					    EGL_NO_CONTEXT, context_attribs);
	assert(display->egl.ctx);

	display->swap_buffers_with_damage = NULL;
	extensions = eglQueryString(display->egl.dpy, EGL_EXTENSIONS);
	if (extensions &&
	    strstr(extensions, "EGL_EXT_swap_buffers_with_damage") &&
	    strstr(extensions, "EGL_EXT_buffer_age"))
		display->swap_buffers_with_damage =
			(PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC)
			eglGetProcAddress("eglSwapBuffersWithDamageEXT");

	if (display->swap_buffers_with_damage)
		HMI_DEBUG(log_prefix,"has EGL_EXT_buffer_age and EGL_EXT_swap_buffers_with_damage");

}

static void
fini_egl(struct display *display)
{
	eglTerminate(display->egl.dpy);
	eglReleaseThread();
}

static GLuint
create_shader(struct window *window, const char *source, GLenum shader_type)
{
	GLuint shader;
	GLint status;

	shader = glCreateShader(shader_type);
	assert(shader != 0);

	glShaderSource(shader, 1, (const char **) &source, NULL);
	glCompileShader(shader);

	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status) {
		char log[1000];
		GLsizei len;
		glGetShaderInfoLog(shader, 1000, &len, log);
		HMI_ERROR(log_prefix,"Error: compiling %s: %*s",
			shader_type == GL_VERTEX_SHADER ? "vertex" : "fragment",
			len, log);
		exit(1);
	}

	return shader;
}

static void
init_gl(struct window *window)
{
	GLuint frag, vert;
	GLuint program;
	GLint status;

	frag = create_shader(window, frag_shader_text, GL_FRAGMENT_SHADER);
	vert = create_shader(window, vert_shader_text, GL_VERTEX_SHADER);

	program = glCreateProgram();
	glAttachShader(program, frag);
	glAttachShader(program, vert);
	glLinkProgram(program);

	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status) {
		char log[1000];
		GLsizei len;
		glGetProgramInfoLog(program, 1000, &len, log);
		HMI_ERROR(log_prefix,"Error: linking:%*s", len, log);
		exit(1);
	}

	glUseProgram(program);

	window->gl.pos = 0;
	window->gl.col = 1;

	glBindAttribLocation(program, window->gl.pos, "pos");
	glBindAttribLocation(program, window->gl.col, "color");
	glLinkProgram(program);

	window->gl.rotation_uniform =
		glGetUniformLocation(program, "rotation");
}

static void
create_ivi_surface(struct window *window, struct display *display)
{
	uint32_t id_ivisurf = g_id_ivisurf;
	window->ivi_surface =
		ivi_application_surface_create(display->ivi_application,
					       id_ivisurf, window->surface);

	if (window->ivi_surface == NULL) {
		HMI_ERROR(log_prefix,"Failed to create ivi_client_surface");
		abort();
	}

}

static void
create_surface(struct window *window)
{
	struct display *display = window->display;
	EGLBoolean ret;

	window->surface = wl_compositor_create_surface(display->compositor);

	window->native =
		wl_egl_window_create(window->surface,
				     window->geometry.width,
				     window->geometry.height);
	window->egl_surface =
		weston_platform_create_egl_surface(display->egl.dpy,
						   display->egl.conf,
						   window->native, NULL);


	if (display->ivi_application ) {
		create_ivi_surface(window, display);
	} else {
		assert(0);
	}

	ret = eglMakeCurrent(window->display->egl.dpy, window->egl_surface,
			     window->egl_surface, window->display->egl.ctx);
	assert(ret == EGL_TRUE);

	if (!window->frame_sync)
		eglSwapInterval(display->egl.dpy, 0);

}

static void
destroy_surface(struct window *window)
{
	/* Required, otherwise segfault in egl_dri2.c: dri2_make_current()
	 * on eglReleaseThread(). */
	eglMakeCurrent(window->display->egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
		       EGL_NO_CONTEXT);

	eglDestroySurface(window->display->egl.dpy, window->egl_surface);
	wl_egl_window_destroy(window->native);

	if (window->display->ivi_application)
		ivi_surface_destroy(window->ivi_surface);
	wl_surface_destroy(window->surface);

	if (window->callback)
		wl_callback_destroy(window->callback);
}

static void
redraw(void *data, struct wl_callback *callback, uint32_t time)
{
	struct window *window = data;
	struct display *display = window->display;
	static const GLfloat verts[3][2] = {
		{ -0.5, -0.5 },
		{  0.5, -0.5 },
		{  0,    0.5 }
	};
	static const GLfloat colors[3][3] = {
		{ 1, 0, 0 },
		{ 0, 1, 0 },
		{ 0, 0, 1 }
	};
	GLfloat angle;
	GLfloat rotation[4][4] = {
		{ 1, 0, 0, 0 },
		{ 0, 1, 0, 0 },
		{ 0, 0, 1, 0 },
		{ 0, 0, 0, 1 }
	};
	static const uint32_t speed_div = 5, benchmark_interval = 5;
	struct wl_region *region;
	EGLint rect[4];
	EGLint buffer_age = 0;
	struct timeval tv;

	assert(window->callback == callback);
	window->callback = NULL;

	if (callback)
		wl_callback_destroy(callback);

	gettimeofday(&tv, NULL);
	time = tv.tv_sec * 1000 + tv.tv_usec / 1000;
	if (window->frames == 0)
		window->benchmark_time = time;
	if (time - window->benchmark_time > (benchmark_interval * 1000)) {
		HMI_DEBUG(log_prefix,"%d frames in %d seconds: %f fps",
		       window->frames,
		       benchmark_interval,
		       (float) window->frames / benchmark_interval);
		window->benchmark_time = time;
		window->frames = 0;
	}

	angle = (time / speed_div) % 360 * M_PI / 180.0;
	rotation[0][0] =  cos(angle);
	rotation[0][2] =  sin(angle);
	rotation[2][0] = -sin(angle);
	rotation[2][2] =  cos(angle);

	if (display->swap_buffers_with_damage)
		eglQuerySurface(display->egl.dpy, window->egl_surface,
				EGL_BUFFER_AGE_EXT, &buffer_age);

	glViewport(0, 0, window->geometry.width, window->geometry.height);

	glUniformMatrix4fv(window->gl.rotation_uniform, 1, GL_FALSE,
			   (GLfloat *) rotation);

	glClearColor(0.0, 0.0, 0.0, 0.5);
	glClear(GL_COLOR_BUFFER_BIT);

	glVertexAttribPointer(window->gl.pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
	glVertexAttribPointer(window->gl.col, 3, GL_FLOAT, GL_FALSE, 0, colors);
	glEnableVertexAttribArray(window->gl.pos);
	glEnableVertexAttribArray(window->gl.col);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	glDisableVertexAttribArray(window->gl.pos);
	glDisableVertexAttribArray(window->gl.col);

	//usleep(window->delay);

	if (window->opaque || window->fullscreen) {
		region = wl_compositor_create_region(window->display->compositor);
		wl_region_add(region, 0, 0,
			      window->geometry.width,
			      window->geometry.height);
		wl_surface_set_opaque_region(window->surface, region);
		wl_region_destroy(region);
	} else {
		wl_surface_set_opaque_region(window->surface, NULL);
	}

	if (display->swap_buffers_with_damage && buffer_age > 0) {
		rect[0] = window->geometry.width / 4 - 1;
		rect[1] = window->geometry.height / 4 - 1;
		rect[2] = window->geometry.width / 2 + 2;
		rect[3] = window->geometry.height / 2 + 2;
		display->swap_buffers_with_damage(display->egl.dpy,
						  window->egl_surface,
						  rect, 1);
	} else {
		eglSwapBuffers(display->egl.dpy, window->egl_surface);
	}
	window->frames++;
}

static void
pointer_handle_enter(void *data, struct wl_pointer *pointer,
		     uint32_t serial, struct wl_surface *surface,
		     wl_fixed_t sx, wl_fixed_t sy)
{
	// struct display *display = data;
	// struct wl_buffer *buffer;
	// struct wl_cursor *cursor = display->default_cursor;
	// struct wl_cursor_image *image;

	// if (display->window->fullscreen)
	// 	wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
	// else if (cursor) {
	// 	image = display->default_cursor->images[0];
	// 	buffer = wl_cursor_image_get_buffer(image);
	// 	if (!buffer)
	// 		return;
	// 	wl_pointer_set_cursor(pointer, serial,
	// 			      display->cursor_surface,
	// 			      image->hotspot_x,
	// 			      image->hotspot_y);
	// 	wl_surface_attach(display->cursor_surface, buffer, 0, 0);
	// 	wl_surface_damage(display->cursor_surface, 0, 0,
	// 			  image->width, image->height);
	// 	wl_surface_commit(display->cursor_surface);
	// }
}

static void
pointer_handle_leave(void *data, struct wl_pointer *pointer,
		     uint32_t serial, struct wl_surface *surface)
{
}

static void
pointer_handle_motion(void *data, struct wl_pointer *pointer,
		      uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
}

static void
pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
		      uint32_t serial, uint32_t time, uint32_t button,
		      uint32_t state)
{
	// struct display *display = data;

	// if (!display->window->xdg_toplevel)
	// 	return;

	// if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED)
	// 	xdg_toplevel_move(display->window->xdg_toplevel,
	// 			  display->seat, serial);
}

static void
pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
		    uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static const struct wl_pointer_listener pointer_listener = {
	pointer_handle_enter,
	pointer_handle_leave,
	pointer_handle_motion,
	pointer_handle_button,
	pointer_handle_axis,
};

static void
touch_handle_down(void *data, struct wl_touch *wl_touch,
		  uint32_t serial, uint32_t time, struct wl_surface *surface,
		  int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
	// struct display *d = (struct display *)data;

	// if (!d->wm_base)
	// 	return;

	// xdg_toplevel_move(d->window->xdg_toplevel, d->seat, serial);
}

static void
touch_handle_up(void *data, struct wl_touch *wl_touch,
		uint32_t serial, uint32_t time, int32_t id)
{
}

static void
touch_handle_motion(void *data, struct wl_touch *wl_touch,
		    uint32_t time, int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
}

static void
touch_handle_frame(void *data, struct wl_touch *wl_touch)
{
}

static void
touch_handle_cancel(void *data, struct wl_touch *wl_touch)
{
}

static const struct wl_touch_listener touch_listener = {
	touch_handle_down,
	touch_handle_up,
	touch_handle_motion,
	touch_handle_frame,
	touch_handle_cancel,
};

static void
keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
		       uint32_t format, int fd, uint32_t size)
{
	/* Just so we don’t leak the keymap fd */
	close(fd);
}

static void
keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
		      uint32_t serial, struct wl_surface *surface,
		      struct wl_array *keys)
{
}

static void
keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
		      uint32_t serial, struct wl_surface *surface)
{
}

static void
keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
		    uint32_t serial, uint32_t time, uint32_t key,
		    uint32_t state)
{
	// struct display *d = data;

	// if (!d->wm_base)
	// 	return;

	// if (key == KEY_F11 && state) {
	// 	if (d->window->fullscreen)
	// 		xdg_toplevel_unset_fullscreen(d->window->xdg_toplevel);
	// 	else
	// 		xdg_toplevel_set_fullscreen(d->window->xdg_toplevel, NULL);
	// } else if (key == KEY_ESC && state)
	// 	running = 0;
}

static void
keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard,
			  uint32_t serial, uint32_t mods_depressed,
			  uint32_t mods_latched, uint32_t mods_locked,
			  uint32_t group)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
	keyboard_handle_keymap,
	keyboard_handle_enter,
	keyboard_handle_leave,
	keyboard_handle_key,
	keyboard_handle_modifiers,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *seat,
			 enum wl_seat_capability caps)
{
	HMI_DEBUG(log_prefix,"seat_handle_capabilities called !!!!!!!!!!!!!!!!!!!!!!!!");
	struct display *d = data;

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !d->pointer) {
		HMI_DEBUG(log_prefix,"WL_SEAT_CAPABILITY_POINTER yes");
		d->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(d->pointer, &pointer_listener, d);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && d->pointer) {
		HMI_DEBUG(log_prefix,"WL_SEAT_CAPABILITY_POINTER no");
		wl_pointer_destroy(d->pointer);
		d->pointer = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d->keyboard) {
		HMI_DEBUG(log_prefix,"WL_SEAT_CAPABILITY_KEYBOARD yes");
		d->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(d->keyboard, &keyboard_listener, d);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && d->keyboard) {
		HMI_DEBUG(log_prefix,"WL_SEAT_CAPABILITY_KEYBOARD no");
		wl_keyboard_destroy(d->keyboard);
		d->keyboard = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !d->touch) {
		HMI_DEBUG(log_prefix,"WL_SEAT_CAPABILITY_TOUCH yes");
		d->touch = wl_seat_get_touch(seat);
		wl_touch_set_user_data(d->touch, d);
		wl_touch_add_listener(d->touch, &touch_listener, d);
	} else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && d->touch) {
		HMI_DEBUG(log_prefix,"WL_SEAT_CAPABILITY_TOUCH no");
		wl_touch_destroy(d->touch);
		d->touch = NULL;
	}
}

static const struct wl_seat_listener seat_listener = {
	seat_handle_capabilities,
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
		       uint32_t name, const char *interface, uint32_t version)
{
	struct display *d = data;

	if (strcmp(interface, "wl_compositor") == 0) {
		d->compositor =
			wl_registry_bind(registry, name,
					 &wl_compositor_interface, 1);
	} else if (strcmp(interface, "ivi_application") == 0) {
		d->ivi_application =
			wl_registry_bind(registry, name,
					 &ivi_application_interface, 1);
	} else if (strcmp(interface, "wl_seat") == 0) {
		d->seat = wl_registry_bind(registry, name,
					   &wl_seat_interface, 1);
		wl_seat_add_listener(d->seat, &seat_listener, d);
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
			      uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

static void
signal_int(int signum)
{
	running = 0;
}

int
init_wm(LibWindowmanager *wm, struct window *window)
{
	HMI_DEBUG(log_prefix,"called");

	if (wm->init(port, token) != 0) {
		HMI_ERROR(log_prefix,"wm init failed. ");
		return -1;
	}

	g_id_ivisurf = wm->requestSurface(main_role);
	if (g_id_ivisurf < 0) {
		HMI_ERROR(log_prefix,"wm request surface failed ");
		return -1;
	}
	HMI_DEBUG(log_prefix,"IVI_SURFACE_ID: %d ", g_id_ivisurf);

	WMHandler wmh;
	wmh.on_visible = [](const char* role, bool visible){
		// Sample code if user uses visible event
		HMI_DEBUG(log_prefix, "role: %s, visible: %s", role, visible ? "true" : "false");
	};

	wmh.on_sync_draw = [wm, window](const char* role, const char* area, Rect rect) {

		struct Screen scr = wm->getScreenInfo();

		HMI_DEBUG(log_prefix,"Surface %s got syncDraw! Area: %s. w:%d, h:%d", role, area, scr.width_dp, scr.height_dp);
		// HMI_DEBUG(log_prefix,"Surface %s got syncDraw! Area: %s. w:%d, h:%d", role, area, rect.width(), rect.height());

		// wl_egl_window_resize(window->native, rect.width(), rect.height(), 0, 0);
		// window->geometry.width  = rect.width();
		// window->geometry.height = rect.height();

		wm->endDraw(role);
	};

	wm->setEventHandler(wmh);

	return 0;
}

int
init_hs(LibHomeScreen* hs){
	if(hs->init(port, token)!=0)
	{
		HMI_ERROR(log_prefix,"homescreen init failed. ");
		return -1;
	}

	hs->set_event_handler(LibHomeScreen::Event_ShowWindow, [hs](json_object *object){
		HMI_DEBUG("simple-egl","try to activeWindow %s ", app_name.c_str());

		struct json_object *param_obj = json_object_object_get(object, "parameter");
		const char *area = json_object_get_string(
			json_object_object_get(param_obj, "area"));
		// Application should call LibWindowmanager::activateWindow() in showWindow handler
		if(area == nullptr)
			wm->activateWindow(main_role, "normal.full");
		else
			wm->activateWindow(main_role, area);
	});

	return 0;
}

int
main(int argc, char **argv)
{
	struct sigaction sigint;
	struct window  window  = { 0 };
	struct display display = { 0 };
	// int i, ret = 0;

	window.display = &display;
	display.window = &window;
	window.geometry.width  = 1280;
	window.geometry.height = 800;
	window.window_size = window.geometry;
	window.buffer_size = 32;
	window.frame_sync = 1;

	if(argc > 2){
		port = strtol(argv[1], NULL, 10);
		token = argv[2];
	}

	HMI_DEBUG(log_prefix,"main_role: %s, port: %d, token: %s. ", main_role, port, token.c_str());

	display.display = wl_display_connect(NULL);
	assert(display.display);

	display.registry = wl_display_get_registry(display.display);
	wl_registry_add_listener(display.registry,
				 &registry_listener, &display);

	wl_display_roundtrip(display.display);

	init_egl(&display, &window);

	wm = new LibWindowmanager();
	if(init_wm(wm, &window)!=0){
		fini_egl(&display);
		if (display.ivi_application)
			ivi_application_destroy(display.ivi_application);
		if (display.compositor)
			wl_compositor_destroy(display.compositor);
		wl_registry_destroy(display.registry);
		wl_display_flush(display.display);
		return -1;
	}

	hs = new LibHomeScreen();
	if(init_hs(hs)!=0){
		fini_egl(&display);
		if (display.ivi_application)
			ivi_application_destroy(display.ivi_application);
		if (display.compositor)
			wl_compositor_destroy(display.compositor);
		wl_registry_destroy(display.registry);
		wl_display_flush(display.display);
		return -1;
	}

	create_surface(&window);
	init_gl(&window);

	//Ctrl+C
	sigint.sa_handler = signal_int;
	sigemptyset(&sigint.sa_mask);
	sigint.sa_flags = SA_RESETHAND;
	sigaction(SIGINT, &sigint, NULL);

	eglSwapBuffers(window.display->egl.dpy, window.egl_surface);

	wm->activateWindow(main_role);

	/* The mainloop here is a little subtle.  Redrawing will cause
	 * EGL to read events so we can just call
	 * wl_display_dispatch_pending() to handle any events that got
	 * queued up as a side effect. */
	while (running) {
		wl_display_dispatch_pending(display.display);
		redraw(&window, NULL, 0);
	}
	// while (running && ret != -1) {
	// 	if (window.wait_for_configure) {
	// 		ret = wl_display_dispatch(display.display);
	// 	} else {
	// 		ret = wl_display_dispatch_pending(display.display);
	// 		redraw(&window, NULL, 0);
	// 	}
	// }

	HMI_DEBUG(log_prefix,"simple-egl exiting! ");

	destroy_surface(&window);
	fini_egl(&display);

	if (display.ivi_application)
		ivi_application_destroy(display.ivi_application);

	if (display.compositor)
		wl_compositor_destroy(display.compositor);

	wl_registry_destroy(display.registry);
	wl_display_flush(display.display);
	wl_display_disconnect(display.display);

	return 0;
}

// =================================
// =================================
// =================================
// =================================
// =================================
// =================================
// =================================
// =================================
// =================================
// =================================
// =================================
// =================================
// =================================
// =================================
// =================================
// =================================
// =================================

// #include <string>
// #include <iostream>
// #include <cstring>
// #include <libwindowmanager.h>
// #include "hmi-debug.h"

// #undef DEBUG

// int main(int argc, char *argv[])
// {
// 	int port = 0;
// 	std::string token;
// 	std::string role = "receiver";

// 	try {
// 		port = std::stol(argv[1]);
// 		token = argv[2];
// 	} catch (const std::invalid_argument& e) {
// 		std::cerr << "Invalid argument" << std::endl;
// 		exit(1);
// 	} catch (const std::out_of_range& e) {
// 		std::cerr << "Port out of range" << std::endl;
// 		exit(1);
// 	}

// 	for(;;);

// 	return 0;
// }
