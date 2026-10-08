/*
 * php-vio - Linux gamepads through evdev, shared by the X11 and Wayland
 * platforms (header-only: each platform keeps its own table, only one is
 * active). Pads in the kernel's gamepad layout - BTN_GAMEPAD plus ABS_X -
 * are opened from /dev/input/event* (rescanned at most once a second) and
 * mapped to VIO_GAMEPAD_* in the xpad convention: BTN_X is X, BTN_Y is Y,
 * sticks -1..1 with Y down positive (up = -1, as GLFW), triggers -1..1.
 */

#ifndef VIO_EVDEV_GAMEPAD_H
#define VIO_EVDEV_GAMEPAD_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* ── Gamepads: evdev ──────────────────────────────────────────────── */

#define EVDEV_PADS 16
#define EVDEV_BITS(n) (((n) + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long)))

typedef struct {
    int           fd;
    char          path[64];
    char          name[128];
    unsigned char buttons[15];
    float         axes[6];
    int           hat_x, hat_y;
    struct input_absinfo abs[ABS_CNT];
    int           has_abs[ABS_CNT];
} evdev_pad;

static evdev_pad evdev_pads[EVDEV_PADS];
static double  evdev_pad_scan_time = -10.0;

static double evdev_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int evdev_test_bit(const unsigned long *bits, int n)
{
    return (bits[n / (8 * sizeof(unsigned long))] >> (n % (8 * sizeof(unsigned long)))) & 1;
}

static void evdev_pad_close(evdev_pad *p)
{
    if (p->fd > 0) close(p->fd);
    memset(p, 0, sizeof(*p));
}

/* Opens new gamepads at most once a second (vio_gamepads() asks every frame). */
static void evdev_pads_scan(void)
{
    double now = evdev_now();
    if (now - evdev_pad_scan_time < 1.0) return;
    evdev_pad_scan_time = now;
    DIR *dir = opendir("/dev/input");
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int known = 0;
        for (int i = 0; i < EVDEV_PADS && !known; i++) known = evdev_pads[i].fd > 0 && strcmp(evdev_pads[i].path, path) == 0;
        if (known) continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long keys[EVDEV_BITS(KEY_CNT)], abs[EVDEV_BITS(ABS_CNT)];
        memset(keys, 0, sizeof(keys));
        memset(abs, 0, sizeof(abs));
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
        if (!evdev_test_bit(keys, BTN_GAMEPAD) || !evdev_test_bit(abs, ABS_X)) { close(fd); continue; }
        int slot = -1;
        for (int i = 0; i < EVDEV_PADS && slot < 0; i++) if (evdev_pads[i].fd <= 0) slot = i;
        if (slot < 0) { close(fd); break; }
        evdev_pad *p = &evdev_pads[slot];
        memset(p, 0, sizeof(*p));
        p->fd = fd;
        snprintf(p->path, sizeof(p->path), "%s", path);
        if (ioctl(fd, EVIOCGNAME(sizeof(p->name) - 1), p->name) < 0) snprintf(p->name, sizeof(p->name), "Gamepad");
        for (int a = 0; a < ABS_CNT; a++) {
            if (evdev_test_bit(abs, a) && ioctl(fd, EVIOCGABS(a), &p->abs[a]) == 0) p->has_abs[a] = 1;
        }
        p->axes[4] = p->axes[5] = -1.0f;   /* released triggers */
    }
    closedir(dir);
}

static float evdev_pad_norm(evdev_pad *p, int code, int value)
{
    const struct input_absinfo *ai = &p->abs[code];
    if (ai->maximum == ai->minimum) return 0.0f;
    float v = 2.0f * (float)(value - ai->minimum) / (float)(ai->maximum - ai->minimum) - 1.0f;
    return v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
}

/* Kernel gamepad layout (xpad convention: BTN_X is X, BTN_Y is Y) -> VIO_GAMEPAD_* order. */
static void evdev_pad_key(evdev_pad *p, int code, int value)
{
    int b = -1;
    switch (code) {
        case BTN_A: b = 0; break;       case BTN_B: b = 1; break;
        case BTN_X: b = 2; break;       case BTN_Y: b = 3; break;
        case BTN_TL: b = 4; break;      case BTN_TR: b = 5; break;
        case BTN_SELECT: b = 6; break;  case BTN_START: b = 7; break;
        case BTN_MODE: b = 8; break;    case BTN_THUMBL: b = 9; break;
        case BTN_THUMBR: b = 10; break;
        case BTN_DPAD_UP: b = 11; break; case BTN_DPAD_RIGHT: b = 12; break;
        case BTN_DPAD_DOWN: b = 13; break; case BTN_DPAD_LEFT: b = 14; break;
    }
    if (b >= 0) p->buttons[b] = value ? 1 : 0;
}

static void evdev_pad_abs(evdev_pad *p, int code, int value)
{
    switch (code) {
        case ABS_X:  p->axes[0] = evdev_pad_norm(p, code, value); break;
        case ABS_Y:  p->axes[1] = evdev_pad_norm(p, code, value); break;   /* down positive: up = -1 */
        case ABS_RX: p->axes[2] = evdev_pad_norm(p, code, value); break;
        case ABS_RY: p->axes[3] = evdev_pad_norm(p, code, value); break;
        case ABS_Z: case ABS_BRAKE: p->axes[4] = evdev_pad_norm(p, code, value); break;
        case ABS_RZ: case ABS_GAS:  p->axes[5] = evdev_pad_norm(p, code, value); break;
        case ABS_HAT0X: p->hat_x = value; p->buttons[14] = value < 0; p->buttons[12] = value > 0; break;
        case ABS_HAT0Y: p->hat_y = value; p->buttons[11] = value < 0; p->buttons[13] = value > 0; break;
    }
}

/* Drain the pad's pending events; 0 when it is gone. */
static int evdev_pad_update(int id)
{
    if (id < 0 || id >= EVDEV_PADS) return 0;
    evdev_pads_scan();
    evdev_pad *p = &evdev_pads[id];
    if (p->fd <= 0) return 0;
    struct input_event ev[32];
    for (;;) {
        ssize_t n = read(p->fd, ev, sizeof(ev));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) break;
            evdev_pad_close(p);   /* unplugged */
            return 0;
        }
        if (n == 0) break;
        for (size_t i = 0; i < (size_t)n / sizeof(ev[0]); i++) {
            if (ev[i].type == EV_KEY) evdev_pad_key(p, ev[i].code, ev[i].value);
            else if (ev[i].type == EV_ABS) evdev_pad_abs(p, ev[i].code, ev[i].value);
        }
    }
    return 1;
}

static int evdev_joystick_present(int id) { return evdev_pad_update(id); }

static const char *evdev_pad_name(int id)
{
    return evdev_pad_update(id) ? evdev_pads[id].name : NULL;
}

static int evdev_gamepad_state(int id, unsigned char buttons[15], float axes[6])
{
    if (!evdev_pad_update(id)) return 0;
    memcpy(buttons, evdev_pads[id].buttons, 15);
    memcpy(axes, evdev_pads[id].axes, sizeof(float) * 6);
    return 1;
}

static const unsigned char *evdev_joystick_buttons(int id, int *count)
{
    *count = 0;
    if (!evdev_pad_update(id)) return NULL;
    *count = 15;
    return evdev_pads[id].buttons;
}

static const float *evdev_joystick_axes(int id, int *count)
{
    *count = 0;
    if (!evdev_pad_update(id)) return NULL;
    *count = 6;
    return evdev_pads[id].axes;
}

#endif /* VIO_EVDEV_GAMEPAD_H */
