/*
 * php-vio - Metal Backend (macOS)
 */

#ifndef VIO_METAL_H
#define VIO_METAL_H

#include "../../../include/vio_backend.h"

void vio_backend_metal_register(void);

/*
 * Setup Metal context: create device, command queue, CAMetalLayer on GLFW window.
 * Called from vio_create() after window creation.
 * Returns 0 on success, -1 on failure.
 */
int vio_metal_setup_context(void *glfw_window, vio_config *cfg);

/*
 * Shutdown Metal context and release all resources.
 */
void vio_metal_shutdown_context(void);

#endif /* VIO_METAL_H */
