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

/* Cursor shapes (the VIO_CURSOR_SHAPE_* values, GLFW 3.4's standard cursors). */
#define VIO_PLATFORM_SHAPE_ARROW        0
#define VIO_PLATFORM_SHAPE_IBEAM        1
#define VIO_PLATFORM_SHAPE_CROSSHAIR    2
#define VIO_PLATFORM_SHAPE_HAND         3
#define VIO_PLATFORM_SHAPE_RESIZE_EW    4
#define VIO_PLATFORM_SHAPE_RESIZE_NS    5
#define VIO_PLATFORM_SHAPE_RESIZE_NWSE  6
#define VIO_PLATFORM_SHAPE_RESIZE_NESW  7
#define VIO_PLATFORM_SHAPE_RESIZE_ALL   8
#define VIO_PLATFORM_SHAPE_NOT_ALLOWED  9
#define VIO_PLATFORM_SHAPE_COUNT        10

/* vio_file_dialog (DESKTOP-APP-PLAN Phase 2). Patterns are bare extensions
 * ("png"); the caller has checked them (no separators or wildcards). */
#define VIO_PLATFORM_DIALOG_OPEN   0
#define VIO_PLATFORM_DIALOG_SAVE   1
#define VIO_PLATFORM_DIALOG_FOLDER 2
typedef struct _vio_dialog_filter {
    const char  *name;
    const char **extensions;
    int          count;
} vio_dialog_filter;
typedef struct _vio_file_dialog_desc {
    int                      mode;
    int                      multiple;
    const char              *title;          /* NULL: the platform's */
    const char              *default_path;   /* NULL, a folder, or (save) a file */
    const vio_dialog_filter *filters;
    int                      filter_count;
} vio_file_dialog_desc;

#define VIO_PLATFORM_MESSAGE_INFO     0
#define VIO_PLATFORM_MESSAGE_WARNING  1
#define VIO_PLATFORM_MESSAGE_ERROR    2
#define VIO_PLATFORM_MESSAGE_QUESTION 3
typedef struct _vio_message_box_desc {
    const char  *title, *message;
    const char **buttons;   /* 1..3; closing the box (Escape) picks the last */
    int          button_count;
    int          kind;
} vio_message_box_desc;

/* Menus (DESKTOP-APP-PLAN Phase 2b). A leaf has a tag (>= 0, its index in
 * the caller's id table) and is what a selection reports; a submenu has
 * children; a separator has neither. key/mods: VIO_KEY_* / VIO_MOD_* of the
 * shortcut, key 0 = none. The platform copies what it keeps. */
typedef struct _vio_menu_item {
    const char *label;
    int         tag;          /* -1 for submenus and separators */
    int         separator;
    int         enabled, checked;
    int         key, mods;
    const struct _vio_menu_item *children;
    int         child_count;
} vio_menu_item;

