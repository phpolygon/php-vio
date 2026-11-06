/*
 * php-vio - Linux webview (WEBVIEW-PLAN W2): WPE WebKit when it is installed,
 * else WebKitGTK, shared by the X11 and Wayland platforms.
 *
 * Both ports have the same webkit_* API (contexts, URI schemes, user content,
 * signals), so scheme requests, the bridge, events and streamed answers are
 * written once; only where the view lives differs:
 *   - WebKitGTK: a GTK window. Under X11 it is reparented into vio's window
 *     (a child at the bounds), under Wayland it is a toplevel of its own
 *     (one client cannot place a surface into another's), headless it is a
 *     GtkOffscreenWindow.
 *   - WPE: renders into shared memory (WPEBackend-fdo); vio shows the
 *     pixels and passes input on (see the WPE part below).
 * Nothing is linked: libwebkit2gtk-4.1 / libWPEWebKit-2.0 are dlopen'ed and
 * everything else (GLib, GTK, libsoup) is found through their handle.
 * VIO_WEBVIEW_ENGINE=wpe|gtk picks one (tests run both).
 *
 * Every engine callback runs inside GLib dispatch, which happens in the
 * platform's poll / wait (vio_wvl_pump): the PHP thread, as vio_platform.h asks.
 * A scheme answer is held until PHP ends it, then given to WebKit as a memory
 * stream; one that is still open at the next pump becomes a stream over a
 * socket pair (Server-Sent Events, generators).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if defined(__linux__)

#include "vio_webview_linux.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>

#ifdef HAVE_X11
#include <X11/Xlib.h>
#endif

/* ── Just enough GLib / GTK / WebKit to call them through pointers ── */

typedef int gboolean;
typedef void *gpointer;
typedef unsigned int GQuark;
typedef struct { GQuark domain; int code; char *message; } GError;
typedef struct { int fd; unsigned short events, revents; } GPollFD;   /* = struct pollfd on Linux */
typedef void (*GCallback)(void);
typedef void (*GDestroyNotify)(gpointer);
typedef struct { double red, green, blue, alpha; } wvl_rgba;          /* GdkRGBA / WebKitColor */

#define WVL_SYMS(X) \
    X(void *, g_main_context_default, (void)) \
    X(gboolean, g_main_context_acquire, (void *)) \
    X(void, g_main_context_release, (void *)) \
    X(gboolean, g_main_context_prepare, (void *, int *)) \
    X(int, g_main_context_query, (void *, int, int *, GPollFD *, int)) \
    X(gboolean, g_main_context_check, (void *, int, GPollFD *, int)) \
    X(void, g_main_context_dispatch, (void *)) \
    X(gboolean, g_main_context_iteration, (void *, gboolean)) \
    X(void *, g_object_new, (unsigned long, const char *, ...)) \
    X(void *, g_object_ref, (void *)) \
    X(void, g_object_unref, (void *)) \
    X(unsigned long, g_signal_connect_data, (void *, const char *, GCallback, gpointer, void *, int)) \
    X(unsigned, g_signal_handlers_disconnect_matched, (void *, int, unsigned, GQuark, void *, void *, gpointer)) \
    X(void, g_free, (void *)) \
    X(void, g_error_free, (GError *)) \
    X(GError *, g_error_new_literal, (GQuark, int, const char *)) \
    X(GQuark, g_quark_from_static_string, (const char *)) \
    X(gboolean, g_input_stream_read_all, (void *, void *, size_t, size_t *, void *, GError **)) \
    X(void *, g_memory_input_stream_new_from_data, (const void *, long, GDestroyNotify)) \
    X(void *, g_unix_input_stream_new, (int, gboolean)) \
    X(void *, soup_message_headers_new, (int)) \
    X(void, soup_message_headers_append, (void *, const char *, const char *)) \
    X(void, soup_message_headers_foreach, (void *, void (*)(const char *, const char *, gpointer), gpointer)) \
    X(void *, webkit_web_context_new, (void)) \
    X(void, webkit_web_context_register_uri_scheme, (void *, const char *, void (*)(void *, gpointer), gpointer, GDestroyNotify)) \
    X(void *, webkit_web_context_get_security_manager, (void *)) \
    X(void, webkit_security_manager_register_uri_scheme_as_secure, (void *, const char *)) \
    X(void, webkit_security_manager_register_uri_scheme_as_cors_enabled, (void *, const char *)) \
    X(void *, webkit_user_content_manager_new, (void)) \
    X(gboolean, webkit_user_content_manager_register_script_message_handler, (void *, const char *, const char *)) \
    X(void, webkit_user_content_manager_add_script, (void *, void *)) \
    X(void, webkit_user_content_manager_remove_all_scripts, (void *)) \
    X(void *, webkit_user_script_new, (const char *, int, int, const char *const *, const char *const *)) \
    X(void, webkit_user_script_unref, (void *)) \
    X(unsigned long, webkit_web_view_get_type, (void)) \
    X(void, webkit_web_view_load_uri, (void *, const char *)) \
    X(void, webkit_web_view_stop_loading, (void *)) \
    X(const char *, webkit_web_view_get_uri, (void *)) \
    X(const char *, webkit_web_view_get_title, (void *)) \
    X(void, webkit_web_view_evaluate_javascript, (void *, const char *, long, const char *, const char *, void *, void *, gpointer)) \
    X(void *, webkit_web_view_get_settings, (void *)) \
    X(void, webkit_settings_set_enable_developer_extras, (void *, gboolean)) \
    X(void, webkit_settings_set_javascript_can_open_windows_automatically, (void *, gboolean)) \
    X(unsigned, g_log_set_handler, (const char *, int, void (*)(const char *, int, const char *, gpointer), gpointer)) \
    X(void, g_log_default_handler, (const char *, int, const char *, gpointer)) \
    X(void, webkit_web_view_set_background_color, (void *, const wvl_rgba *)) \
    X(const char *, webkit_uri_scheme_request_get_uri, (void *)) \
    X(const char *, webkit_uri_scheme_request_get_http_method, (void *)) \
    X(void *, webkit_uri_scheme_request_get_http_headers, (void *)) \
    X(void *, webkit_uri_scheme_request_get_http_body, (void *)) \
    X(void *, webkit_uri_scheme_request_get_web_view, (void *)) \
    X(void, webkit_uri_scheme_request_finish_with_response, (void *, void *)) \
    X(void, webkit_uri_scheme_request_finish_error, (void *, GError *)) \
    X(void *, webkit_uri_scheme_response_new, (void *, long)) \
    X(void, webkit_uri_scheme_response_set_status, (void *, unsigned, const char *)) \
    X(void, webkit_uri_scheme_response_set_content_type, (void *, const char *)) \
    X(void, webkit_uri_scheme_response_set_http_headers, (void *, void *)) \
    X(void *, webkit_navigation_action_get_request, (void *)) \
    X(const char *, webkit_uri_request_get_uri, (void *)) \
    X(char *, jsc_value_to_string, (void *))

