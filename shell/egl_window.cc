// Copyright 2020 Toyota Connected North America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "egl_window.h"

#include <fcntl.h>
#include <flutter/fml/logging.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cassert>
#include <cstring>
#include <utility>

#include "constants.h"
#include "display.h"

EglWindow::EglWindow(size_t index,
                     const std::shared_ptr<Display>& display,
                     enum window_type type,
                     std::string app_id,
                     bool fullscreen,
                     bool debug_egl,
                     bool sprawl,
                     int32_t width,
                     int32_t height)
    : Egl(display->GetDisplay(), debug_egl),
      m_index(index),
      m_display(display),
      m_width(width),
      m_height(height),
      m_type(type),
      m_app_id(std::move(app_id)),
      m_callback(nullptr),
      m_configured(false),
      m_fullscreen(fullscreen),
      m_sprawl(sprawl),
      m_frame_sync(0) {  // disable vsync
  FML_DLOG(INFO) << "+ EglWindow()";

  m_surface = wl_compositor_create_surface(m_display->GetCompositor());
  assert(m_surface);
  FML_DLOG(INFO) << "EglWindow::m_surface = " << static_cast<void*>(m_surface);

  m_fps_surface = wl_compositor_create_surface(m_display->GetCompositor());
  m_subsurface = wl_subcompositor_get_subsurface(m_display->GetSubCompositor(),
                                                 m_fps_surface, m_surface);
  wl_subsurface_set_position(m_subsurface, 50, 50);
  wl_subsurface_set_sync(m_subsurface);

  m_shell_surface =
      wl_shell_get_shell_surface(m_display->GetShell(), m_surface);
  assert(m_shell_surface);
  FML_DLOG(INFO) << "EglWindow::m_shell_surface = "
                 << static_cast<void*>(m_shell_surface);

  wl_shell_surface_add_listener(m_shell_surface, &shell_surface_listener, this);

  m_display->WaitForConfig();

  m_width = (m_sprawl && m_display->GetModeWidth() > 0)
                ? m_display->GetModeWidth()
                : width;
  m_height = (m_sprawl && m_display->GetModeHeight() > 0)
                 ? m_display->GetModeHeight()
                 : height;

  m_egl_window[index] = wl_egl_window_create(m_surface, m_width, m_height);

  FML_DLOG(INFO) << "create egl_window: " << m_width << "x" << m_height;

  m_egl_surface[index] = create_egl_surface(this, m_egl_window[index], nullptr);

  wl_shell_surface_set_title(m_shell_surface, m_app_id.c_str());

  toggle_fullscreen();
  struct wl_callback* callback;

  if (m_fullscreen) {
    wl_shell_surface_set_fullscreen(m_shell_surface,
                                    WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE,
                                    60000, nullptr);
  } else {
    wl_shell_surface_set_toplevel(m_shell_surface);

    callback = wl_display_sync(m_display->GetDisplay());
    wl_callback_add_listener(callback, &shell_configure_callback_listener,
                             this);
  }
  /* Initialise damage to full surface, so the padding gets painted */
  wl_surface_damage(m_surface, 0, 0, m_width, m_height);

  MakeCurrent(m_index);
  eglSwapInterval(m_dpy, m_frame_sync);
  ClearCurrent();

  memset(m_fps, 0, sizeof(m_fps));
  m_fps_idx = 0;
  m_fps_counter = 0;

  this->m_callback = wl_surface_frame(this->m_surface);
  wl_callback_add_listener(this->m_callback, &frame_listener, this);
  wl_surface_commit(this->m_surface);

  FML_DLOG(INFO) << "- EglWindow()";
}

EglWindow::~EglWindow() {
  FML_DLOG(INFO) << "+ ~EglWindow()";

  if (m_callback)
    wl_callback_destroy(m_callback);

  if (m_buffers[0].buffer)
    wl_buffer_destroy(m_buffers[0].buffer);

  if (m_buffers[1].buffer)
    wl_buffer_destroy(m_buffers[1].buffer);

  if (m_shell_surface)
    wl_shell_surface_destroy(m_shell_surface);

  wl_surface_destroy(m_fps_surface);
  wl_surface_destroy(m_surface);

  FML_DLOG(INFO) << "- ~EglWindow()";
}

