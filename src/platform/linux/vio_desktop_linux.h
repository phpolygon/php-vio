/*
 * php-vio - Linux desktop services over D-Bus (src/platform/linux/vio_desktop_linux.c),
 * shared by the X11, Wayland and GLFW platforms.
 */

#ifndef VIO_DESKTOP_LINUX_H
#define VIO_DESKTOP_LINUX_H

#include "../../../include/vio_platform.h"

#if defined(__linux__)
int vio_linux_available(int feature);   /* VIO_PLATFORM_FEATURE_* */
int vio_linux_notify(const char *title, const char *body, const char *icon, const char *key);
int vio_linux_set_progress(double value, int state);
int vio_linux_file_dialog(const vio_file_dialog_desc *d, char ***out_paths, int *out_count);
int vio_linux_set_tray(const vio_tray_desc *d);
void vio_linux_dispatch(void);   /* answer tray calls (from the event loop) */
int vio_linux_dbus_fd(void);     /* the session bus socket while a tray is up, else -1 */
#endif

#endif /* VIO_DESKTOP_LINUX_H */