#define WVL_GTK_SYMS(X) \
    X(gboolean, gtk_init_check, (int *, char ***)) \
    X(void, gdk_set_allowed_backends, (const char *)) \
    X(void *, gtk_window_new, (int)) \
    X(void *, gtk_offscreen_window_new, (void)) \
    X(void, gtk_container_add, (void *, void *)) \
    X(void, gtk_widget_show_all, (void *)) \
    X(void, gtk_widget_realize, (void *)) \
    X(void *, gtk_widget_get_window, (void *)) \
    X(void, gtk_widget_destroy, (void *)) \
    X(void, gtk_widget_set_size_request, (void *, int, int)) \
    X(void, gtk_window_resize, (void *, int, int)) \
    X(void, gtk_window_move, (void *, int, int)) \
    X(void, gtk_window_set_title, (void *, const char *)) \
    X(void *, webkit_javascript_result_get_js_value, (void *))

#define WVL_DECL(ret, name, args) ret (*name) args;
static struct { WVL_SYMS(WVL_DECL) } gl;
static struct { WVL_GTK_SYMS(WVL_DECL) } gk;
static unsigned long (*gdk_x11_window_get_xid_p)(void *);   /* only in an X11-enabled GDK */

enum { WVL_NONE = 0, WVL_WPE, WVL_GTK };
static int   wvl_engine = -1;     /* -1 = not probed */
static void *wvl_lib;

/* WebKit enums (same values in both ports) */
#define WVL_INJECT_TOP_FRAME       1
#define WVL_INJECT_DOCUMENT_START  0
#define WVL_LOAD_COMMITTED         2
#define WVL_SIGNAL_MATCH_DATA      16
#define WVL_SOUP_HEADERS_RESPONSE  1
#define WVL_GTK_WINDOW_TOPLEVEL    0
#define WVL_GTK_WINDOW_POPUP       1

static int wvl_bind(void *h, const char *name, void **slot)
{
    *slot = dlsym(h, name);
    return *slot != NULL;
}

static int wvl_load_common(void *h)
{
    int ok = 1;
#define WVL_BIND(ret, name, args) ok &= wvl_bind(h, #name, (void **)&gl.name);
    WVL_SYMS(WVL_BIND)
    return ok;
}

static void wvl_quiet(const char *domain, int level, const char *message, gpointer data)
{
    (void)domain; (void)level; (void)message; (void)data;
}

/* A Wayland compositor without a seat (headless weston, kiosks): GDK asserts on every keyboard lookup */
static void wvl_gdk_log(const char *domain, int level, const char *message, gpointer data)
{
    if (message && strstr(message, "GDK_IS_SEAT")) return;
    gl.g_log_default_handler(domain, level, message, data);
}

