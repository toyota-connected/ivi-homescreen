/*
 * Copyright © 2011 Benjamin Franzke
 * Copyright 2022 Toyota Connected North America
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include "context.h"

#include <EGL/eglext.h>
#include <wayland-client.h>

#include <cassert>
#include <cstring>
#include <iostream>

uint32_t CompSurfContext::version() {
    return 0x00010000;
}

CompSurfContext::CompSurfContext(const char *accessToken,
                                 int width,
                                 int height,
                                 void *nativeWindow,
                                 const char *assetsPath,
                                 const char *cachePath)
        : accessToken_(accessToken),
          assetsPath_(assetsPath),
          cachePath_(cachePath),
          window_{nullptr},
          display_{nullptr} {
    std::cout << "[comp_surf_egl]" << std::endl;
    std::cout << "assetsPath: " << assetsPath_ << std::endl;
    std::cout << "cachePath: " << cachePath_ << std::endl;

    window_.display = &display_;
    window_.window_size.width = width;
    window_.window_size.height = height;
    window_.geometry.width = width;
    window_.geometry.height = height;

    typedef struct {
        struct wl_display *wl_display;
        struct wl_surface *wl_surface;
        EGLDisplay egl_display;
        struct wl_egl_window *egl_window;
    } wl;

    auto p = reinterpret_cast<wl *>(nativeWindow);
    assert(p);

    window_.surface = p->wl_surface;
    display_.egl.dpy = p->egl_display;
    window_.native = p->egl_window;

    assert(window_.surface);
    assert(display_.egl.dpy);
    assert(window_.native);

    init_egl(p->egl_window, p->egl_display, window_.egl_surface,
             display_.egl.ctx);

    eglMakeCurrent(display_.egl.dpy, window_.egl_surface, window_.egl_surface,
                   display_.egl.ctx);

    init_gl(&window_);

    eglSwapInterval(display_.egl.dpy, 0);
}

void CompSurfContext::de_initialize() const {

    // prevent segfault
    eglMakeCurrent(display_.egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(display_.egl.dpy);
    eglReleaseThread();
}

void CompSurfContext::run_task() {}

void CompSurfContext::resize(int width, int height) {
    window_.geometry.width = width;
    window_.geometry.height = height;

    window_.window_size = window_.geometry;
}

void *CompSurfContext::get_egl_proc_address(const char *address) {
    const char *extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

    if (extensions && (strstr(extensions, "EGL_EXT_platform_wayland") ||
                       strstr(extensions, "EGL_KHR_platform_wayland"))) {
        return (void *) eglGetProcAddress(address);
    }

    return nullptr;
}

EGLSurface CompSurfContext::create_egl_surface(EGLDisplay &eglDisplay,
                                               EGLConfig &eglConfig,
                                               void *native_window,
                                               const EGLint *attrib_list) {
    static PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC create_platform_window =
            nullptr;

    if (!create_platform_window) {
        create_platform_window =
                (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC) get_egl_proc_address(
                        "eglCreatePlatformWindowSurfaceEXT");
    }

    if (create_platform_window)
        return create_platform_window(eglDisplay, eglConfig, native_window,
                                      attrib_list);

    return eglCreateWindowSurface(
            eglDisplay, eglConfig, (EGLNativeWindowType) native_window, attrib_list);
}

void CompSurfContext::init_egl(void *nativeWindow,
                               EGLDisplay &eglDisplay,
                               EGLSurface &eglSurface,
                               EGLContext &eglContext) {
    EGLint config_attribs[] = {
            // clang-format off
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 1,
            EGL_GREEN_SIZE, 1,
            EGL_BLUE_SIZE, 1,
            EGL_ALPHA_SIZE, 1,
            EGL_NONE
            // clang-format on
    };

    static const EGLint context_attribs[] = {
            // clang-format off
            EGL_CONTEXT_MAJOR_VERSION, 3,
            EGL_CONTEXT_MAJOR_VERSION, 2,
            EGL_NONE
            // clang-format on
    };

    EGLint major, minor;
    EGLBoolean ret = eglInitialize(eglDisplay, &major, &minor);
    assert(ret == EGL_TRUE);
    std::cout << "EGL major: " << major << ", minor: " << minor << std::endl;

    ret = eglBindAPI(EGL_OPENGL_ES_API);
    assert(ret == EGL_TRUE);

    EGLint count;
    eglGetConfigs(eglDisplay, nullptr, 0, &count);
    assert(count);

    auto *configs =
            reinterpret_cast<EGLConfig *>(calloc(count, sizeof(EGLConfig)));
    assert(configs);

    EGLint n;
    ret = eglChooseConfig(eglDisplay, config_attribs, configs, count, &n);
    assert(ret && n >= 1);

    EGLint size;
    EGLConfig egl_conf = nullptr;
    for (EGLint i = 0; i < n; i++) {
        eglGetConfigAttrib(eglDisplay, configs[i], EGL_BUFFER_SIZE, &size);
        if (kEglBufferSize <= size) {
            egl_conf = configs[i];
            break;
        }
    }
    free(configs);
    if (egl_conf == nullptr) {
        assert(false);
    }

    eglContext =
            eglCreateContext(eglDisplay, egl_conf, EGL_NO_CONTEXT, context_attribs);
    eglSurface = create_egl_surface(eglDisplay, egl_conf, nativeWindow, nullptr);
    assert(eglSurface != EGL_NO_SURFACE);
}

GLuint CompSurfContext::create_shader(const char *source, GLenum shader_type) {
    GLuint shader;
    GLint status;

    shader = glCreateShader(shader_type);
    assert(shader != 0);

    glShaderSource(shader, 1, (const char **) &source, nullptr);
    glCompileShader(shader);

    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[1000];
        GLsizei len;
        glGetShaderInfoLog(shader, 1000, &len, log);
        fprintf(stderr, "Error: compiling %s: %*s\n",
                shader_type == GL_VERTEX_SHADER ? "vertex" : "fragment", len, log);
        exit(1);
    }

    return shader;
}

void CompSurfContext::init_gl(CompSurfContext::window *window) {
    GLuint frag, vert;
    GLuint program;
    GLint status;

    frag = create_shader(kFragShaderText, GL_FRAGMENT_SHADER);
    vert = create_shader(kVertShaderText, GL_VERTEX_SHADER);

    program = glCreateProgram();
    glAttachShader(program, frag);
    glAttachShader(program, vert);
    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (!status) {
        char log[1000];
        GLsizei len;
        glGetProgramInfoLog(program, 1000, &len, log);
        fprintf(stderr, "Error: linking:\n%*s\n", len, log);
        exit(1);
    }

    glUseProgram(program);

    window->gl.pos = 0;
    window->gl.col = 1;

    glBindAttribLocation(program, window->gl.pos, "pos");
    glBindAttribLocation(program, window->gl.col, "color");
    glLinkProgram(program);

    window->gl.rotation_uniform = glGetUniformLocation(program, "rotation");
}

void CompSurfContext::draw_frame(uint32_t time) const {

    static const GLfloat verts[3][2] = {{-0.5, -0.5},
                                        {0.5,  -0.5},
                                        {0,    0.5}};
    static const GLfloat colors[3][3] = {{1, 0, 0},
                                         {0, 1, 0},
                                         {0, 0, 1}};
    GLfloat angle;
    GLfloat rotation[4][4] = {
            {1, 0, 0, 0},
            {0, 1, 0, 0},
            {0, 0, 1, 0},
            {0, 0, 0, 1}};
    static const int32_t speed_div = 5;
    static uint32_t start_time = 0;

    if (start_time == 0)
        start_time = time;

    angle = ((time - start_time) / speed_div) % 360 * M_PI / 180.0;
    rotation[0][0] = cos(angle);
    rotation[0][2] = sin(angle);
    rotation[2][0] = -sin(angle);
    rotation[2][2] = cos(angle);

    glViewport(0, 0, window_.geometry.width, window_.geometry.height);

    glUniformMatrix4fv(window_.gl.rotation_uniform, 1, GL_FALSE,
                       (GLfloat *) rotation);

    glClearColor(0.0, 0.0, 0.0, 0.5);
    glClear(GL_COLOR_BUFFER_BIT);

    glVertexAttribPointer(window_.gl.pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glVertexAttribPointer(window_.gl.col, 3, GL_FLOAT, GL_FALSE, 0, colors);
    glEnableVertexAttribArray(window_.gl.pos);
    glEnableVertexAttribArray(window_.gl.col);

    glDrawArrays(GL_TRIANGLES, 0, 3);

    glDisableVertexAttribArray(window_.gl.pos);
    glDisableVertexAttribArray(window_.gl.col);

    eglSwapBuffers(window_.display->egl.dpy, window_.egl_surface);
}
