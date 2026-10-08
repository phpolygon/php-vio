/*
 * php-vio - Window helpers over the active platform (include/vio_platform.h)
 *
 * The window system lives behind the vio_platform vtable (GLFW in
 * src/platform/glfw/, native layers next to it); these names stay as the
 * short spelling the callers already use.
 */

#ifndef VIO_WINDOW_H
#define VIO_WINDOW_H

#include "../include/vio_types.h"
#include "../include/vio_platform.h"

/* Register the built-in platforms and initialise the active one (MINIT). */
int  vio_window_init(void);
/* Shut the active platform down (MSHUTDOWN). */
void vio_window_shutdown(void);

static inline vio_window_handle vio_window_create(vio_config *cfg, const char *backend_name)
{
    return vio_plat()->create_window(cfg, backend_name);
}

static inline void vio_window_destroy(vio_window_handle window)
{
    vio_plat()->destroy_window(window);
}

static inline int vio_window_should_close(vio_window_handle window)
{
    return vio_plat()->should_close(window);
}

static inline void vio_window_set_should_close(vio_window_handle window, int value)
{
    vio_plat()->set_should_close(window, value);
}

static inline void vio_window_poll_events(void)
{
    vio_plat()->poll_events();
}

static inline void vio_window_swap_buffers(vio_window_handle window)
{
    vio_plat()->swap_buffers(window);
}

static inline void vio_window_get_framebuffer_size(vio_window_handle window, int *width, int *height)
{
    vio_plat()->get_framebuffer_size(window, width, height);
}

#endif /* VIO_WINDOW_H */