static int wvl_load_gtk(void)
{
    void *h = dlopen("libwebkit2gtk-4.1.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!h) return 0;
    int ok = wvl_load_common(h);
#define WVL_BIND_GTK(ret, name, args) ok &= wvl_bind(h, #name, (void **)&gk.name);
    WVL_GTK_SYMS(WVL_BIND_GTK)
    gdk_x11_window_get_xid_p = (unsigned long (*)(void *))dlsym(h, "gdk_x11_window_get_xid");
    if (!ok) { dlclose(h); memset(&gl, 0, sizeof(gl)); return 0; }
    /* GTK must speak to the same window system as vio */
    const char *plat = vio_plat()->name;
    gk.gdk_set_allowed_backends(strcmp(plat, "wayland") == 0 ? "wayland" : "x11");
    /* without an accessibility bus (servers, CI) GTK's AT-SPI bridge warns: nothing PHP could act on */
    gl.g_log_set_handler("dbind", 1 << 4 /* G_LOG_LEVEL_WARNING */, wvl_quiet, NULL);
    gl.g_log_set_handler("Gdk", 1 << 3 /* G_LOG_LEVEL_CRITICAL */, wvl_gdk_log, NULL);
    if (!gk.gtk_init_check(NULL, NULL)) { dlclose(h); memset(&gl, 0, sizeof(gl)); return 0; }
    wvl_lib = h;
    return 1;
}

static int wvl_load_wpe(void);

static int wvl_probe(void)
{
    if (wvl_engine >= 0) return wvl_engine;
    wvl_engine = WVL_NONE;
    /* JavaScriptCore suspends threads for its GC with SIGUSR1 and says so on stderr when the signal already
     * has a handler - PHP's deferring one (zend signals). It takes the signal either way; hand it over quietly. */
    signal(SIGUSR1, SIG_DFL);
    const char *want = getenv("VIO_WEBVIEW_ENGINE");
    int try_wpe = !want || !*want || strcmp(want, "wpe") == 0;
    int try_gtk = !want || !*want || strcmp(want, "gtk") == 0;
    if (try_wpe && wvl_load_wpe()) wvl_engine = WVL_WPE;
    else if (try_gtk && wvl_load_gtk()) wvl_engine = WVL_GTK;
    /* without an accessibility bus (servers, CI) GTK's AT-SPI bridge warns once per process: nothing to tell PHP */

    return wvl_engine;
}

int vio_wvl_available(void)
{
    return wvl_probe() != WVL_NONE;
}

/* ── Views and open requests ─────────────────────────────────────── */

typedef struct _wvl_req {
    uint64_t  id;
    void     *request;          /* WebKitURISchemeRequest, referenced */
    int       head, ended;      /* PHP sent the head / the end */
    int       status;
    char     *headers;          /* name\0value\0 ... */
    size_t    headers_len;
    int       header_count;
    char     *buf;              /* body not yet handed on */
    size_t    len, cap;
    int       fd;               /* streaming: our end of the socket pair, else -1 */
    struct _wvl_req *next;
} wvl_req;

typedef struct _wvl_view wvl_view;
struct _wvl_view {
    void            *view;      /* WebKitWebView */
    void            *ctx;       /* WebKitWebContext (one per view: schemes are per context) */
    void            *ucm;       /* WebKitUserContentManager */
    void            *user;
    vio_window_handle win;
    int              headless;
    int              has_bounds, x, y, w, h;   /* requested */
    int              cur_x, cur_y, cur_w, cur_h;   /* applied */
    char            *title;
    wvl_req         *reqs;
    uint64_t         next_id;
    /* WebKitGTK */
    void            *gtk_window;
    unsigned long    xid;
    /* WPE */
    void            *wpe;       /* wvl_wpe_host */
    wvl_view        *next;
};

static wvl_view *wvl_views;

static const char wvl_bridge[] =
    "(function () {"
    "  if (window.vio) return;"
    "  var h = window.webkit.messageHandlers.vio;"
    "  window.vio = Object.freeze({ postMessage: function (v) { h.postMessage(JSON.stringify(v === undefined ? null : v)); } });"
    "  Object.defineProperty(window, '__vioPost', { value: function (s) { h.postMessage(String(s)); } });"
    "})();";

static void wvl_scripts(wvl_view *v, const char *extra)
{
    gl.webkit_user_content_manager_remove_all_scripts(v->ucm);
    void *s = gl.webkit_user_script_new(wvl_bridge, WVL_INJECT_TOP_FRAME, WVL_INJECT_DOCUMENT_START, NULL, NULL);
    gl.webkit_user_content_manager_add_script(v->ucm, s);
    gl.webkit_user_script_unref(s);
    if (extra && *extra) {
        s = gl.webkit_user_script_new(extra, WVL_INJECT_TOP_FRAME, WVL_INJECT_DOCUMENT_START, NULL, NULL);
        gl.webkit_user_content_manager_add_script(v->ucm, s);
        gl.webkit_user_script_unref(s);
    }
}

static wvl_req *wvl_find(wvl_view *v, uint64_t id)
{
    for (wvl_req *r = v->reqs; r; r = r->next) if (r->id == id) return r;
    return NULL;
}

static void wvl_req_free(wvl_view *v, wvl_req *r)
{
    for (wvl_req **pp = &v->reqs; *pp; pp = &(*pp)->next) if (*pp == r) { *pp = r->next; break; }
    if (r->fd >= 0) close(r->fd);
    if (r->request) gl.g_object_unref(r->request);
    free(r->headers);
    free(r->buf);
    free(r);
}

/* ── Engine callbacks (GLib dispatch, PHP thread) ────────────────── */

typedef struct { vio_webview_header *h; int n, cap; } wvl_hlist;

static void wvl_collect_header(const char *name, const char *value, gpointer data)
{
    wvl_hlist *l = (wvl_hlist *)data;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->h = (vio_webview_header *)realloc(l->h, (size_t)l->cap * sizeof(vio_webview_header));
    }
    l->h[l->n].name = name;
    l->h[l->n++].value = value;
}

