/*
 * php-vio - GLFW platform (NATIVE-PLATFORM-PLAN Phase 0, OPEN-ITEMS A1)
 *
 * The only file that talks to GLFW (audit gate 212). It holds what used to be
 * src/vio_window.c (window creation with the GL version ladder, Windows DPI
 * awareness and cursor-monitor placement), the input callbacks that were in
 * src/vio_input.c, and the window / monitor / joystick / native-handle /
 * Vulkan-surface queries php_vio.c and the GPU backends made directly.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "../../../include/vio_platform.h"
#include "../../vio_input.h"

#ifdef HAVE_GLFW

#ifdef HAVE_VULKAN
#include <vulkan/vulkan.h>   /* before glfw3.h: declares glfwCreateWindowSurface */
#endif
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#if defined(__APPLE__)
#  define GLFW_EXPOSE_NATIVE_COCOA
#elif defined(_WIN32)
#  define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
#  define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3native.h>

#ifdef _WIN32
#  include <windows.h>
#endif

static int glfw_initialized = 0;

static void vio_glfw_error_callback(int error, const char *description)
{
    php_error_docref(NULL, E_WARNING, "GLFW error %d: %s", error, description);
}

#ifdef _WIN32
/*
 * Mark the process as Per-Monitor-DPI-Aware V2 before any window is created.
 * Without this, Windows DPI-virtualizes the process: glfwGetFramebufferSize()
 * returns logical pixels (== window size) and DWM stretches the swapchain,
 * producing a blurry image on HiDPI monitors. Loaded dynamically so the
 * extension still runs on Windows versions that lack the V2 entry point.
 */
static void vio_enable_dpi_awareness_windows(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }

    typedef BOOL (WINAPI *SetProcessDpiAwarenessContext_t)(HANDLE);
    SetProcessDpiAwarenessContext_t pSetProcessDpiAwarenessContext =
        (SetProcessDpiAwarenessContext_t)GetProcAddress(user32, "SetProcessDpiAwarenessContext");

    if (pSetProcessDpiAwarenessContext) {
        /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == ((HANDLE)-4) */
        pSetProcessDpiAwarenessContext((HANDLE)-4);
    }
}

/*
 * Position a freshly created window centered on the monitor that currently
 * contains the cursor. This avoids the "wrong monitor" UX problem in
 * multi-monitor setups where the user is interacting with one screen but
 * GLFW would otherwise place the window on the primary. Creating the window
 * on the target monitor from frame 0 also prevents per-monitor DPI changes
 * from invalidating the Vulkan swapchain mid-flight.
 */
static void vio_place_window_on_cursor_monitor(GLFWwindow *window)
{
    if (!window) {
        return;
    }

    POINT pt;
    if (!GetCursorPos(&pt)) {
        return;
    }

    HMONITOR hmon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!hmon) {
        return;
    }

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, &mi)) {
        return;
    }

    int work_w = mi.rcWork.right  - mi.rcWork.left;
    int work_h = mi.rcWork.bottom - mi.rcWork.top;

    int win_w = 0, win_h = 0;
    glfwGetWindowSize(window, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) {
        return;
    }

    int x = mi.rcWork.left + (work_w - win_w) / 2;
    int y = mi.rcWork.top  + (work_h - win_h) / 2;
    glfwSetWindowPos(window, x, y);
}
#endif

static int glfw_init(void)
{
    if (glfw_initialized) {
        return 1;
    }

#ifdef _WIN32
    vio_enable_dpi_awareness_windows();
#endif

    glfwSetErrorCallback(vio_glfw_error_callback);

    if (!glfwInit()) {
        php_error_docref(NULL, E_WARNING, "Failed to initialize GLFW");
        return 0;
    }

    glfw_initialized = 1;
    return 1;
}

static void glfw_shutdown(void)
{
    if (glfw_initialized) {
        glfwTerminate();
        glfw_initialized = 0;
    }
}