void EglWindow::buffer_release(void* data,
                               [[maybe_unused]] struct wl_buffer* buffer) {
  auto* mybuf = reinterpret_cast<struct shm_buffer*>(data);
  mybuf->busy = 0;
}

const struct wl_buffer_listener EglWindow::buffer_listener = {buffer_release};

static int os_fd_set_cloexec(int fd) {
  long flags;

  if (fd == -1)
    return -1;

  flags = fcntl(fd, F_GETFD);
  if (flags == -1)
    return -1;

  if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
    return -1;

  return 0;
}

static int set_cloexec_or_close(int fd) {
  if (os_fd_set_cloexec(fd) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int create_tmpfile_cloexec(char* tmpname) {
  int fd;

#ifdef HAVE_MKOSTEMP
  fd = mkostemp(tmpname, O_CLOEXEC);
  if (fd >= 0)
    unlink(tmpname);
#else
  mode_t prev = umask(077);
  fd = mkstemp(tmpname);
  umask(prev);
  if (fd >= 0) {
    fd = set_cloexec_or_close(fd);
    unlink(tmpname);
  }
#endif

  return fd;
}

/*
 * Create a new, unique, anonymous file of the given size, and
 * return the file descriptor for it. The file descriptor is set
 * CLOEXEC. The file is immediately suitable for mmap()'ing
 * the given size at offset zero.
 *
 * The file should not have a permanent backing store like a disk,
 * but may have if XDG_RUNTIME_DIR is not properly implemented in OS.
 *
 * The file name is deleted from the file system.
 *
 * The file is suitable for buffer sharing between processes by
 * transmitting the file descriptor over Unix sockets using the
 * SCM_RIGHTS methods.
 *
 * If the C library implements posix_fallocate(), it is used to
 * guarantee that disk space is available for the file at the
 * given size. If disk space is insufficient, errno is set to ENOSPC.
 * If posix_fallocate() is not supported, program may receive
 * SIGBUS on accessing mmap()'ed file contents instead.
 *
 * If the C library implements memfd_create(), it is used to create the
 * file purely in memory, without any backing file name on the file
 * system, and then sealing off the possibility of shrinking it.  This
 * can then be checked before accessing mmap()'ed file contents, to
 * make sure SIGBUS can't happen.  It also avoids requiring
 * XDG_RUNTIME_DIR.
 */
static int os_create_anonymous_file(off_t size) {
  static const char weston_template[] = "/weston-shared-XXXXXX";
  const char* path;
  char* name;
  int fd;
  int ret;

#ifdef HAVE_MEMFD_CREATE
  fd = memfd_create("weston-shared", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd >= 0) {
    /* We can add this seal before calling posix_fallocate(), as
     * the file is currently zero-sized anyway.
     *
     * There is also no need to check for the return value, we
     * couldn't do anything with it anyway.
     */
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) {
      return -1;
    }
  } else
#endif
  {
    path = getenv("XDG_RUNTIME_DIR");
    if (!path) {
      errno = ENOENT;
      return -1;
    }

    name = static_cast<char*>(malloc(strlen(path) + sizeof(weston_template)));
    if (!name)
      return -1;

    strcpy(name, path);
    strcat(name, weston_template);

    fd = create_tmpfile_cloexec(name);

    free(name);

    if (fd < 0)
      return -1;
  }

#ifdef HAVE_POSIX_FALLOCATE
  do {
    ret = posix_fallocate(fd, 0, size);
  } while (ret == EINTR);
  if (ret != 0) {
    close(fd);
    errno = ret;
    return -1;
  }
#else
  do {
    ret = ftruncate(fd, size);
  } while (ret < 0 && errno == EINTR);
  if (ret < 0) {
    close(fd);
    return -1;
  }
#endif

  return fd;
}

int EglWindow::create_shm_buffer(Display* display,
                                 struct shm_buffer* buffer,
                                 int width,
                                 int height,
                                 uint32_t format) {
  struct wl_shm_pool* pool;
  int fd, size, stride;
  void* data;

  stride = width * 4;
  size = stride * height;

  fd = os_create_anonymous_file(size);
  if (fd < 0) {
    FML_LOG(ERROR) << "creating a buffer file for " << size
                   << " B failed: " << strerror(errno);
    return -1;
  }

  data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    FML_LOG(ERROR) << "mmap failed: " << strerror(errno);
    close(fd);
    return -1;
  }

  pool = wl_shm_create_pool(display->GetShm(), fd, size);
  buffer->buffer =
      wl_shm_pool_create_buffer(pool, 0, width, height, stride, format);
  wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
  wl_shm_pool_destroy(pool);
  close(fd);

  buffer->shm_data = data;
  return 0;
}