static void wvl_on_scheme(void *request, gpointer data)
{
    wvl_view *v = (wvl_view *)data;
    wvl_req *r = (wvl_req *)calloc(1, sizeof(wvl_req));
    r->id = ++v->next_id;
    r->request = gl.g_object_ref(request);
    r->fd = -1;
    r->next = v->reqs;
    v->reqs = r;

    const char *uri = gl.webkit_uri_scheme_request_get_uri(request);
    const char *method = gl.webkit_uri_scheme_request_get_http_method(request);
    wvl_hlist hl = {0};
    void *headers = gl.webkit_uri_scheme_request_get_http_headers(request);
    if (headers) gl.soup_message_headers_foreach(headers, wvl_collect_header, &hl);
    char *body = NULL;
    size_t len = 0, cap = 0;
    void *in = gl.webkit_uri_scheme_request_get_http_body(request);
    if (in) {
        for (;;) {
            if (cap - len < 65536) { cap = cap ? cap * 2 : 65536; body = (char *)realloc(body, cap); }
            size_t got = 0;
            if (!gl.g_input_stream_read_all(in, body + len, cap - len, &got, NULL, NULL) || got == 0) break;
            len += got;
        }
        gl.g_object_unref(in);
    }
    /* WebKit sends Upgrade-Insecure-Requests only when it loads a document (also after a form POST,
     * where the view still shows the old URL); the first load shows its URL already */
    const char *page = gl.webkit_web_view_get_uri(v->view);
    int navigation = page && uri && strcmp(page, uri) == 0;
    for (int i = 0; i < hl.n; i++) if (strcasecmp(hl.h[i].name, "upgrade-insecure-requests") == 0) navigation = 1;
    vio_webview_request_event(v->user, r->id, method, uri, hl.h, hl.n, body, len, navigation);
    free(hl.h);
    free(body);
}

static void wvl_on_message(void *ucm, void *arg, gpointer data)
{
    wvl_view *v = (wvl_view *)data;
    (void)ucm;
    void *value = wvl_engine == WVL_GTK ? gk.webkit_javascript_result_get_js_value(arg) : arg;
    char *s = gl.jsc_value_to_string(value);
    if (s) {
        vio_webview_message_event(v->user, s, strlen(s));
        gl.g_free(s);
    }
}

static void wvl_on_load(void *view, int event, gpointer data)
{
    wvl_view *v = (wvl_view *)data;
    if (event == WVL_LOAD_COMMITTED) vio_webview_nav_event(v->user, VIO_WEBVIEW_EV_NAVIGATED, gl.webkit_web_view_get_uri(view));
}

static void wvl_on_title(void *view, void *pspec, gpointer data)
{
    wvl_view *v = (wvl_view *)data;
    (void)pspec;
    const char *t = gl.webkit_web_view_get_title(view);
    if (!t || !*t || (v->title && strcmp(v->title, t) == 0)) return;
    free(v->title);
    v->title = strdup(t);
    vio_webview_nav_event(v->user, VIO_WEBVIEW_EV_TITLE, t);
}

/* window.open / target=_blank: reported, never opened */
static void *wvl_on_create(void *view, void *action, gpointer data)
{
    wvl_view *v = (wvl_view *)data;
    (void)view;
    void *req = gl.webkit_navigation_action_get_request(action);
    vio_webview_nav_event(v->user, VIO_WEBVIEW_EV_NEW_WINDOW, req ? gl.webkit_uri_request_get_uri(req) : "");
    return NULL;
}

/* ── Hosting: WebKitGTK ──────────────────────────────────────────── */

static void wvl_gtk_place(wvl_view *v, int x, int y, int w, int h)
{
    gk.gtk_widget_set_size_request(v->view, w, h);
    gk.gtk_window_resize(v->gtk_window, w, h);
#ifdef HAVE_X11
    if (v->xid) {   /* a child of vio's window: move it there (GTK still thinks it is a toplevel) */
        Display *dpy = (Display *)vio_plat()->native_handle(v->win, VIO_NATIVE_XLIB_DISPLAY);
        if (dpy) {
            XMoveResizeWindow(dpy, (Window)v->xid, x, y, (unsigned)w, (unsigned)h);
            XFlush(dpy);
        }
    }
#endif
    (void)x; (void)y;
}

