/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * uinput_gen — synthetic input generator for the event-driven input harness
 * (test/input_event_driven.sh). Creates one virtual device via /dev/uinput and
 * injects a bounded, deterministic event stream so a libinput seat has real
 * devices to process. Opening /dev/uinput needs privilege (it is root-only on a
 * stock distro); the harness runs this under sudo.
 *
 *   uinput_gen mouse    --hz <rate> --seconds <s>            EV_REL pointer storm
 *   uinput_gen keyboard --hold <keysym> --seconds <s>        one key held down
 *   uinput_gen touch    --hz <rate> --seconds <s> --slots N  protocol-B MT burst
 *
 * The device is created, a settle delay lets udev + libinput enumerate it, the
 * stream runs for --seconds, then the device is destroyed. Exit 0 on success.
 */

#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void emit(int fd, int type, int code, int val) {
  struct input_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = type;
  ev.code = code;
  ev.value = val;
  if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
    /* best-effort: a dropped event just thins the stream */
  }
}

static void syn(int fd) {
  emit(fd, EV_SYN, SYN_REPORT, 0);
}

/* Sleep for one 1/hz slice (hz<=0 => a single frame, no pacing). */
static void pace(long hz) {
  if (hz <= 0) {
    return;
  }
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = 1000000000L / hz;
  nanosleep(&ts, NULL);
}

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int setup_common(int fd, const char* name) {
  struct uinput_setup us;
  memset(&us, 0, sizeof(us));
  us.id.bustype = BUS_USB;
  us.id.vendor = 0x1d6b;   /* placeholder */
  us.id.product = 0x0104;  /* placeholder */
  strncpy(us.name, name, sizeof(us.name) - 1);
  if (ioctl(fd, UI_DEV_SETUP, &us) < 0) {
    perror("UI_DEV_SETUP");
    return -1;
  }
  if (ioctl(fd, UI_DEV_CREATE) < 0) {
    perror("UI_DEV_CREATE");
    return -1;
  }
  return 0;
}

static int run_mouse(int fd, long hz, long seconds) {
  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
  ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
  ioctl(fd, UI_SET_EVBIT, EV_REL);
  ioctl(fd, UI_SET_RELBIT, REL_X);
  ioctl(fd, UI_SET_RELBIT, REL_Y);
  if (setup_common(fd, "ivi-uinput-mouse") < 0) {
    return 1;
  }
  sleep(1);  /* udev + libinput enumerate */
  const long deadline = now_ms() + seconds * 1000;
  int dx = 4;
  long i = 0;
  while (now_ms() < deadline) {
    emit(fd, EV_REL, REL_X, dx);
    emit(fd, EV_REL, REL_Y, (i & 8) ? 3 : -3);
    syn(fd);
    if ((++i % 40) == 0) {
      dx = -dx;  /* stay roughly in place */
    }
    pace(hz);
  }
  return 0;
}

static int run_keyboard(int fd, int keysym, long seconds) {
  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  ioctl(fd, UI_SET_KEYBIT, keysym);
  if (setup_common(fd, "ivi-uinput-keyboard") < 0) {
    return 1;
  }
  sleep(1);
  emit(fd, EV_KEY, keysym, 1);  /* press */
  syn(fd);
  const long deadline = now_ms() + seconds * 1000;
  while (now_ms() < deadline) {
    pace(1000);  /* hold; the seat's repeater generates ticks */
  }
  emit(fd, EV_KEY, keysym, 0);  /* release */
  syn(fd);
  return 0;
}

static int run_touch(int fd, long hz, long seconds, int slots) {
  ioctl(fd, UI_SET_EVBIT, EV_ABS);
  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
  ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
  struct uinput_abs_setup abs;
  const int codes[] = {ABS_MT_SLOT,      ABS_MT_TRACKING_ID, ABS_MT_POSITION_X,
                       ABS_MT_POSITION_Y, ABS_X,             ABS_Y};
  const int maxes[] = {slots - 1, 65535, 1920, 1080, 1920, 1080};
  for (size_t k = 0; k < sizeof(codes) / sizeof(codes[0]); ++k) {
    ioctl(fd, UI_SET_ABSBIT, codes[k]);
    memset(&abs, 0, sizeof(abs));
    abs.code = codes[k];
    abs.absinfo.minimum = 0;
    abs.absinfo.maximum = maxes[k];
    ioctl(fd, UI_ABS_SETUP, &abs);
  }
  if (setup_common(fd, "ivi-uinput-touch") < 0) {
    return 1;
  }
  sleep(1);
  const long deadline = now_ms() + seconds * 1000;
  int frame = 0;
  while (now_ms() < deadline) {
    for (int s = 0; s < slots; ++s) {
      emit(fd, EV_ABS, ABS_MT_SLOT, s);
      emit(fd, EV_ABS, ABS_MT_TRACKING_ID, 100 + s);  /* down/persist */
      emit(fd, EV_ABS, ABS_MT_POSITION_X, 100 + s * 40 + (frame % 20));
      emit(fd, EV_ABS, ABS_MT_POSITION_Y, 200 + (frame % 30));
    }
    emit(fd, EV_KEY, BTN_TOUCH, 1);
    syn(fd);  /* one flush == one frame == 10 slot updates */
    ++frame;
    pace(hz);
  }
  /* lift all */
  for (int s = 0; s < slots; ++s) {
    emit(fd, EV_ABS, ABS_MT_SLOT, s);
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
  }
  emit(fd, EV_KEY, BTN_TOUCH, 0);
  syn(fd);
  return 0;
}

static long arg_long(int argc, char** argv, const char* flag, long def) {
  for (int i = 1; i < argc - 1; ++i) {
    if (strcmp(argv[i], flag) == 0) {
      return atol(argv[i + 1]);
    }
  }
  return def;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr,
            "usage: %s mouse|keyboard|touch [--hz N] [--seconds S] "
            "[--hold KEYSYM] [--slots N]\n",
            argv[0]);
    return 2;
  }
  const char* mode = argv[1];
  const long hz = arg_long(argc, argv, "--hz", 1000);
  const long seconds = arg_long(argc, argv, "--seconds", 10);
  const int hold = (int)arg_long(argc, argv, "--hold", KEY_A);
  const int slots = (int)arg_long(argc, argv, "--slots", 10);

  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) {
    perror("open /dev/uinput (needs privilege)");
    return 1;
  }

  int rc;
  if (strcmp(mode, "mouse") == 0) {
    rc = run_mouse(fd, hz, seconds);
  } else if (strcmp(mode, "keyboard") == 0) {
    rc = run_keyboard(fd, hold, seconds);
  } else if (strcmp(mode, "touch") == 0) {
    rc = run_touch(fd, hz, seconds, slots);
  } else {
    fprintf(stderr, "unknown mode '%s'\n", mode);
    rc = 2;
  }

  ioctl(fd, UI_DEV_DESTROY);
  close(fd);
  return rc;
}