void EglWindow::handle_shell_ping([[maybe_unused]] void* data,
                                  struct wl_shell_surface* shell_surface,
                                  uint32_t serial) {
  //  FML_DLOG(INFO) << "** handle_shell_ping **";
  wl_shell_surface_pong(shell_surface, serial);
}

void EglWindow::handle_shell_configure(
    void* data,
    [[maybe_unused]] struct wl_shell_surface* shell_surface,
    [[maybe_unused]] uint32_t edges,
    int32_t width,
    int32_t height) {
  auto* w = reinterpret_cast<EglWindow*>(data);
  FML_DLOG(INFO) << "** handle_shell_configure ** " << width << " * " << height;

  if (w->m_egl_window[w->m_index]) {
    wl_egl_window_resize(w->m_egl_window[w->m_index], width, height, 0, 0);
  }

  w->m_width = width;
  w->m_height = height;
}

void EglWindow::handle_shell_popup_done(
    [[maybe_unused]] void* data,
    [[maybe_unused]] struct wl_shell_surface* shell_surface) {
  FML_DLOG(INFO) << "** handle_shell_popup_done **";
}

const struct wl_shell_surface_listener EglWindow::shell_surface_listener = {
    .ping = handle_shell_ping,
    .configure = handle_shell_configure,
    .popup_done = handle_shell_popup_done};

void EglWindow::handle_shell_configure_callback(
    void* data,
    struct wl_callback* callback,
    [[maybe_unused]] uint32_t time) {
  auto* w = reinterpret_cast<EglWindow*>(data);

  FML_DLOG(INFO) << "** handle_shell_configure_callback **";

  wl_callback_destroy(callback);
  w->m_configured = true;
}

const struct wl_callback_listener EglWindow::shell_configure_callback_listener =
    {.done = handle_shell_configure_callback};