static int wvl_gtk_create(wvl_view *v, const vio_webview_desc *d)
{
    v->view = gl.g_object_new(gl.webkit_web_view_get_type(), "web-context", v->ctx, "user-content-manager", v->ucm, NULL);
    if (!v->view) return 0;
    gl.g_object_ref(v->view);   /* the container takes the floating one */
    int wayland = strcmp(vio_plat()->name, "wayland") == 0;
    if (v->headless) v->gtk_window = gk.gtk_offscreen_window_new();
    else v->gtk_window = gk.gtk_window_new(wayland ? WVL_GTK_WINDOW_TOPLEVEL : WVL_GTK_WINDOW_POPUP);
    gk.gtk_container_add(v->gtk_window, v->view);
    if (wayland && !v->headless) gk.gtk_window_set_title(v->gtk_window, "php-vio webview");
#ifdef HAVE_X11
    if (!v->headless && !wayland && gdk_x11_window_get_xid_p) {
        gk.gtk_widget_realize(v->gtk_window);
        void *gdkwin = gk.gtk_widget_get_window(v->gtk_window);
        Display *dpy = (Display *)vio_plat()->native_handle(v->win, VIO_NATIVE_XLIB_DISPLAY);
        Window parent = (Window)(uintptr_t)vio_plat()->native_handle(v->win, VIO_NATIVE_XLIB_WINDOW);
        if (gdkwin && dpy && parent) {
            v->xid = gdk_x11_window_get_xid_p(gdkwin);
            XReparentWindow(dpy, (Window)v->xid, parent, 0, 0);
            XFlush(dpy);
        }
    }
#endif
    gk.gtk_widget_show_all(v->gtk_window);
    return 1;
}

static void wvl_gtk_destroy(wvl_view *v)
{
    if (v->gtk_window) gk.gtk_widget_destroy(v->gtk_window);   /* takes the view widget along */
    v->gtk_window = NULL;
}

/* ── Hosting: WPE (filled in below) ──────────────────────────────── */

static int  wvl_wpe_create(wvl_view *v, const vio_webview_desc *d);
static void wvl_wpe_destroy(wvl_view *v);
static void wvl_wpe_place(wvl_view *v, int x, int y, int w, int h);

/* ── Layout: bounds, or the client area ─────────────────────────── */

static void wvl_layout(wvl_view *v)
{
    int x = v->x, y = v->y, w = v->w, h = v->h;
    if (!v->has_bounds) {
        x = y = 0;
        vio_plat()->get_window_size(v->win, &w, &h);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
    }
    if (x == v->cur_x && y == v->cur_y && w == v->cur_w && h == v->cur_h) return;
    v->cur_x = x; v->cur_y = y; v->cur_w = w; v->cur_h = h;
    if (wvl_engine == WVL_GTK) wvl_gtk_place(v, x, y, w, h);
    else wvl_wpe_place(v, x, y, w, h);
}

/* ── Slots ──────────────────────────────────────────────────────── */

typedef struct { void *egl_dpy, *egl_draw, *egl_read, *egl_ctx; void *glx_dpy, *glx_ctx; unsigned long glx_draw; } wvl_gl_state;
static void wvl_gl_save(wvl_gl_state *st);
static void wvl_gl_restore(const wvl_gl_state *st);

void *vio_wvl_create(vio_window_handle w, const vio_webview_desc *d, int headless)
{
    if (!w || wvl_probe() == WVL_NONE) return NULL;
    wvl_gl_state st;
    wvl_gl_save(&st);
    wvl_view *v = (wvl_view *)calloc(1, sizeof(wvl_view));
    v->win = w;
    v->user = d->user;
    v->headless = headless;
    v->has_bounds = d->has_bounds;
    v->x = d->x; v->y = d->y; v->w = d->width; v->h = d->height;
    v->cur_w = v->cur_h = -1;
    v->ctx = gl.webkit_web_context_new();
    void *sm = gl.webkit_web_context_get_security_manager(v->ctx);
    for (int i = 0; i < d->scheme_count; i++) {
        gl.webkit_web_context_register_uri_scheme(v->ctx, d->schemes[i], wvl_on_scheme, v, NULL);
        gl.webkit_security_manager_register_uri_scheme_as_secure(sm, d->schemes[i]);
        gl.webkit_security_manager_register_uri_scheme_as_cors_enabled(sm, d->schemes[i]);
    }
    v->ucm = gl.webkit_user_content_manager_new();
    gl.webkit_user_content_manager_register_script_message_handler(v->ucm, "vio", NULL);   /* GTK 4.1 takes no world: ignored */
    gl.g_signal_connect_data(v->ucm, "script-message-received::vio", (GCallback)wvl_on_message, v, NULL, 0);
    wvl_scripts(v, d->script);

    int ok = wvl_engine == WVL_GTK ? wvl_gtk_create(v, d) : wvl_wpe_create(v, d);
    if (!ok) {
        gl.g_object_unref(v->ucm);
        gl.g_object_unref(v->ctx);
        free(v);
        wvl_gl_restore(&st);
        return NULL;
    }
    gl.g_signal_connect_data(v->view, "load-changed", (GCallback)wvl_on_load, v, NULL, 0);
    gl.g_signal_connect_data(v->view, "notify::title", (GCallback)wvl_on_title, v, NULL, 0);
    gl.g_signal_connect_data(v->view, "create", (GCallback)wvl_on_create, v, NULL, 0);
    void *settings = gl.webkit_web_view_get_settings(v->view);
    if (d->devtools) gl.webkit_settings_set_enable_developer_extras(settings, 1);
    gl.webkit_settings_set_javascript_can_open_windows_automatically(settings, 1);   /* window.open reaches "create" (new_window) */
    if (d->transparent) {
        wvl_rgba clear = {0, 0, 0, 0};
        gl.webkit_web_view_set_background_color(v->view, &clear);
    }
    v->next = wvl_views;
    wvl_views = v;
    wvl_layout(v);
    gl.webkit_web_view_load_uri(v->view, d->url ? d->url : "about:blank");
    wvl_gl_restore(&st);
    return v;
}