static vio_window_handle glfw_create_window(vio_config *cfg, const char *backend_name)
{
    if (!glfw_initialized) {
        if (!glfw_init()) {
            return NULL;
        }
    }

    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    /* Auto-scale window size by monitor content scale on HiDPI displays.
     * Combined with per-monitor DPI awareness this means: a request for
     * 1280x720 on a 4K@200% display creates a 2560x1440 physical window,
     * glfwGetWindowSize() returns 1280x720 (logical, layout-stable), and
     * glfwGetFramebufferSize() returns 2560x1440 (physical, sharp render).
     *
     * Headless contexts skip the auto-scale — they render to a hidden FBO
     * at the requested pixel dimensions, so DPI awareness only introduces
     * unwanted asymmetric scaling (which is why headless callers asking for
     * 64×48 used to get 120×48 on a 1.875× display). */
    if (!cfg->headless) {
        glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    } else {
        /* Undecorated: a decorated (WS_OVERLAPPEDWINDOW) window has an OS
         * minimum width, so a hidden 32x32 headless window came back with a
         * ~350px client area — and backends that render into the swapchain /
         * surface (D3D11, D3D12, Vulkan) sized it from that. WS_POPUP has no
         * such minimum, so the surface matches the requested size 1:1. */
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    }

    int is_opengl = (backend_name && strcmp(backend_name, "opengl") == 0);

    if (!is_opengl) {
        /* Vulkan/Metal/D3D — no GL context */
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    }

    if (cfg->samples > 0) {
        glfwWindowHint(GLFW_SAMPLES, cfg->samples);
    }

    /* Always create hidden so we can position the window on the active
     * monitor before the first frame. We make it visible explicitly below. */
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    int width  = cfg->width  > 0 ? cfg->width  : 800;
    int height = cfg->height > 0 ? cfg->height : 600;
    const char *title = cfg->title ? cfg->title : "php-vio";

    GLFWwindow *window = NULL;

    if (is_opengl) {
        /* OpenGL context ladder: try newest → oldest until one succeeds.
         * Floor is GL 3.0 (Sandy Bridge / older Intel iGPUs / Mesa reporting
         * 3.0–3.1). A core profile is requested from 3.2 up (the first version
         * with the `core` keyword); 3.0/3.1 predate the core/compatibility split
         * so no profile hint is set there.
         *
         * NOTE: obtaining a < 3.3 context only guarantees the 2D pipeline. For
         * vio-3D on such a context the SPIRV-Cross GLSL output version
         * (vio_spirv_to_glsl) must also be lowered to match; until then the 3D
         * shaders assume >= 3.3. Apple caps at 4.1 so we skip the higher rungs
         * there. */
        static const int gl_ladder_desktop[][2] = {
            {4, 6}, {4, 5}, {4, 3}, {4, 1}, {3, 3}, {3, 2}, {3, 1}, {3, 0}
        };
#ifdef __APPLE__
        static const int gl_ladder_apple[][2] = { {4, 1}, {3, 3} };
        const int (*ladder)[2] = gl_ladder_apple;
        size_t ladder_n = sizeof(gl_ladder_apple) / sizeof(gl_ladder_apple[0]);
#else
        const int (*ladder)[2] = gl_ladder_desktop;
        size_t ladder_n = sizeof(gl_ladder_desktop) / sizeof(gl_ladder_desktop[0]);
#endif
        /* Silence the GLFW error callback during the probe — every ladder
         * step except the successful one produces an "EGL: failed to create
         * context" warning that the user shouldn't see, since the fallback
         * is expected. We restore the callback after the loop. */
        GLFWerrorfun prev_cb = glfwSetErrorCallback(NULL);

        for (size_t i = 0; i < ladder_n && !window; i++) {
            /* Hints reset between attempts so a previous CONTEXT_VERSION
             * doesn't leak into the next try. Hints set before this branch
             * (RESIZABLE, SCALE_TO_MONITOR, VISIBLE, SAMPLES) need to be
             * re-applied. */
            glfwDefaultWindowHints();
            glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
            if (!cfg->headless) {
                glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
            } else {
                glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
            }
            if (cfg->samples > 0) {
                glfwWindowHint(GLFW_SAMPLES, cfg->samples);
            }
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

            glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, ladder[i][0]);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, ladder[i][1]);
            /* Core profile only from GL 3.2 (first version with the keyword). */
            if (ladder[i][0] * 10 + ladder[i][1] >= 32) {
                glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
                glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
            }
            window = glfwCreateWindow(width, height, title, NULL, NULL);
        }

        glfwSetErrorCallback(prev_cb);

        if (!window) {
            php_error_docref(NULL, E_WARNING,
                "Failed to create GLFW window: no OpenGL context >= 3.0 available");
            return NULL;
        }
    } else {
        window = glfwCreateWindow(width, height, title, NULL, NULL);
        if (!window) {
            php_error_docref(NULL, E_WARNING, "Failed to create GLFW window");
            return NULL;
        }
    }

    /* Never let a fullscreen window auto-minimize when it loses focus (e.g. the
     * user presses Print Screen / opens the Windows snipping tool, or alt-tabs).
     * GLFW iconifies an exclusive-fullscreen window on focus loss by default,
     * which dropped players to the desktop and made screenshots impossible — they
     * only worked in windowed/borderless. Set on the created window so it holds
     * across every fullscreen entry (glfwSetWindowMonitor in vio_set_fullscreen). */
    glfwSetWindowAttrib(window, GLFW_AUTO_ICONIFY, GLFW_FALSE);

