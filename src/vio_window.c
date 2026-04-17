/*
 * php-vio - GLFW window management implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "vio_window.h"

#ifdef HAVE_GLFW

static int glfw_initialized = 0;

static void vio_glfw_error_callback(int error, const char *description)
{
    php_error_docref(NULL, E_WARNING, "GLFW error %d: %s", error, description);
}

int vio_window_init(void)
{
    if (glfw_initialized) {
        return 1;
    }

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

    if (cfg->headless) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    int width  = cfg->width  > 0 ? cfg->width  : 800;
    int height = cfg->height > 0 ? cfg->height : 600;
    const char *title = cfg->title ? cfg->title : "php-vio";

    GLFWwindow *window = glfwCreateWindow(width, height, title, NULL, NULL);
    if (!window) {
        php_error_docref(NULL, E_WARNING, "Failed to create GLFW window");
        return NULL;
    }

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