void vio_wvl_destroy(void *p)
{
    wvl_view *v = (wvl_view *)p;
    if (!v) return;
    wvl_gl_state st;
    wvl_gl_save(&st);
    for (wvl_view **pp = &wvl_views; *pp; pp = &(*pp)->next) if (*pp == v) { *pp = v->next; break; }
    gl.g_signal_handlers_disconnect_matched(v->view, WVL_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, v);
    gl.g_signal_handlers_disconnect_matched(v->ucm, WVL_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, v);
    gl.webkit_web_view_stop_loading(v->view);
    /* open requests: an answer WebKit never gets would keep the load hanging */
    while (v->reqs) {
        wvl_req *r = v->reqs;
        if (r->fd < 0) {
            GError *e = gl.g_error_new_literal(gl.g_quark_from_static_string("vio-webview"), 1, "webview closed");
            gl.webkit_uri_scheme_request_finish_error(r->request, e);
            gl.g_error_free(e);
        }
        wvl_req_free(v, r);
    }
    if (wvl_engine == WVL_GTK) wvl_gtk_destroy(v);
    else wvl_wpe_destroy(v);
    gl.g_object_unref(v->view);
    gl.g_object_unref(v->ucm);
    gl.g_object_unref(v->ctx);
    free(v->title);
    free(v);
    gl.g_main_context_iteration(NULL, 0);   /* let the teardown run */
    wvl_gl_restore(&st);
}

int vio_wvl_navigate(void *p, const char *url)
{
    gl.webkit_web_view_load_uri(((wvl_view *)p)->view, url);
    return 1;
}

int vio_wvl_eval(void *p, const char *js)
{
    gl.webkit_web_view_evaluate_javascript(((wvl_view *)p)->view, js, -1, NULL, NULL, NULL, NULL, NULL);
    return 1;
}

int vio_wvl_set_bounds(void *p, int has_bounds, int x, int y, int width, int height)
{
    wvl_view *v = (wvl_view *)p;
    v->has_bounds = has_bounds;
    v->x = x; v->y = y; v->w = width; v->h = height;
    wvl_layout(v);
    return 1;
}

int vio_wvl_set_script(void *p, const char *js)
{
    wvl_scripts((wvl_view *)p, js);
    return 1;
}

int vio_wvl_reply_head(void *p, uint64_t id, int status, const vio_webview_header *h, int count)
{
    wvl_req *r = wvl_find((wvl_view *)p, id);
    if (!r || r->head) return 0;
    r->head = 1;
    r->status = status;
    size_t total = 0;
    for (int i = 0; i < count; i++) total += strlen(h[i].name) + strlen(h[i].value) + 2;
    r->headers = (char *)malloc(total ? total : 1);
    char *q = r->headers;
    for (int i = 0; i < count; i++) {
        size_t a = strlen(h[i].name) + 1, b = strlen(h[i].value) + 1;
        memcpy(q, h[i].name, a); q += a;
        memcpy(q, h[i].value, b); q += b;
    }
    r->headers_len = total;
    r->header_count = count;
    return 1;
}