#ifdef _WIN32
    /* Place window on the monitor under the mouse cursor (multi-monitor UX) */
    vio_place_window_on_cursor_monitor(window);
#endif

    if (is_opengl) {
        glfwMakeContextCurrent(window);
        glfwSwapInterval(cfg->vsync ? 1 : 0);
    }

    /* Ensure the window is visible for non-headless contexts.
     * GLFW_NO_API on Windows can leave the window hidden even when
     * GLFW_VISIBLE defaults to GLFW_TRUE. */
    if (!cfg->headless) {
        glfwShowWindow(window);
        /* Pump events so WM_SHOWWINDOW/WM_PAINT propagate to DWM before the
         * backend creates the swapchain. Without this, DXGI FLIP_DISCARD
         * swapchains see the HWND as occluded and Present() silently returns
         * DXGI_STATUS_OCCLUDED — no vsync, no composition, blank screen. */
        glfwPollEvents();
    }

    return window;
}

static void glfw_destroy_window(vio_window_handle w)
{
    if (w) glfwDestroyWindow((GLFWwindow *)w);
}

static int glfw_should_close(vio_window_handle w)
{
    return w ? glfwWindowShouldClose((GLFWwindow *)w) : 1;
}

static void glfw_set_should_close(vio_window_handle w, int value)
{
    if (w) glfwSetWindowShouldClose((GLFWwindow *)w, value);
}

static void glfw_poll_events(void)
{
    if (glfw_initialized) glfwPollEvents();
}

static void glfw_wait_events(void)
{
    if (glfw_initialized) glfwWaitEvents();
}

static void glfw_swap_buffers(vio_window_handle w)
{
    if (w) glfwSwapBuffers((GLFWwindow *)w);
}

static void glfw_get_framebuffer_size(vio_window_handle w, int *width, int *height)
{
    if (w) glfwGetFramebufferSize((GLFWwindow *)w, width, height);
}

static void glfw_get_window_size(vio_window_handle w, int *width, int *height)
{
    if (w) glfwGetWindowSize((GLFWwindow *)w, width, height);
}

static void glfw_get_content_scale(vio_window_handle w, float *sx, float *sy)
{
    if (w) glfwGetWindowContentScale((GLFWwindow *)w, sx, sy);
}

static void glfw_get_cursor_pos(vio_window_handle w, double *x, double *y)
{
    if (w) glfwGetCursorPos((GLFWwindow *)w, x, y);
}

static int glfw_gl_make_current(vio_window_handle w)
{
    glfwMakeContextCurrent((GLFWwindow *)w);
    return 0;
}

static void glfw_gl_swap_interval(int interval)
{
    glfwSwapInterval(interval);
}

static void *glfw_gl_get_proc_address(const char *name)
{
    return (void *)glfwGetProcAddress(name);
}

static void glfw_set_title(vio_window_handle w, const char *utf8)
{
    if (w) glfwSetWindowTitle((GLFWwindow *)w, utf8);
}

static void glfw_set_window_size(vio_window_handle w, int width, int height)
{
    if (w) glfwSetWindowSize((GLFWwindow *)w, width, height);
}

static void glfw_get_window_pos(vio_window_handle w, int *x, int *y)
{
    if (w) glfwGetWindowPos((GLFWwindow *)w, x, y);
}

static void glfw_set_window_pos(vio_window_handle w, int x, int y)
{
    if (w) glfwSetWindowPos((GLFWwindow *)w, x, y);
}