void EglWindow::paint_pixels_top(void* image,
                                 [[maybe_unused]] int padding,
                                 int width,
                                 int height,
                                 [[maybe_unused]] uint32_t time) {
  memset(image, 0xa0,
         static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
}

void EglWindow::paint_pixels_bottom(void* image,
                                    [[maybe_unused]] int padding,
                                    int width,
                                    int height,
                                    [[maybe_unused]] uint32_t time) {
  memset(image, 0xa0,
         static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
}

void EglWindow::paint_pixels(void* image,
                             int padding,
                             int width,
                             int height,
                             uint32_t time) {
  const int halfh = padding + (height - padding * 2) / 2;
  const int halfw = padding + (width - padding * 2) / 2;
  int ir;
  int _or;
  auto* pixel = reinterpret_cast<uint32_t*>(image);
  int y;

  /* squared radii thresholds */
  _or = (halfw < halfh ? halfw : halfh) - 8;
  ir = _or - 32;
  _or *= _or;
  ir *= ir;

  pixel += padding * width;
  for (y = padding; y < height - padding; y++) {
    int x;
    int y2 = (y - halfh) * (y - halfh);

    pixel += padding;
    for (x = padding; x < width - padding; x++) {
      uint32_t v;

      /* squared distance from center */
      int r2 = (x - halfw) * (x - halfw) + y2;

      if (r2 < ir)
        v = (r2 / 32 + time / 64) * 0x0080401;
      else if (r2 < _or)
        v = (y + time / 32) * 0x0080401;
      else
        v = (x + time / 16) * 0x0080401;
      v &= 0x00ffffff;

      /* cross if compositor uses X from XRGB as alpha */
      if (abs(x - y) > 6 && abs(x + y - height) > 6)
        v |= 0xff000000;

      *pixel++ = v;
    }

    pixel += padding;
  }
}

void EglWindow::redraw(void* data,
                       struct wl_callback* callback,
                       [[maybe_unused]] uint32_t time) {
  auto* window = reinterpret_cast<EglWindow*>(data);

  if (callback)
    wl_callback_destroy(callback);

  window->m_callback = wl_surface_frame(window->m_surface);
  wl_callback_add_listener(window->m_callback, &frame_listener, window);

  window->m_fps_counter++;
  window->m_fps_counter++;
}

uint32_t EglWindow::GetFpsCounter() {
  uint32_t fps_counter = m_fps_counter;
  m_fps_counter = 0;

  return fps_counter;
}

const struct wl_callback_listener EglWindow::frame_listener = {redraw};
[[maybe_unused]] EglWindow::shm_buffer* EglWindow::next_buffer(
    [[maybe_unused]] EglWindow* window) {
  return nullptr;
}
void EglWindow::toggle_fullscreen() {
  if (m_fullscreen) {
    wl_shell_surface_set_fullscreen(m_shell_surface,
                                    WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE,
                                    60000, nullptr);
  } else {
    wl_shell_surface_set_toplevel(m_shell_surface);
    handle_shell_configure(this, m_shell_surface, 0, m_width, m_height);
  }

  wl_callback_add_listener(wl_display_sync(m_display->GetDisplay()),
                           &shell_configure_callback_listener, this);
}

bool EglWindow::ActivateSystemCursor(int32_t device, const std::string& kind) {
  return m_display->ActivateSystemCursor(device, kind);
}

void EglWindow::DrawFps(uint8_t fps) {
  const int bars = 20;
  const int bar_w = 10;
  const int bar_space = 2;
  const int surface_w = bars * (bar_w + bar_space) - bar_space;
  const int surface_h = 150;
  int x, y;

  // update fps array
  m_fps[m_fps_idx] = fps;
  m_fps_idx = (m_fps_idx + 1) % bars;

  // create buffer
  if (!m_fps_buffer.buffer) {
    create_shm_buffer(m_display.get(), &m_fps_buffer, surface_w, surface_h,
                      WL_SHM_FORMAT_XRGB8888);
  }
  memset(m_fps_buffer.shm_data, 0x00,
         static_cast<size_t>(surface_w) * static_cast<size_t>(surface_h) * 4);

  // draw bar
  [[maybe_unused]] auto pixels =
      reinterpret_cast<uint32_t*>(m_fps_buffer.shm_data);

  for (int i = 0; i < bars; i++) {
    [[maybe_unused]] auto p =
        std::clamp(m_fps[(m_fps_idx + i) % bars] / 60.0, 0.0, 1.0);
    int draw_y = surface_h * (1.0 - p);
    int draw_x = i * (bar_w + bar_space);

    if (draw_y < 0)
      draw_y = 0;

    for (y = draw_y; y < surface_h; y++) {
      for (x = draw_x; x < draw_x + bar_w; x++) {
        pixels[y * surface_w + x] = 0xFF << 24 | (int)(0xFF * (1.0 - p)) << 16 |
                                    (int)(0xFF * p) << 8 | 0x00 << 0;
      }
    }
  }

  // commit buffer
  wl_surface_attach(m_fps_surface, m_fps_buffer.buffer, 0, 0);
  wl_surface_damage(m_fps_surface, 0, 0, surface_w, surface_h);
  wl_surface_commit(m_fps_surface);
}