/* write what is buffered to the stream; 1 = all of it went out */
static int wvl_flush(wvl_view *v, wvl_req *r)
{
    size_t off = 0;
    while (off < r->len) {
        ssize_t n = send(r->fd, r->buf + off, r->len - off, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        /* the page went away: WebKit closed its end */
        vio_webview_cancel_event(v->user, r->id);
        r->len = 0;
        r->ended = 1;
        return 1;
    }
    memmove(r->buf, r->buf + off, r->len - off);
    r->len -= off;
    return r->len == 0;
}

int vio_wvl_reply_data(void *p, uint64_t id, const void *data, size_t len)
{
    wvl_view *v = (wvl_view *)p;
    wvl_req *r = wvl_find(v, id);
    if (!r || !r->head || r->ended) return 0;
    if (r->cap - r->len < len) {
        r->cap = r->cap * 2 > r->len + len ? r->cap * 2 : r->len + len;
        r->buf = (char *)realloc(r->buf, r->cap);
    }
    memcpy(r->buf + r->len, data, len);
    r->len += len;
    if (r->fd >= 0) wvl_flush(v, r);
    return 1;
}

static void *wvl_response(wvl_req *r, void *stream, long len)
{
    void *res = gl.webkit_uri_scheme_response_new(stream, len);
    gl.webkit_uri_scheme_response_set_status(res, (unsigned)r->status, NULL);
    void *hs = gl.soup_message_headers_new(WVL_SOUP_HEADERS_RESPONSE);
    const char *q = r->headers;
    for (int i = 0; i < r->header_count; i++) {
        const char *name = q; q += strlen(q) + 1;
        const char *value = q; q += strlen(q) + 1;
        if (strcasecmp(name, "content-type") == 0) gl.webkit_uri_scheme_response_set_content_type(res, value);
        gl.soup_message_headers_append(hs, name, value);
    }
    gl.webkit_uri_scheme_response_set_http_headers(res, hs);   /* takes them */
    return res;
}

int vio_wvl_reply_end(void *p, uint64_t id)
{
    wvl_view *v = (wvl_view *)p;
    wvl_req *r = wvl_find(v, id);
    if (!r || !r->head) return 0;
    r->ended = 1;
    if (r->fd >= 0) {   /* streaming: closed once the rest is out */
        if (wvl_flush(v, r)) wvl_req_free(v, r);
        return 1;
    }
    void *stream = gl.g_memory_input_stream_new_from_data(r->buf, (long)r->len, free);
    long len = (long)r->len;
    r->buf = NULL;
    r->len = r->cap = 0;
    void *res = wvl_response(r, stream, len);
    gl.webkit_uri_scheme_request_finish_with_response(r->request, res);
    gl.g_object_unref(res);
    gl.g_object_unref(stream);
    wvl_req_free(v, r);
    return 1;
}

int vio_wvl_reply_fail(void *p, uint64_t id)
{
    wvl_view *v = (wvl_view *)p;
    wvl_req *r = wvl_find(v, id);
    if (!r || r->fd >= 0) return 0;
    GError *e = gl.g_error_new_literal(gl.g_quark_from_static_string("vio-webview"), 1, "too many redirects");
    gl.webkit_uri_scheme_request_finish_error(r->request, e);
    gl.g_error_free(e);
    wvl_req_free(v, r);
    return 1;
}

/* An answer PHP began but did not end within one dispatch becomes a stream. */
static void wvl_start_stream(wvl_view *v, wvl_req *r)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) return;
    r->fd = sv[0];
    void *stream = gl.g_unix_input_stream_new(sv[1], 1);
    void *res = wvl_response(r, stream, -1);
    gl.webkit_uri_scheme_request_finish_with_response(r->request, res);
    gl.g_object_unref(res);
    gl.g_object_unref(stream);
    wvl_flush(v, r);
}

/* ── Event loop ─────────────────────────────────────────────────── */

/* GTK draws with GL/EGL on this thread and leaves its own context current;
 * vio's backend expects its own. Everything that runs GTK keeps it. */
static struct {
    int tried;
    void *(*eglGetCurrentContext)(void); void *(*eglGetCurrentDisplay)(void); void *(*eglGetCurrentSurface)(int);
    unsigned (*eglMakeCurrent)(void *, void *, void *, void *);
    void *(*glXGetCurrentContext)(void); void *(*glXGetCurrentDisplay)(void); unsigned long (*glXGetCurrentDrawable)(void);
    int (*glXMakeCurrent)(void *, unsigned long, void *);
} wgl;

static void wvl_gl_save(wvl_gl_state *st)
{
    memset(st, 0, sizeof(*st));
    if (!wgl.tried) {
        wgl.tried = 1;
        void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
        if (egl) {
            wgl.eglGetCurrentContext = (void *(*)(void))dlsym(egl, "eglGetCurrentContext");
            wgl.eglGetCurrentDisplay = (void *(*)(void))dlsym(egl, "eglGetCurrentDisplay");
            wgl.eglGetCurrentSurface = (void *(*)(int))dlsym(egl, "eglGetCurrentSurface");
            wgl.eglMakeCurrent = (unsigned (*)(void *, void *, void *, void *))dlsym(egl, "eglMakeCurrent");
        }
        void *glx = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
        if (glx) {
            wgl.glXGetCurrentContext = (void *(*)(void))dlsym(glx, "glXGetCurrentContext");
            wgl.glXGetCurrentDisplay = (void *(*)(void))dlsym(glx, "glXGetCurrentDisplay");
            wgl.glXGetCurrentDrawable = (unsigned long (*)(void))dlsym(glx, "glXGetCurrentDrawable");
            wgl.glXMakeCurrent = (int (*)(void *, unsigned long, void *))dlsym(glx, "glXMakeCurrent");
        }
    }
    if (wgl.eglGetCurrentContext && wgl.eglMakeCurrent && (st->egl_ctx = wgl.eglGetCurrentContext())) {
        st->egl_dpy = wgl.eglGetCurrentDisplay();
        st->egl_draw = wgl.eglGetCurrentSurface(0x3059 /* EGL_DRAW */);
        st->egl_read = wgl.eglGetCurrentSurface(0x305A /* EGL_READ */);
    }
    if (wgl.glXGetCurrentContext && wgl.glXMakeCurrent && (st->glx_ctx = wgl.glXGetCurrentContext())) {
        st->glx_dpy = wgl.glXGetCurrentDisplay();
        st->glx_draw = wgl.glXGetCurrentDrawable();
    }
}