static int glfw_attrib_id(vio_window_attrib a)
{
    switch (a) {
        case VIO_WINDOW_DECORATED:    return GLFW_DECORATED;
        case VIO_WINDOW_MAXIMIZED:    return GLFW_MAXIMIZED;
        case VIO_WINDOW_AUTO_ICONIFY: return GLFW_AUTO_ICONIFY;
        case VIO_WINDOW_FOCUSED:      return GLFW_FOCUSED;
        case VIO_WINDOW_VISIBLE:      return GLFW_VISIBLE;
    }
    return 0;
}

static int glfw_get_attrib(vio_window_handle w, vio_window_attrib a)
{
    return w ? glfwGetWindowAttrib((GLFWwindow *)w, glfw_attrib_id(a)) : 0;
}

static void glfw_set_attrib(vio_window_handle w, vio_window_attrib a, int value)
{
    if (w) glfwSetWindowAttrib((GLFWwindow *)w, glfw_attrib_id(a), value ? GLFW_TRUE : GLFW_FALSE);
}

static void glfw_maximize(vio_window_handle w)
{
    if (w) glfwMaximizeWindow((GLFWwindow *)w);
}

static void glfw_restore(vio_window_handle w)
{
    if (w) glfwRestoreWindow((GLFWwindow *)w);
}

