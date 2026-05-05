/*
 * php-vio - GLFW window management implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "vio_window.h"

#ifdef HAVE_GLFW

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

int vio_window_init(void)
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

void vio_window_shutdown(void)
{
    if (glfw_initialized) {
        glfwTerminate();
        glfw_initialized = 0;
    }
}

GLFWwindow *vio_window_create(vio_config *cfg, const char *backend_name)
{
    if (!glfw_initialized) {
        if (!vio_window_init()) {
            return NULL;
        }
    }

    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    /* Auto-scale window size by monitor content scale on HiDPI displays.
     * Combined with per-monitor DPI awareness this means: a request for
     * 1280x720 on a 4K@200% display creates a 2560x1440 physical window,
     * glfwGetWindowSize() returns 1280x720 (logical, layout-stable), and
     * glfwGetFramebufferSize() returns 2560x1440 (physical, sharp render). */
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    if (backend_name && strcmp(backend_name, "opengl") == 0) {
        /* OpenGL 4.1 Core Profile */
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    } else {
        /* Vulkan/other - no GL context */
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

    GLFWwindow *window = glfwCreateWindow(width, height, title, NULL, NULL);
    if (!window) {
        php_error_docref(NULL, E_WARNING, "Failed to create GLFW window");
        return NULL;
    }

#ifdef _WIN32
    /* Place window on the monitor under the mouse cursor (multi-monitor UX) */
    vio_place_window_on_cursor_monitor(window);
#endif

    if (backend_name && strcmp(backend_name, "opengl") == 0) {
        glfwMakeContextCurrent(window);
        if (cfg->vsync) {
            glfwSwapInterval(1);
        } else {
            glfwSwapInterval(0);
        }
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

void vio_window_destroy(GLFWwindow *window)
{
    if (window) {
        glfwDestroyWindow(window);
    }
}

int vio_window_should_close(GLFWwindow *window)
{
    if (!window) {
        return 1;
    }
    return glfwWindowShouldClose(window);
}

void vio_window_set_should_close(GLFWwindow *window, int value)
{
    if (window) {
        glfwSetWindowShouldClose(window, value);
    }
}

void vio_window_poll_events(void)
{
    if (glfw_initialized) {
        glfwPollEvents();
    }
}

void vio_window_swap_buffers(GLFWwindow *window)
{
    if (window) {
        glfwSwapBuffers(window);
    }
}

void vio_window_get_framebuffer_size(GLFWwindow *window, int *width, int *height)
{
    if (window) {
        glfwGetFramebufferSize(window, width, height);
    }
}

#else /* !HAVE_GLFW */

int vio_window_init(void)
{
    return 1; /* No-op when GLFW is not available */
}

void vio_window_shutdown(void)
{
}

#endif /* HAVE_GLFW */
