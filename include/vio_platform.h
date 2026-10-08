/*
 * php-vio - Platform layer (windowing, input, monitors, gamepads)
 *
 * NATIVE-PLATFORM-PLAN / WIN32-PLATFORM-PLAN, OPEN-ITEMS A1. Everything the
 * extension needs from the window system goes through this vtable, the way
 * every GPU operation goes through vio_backend: GLFW is one implementation
 * (src/platform/glfw/), native ones sit next to it (src/platform/win32/, ...),
 * and the null platform (src/platform/null/) answers when there is none.
 * Audit gate 212: no glfw* / GLFW_ outside src/platform/glfw/.
 *
 * The slots keep GLFW's semantics (sizes in the platform's screen
 * coordinates, framebuffer sizes in pixels, monitors by index instead of
 * pointer), so the callers did not change behaviour when they moved here.
 */

#ifndef VIO_PLATFORM_H
#define VIO_PLATFORM_H

#include <stdint.h>
#include <string.h>
#include "vio_types.h"

/* What the active platform created; only it knows the real type. */
typedef void *vio_window_handle;

typedef enum _vio_native_kind {
    VIO_NATIVE_HWND = 0,          /* Windows: HWND */
    VIO_NATIVE_HINSTANCE,         /* Windows: HINSTANCE of the window class */
    VIO_NATIVE_NSWINDOW,          /* macOS: NSWindow* */
    VIO_NATIVE_XLIB_WINDOW,       /* X11: Window */
    VIO_NATIVE_XLIB_DISPLAY,      /* X11: Display* */
    VIO_NATIVE_WAYLAND_SURFACE,   /* Wayland: wl_surface* */
    VIO_NATIVE_WAYLAND_DISPLAY    /* Wayland: wl_display* */
} vio_native_kind;

/* Window attributes the API reads or sets. */
typedef enum _vio_window_attrib {
    VIO_WINDOW_DECORATED = 0,
    VIO_WINDOW_MAXIMIZED,
    VIO_WINDOW_AUTO_ICONIFY,
    VIO_WINDOW_FOCUSED,
    VIO_WINDOW_VISIBLE
} vio_window_attrib;

/* Cursor modes (the VIO_CURSOR_* values of vio_set_cursor_mode). */
#define VIO_PLATFORM_CURSOR_NORMAL   0
#define VIO_PLATFORM_CURSOR_DISABLED 1
#define VIO_PLATFORM_CURSOR_HIDDEN   2

/* "Leave it to the platform" for a refresh rate. */
#define VIO_PLATFORM_DONT_CARE (-1)

typedef struct _vio_video_mode {
    int width, height, refresh_hz;
    int red_bits, green_bits, blue_bits;
} vio_video_mode;

typedef struct _vio_monitor_desc {
    char  name[128];
    int   x, y;                                 /* position on the virtual desktop */
    int   work_x, work_y, work_width, work_height;
    float scale_x, scale_y;                     /* content scale (1.0 = 96 dpi) */
    vio_video_mode mode;                        /* current mode */
    int   primary;
} vio_monitor_desc;

/* Joysticks: ids 0..VIO_PLATFORM_JOYSTICK_LAST (GLFW's 16 slots). */
#define VIO_PLATFORM_JOYSTICK_LAST 15

struct _vio_input_state;