static void glfw_set_cursor_mode(vio_window_handle w, int mode)
{
    if (!w) return;
    int glfw_mode;
    switch (mode) {
        case VIO_PLATFORM_CURSOR_DISABLED: glfw_mode = GLFW_CURSOR_DISABLED; break;
        case VIO_PLATFORM_CURSOR_HIDDEN:   glfw_mode = GLFW_CURSOR_HIDDEN; break;
        default:                           glfw_mode = GLFW_CURSOR_NORMAL; break;
    }
    glfwSetInputMode((GLFWwindow *)w, GLFW_CURSOR, glfw_mode);

    /* When switching to disabled mode, enable raw mouse motion if available */
    if (mode == VIO_PLATFORM_CURSOR_DISABLED && glfwRawMouseMotionSupported()) {
        glfwSetInputMode((GLFWwindow *)w, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }
}

static GLFWmonitor *glfw_monitor_at(int index)
{
    int count = 0;
    GLFWmonitor **mons = glfw_initialized ? glfwGetMonitors(&count) : NULL;
    return (mons && index >= 0 && index < count) ? mons[index] : NULL;
}

static int glfw_monitor_index(GLFWmonitor *m)
{
    int count = 0;
    GLFWmonitor **mons = (m && glfw_initialized) ? glfwGetMonitors(&count) : NULL;
    for (int i = 0; mons && i < count; i++) if (mons[i] == m) return i;
    return -1;
}

static int glfw_window_monitor(vio_window_handle w)
{
    if (!w) return -1;
    GLFWmonitor *m = glfwGetWindowMonitor((GLFWwindow *)w);
    if (!m) return -1;
    int i = glfw_monitor_index(m);
    return i >= 0 ? i : 0;
}

static void glfw_set_window_monitor(vio_window_handle w, int monitor, int x, int y, int width, int height, int refresh_hz)
{
    if (!w) return;
    GLFWmonitor *m = monitor >= 0 ? glfw_monitor_at(monitor) : NULL;
    if (monitor >= 0 && !m) m = glfwGetPrimaryMonitor();
    glfwSetWindowMonitor((GLFWwindow *)w, m, x, y, width, height,
                         refresh_hz > 0 ? refresh_hz : GLFW_DONT_CARE);
}

static int glfw_monitor_count(void)
{
    int count = 0;
    if (glfw_initialized) glfwGetMonitors(&count);
    return count;
}

static int glfw_primary_monitor(void)
{
    return glfw_initialized ? glfw_monitor_index(glfwGetPrimaryMonitor()) : -1;
}

static int glfw_monitor_desc(int index, vio_monitor_desc *out)
{
    GLFWmonitor *m = glfw_monitor_at(index);
    memset(out, 0, sizeof(*out));
    if (!m) return -1;
    const GLFWvidmode *mode = glfwGetVideoMode(m);
    const char *name = glfwGetMonitorName(m);
    snprintf(out->name, sizeof(out->name), "%s", name ? name : "");
    glfwGetMonitorPos(m, &out->x, &out->y);
    glfwGetMonitorWorkarea(m, &out->work_x, &out->work_y, &out->work_width, &out->work_height);
    out->scale_x = out->scale_y = 1.0f;
    glfwGetMonitorContentScale(m, &out->scale_x, &out->scale_y);
    if (mode) {
        out->mode.width = mode->width;
        out->mode.height = mode->height;
        out->mode.refresh_hz = mode->refreshRate;
        out->mode.red_bits = mode->redBits;
        out->mode.green_bits = mode->greenBits;
        out->mode.blue_bits = mode->blueBits;
    }
    out->primary = m == glfwGetPrimaryMonitor();
    return 0;
}

static int glfw_video_modes(int index, vio_video_mode *out, int max)
{
    GLFWmonitor *m = glfw_monitor_at(index);
    if (!m) return 0;
    int count = 0;
    const GLFWvidmode *modes = glfwGetVideoModes(m, &count);
    if (!modes) return 0;
    int n = 0;
    for (int i = 0; i < count && n < max; i++, n++) {
        out[n].width = modes[i].width;
        out[n].height = modes[i].height;
        out[n].refresh_hz = modes[i].refreshRate;
        out[n].red_bits = modes[i].redBits;
        out[n].green_bits = modes[i].greenBits;
        out[n].blue_bits = modes[i].blueBits;
    }
    return n;
}

static int glfw_joystick_ok(int id)
{
    return glfw_initialized && id >= GLFW_JOYSTICK_1 && id <= GLFW_JOYSTICK_LAST;
}

static int glfw_joystick_present(int id)
{
    return glfw_joystick_ok(id) && glfwJoystickPresent(id);
}

static int glfw_joystick_is_gamepad(int id)
{
    return glfw_joystick_ok(id) && glfwJoystickIsGamepad(id);
}

static const char *glfw_joystick_name(int id)
{
    return glfw_joystick_ok(id) ? glfwGetJoystickName(id) : NULL;
}

static const char *glfw_gamepad_name(int id)
{
    return glfw_joystick_ok(id) ? glfwGetGamepadName(id) : NULL;
}

static int glfw_gamepad_state(int id, unsigned char buttons[15], float axes[6])
{
    GLFWgamepadstate gs;
    if (!glfw_joystick_ok(id) || !glfwGetGamepadState(id, &gs)) return 0;
    for (int i = 0; i <= GLFW_GAMEPAD_BUTTON_LAST && i < 15; i++) buttons[i] = gs.buttons[i] == GLFW_PRESS;
    for (int i = 0; i <= GLFW_GAMEPAD_AXIS_LAST && i < 6; i++) axes[i] = gs.axes[i];
    return 1;
}

static const unsigned char *glfw_joystick_buttons(int id, int *count)
{
    *count = 0;
    return glfw_joystick_ok(id) ? glfwGetJoystickButtons(id, count) : NULL;
}

static const float *glfw_joystick_axes(int id, int *count)
{
    *count = 0;
    return glfw_joystick_ok(id) ? glfwGetJoystickAxes(id, count) : NULL;
}

static void *glfw_native_handle(vio_window_handle w, vio_native_kind kind)
{
    if (!w) return NULL;
    switch (kind) {
#if defined(_WIN32)
        case VIO_NATIVE_HWND:      return (void *)glfwGetWin32Window((GLFWwindow *)w);
        case VIO_NATIVE_HINSTANCE: return (void *)GetModuleHandleW(NULL);
#elif defined(__APPLE__)
        case VIO_NATIVE_NSWINDOW:  return (void *)glfwGetCocoaWindow((GLFWwindow *)w);
#elif defined(__linux__)
        case VIO_NATIVE_XLIB_WINDOW:  return (void *)(uintptr_t)glfwGetX11Window((GLFWwindow *)w);
        case VIO_NATIVE_XLIB_DISPLAY: return (void *)glfwGetX11Display();
#endif
        default: return NULL;
    }
}

#ifdef HAVE_VULKAN
static const char **glfw_vk_instance_extensions(uint32_t *count)
{
    *count = 0;
    if (!glfw_initialized) return NULL;
    return glfwGetRequiredInstanceExtensions(count);
}

static int glfw_vk_create_surface(vio_window_handle w, void *instance, void *out_surface)
{
    return (int)glfwCreateWindowSurface((VkInstance)instance, (GLFWwindow *)w, NULL, (VkSurfaceKHR *)out_surface);
}
#endif

/* ── Input callbacks ──────────────────────────────────────────────── */

/* While a replay runs it owns the input: OS events are dropped, so a human
 * touching the mouse cannot knock a replayed run off course. */
static vio_input_state *glfw_input_state(GLFWwindow *window)
{
    vio_input_state *state = (vio_input_state *)glfwGetWindowUserPointer(window);
    return (state && !state->replaying) ? state : NULL;
}

static void glfw_key_callback(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    (void)scancode;
    vio_input_key_event(glfw_input_state(window), key, action, mods);
}

static void glfw_char_callback(GLFWwindow *window, unsigned int codepoint)
{
    vio_input_emit_char(glfw_input_state(window), codepoint);
}

static void glfw_cursor_pos_callback(GLFWwindow *window, double xpos, double ypos)
{
    vio_input_cursor_event(glfw_input_state(window), xpos, ypos);
}

static void glfw_mouse_button_callback(GLFWwindow *window, int button, int action, int mods)
{
    (void)mods;
    vio_input_button_event(glfw_input_state(window), button, action);
}

static void glfw_scroll_callback(GLFWwindow *window, double xoffset, double yoffset)
{
    vio_input_scroll_event(glfw_input_state(window), xoffset, yoffset);
}

static void glfw_framebuffer_size_callback(GLFWwindow *window, int width, int height)
{
    vio_input_resize_event((vio_input_state *)glfwGetWindowUserPointer(window), width, height);
}

static void glfw_install_input(vio_window_handle w, vio_input_state *state)
{
    GLFWwindow *window = (GLFWwindow *)w;
    if (!window) return;
    glfwSetWindowUserPointer(window, state);
    glfwSetKeyCallback(window, glfw_key_callback);
    glfwSetCharCallback(window, glfw_char_callback);
    glfwSetCursorPosCallback(window, glfw_cursor_pos_callback);
    glfwSetMouseButtonCallback(window, glfw_mouse_button_callback);
    glfwSetScrollCallback(window, glfw_scroll_callback);
    glfwSetFramebufferSizeCallback(window, glfw_framebuffer_size_callback);
}

static const vio_platform vio_platform_glfw = {
    .name                 = "glfw",
    .init                 = glfw_init,
    .shutdown             = glfw_shutdown,
    .create_window        = glfw_create_window,
    .destroy_window       = glfw_destroy_window,
    .should_close         = glfw_should_close,
    .set_should_close     = glfw_set_should_close,
    .poll_events          = glfw_poll_events,
    .wait_events          = glfw_wait_events,
    .swap_buffers         = glfw_swap_buffers,
    .get_framebuffer_size = glfw_get_framebuffer_size,
    .get_window_size      = glfw_get_window_size,
    .get_content_scale    = glfw_get_content_scale,
    .get_cursor_pos       = glfw_get_cursor_pos,
    .gl_make_current      = glfw_gl_make_current,
    .gl_swap_interval     = glfw_gl_swap_interval,
    .gl_get_proc_address  = glfw_gl_get_proc_address,
    .set_title            = glfw_set_title,
    .set_window_size      = glfw_set_window_size,
    .get_window_pos       = glfw_get_window_pos,
    .set_window_pos       = glfw_set_window_pos,
    .get_attrib           = glfw_get_attrib,
    .set_attrib           = glfw_set_attrib,
    .maximize             = glfw_maximize,
    .restore              = glfw_restore,
    .set_cursor_mode      = glfw_set_cursor_mode,
    .window_monitor       = glfw_window_monitor,
    .set_window_monitor   = glfw_set_window_monitor,
    .monitor_count        = glfw_monitor_count,
    .primary_monitor      = glfw_primary_monitor,
    .monitor_desc         = glfw_monitor_desc,
    .video_modes          = glfw_video_modes,
    .joystick_present     = glfw_joystick_present,
    .joystick_is_gamepad  = glfw_joystick_is_gamepad,
    .joystick_name        = glfw_joystick_name,
    .gamepad_name         = glfw_gamepad_name,
    .gamepad_state        = glfw_gamepad_state,
    .joystick_buttons     = glfw_joystick_buttons,
    .joystick_axes        = glfw_joystick_axes,
    .native_handle        = glfw_native_handle,
#ifdef HAVE_VULKAN
    .vk_instance_extensions = glfw_vk_instance_extensions,
    .vk_create_surface      = glfw_vk_create_surface,
#endif
    .install_input        = glfw_install_input,
};

void vio_platform_glfw_register(void)
{
    vio_register_platform(&vio_platform_glfw);
}

#endif /* HAVE_GLFW */
