/*
 * php-vio - GLFW window management
 */

#ifndef VIO_WINDOW_H
#define VIO_WINDOW_H

#include "../include/vio_types.h"

#ifdef HAVE_GLFW
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif

/* Initialize GLFW globally (call once in MINIT) */
int vio_window_init(void);

/* Shutdown GLFW globally (call once in MSHUTDOWN) */
void vio_window_shutdown(void);

#ifdef HAVE_GLFW

/* Create a GLFW window. backend_name determines API hints (e.g., "opengl"). */
GLFWwindow *vio_window_create(vio_config *cfg, const char *backend_name);

/* Destroy a GLFW window. */
void vio_window_destroy(GLFWwindow *window);

/* Check if window should close. */
int vio_window_should_close(GLFWwindow *window);

/* Set window should close flag. */
void vio_window_set_should_close(GLFWwindow *window, int value);

/* Poll all pending events. */
void vio_window_poll_events(void);

/* Swap buffers (for OpenGL). */
void vio_window_swap_buffers(GLFWwindow *window);

/* Get framebuffer size. */
void vio_window_get_framebuffer_size(GLFWwindow *window, int *width, int *height);

#endif /* HAVE_GLFW */

#endif /* VIO_WINDOW_H */