static void wvl_gl_restore(const wvl_gl_state *st)
{
    if (st->egl_ctx && wgl.eglGetCurrentContext() != st->egl_ctx)
        wgl.eglMakeCurrent(st->egl_dpy, st->egl_draw, st->egl_read, st->egl_ctx);
    if (st->glx_ctx && wgl.glXGetCurrentContext() != st->glx_ctx)
        wgl.glXMakeCurrent(st->glx_dpy, st->glx_draw, st->glx_ctx);
}

#define WVL_MAX_GFDS 64
static GPollFD wvl_gfds[WVL_MAX_GFDS];
static int     wvl_gn, wvl_prio, wvl_acquired;

static int wvl_pending_writes(void)
{
    for (wvl_view *v = wvl_views; v; v = v->next)
        for (wvl_req *r = v->reqs; r; r = r->next) if (r->fd >= 0 && r->len) return 1;
    return 0;
}

int vio_wvl_poll_begin(struct pollfd *fds, int n, int max, int *timeout_ms)
{
    if (wvl_engine <= WVL_NONE || !wvl_views) return n;
    void *ctx = gl.g_main_context_default();
    if (!gl.g_main_context_acquire(ctx)) return n;
    wvl_acquired = 1;
    gl.g_main_context_prepare(ctx, &wvl_prio);
    int gtimeout = -1;
    wvl_gn = gl.g_main_context_query(ctx, wvl_prio, &gtimeout, wvl_gfds, WVL_MAX_GFDS);
    if (wvl_gn > WVL_MAX_GFDS) wvl_gn = WVL_MAX_GFDS;
    for (int i = 0; i < wvl_gn && n < max; i++) {
        fds[n].fd = wvl_gfds[i].fd;
        fds[n].events = (short)wvl_gfds[i].events;
        fds[n].revents = 0;
        n++;
    }
    if (gtimeout >= 0 && (*timeout_ms < 0 || gtimeout < *timeout_ms)) *timeout_ms = gtimeout;
    if (wvl_pending_writes() && (*timeout_ms < 0 || *timeout_ms > 10)) *timeout_ms = 10;
    return n;
}

void vio_wvl_poll_end(struct pollfd *fds, int n_before, int n_after)
{
    if (!wvl_acquired) return;
    for (int i = 0; i < wvl_gn; i++) wvl_gfds[i].revents = n_before + i < n_after ? (unsigned short)fds[n_before + i].revents : 0;
    void *ctx = gl.g_main_context_default();
    wvl_gl_state st;
    wvl_gl_save(&st);
    if (gl.g_main_context_check(ctx, wvl_prio, wvl_gfds, wvl_gn)) gl.g_main_context_dispatch(ctx);
    wvl_gl_restore(&st);
    gl.g_main_context_release(ctx);
    wvl_acquired = 0;
}

void vio_wvl_pump(void)
{
    if (wvl_engine <= WVL_NONE || !wvl_views) return;
    wvl_gl_state st;
    wvl_gl_save(&st);
    for (int i = 0; i < 64 && gl.g_main_context_iteration(NULL, 0); i++) {}
    for (wvl_view *v = wvl_views; v; v = v->next) {
        wvl_layout(v);
        for (wvl_req *r = v->reqs, *next; r; r = next) {
            next = r->next;
            if (r->head && !r->ended && r->fd < 0) wvl_start_stream(v, r);
            else if (r->fd >= 0 && (r->len == 0 || wvl_flush(v, r)) && r->ended) wvl_req_free(v, r);
        }
    }
    wvl_gl_restore(&st);
}

/* ── WPE ────────────────────────────────────────────────────────── */

static int wvl_load_wpe(void) { return 0; }   /* next step */
static int  wvl_wpe_create(wvl_view *v, const vio_webview_desc *d) { (void)v; (void)d; return 0; }
static void wvl_wpe_destroy(wvl_view *v) { (void)v; }
static void wvl_wpe_place(wvl_view *v, int x, int y, int w, int h) { (void)v; (void)x; (void)y; (void)w; (void)h; }

#ifdef HAVE_X11
int vio_wvl_x11_event(void *xevent) { (void)xevent; return 0; }
#endif

#endif /* __linux__ */