typedef struct _vio_platform {
    const char *name;                       /* "glfw" | "win32" | "cocoa" | "x11" | "null" */

    /* ── Process lifecycle (MINIT / MSHUTDOWN) ─────────────────────── */
    int   (*init)(void);                    /* 1 = usable */
    void  (*shutdown)(void);

    /* ── Window lifecycle ─────────────────────────────────────────── */
    /* backend_name selects the surface: "opengl" makes a GL context current
     * (version ladder 4.6 -> 3.0), anything else a window without client API. */
    vio_window_handle (*create_window)(vio_config *cfg, const char *backend_name);
    void  (*destroy_window)(vio_window_handle w);
    int   (*should_close)(vio_window_handle w);
    void  (*set_should_close)(vio_window_handle w, int value);

    /* ── Frame ────────────────────────────────────────────────────── */
    void  (*poll_events)(void);
    void  (*wait_events)(void);                                       /* block until an event (minimised window) */
    void  (*swap_buffers)(vio_window_handle w);                       /* GL */
    void  (*get_framebuffer_size)(vio_window_handle w, int *width, int *height);
    void  (*get_window_size)(vio_window_handle w, int *width, int *height);
    void  (*get_content_scale)(vio_window_handle w, float *sx, float *sy);
    void  (*get_cursor_pos)(vio_window_handle w, double *x, double *y);

    /* ── GL context (NULL slots: no native GL on this platform) ────── */
    int   (*gl_make_current)(vio_window_handle w);
    void  (*gl_swap_interval)(int interval);
    void *(*gl_get_proc_address)(const char *name);

    /* ── Window properties ────────────────────────────────────────── */
    void  (*set_title)(vio_window_handle w, const char *utf8);
    void  (*set_window_size)(vio_window_handle w, int width, int height);
    void  (*get_window_pos)(vio_window_handle w, int *x, int *y);
    void  (*set_window_pos)(vio_window_handle w, int x, int y);
    int   (*get_attrib)(vio_window_handle w, vio_window_attrib attrib);
    void  (*set_attrib)(vio_window_handle w, vio_window_attrib attrib, int value);
    void  (*maximize)(vio_window_handle w);
    void  (*restore)(vio_window_handle w);
    void  (*set_cursor_mode)(vio_window_handle w, int mode);          /* VIO_PLATFORM_CURSOR_*; DISABLED also asks for raw motion */
    /* The monitor a fullscreen window occupies, -1 when windowed. */
    int   (*window_monitor)(vio_window_handle w);
    /* monitor >= 0: exclusive fullscreen on it at width x height @ refresh;
     * monitor < 0: windowed at x, y, width, height. */
    void  (*set_window_monitor)(vio_window_handle w, int monitor, int x, int y, int width, int height, int refresh_hz);

    /* ── Monitors ─────────────────────────────────────────────────── */
    int   (*monitor_count)(void);
    int   (*primary_monitor)(void);                                   /* index, -1 = none */
    int   (*monitor_desc)(int index, vio_monitor_desc *out);          /* 0 = filled */
    int   (*video_modes)(int index, vio_video_mode *out, int max);    /* count, ascending */

    /* ── Joysticks / gamepads ─────────────────────────────────────── */
    int   (*joystick_present)(int id);
    int   (*joystick_is_gamepad)(int id);
    const char *(*joystick_name)(int id);
    const char *(*gamepad_name)(int id);
    /* Mapped state: buttons in VIO_GAMEPAD_* order (1 = pressed, 15 of them),
     * axes in VIO_GAMEPAD_AXIS_* order (6). 0 = no mapping for this joystick. */
    int   (*gamepad_state)(int id, unsigned char buttons[15], float axes[6]);
    const unsigned char *(*joystick_buttons)(int id, int *count);
    const float *(*joystick_axes)(int id, int *count);

    /* ── Native handles for the GPU backends ──────────────────────── */
    void *(*native_handle)(vio_window_handle w, vio_native_kind kind);

    /* ── Vulkan surface (NULL: the backend cannot present on this platform) ── */
    const char **(*vk_instance_extensions)(uint32_t *count);
    /* instance: VkInstance, out_surface: VkSurfaceKHR*. Returns a VkResult. */
    int   (*vk_create_surface)(vio_window_handle w, void *instance, void *out_surface);

    /* ── Input: route the window's events into the context's input state ── */
    void  (*install_input)(vio_window_handle w, struct _vio_input_state *state);
} vio_platform;

/* Registry (src/vio_platform_registry.c): platforms register in MINIT; the
 * active one is chosen once (VIO_PLATFORM env or the first registered of
 * win32 > cocoa > x11 > glfw) and is never NULL - the null platform is the
 * floor. */
int                 vio_register_platform(const vio_platform *p);
const vio_platform *vio_find_platform(const char *name);
const vio_platform *vio_platform_active(void);
void                vio_platform_set_active(const vio_platform *p);
/* The active platform (shorthand used at every call site). */
#define vio_plat() vio_platform_active()
/* 0 when there is no window system (the null platform): no windows, offscreen only. */
#define vio_platform_has_windows() (vio_platform_active()->create_window && strcmp(vio_platform_active()->name, "null") != 0)

/* Implementations */
void vio_platform_null_register(void);
#ifdef HAVE_GLFW
void vio_platform_glfw_register(void);
#endif
#ifdef _WIN32
void vio_platform_win32_register(void);
#endif

#endif /* VIO_PLATFORM_H */