/* An RGBA image (window icon), top row first. */
typedef struct _vio_platform_image {
    int width, height;
    const unsigned char *rgba;
} vio_platform_image;

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
    /* Block until an event arrives, timeout seconds pass (< 0: no timeout) or
     * wake() was called, then dispatch what is queued like poll_events. A
     * wake() while nobody waits makes the next wait return at once; a poll
     * leaves it pending. Waits may end early on events vio ignores. */
    void  (*wait_events)(double timeout);
    void  (*wake)(void);                                              /* any thread */
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

    /* ── Clipboard (process-wide UTF-8 text, no window needed) ─────────
     * NULL slots: no system clipboard on this platform - vio_clipboard_*
     * then keep a process-local one. Both return 0 when the system
     * clipboard cannot be reached right now (no display yet), so the
     * caller falls back the same way.
     * clipboard_get: 1 = answered; *out_utf8 is malloc'd (caller free()s)
     * or NULL when the clipboard is empty / holds no text. */
    int   (*clipboard_get)(char **out_utf8);
    int   (*clipboard_set)(const char *utf8);

    /* ── Keyboard layout ──────────────────────────────────────────────
     * What printable key $key (VIO_KEY_*: physical position, US names)
     * types in the current layout without modifiers, lower case UTF-8 in a
     * static buffer (valid until the next call), NULL when it types nothing
     * printable. NULL slot: the layout is unknown (vio_key_name -> null).
     * GLFW's glfwGetKeyName semantics. */
    const char *(*key_name)(int key);

    /* ── Desktop window details (DESKTOP-APP-PLAN Phase 1) ─────────────
     * NULL slot: the platform has no such thing (vio_platform_info says
     * false, the PHP call returns false). Each returns 1 when applied.
     * Sizes are in the platform's screen coordinates, -1 = no limit. */
    int   (*set_cursor_shape)(vio_window_handle w, int shape);       /* VIO_PLATFORM_SHAPE_* */
    /* RGBA, top row first; replaces the shape until the next set_cursor_shape. */
    int   (*set_cursor_image)(vio_window_handle w, const unsigned char *rgba, int width, int height, int hot_x, int hot_y);
    /* count 0: back to the default icon. macOS has no window icons: the Dock icon. */
    int   (*set_window_icon)(vio_window_handle w, int count, const vio_platform_image *images);
    int   (*set_size_limits)(vio_window_handle w, int min_w, int min_h, int max_w, int max_h);
    int   (*set_aspect_ratio)(vio_window_handle w, int numerator, int denominator);   /* 0, 0: free */
    int   (*set_floating)(vio_window_handle w, int floating);
    int   (*set_opacity)(vio_window_handle w, float opacity);             /* 0..1 */
    /* No window needed. url: has a scheme, never file: (checked by the caller).
     * path: exists (checked by the caller). */
    int   (*open_url)(const char *utf8_url);
    int   (*reveal_file)(const char *utf8_path);

    /* ── Desktop integration (DESKTOP-APP-PLAN Phase 2) ──────────────────
     * File drops need no slot to start them: a platform that accepts them
     * sets accepts_drops and reports each drop through vio_input_drop_event. */
    int   accepts_drops;
    /* Modal. 1 = answered: *out_paths (malloc'd array of malloc'd UTF-8
     * strings, caller frees) with *out_count entries, 0 entries = cancelled.
     * 0 = could not show one. The window may be NULL (no window). */
    int   (*file_dialog)(vio_window_handle w, const vio_file_dialog_desc *d, char ***out_paths, int *out_count);
    /* Modal. The pressed button's index, -1 = could not show one. */
    int   (*message_box)(vio_window_handle w, const vio_message_box_desc *d);
    /* The window's menu bar (macOS: the application's, behind vio's app
     * menu); count 0 removes it. Selections arrive as vio_input_menu_event
     * with the item's tag. 1 = set. */
    int   (*set_menu_bar)(vio_window_handle w, const vio_menu_item *menus, int count);
    /* Modal context menu at x, y (cursor space): the chosen tag, -1 = none,
     * -2 = could not show one. */
    int   (*popup_menu)(vio_window_handle w, const vio_menu_item *items, int count, double x, double y);
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

/* Wake helpers shared by the platforms (src/vio_platform_registry.c).
 * wake_signal sets the sticky flag and makes the waitable ready (any thread);
 * wake_take clears both and returns whether the flag was set. The waitable is
 * wake_fd (POSIX, readable while set) or wake_handle (Windows HANDLE, signalled
 * while set). timeout_ms rounds a wait_events timeout up to milliseconds,
 * -1 for none. */
void  vio_platform_wake_signal(void);
int   vio_platform_wake_take(void);
int   vio_platform_wake_pending(void);
int   vio_platform_wake_fd(void);
void *vio_platform_wake_handle(void);
int   vio_platform_timeout_ms(double timeout);
void  vio_platform_wait_wake_only(double timeout);

/* open_url / reveal_file by spawning an opener (src/vio_platform_registry.c,
 * POSIX only): VIO_OPEN_COMMAND if set, else default_cmd; argv = cmd, args.
 * Detached, no shell; children are reaped on later calls. 1 = started. */
int   vio_platform_spawn_opener(const char *default_cmd, const char *const *args, int nargs);

/* VIO_TEST_DIALOG="<ms>:<action>" (tests only): the platform dismisses its
 * next dialog after ms - action "cancel", "accept" (save: the default name) or
 * a button index. 1 = set, *ms and *action (cancel -1, accept -2, index) filled. */
int   vio_platform_test_dialog(int *ms, int *action);

/* Implementations */
void vio_platform_null_register(void);
#ifdef HAVE_GLFW
void vio_platform_glfw_register(void);
#endif
#ifdef _WIN32
void vio_platform_win32_register(void);
#endif
#ifdef HAVE_X11
void vio_platform_x11_register(void);
#endif
#ifdef HAVE_WAYLAND
void vio_platform_wayland_register(void);
#endif
#if defined(HAVE_COCOA) && !defined(HAVE_IOS)
void vio_platform_cocoa_register(void);
#endif

#endif /* VIO_PLATFORM_H */
