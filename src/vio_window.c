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
     * glfwGetFramebufferSize() returns 2560x1440 (physical, sharp render).
     *
     * Headless contexts skip the auto-scale — they render to a hidden FBO
     * at the requested pixel dimensions, so DPI awareness only introduces
     * unwanted asymmetric scaling (which is why headless callers asking for
     * 64×48 used to get 120×48 on a 1.875× display). */
    if (!cfg->headless) {
        glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
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
