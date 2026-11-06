/*
 * php-vio - Linux webview (WEBVIEW-PLAN W2, src/platform/linux/vio_webview_linux.c),
 * shared by the X11 and Wayland platforms: WPE WebKit when it is installed,
 * else WebKitGTK, both loaded at runtime.
 */

#ifndef VIO_WEBVIEW_LINUX_H
#define VIO_WEBVIEW_LINUX_H

#include "../../../include/vio_platform.h"

#if defined(__linux__)
#include <poll.h>

int   vio_wvl_available(void);
/* headless: the window is never shown (the webview renders offscreen) */
void *vio_wvl_create(vio_window_handle w, const vio_webview_desc *d, int headless);
void  vio_wvl_destroy(void *wv);
int   vio_wvl_navigate(void *wv, const char *url);
int   vio_wvl_eval(void *wv, const char *js);
int   vio_wvl_set_bounds(void *wv, int has_bounds, int x, int y, int width, int height);
int   vio_wvl_set_script(void *wv, const char *js);
int   vio_wvl_reply_head(void *wv, uint64_t id, int status, const vio_webview_header *h, int count);
int   vio_wvl_reply_data(void *wv, uint64_t id, const void *data, size_t len);
int   vio_wvl_reply_end(void *wv, uint64_t id);
int   vio_wvl_reply_fail(void *wv, uint64_t id);

/* Event loop: the platform's poll() also waits on GLib's descriptors (the
 * engine's sockets, streamed answers). poll_begin adds them after fds[0..n)
 * (at most max in all) and may shorten *timeout_ms; it returns the new
 * count. poll_end after the poll (also when it was skipped), then pump. */
int   vio_wvl_poll_begin(struct pollfd *fds, int n, int max, int *timeout_ms);
void  vio_wvl_poll_end(struct pollfd *fds, int n_before, int n_after);
void  vio_wvl_pump(void);   /* non-blocking: GLib sources, streamed answers, layout */

#ifdef HAVE_X11
/* X11 events of the windows the webview put into vio's windows (WPE): 1 = taken */
int   vio_wvl_x11_event(void *xevent);
#endif
#endif

#endif /* VIO_WEBVIEW_LINUX_H */
