/*
 * php-vio - System webview (WEBVIEW-PLAN)
 *
 * A VioWebview is a child view of a VioContext's or VioWindow's platform
 * window (WebView2, WKWebView, WPE WebKit behind the vio_platform slots).
 * The engines report scheme requests, script messages and navigation events
 * through vio_webview_*_event while the platform pumps its events; they are
 * queued here and delivered to PHP by vio_webview_dispatch after
 * vio_poll_events / vio_wait_events (DESKTOP-APP-PLAN principle 4), so a
 * callback never runs inside an engine callback. A scheme request stays open
 * in the engine until PHP has answered it; an iterable body is streamed one
 * chunk per dispatch, and the loop is woken while chunks are left.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "zend_interfaces.h"
#include "zend_smart_str.h"
#include "ext/json/php_json.h"
#include "ext/date/php_date.h"
#include <time.h>
#include "vio_webview.h"
#include "vio_context.h"
#include "../include/vio_platform.h"

zend_class_entry *vio_webview_ce = NULL;
static zend_object_handlers vio_webview_handlers;

typedef struct _vio_wv_stream {
    uint64_t               id;
    zend_object_iterator  *it;
    int                    started;
    struct _vio_wv_stream *next;
} vio_wv_stream;

/* Cookie jar (WEBVIEW-PLAN decision 2): engines do not keep cookies for
 * custom schemes (WKWebView keeps none at all), so vio does, the same on
 * every engine: Set-Cookie of an answer goes here, not to the engine, and
 * every request gets its Cookie header from here. Session-only. */
typedef struct _vio_wv_cookie {
    char    *name, *value, *host, *path;
    double   expires;     /* unix time, 0 = session */
    int      httponly;
    uint64_t seq;         /* creation order */
} vio_wv_cookie;

typedef struct _vio_webview_object {
    vio_wv_cookie *cookies;
    int            cookie_count, cookie_cap;
    uint64_t       cookie_seq;
    void          *handle;    /* platform webview; NULL once closed */
    void          *window;    /* its platform window */
    zend_object   *owner;     /* VioContext / VioWindow, referenced until the object goes */
    zval           schemes;   /* array: scheme => callable */
    zval           on_message, on_event;
    vio_wv_stream *streams;
    struct _vio_webview_object *next;   /* live list */
    zend_object    std;
} vio_webview_object;

static inline vio_webview_object *vio_webview_from_obj(zend_object *obj)
{
    return (vio_webview_object *)((char *)obj - XtOffsetOf(vio_webview_object, std));
}
#define Z_VIO_WEBVIEW_P(zv) vio_webview_from_obj(Z_OBJ_P(zv))

static vio_webview_object *vio_webviews = NULL;   /* open webviews (handle != NULL) */

/* ── Event queue (C memory: engines fill it from inside the platform's pump) ── */

enum { WV_EV_REQUEST = 1, WV_EV_CANCEL, WV_EV_MESSAGE, WV_EV_NAV };

typedef struct _vio_wv_event {
    int                 kind, nav, navigation;
    vio_webview_object *wv;
    uint64_t            id;
    char               *method, *url, *text;
    char               *headers;      /* name\0value\0 ... */
    size_t              headers_len;
    int                 header_count;
    char               *body;
    size_t              body_len;
    struct _vio_wv_event *next;
} vio_wv_event;

static vio_wv_event *wv_queue_head = NULL, *wv_queue_tail = NULL;

static char *wv_strdup(const char *s) { return s ? strdup(s) : strdup(""); }

static void wv_event_free(vio_wv_event *e)
{
    free(e->method); free(e->url); free(e->text); free(e->headers); free(e->body);
    free(e);
}

static void wv_push(vio_wv_event *e)
{
    e->next = NULL;
    if (wv_queue_tail) wv_queue_tail->next = e; else wv_queue_head = e;
    wv_queue_tail = e;
}

static vio_wv_event *wv_new(void *user, int kind)
{
    vio_webview_object *wv = (vio_webview_object *)user;
    if (!wv || !wv->handle) return NULL;
    vio_wv_event *e = (vio_wv_event *)calloc(1, sizeof(vio_wv_event));
    if (!e) return NULL;
    e->kind = kind;
    e->wv = wv;
    return e;
}

void vio_webview_request_event(void *user, uint64_t id, const char *method, const char *url,
                               const vio_webview_header *h, int count, const void *body, size_t len, int navigation)
{
    vio_wv_event *e = wv_new(user, WV_EV_REQUEST);
    if (!e) return;
    e->id = id;
    e->navigation = navigation;
    e->method = wv_strdup(method && *method ? method : "GET");
    e->url = wv_strdup(url);
    size_t total = 0;
    for (int i = 0; i < count; i++) total += strlen(h[i].name) + strlen(h[i].value ? h[i].value : "") + 2;
    e->headers = (char *)malloc(total ? total : 1);
    char *p = e->headers;
    for (int i = 0; i < count; i++) {
        size_t n = strlen(h[i].name), v = strlen(h[i].value ? h[i].value : "");
        memcpy(p, h[i].name, n + 1); p += n + 1;
        memcpy(p, h[i].value ? h[i].value : "", v + 1); p += v + 1;
    }
    e->headers_len = total;
    e->header_count = count;
    e->body = (char *)malloc(len ? len : 1);
    if (len && body) memcpy(e->body, body, len);
    e->body_len = len;
    wv_push(e);
}

void vio_webview_cancel_event(void *user, uint64_t id)
{
    vio_wv_event *e = wv_new(user, WV_EV_CANCEL);
    if (!e) return;
    e->id = id;
    wv_push(e);
}

void vio_webview_message_event(void *user, const char *json, size_t len)
{
    vio_wv_event *e = wv_new(user, WV_EV_MESSAGE);
    if (!e) return;
    e->body = (char *)malloc(len ? len : 1);
    if (len) memcpy(e->body, json, len);
    e->body_len = len;
    wv_push(e);
}

void vio_webview_nav_event(void *user, int kind, const char *text)
{
    vio_wv_event *e = wv_new(user, WV_EV_NAV);
    if (!e) return;
    e->nav = kind;
    e->text = wv_strdup(text);
    wv_push(e);
}

/* Drop queued events of a webview that is going away. */
static void wv_purge(vio_webview_object *wv)
{
    vio_wv_event **pp = &wv_queue_head, *last = NULL;
    while (*pp) {
        vio_wv_event *e = *pp;
        if (e->wv == wv) { *pp = e->next; wv_event_free(e); continue; }
        last = e;
        pp = &e->next;
    }
    wv_queue_tail = last;
}


/* ── Cookie jar ───────────────────────────────────────────────────── */

/* host (with port) and path of scheme://host/path?query */
static void wv_url_parts(const char *url, char *host, size_t hcap, char *path, size_t pcap)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t n = strcspn(p, "/?#");
    snprintf(host, hcap, "%.*s", (int)n, p);
    p += n;
    size_t m = *p == '/' ? strcspn(p, "?#") : 0;
    if (m) snprintf(path, pcap, "%.*s", (int)m, p);
    else snprintf(path, pcap, "/");
}

static void wv_cookie_free(vio_wv_cookie *c)
{
    free(c->name); free(c->value); free(c->host); free(c->path);
}

static void wv_jar_clear(vio_webview_object *wv)
{
    for (int i = 0; i < wv->cookie_count; i++) wv_cookie_free(&wv->cookies[i]);
    free(wv->cookies);
    wv->cookies = NULL;
    wv->cookie_count = wv->cookie_cap = 0;
}

static char *wv_trimdup(const char *s, size_t n)
{
    while (n && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    char *r = (char *)malloc(n + 1);
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

/* One Set-Cookie line (or a document.cookie assignment, from_js) for a
 * request to host/path. 1 = the jar changed. */
static int wv_jar_set(vio_webview_object *wv, const char *host, const char *req_path, const char *line, int from_js)
{
    size_t first = strcspn(line, ";");
    const char *eq = memchr(line, '=', first);
    if (!eq) return 0;
    char *name = wv_trimdup(line, (size_t)(eq - line));
    char *value = wv_trimdup(eq + 1, first - (size_t)(eq + 1 - line));
    if (!*name) { free(name); free(value); return 0; }
    char *path = NULL;
    double expires = 0;
    int httponly = 0, has_max_age = 0;
    double now = (double)time(NULL);
    const char *a = line + first;
    while (*a == ';') {
        a++;
        size_t n = strcspn(a, ";");
        const char *aeq = memchr(a, '=', n);
        char *key = wv_trimdup(a, aeq ? (size_t)(aeq - a) : n);
        char *val = aeq ? wv_trimdup(aeq + 1, n - (size_t)(aeq + 1 - a)) : strdup("");
        if (strcasecmp(key, "path") == 0 && val[0] == '/') { free(path); path = strdup(val); }
        else if (strcasecmp(key, "max-age") == 0 && *val) { has_max_age = 1; expires = now + strtod(val, NULL); if (expires <= now) expires = -1; }
        else if (strcasecmp(key, "expires") == 0 && !has_max_age && *val) {
            zend_long t = php_parse_date(val, NULL);
            if (t != -1) expires = (double)t <= now ? -1 : (double)t;
        }
        else if (strcasecmp(key, "httponly") == 0) httponly = 1;
        free(key);
        free(val);
        a += n;
    }
    if (!path) {   /* RFC 6265 5.1.4 default-path */
        const char *slash = strrchr(req_path, '/');
        path = slash && slash != req_path ? wv_trimdup(req_path, (size_t)(slash - req_path)) : strdup("/");
    }
    int changed = 0;
    int found = -1;
    for (int i = 0; i < wv->cookie_count; i++) {
        vio_wv_cookie *c = &wv->cookies[i];
        if (strcmp(c->name, name) == 0 && strcmp(c->host, host) == 0 && strcmp(c->path, path) == 0) { found = i; break; }
    }
    if (from_js && (httponly || (found >= 0 && wv->cookies[found].httponly))) goto done;   /* script cannot touch HttpOnly */
    if (expires < 0) {
        if (found >= 0) {
            wv_cookie_free(&wv->cookies[found]);
            wv->cookies[found] = wv->cookies[--wv->cookie_count];
            changed = 1;
        }
        goto done;
    }
    if (found >= 0) {
        vio_wv_cookie *c = &wv->cookies[found];
        changed = strcmp(c->value, value) != 0 || c->httponly != httponly || c->expires != expires;
        free(c->value);
        c->value = value; value = NULL;
        c->httponly = httponly;
        c->expires = expires;
        goto done;
    }
    if (wv->cookie_count == wv->cookie_cap) {
        if (wv->cookie_cap >= 1024) goto done;
        wv->cookie_cap = wv->cookie_cap ? wv->cookie_cap * 2 : 8;
        wv->cookies = (vio_wv_cookie *)realloc(wv->cookies, (size_t)wv->cookie_cap * sizeof(vio_wv_cookie));
    }
    vio_wv_cookie *c = &wv->cookies[wv->cookie_count++];
    c->name = name; name = NULL;
    c->value = value; value = NULL;
    c->host = strdup(host);
    c->path = path; path = NULL;
    c->expires = expires;
    c->httponly = httponly;
    c->seq = ++wv->cookie_seq;
    changed = 1;
done:
    free(name); free(value); free(path);
    return changed;
}

static int wv_path_match(const char *req, const char *cp)
{
    size_t n = strlen(cp);
    if (strncmp(req, cp, n) != 0) return 0;
    return req[n] == '\0' || cp[n - 1] == '/' || req[n] == '/';
}

static int wv_cookie_cmp(const void *a, const void *b)
{
    const vio_wv_cookie *x = *(vio_wv_cookie *const *)a, *y = *(vio_wv_cookie *const *)b;
    size_t lx = strlen(x->path), ly = strlen(y->path);
    if (lx != ly) return lx > ly ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

/* "a=1; b=2" for a request, NULL when no cookie applies (malloc'd) */
static char *wv_jar_header(vio_webview_object *wv, const char *host, const char *path)
{
    if (!wv->cookie_count) return NULL;
    double now = (double)time(NULL);
    vio_wv_cookie **m = (vio_wv_cookie **)malloc((size_t)wv->cookie_count * sizeof(*m));
    int n = 0;
    for (int i = 0; i < wv->cookie_count; i++) {
        vio_wv_cookie *c = &wv->cookies[i];
        if (strcmp(c->host, host) != 0 || !wv_path_match(path, c->path)) continue;
        if (c->expires > 0 && c->expires <= now) continue;
        m[n++] = c;
    }
    char *out = NULL;
    if (n) {
        qsort(m, (size_t)n, sizeof(*m), wv_cookie_cmp);
        smart_str s = {0};
        for (int i = 0; i < n; i++) {
            if (i) smart_str_appends(&s, "; ");
            smart_str_appends(&s, m[i]->name);
            smart_str_appendc(&s, '=');
            smart_str_appends(&s, m[i]->value);
        }
        smart_str_0(&s);
        out = strdup(ZSTR_VAL(s.s));
        smart_str_free(&s);
    }
    free(m);
    return out;
}

/* document.cookie on the custom-scheme pages: the cookies script may see
 * (not HttpOnly), kept by the page and updated by vio; assignments go to the
 * jar through window.__vioPost("\x01cookie:<url>\n<assignment>"). */
static const char wv_cookie_shim[] =
    "(function () {\n"
    "  var schemes = %s, jar = %s;\n"
    "  if (schemes.indexOf(location.protocol.slice(0, -1)) < 0 || !window.__vioPost) return;\n"
    "  window.__vioCookies = function (j) { jar = j; };\n"
    "  function match(c) {\n"
    "    var p = location.pathname || '/';\n"
    "    if (c.h !== location.host || (c.e && c.e * 1000 <= Date.now())) return false;\n"
    "    return p === c.p || p.indexOf(c.p) === 0 && (c.p.slice(-1) === '/' || p.charAt(c.p.length) === '/');\n"
    "  }\n"
    "  Object.defineProperty(document, 'cookie', { configurable: true,\n"
    "    get: function () {\n"
    "      return jar.filter(match).sort(function (a, b) { return b.p.length - a.p.length || a.s - b.s; })\n"
    "                .map(function (c) { return c.n + '=' + c.v; }).join('; ');\n"
    "    },\n"
    "    set: function (v) {\n"
    "      v = String(v);\n"
    "      var parts = v.split(';'), kv = parts[0], i = kv.indexOf('=');\n"
    "      if (i < 0) return;\n"
    "      var c = { n: kv.slice(0, i).trim(), v: kv.slice(i + 1).trim(), h: location.host, p: null, e: 0, s: Date.now() };\n"
    "      var gone = false;\n"
    "      for (var k = 1; k < parts.length; k++) {\n"
    "        var a = parts[k], j = a.indexOf('='), key = (j < 0 ? a : a.slice(0, j)).trim().toLowerCase(), val = j < 0 ? '' : a.slice(j + 1).trim();\n"
    "        if (key === 'httponly') return;\n"
    "        if (key === 'path' && val.charAt(0) === '/') c.p = val;\n"
    "        if (key === 'max-age' && val !== '') { var s = parseFloat(val); if (s <= 0) gone = true; else c.e = Date.now() / 1000 + s; }\n"
    "        if (key === 'expires' && val !== '') { var t = Date.parse(val); if (!isNaN(t)) { if (t <= Date.now()) gone = true; else c.e = t / 1000; } }\n"
    "      }\n"
    "      if (!c.n) return;\n"
    "      if (c.p === null) { var d = location.pathname || '/', l = d.lastIndexOf('/'); c.p = l > 0 ? d.slice(0, l) : '/'; }\n"
    "      jar = jar.filter(function (o) { return !(o.n === c.n && o.h === c.h && o.p === c.p); });\n"
    "      if (!gone) jar.push(c);\n"
    "      window.__vioPost('\\u0001cookie:' + location.href + '\\n' + v);\n"
    "    }\n"
    "  });\n"
    "})();\n";

/* [{"n","v","h","p","e","s"}] of the cookies script may see */
static zend_string *wv_jar_json(vio_webview_object *wv)
{
    zval arr;
    array_init(&arr);
    for (int i = 0; i < wv->cookie_count; i++) {
        vio_wv_cookie *c = &wv->cookies[i];
        if (c->httponly) continue;
        zval o;
        array_init(&o);
        add_assoc_string(&o, "n", c->name);
        add_assoc_string(&o, "v", c->value);
        add_assoc_string(&o, "h", c->host);
        add_assoc_string(&o, "p", c->path);
        add_assoc_double(&o, "e", c->expires);
        add_assoc_long(&o, "s", (zend_long)c->seq);
        add_next_index_zval(&arr, &o);
    }
    smart_str buf = {0};
    php_json_encode(&buf, &arr, PHP_JSON_UNESCAPED_SLASHES | PHP_JSON_HEX_TAG);
    smart_str_0(&buf);
    zval_ptr_dtor(&arr);
    return buf.s;
}

static char *wv_shim_script(vio_webview_object *wv)
{
    zval names;
    array_init(&names);
    if (Z_TYPE(wv->schemes) == IS_ARRAY) {
        zend_string *k;
        ZEND_HASH_FOREACH_STR_KEY(Z_ARRVAL(wv->schemes), k) {
            if (k) add_next_index_str(&names, zend_string_copy(k));
        } ZEND_HASH_FOREACH_END();
    }
    smart_str sn = {0};
    php_json_encode(&sn, &names, 0);
    smart_str_0(&sn);
    zval_ptr_dtor(&names);
    zend_string *jar = wv_jar_json(wv);
    size_t len = sizeof(wv_cookie_shim) + ZSTR_LEN(sn.s) + ZSTR_LEN(jar);
    char *out = (char *)malloc(len);
    snprintf(out, len, wv_cookie_shim, ZSTR_VAL(sn.s), ZSTR_VAL(jar));
    smart_str_free(&sn);
    zend_string_release(jar);
    return out;
}

/* After the jar changed: the next documents start with it; live: the page too. */
static void wv_jar_publish(vio_webview_object *wv, int live)
{
    if (!wv->handle) return;
    const vio_platform *p = vio_plat();
    if (p->webview_set_script) {
        char *js = wv_shim_script(wv);
        p->webview_set_script(wv->handle, js);
        free(js);
    }
    if (live) {
        zend_string *jar = wv_jar_json(wv);
        smart_str s = {0};
        smart_str_appends(&s, "window.__vioCookies && window.__vioCookies(");
        smart_str_append(&s, jar);
        smart_str_appends(&s, ");");
        smart_str_0(&s);
        p->webview_eval(wv->handle, ZSTR_VAL(s.s));
        smart_str_free(&s);
        zend_string_release(jar);
    }
}

/* ── Object ───────────────────────────────────────────────────────── */

static void wv_stream_free(vio_wv_stream *s)
{
    if (s->it) zend_iterator_dtor(s->it);
    efree(s);
}

/* Close the platform webview (idempotent); the object stays, refusing calls. */
static void vio_webview_close(vio_webview_object *wv)
{
    if (!wv->handle) return;
    void *h = wv->handle;
    wv->handle = NULL;
    wv->window = NULL;
    for (vio_webview_object **pp = &vio_webviews; *pp; pp = &(*pp)->next) {
        if (*pp == wv) { *pp = wv->next; break; }
    }
    wv->next = NULL;
    vio_plat()->webview_destroy(h);
    wv_purge(wv);
    while (wv->streams) {
        vio_wv_stream *s = wv->streams;
        wv->streams = s->next;
        wv_stream_free(s);
    }
}

void vio_webview_window_closing(vio_window_handle window)
{
    if (!window) return;
    for (vio_webview_object *wv = vio_webviews; wv; ) {
        vio_webview_object *next = wv->next;
        if (wv->window == window) vio_webview_close(wv);
        wv = next;
    }
}

static zend_object *vio_webview_create_object(zend_class_entry *ce)
{
    vio_webview_object *wv = zend_object_alloc(sizeof(vio_webview_object), ce);
    wv->handle = wv->window = NULL;
    wv->cookies = NULL;
    wv->cookie_count = wv->cookie_cap = 0;
    wv->cookie_seq = 0;
    wv->owner = NULL;
    wv->streams = NULL;
    wv->next = NULL;
    ZVAL_UNDEF(&wv->schemes);
    ZVAL_UNDEF(&wv->on_message);
    ZVAL_UNDEF(&wv->on_event);
    zend_object_std_init(&wv->std, ce);
    object_properties_init(&wv->std, ce);
    wv->std.handlers = &vio_webview_handlers;
    return &wv->std;
}

static void vio_webview_free_object(zend_object *obj)
{
    vio_webview_object *wv = vio_webview_from_obj(obj);
    vio_webview_close(wv);
    wv_purge(wv);   /* events requeued after an exception may still name it */
    wv_jar_clear(wv);
    zval_ptr_dtor(&wv->schemes);
    zval_ptr_dtor(&wv->on_message);
    zval_ptr_dtor(&wv->on_event);
    ZVAL_UNDEF(&wv->schemes);
    ZVAL_UNDEF(&wv->on_message);
    ZVAL_UNDEF(&wv->on_event);
    if (wv->owner) {
        zend_object *owner = wv->owner;
        wv->owner = NULL;
        OBJ_RELEASE(owner);
    }
    zend_object_std_dtor(&wv->std);
}

static HashTable *vio_webview_get_gc(zend_object *obj, zval **table, int *n)
{
    vio_webview_object *wv = vio_webview_from_obj(obj);
    zend_get_gc_buffer *buf = zend_get_gc_buffer_create();
    zend_get_gc_buffer_add_zval(buf, &wv->schemes);
    zend_get_gc_buffer_add_zval(buf, &wv->on_message);
    zend_get_gc_buffer_add_zval(buf, &wv->on_event);
    if (wv->owner) zend_get_gc_buffer_add_obj(buf, wv->owner);
    zend_get_gc_buffer_use(buf, table, n);
    return zend_std_get_properties(obj);
}

void vio_webview_register(void)
{
    zend_class_entry ce;
    INIT_CLASS_ENTRY(ce, "VioWebview", NULL);
    vio_webview_ce = zend_register_internal_class(&ce);
    vio_webview_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_webview_ce->create_object = vio_webview_create_object;
    memcpy(&vio_webview_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_webview_handlers.offset = XtOffsetOf(vio_webview_object, std);
    vio_webview_handlers.free_obj = vio_webview_free_object;
    vio_webview_handlers.get_gc = vio_webview_get_gc;
    vio_webview_handlers.clone_obj = NULL;
}

int vio_webview_available(void)
{
    const vio_platform *p = vio_plat();
    if (!p->webview_create || !vio_platform_has_windows()) return 0;
    return !p->available || p->available(VIO_PLATFORM_FEATURE_WEBVIEW);
}

void vio_webview_shutdown(void)
{
    while (wv_queue_head) {
        vio_wv_event *e = wv_queue_head;
        wv_queue_head = e->next;
        wv_event_free(e);
    }
    wv_queue_tail = NULL;
}

/* ── Delivery ─────────────────────────────────────────────────────── */

static void wv_reply_simple(vio_webview_object *wv, uint64_t id, int status, const char *body)
{
    const vio_platform *p = vio_plat();
    vio_webview_header h = {"Content-Type", "text/plain; charset=utf-8"};
    p->webview_reply_head(wv->handle, id, status, &h, 1);
    if (body && *body) p->webview_reply_data(wv->handle, id, body, strlen(body));
    p->webview_reply_end(wv->handle, id);
}

/* The callback's answer: array (status, headers, body) or a string body. */
static void wv_reply(vio_webview_object *wv, uint64_t id, zval *ret, const char *url)
{
    const vio_platform *p = vio_plat();
    if (Z_TYPE_P(ret) == IS_STRING) {
        p->webview_reply_head(wv->handle, id, 200, NULL, 0);
        p->webview_reply_data(wv->handle, id, Z_STRVAL_P(ret), Z_STRLEN_P(ret));
        p->webview_reply_end(wv->handle, id);
        return;
    }
    if (Z_TYPE_P(ret) != IS_ARRAY) {
        wv_reply_simple(wv, id, 500, "scheme callback returned neither an array nor a string");
        zend_type_error("vio_webview: a scheme callback must return array|string, %s returned", zend_zval_value_name(ret));
        return;
    }
    HashTable *r = Z_ARRVAL_P(ret);
    int status = 200;
    zval *v;
    if ((v = zend_hash_str_find(r, "status", sizeof("status") - 1))) {
        zend_long s = zval_get_long(v);
        if (s >= 100 && s <= 599) status = (int)s;
    }
    vio_webview_header *hs = NULL;
    zend_string **keep = NULL;
    int hc = 0, cap = 0;
    if ((v = zend_hash_str_find(r, "headers", sizeof("headers") - 1)) && Z_TYPE_P(v) == IS_ARRAY) {
        cap = (int)zend_hash_num_elements(Z_ARRVAL_P(v)) * 4 + 1;
        hs = (vio_webview_header *)ecalloc(cap, sizeof(vio_webview_header));
        keep = (zend_string **)ecalloc(cap * 2, sizeof(zend_string *));
        zend_string *name;
        zval *hv;
        int kc = 0, jar_changed = 0;
        char host[256], path[1024];
        wv_url_parts(url, host, sizeof(host), path, sizeof(path));
        ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(v), name, hv) {
            if (!name) continue;
            if (strcasecmp(ZSTR_VAL(name), "set-cookie") == 0) {   /* into vio's jar, not the engine's */
                if (Z_TYPE_P(hv) == IS_ARRAY) {
                    zval *one;
                    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(hv), one) {
                        zend_string *line = zval_get_string(one);
                        jar_changed |= wv_jar_set(wv, host, path, ZSTR_VAL(line), 0);
                        zend_string_release(line);
                    } ZEND_HASH_FOREACH_END();
                } else {
                    zend_string *line = zval_get_string(hv);
                    jar_changed |= wv_jar_set(wv, host, path, ZSTR_VAL(line), 0);
                    zend_string_release(line);
                }
                continue;
            }
            if (Z_TYPE_P(hv) == IS_ARRAY) {   /* several values: one header line each (Set-Cookie) */
                zval *one;
                ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(hv), one) {
                    if (hc >= cap) break;
                    zend_string *s = zval_get_string(one);
                    keep[kc++] = s;
                    hs[hc].name = ZSTR_VAL(name);
                    hs[hc++].value = ZSTR_VAL(s);
                } ZEND_HASH_FOREACH_END();
            } else if (hc < cap) {
                zend_string *s = zval_get_string(hv);
                keep[kc++] = s;
                hs[hc].name = ZSTR_VAL(name);
                hs[hc++].value = ZSTR_VAL(s);
            }
        } ZEND_HASH_FOREACH_END();
        if (jar_changed) wv_jar_publish(wv, 1);   /* before the answer: the page sees the cookies when it gets it */
        p->webview_reply_head(wv->handle, id, status, hs, hc);
        for (int i = 0; i < kc; i++) zend_string_release(keep[i]);
        efree(keep);
        efree(hs);
    } else {
        p->webview_reply_head(wv->handle, id, status, NULL, 0);
    }

    v = zend_hash_str_find(r, "body", sizeof("body") - 1);
    if (!v || Z_TYPE_P(v) == IS_NULL) {
        p->webview_reply_end(wv->handle, id);
    } else if (Z_TYPE_P(v) == IS_ARRAY) {
        zval *chunk;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(v), chunk) {
            zend_string *s = zval_get_string(chunk);
            if (ZSTR_LEN(s)) p->webview_reply_data(wv->handle, id, ZSTR_VAL(s), ZSTR_LEN(s));
            zend_string_release(s);
        } ZEND_HASH_FOREACH_END();
        p->webview_reply_end(wv->handle, id);
    } else if (Z_TYPE_P(v) == IS_OBJECT && Z_OBJCE_P(v)->get_iterator) {
        zend_object_iterator *it = Z_OBJCE_P(v)->get_iterator(Z_OBJCE_P(v), v, 0);
        if (!it || EG(exception)) {
            if (it) zend_iterator_dtor(it);
            p->webview_reply_end(wv->handle, id);
            return;
        }
        vio_wv_stream *s = (vio_wv_stream *)emalloc(sizeof(vio_wv_stream));
        s->id = id;
        s->it = it;
        s->started = 0;
        s->next = NULL;
        vio_wv_stream **pp = &wv->streams;   /* append: streams advance in arrival order */
        while (*pp) pp = &(*pp)->next;
        *pp = s;
    } else {
        zend_string *s = zval_get_string(v);
        if (ZSTR_LEN(s)) p->webview_reply_data(wv->handle, id, ZSTR_VAL(s), ZSTR_LEN(s));
        zend_string_release(s);
        p->webview_reply_end(wv->handle, id);
    }
}

static zval *wv_scheme_callback(vio_webview_object *wv, const char *url)
{
    if (Z_TYPE(wv->schemes) != IS_ARRAY) return NULL;
    const char *colon = strchr(url, ':');
    if (!colon) return NULL;
    return zend_hash_str_find(Z_ARRVAL(wv->schemes), url, (size_t)(colon - url));
}

/* Set-Cookie lines of an answer's headers array into the jar; 1 = changed */
static int wv_apply_set_cookies(vio_webview_object *wv, zval *headers, const char *url)
{
    if (!headers || Z_TYPE_P(headers) != IS_ARRAY) return 0;
    char host[256], path[1024];
    wv_url_parts(url, host, sizeof(host), path, sizeof(path));
    int changed = 0;
    zend_string *name;
    zval *hv;
    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(headers), name, hv) {
        if (!name || strcasecmp(ZSTR_VAL(name), "set-cookie") != 0) continue;
        if (Z_TYPE_P(hv) == IS_ARRAY) {
            zval *one;
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(hv), one) {
                zend_string *line = zval_get_string(one);
                changed |= wv_jar_set(wv, host, path, ZSTR_VAL(line), 0);
                zend_string_release(line);
            } ZEND_HASH_FOREACH_END();
        } else {
            zend_string *line = zval_get_string(hv);
            changed |= wv_jar_set(wv, host, path, ZSTR_VAL(line), 0);
            zend_string_release(line);
        }
    } ZEND_HASH_FOREACH_END();
    return changed;
}

/* Location of a redirect answer resolved against url (malloc'd), or NULL */
static char *wv_redirect_target(zval *ret, const char *url, int *status)
{
    if (Z_TYPE_P(ret) != IS_ARRAY) return NULL;
    zval *v = zend_hash_str_find(Z_ARRVAL_P(ret), "status", sizeof("status") - 1);
    zend_long st = v ? zval_get_long(v) : 200;
    if (st != 301 && st != 302 && st != 303 && st != 307 && st != 308) return NULL;
    zval *headers = zend_hash_str_find(Z_ARRVAL_P(ret), "headers", sizeof("headers") - 1);
    if (!headers || Z_TYPE_P(headers) != IS_ARRAY) return NULL;
    zend_string *name;
    zval *hv;
    zend_string *loc = NULL;
    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(headers), name, hv) {
        if (name && strcasecmp(ZSTR_VAL(name), "location") == 0 && Z_TYPE_P(hv) == IS_STRING) loc = Z_STR_P(hv);
    } ZEND_HASH_FOREACH_END();
    if (!loc || !ZSTR_LEN(loc)) return NULL;
    *status = (int)st;
    const char *l = ZSTR_VAL(loc);
    const char *colon = strchr(url, ':');
    size_t scheme_len = colon ? (size_t)(colon - url) : 0;
    const char *authority = strstr(url, "://");
    const char *path_start = authority ? authority + 3 + strcspn(authority + 3, "/?#") : url + strlen(url);
    size_t n = strspn(l, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+.-");
    smart_str s = {0};
    if (n && l[n] == ':' && (l[0] | 0x20) >= 'a' && (l[0] | 0x20) <= 'z') {
        smart_str_appends(&s, l);                                    /* absolute */
    } else if (l[0] == '/' && l[1] == '/') {
        smart_str_appendl(&s, url, scheme_len + 1);                  /* scheme-relative */
        smart_str_appends(&s, l);
    } else if (l[0] == '/') {
        smart_str_appendl(&s, url, (size_t)(path_start - url));     /* host-relative */
        smart_str_appends(&s, l);
    } else {
        size_t end = strcspn(path_start, "?#");                      /* path-relative */
        size_t keep = (size_t)(path_start - url);
        if (l[0] == '?') keep += end;
        else for (size_t i = end; i > 0; i--) if (path_start[i - 1] == '/') { keep += i; break; }
        smart_str_appendl(&s, url, keep);
        if (l[0] != '?' && keep == (size_t)(path_start - url)) smart_str_appendc(&s, '/');
        smart_str_appends(&s, l);
    }
    smart_str_0(&s);
    char *out = strdup(ZSTR_VAL(s.s));
    smart_str_free(&s);
    return out;
}

#define WV_MAX_REDIRECTS 20   /* as fetch() */

static void wv_deliver_request_depth(vio_wv_event *e, int depth)
{
    vio_webview_object *wv = e->wv;
    zval *cb = wv_scheme_callback(wv, e->url);
    if (!cb) { wv_reply_simple(wv, e->id, 404, "no handler for this scheme"); return; }

    zval req, headers, ret;
    array_init(&req);
    add_assoc_long(&req, "id", (zend_long)e->id);
    add_assoc_string(&req, "method", e->method);
    add_assoc_string(&req, "url", e->url);
    array_init(&headers);
    const char *p = e->headers;
    for (int i = 0; i < e->header_count; i++) {
        const char *name = p; p += strlen(p) + 1;
        const char *value = p; p += strlen(p) + 1;
        if (strcasecmp(name, "cookie") == 0) continue;   /* vio's jar answers for cookies */
        add_assoc_string(&headers, name, value);
    }
    char host[256], path[1024];
    wv_url_parts(e->url, host, sizeof(host), path, sizeof(path));
    char *cookie = wv_jar_header(wv, host, path);
    if (cookie) { add_assoc_string(&headers, "Cookie", cookie); free(cookie); }
    add_assoc_zval(&req, "headers", &headers);
    add_assoc_stringl(&req, "body", e->body, e->body_len);

    ZVAL_UNDEF(&ret);
    GC_ADDREF(&wv->std);   /* the callback may drop the last reference */
    int ok = call_user_function(NULL, NULL, cb, &ret, 1, &req) == SUCCESS;
    zval_ptr_dtor(&req);
    int status = 0;
    char *target = NULL;
    if (wv->handle && !EG(exception) && ok && (target = wv_redirect_target(&ret, e->url, &status)) && !wv_scheme_callback(wv, target)) {
        free(target);   /* another scheme (https://...): the page gets the redirect answer as it is */
        target = NULL;
    }
    if (!wv->handle) {
        /* closed by the callback */
    } else if (EG(exception) || !ok) {
        wv_reply_simple(wv, e->id, 500, "scheme callback failed");
    } else if (target) {
        /* Engines do not follow redirects of custom schemes (WKWebView hands fetch the raw 302 and shows a
         * blank page after a redirected form POST), so vio does - cookies of the redirect first. A page
         * load gets a page that replaces itself with the target (the address is right afterwards, the
         * target is loaded with GET as after 303); a fetch gets the target's answer through the handler
         * again, as GET after 301/302 POST and 303, as is after 307/308 (Response.url stays the first URL). */
        zval *rh = zend_hash_str_find(Z_ARRVAL(ret), "headers", sizeof("headers") - 1);
        if (wv_apply_set_cookies(wv, rh, e->url)) wv_jar_publish(wv, 1);
        if (e->navigation) {
            smart_str js = {0};
            smart_str_appends(&js, "<!doctype html><script>location.replace(");
            zval t;
            ZVAL_STRING(&t, target);
            php_json_encode(&js, &t, PHP_JSON_UNESCAPED_SLASHES | PHP_JSON_HEX_TAG);
            zval_ptr_dtor(&t);
            smart_str_appends(&js, ")</script>");
            smart_str_0(&js);
            vio_webview_header h = {"Content-Type", "text/html; charset=utf-8"};
            vio_plat()->webview_reply_head(wv->handle, e->id, 200, &h, 1);
            vio_plat()->webview_reply_data(wv->handle, e->id, ZSTR_VAL(js.s), ZSTR_LEN(js.s));
            vio_plat()->webview_reply_end(wv->handle, e->id);
            smart_str_free(&js);
        } else if (depth >= WV_MAX_REDIRECTS) {
            if (vio_plat()->webview_reply_fail) vio_plat()->webview_reply_fail(wv->handle, e->id);
            else wv_reply_simple(wv, e->id, 508, "too many redirects");
        } else {
            int to_get = status == 303 || ((status == 301 || status == 302) && strcmp(e->method, "POST") == 0);
            vio_wv_event next = *e;
            next.url = target;
            next.method = to_get ? (char *)"GET" : e->method;
            if (to_get) next.body_len = 0;
            wv_deliver_request_depth(&next, depth + 1);
        }
    } else {
        wv_reply(wv, e->id, &ret, e->url);
    }
    free(target);
    zval_ptr_dtor(&ret);
    OBJ_RELEASE(&wv->std);
}

static void wv_deliver_request(vio_wv_event *e)
{
    wv_deliver_request_depth(e, 0);
}

static void wv_drop_stream(vio_webview_object *wv, uint64_t id)
{
    for (vio_wv_stream **pp = &wv->streams; *pp; pp = &(*pp)->next) {
        if ((*pp)->id == id) {
            vio_wv_stream *s = *pp;
            *pp = s->next;
            wv_stream_free(s);
            return;
        }
    }
}

static void wv_deliver(vio_wv_event *e)
{
    vio_webview_object *wv = e->wv;
    switch (e->kind) {
    case WV_EV_REQUEST:
        wv_deliver_request(e);
        break;
    case WV_EV_CANCEL:
        wv_drop_stream(wv, e->id);
        break;
    case WV_EV_MESSAGE:
        if (e->body_len >= 8 && memcmp(e->body, "\x01" "cookie:", 8) == 0) {   /* document.cookie = ... */
            char *msg = (char *)malloc(e->body_len - 7);
            memcpy(msg, e->body + 8, e->body_len - 8);
            msg[e->body_len - 8] = '\0';
            char *nl = strchr(msg, '\n');
            if (nl) {
                *nl = '\0';
                char host[256], path[1024];
                wv_url_parts(msg, host, sizeof(host), path, sizeof(path));
                if (wv_jar_set(wv, host, path, nl + 1, 1)) wv_jar_publish(wv, 0);
            }
            free(msg);
            break;
        }
        if (e->body_len && e->body[0] == '\x01') break;   /* other vio-internal messages */
        if (Z_TYPE(wv->on_message) != IS_UNDEF) {
            zval arg, ret;
            ZVAL_STRINGL(&arg, e->body, e->body_len);
            ZVAL_UNDEF(&ret);
            GC_ADDREF(&wv->std);
            call_user_function(NULL, NULL, &wv->on_message, &ret, 1, &arg);
            zval_ptr_dtor(&arg);
            zval_ptr_dtor(&ret);
            OBJ_RELEASE(&wv->std);
        }
        break;
    case WV_EV_NAV: {
        const char *type = e->nav == VIO_WEBVIEW_EV_NAVIGATED ? "navigated"
                         : e->nav == VIO_WEBVIEW_EV_TITLE ? "title" : "new_window";
        if (Z_TYPE(wv->on_event) == IS_UNDEF) {
            /* without a handler a link meant for a new window opens in the system browser */
            if (e->nav == VIO_WEBVIEW_EV_NEW_WINDOW && vio_plat()->open_url &&
                (strncmp(e->text, "https://", 8) == 0 || strncmp(e->text, "http://", 7) == 0))
                vio_plat()->open_url(e->text);
            break;
        }
        zval args[2], ret;
        ZVAL_STRING(&args[0], type);
        array_init(&args[1]);
        add_assoc_string(&args[1], e->nav == VIO_WEBVIEW_EV_TITLE ? "title" : "url", e->text);
        ZVAL_UNDEF(&ret);
        GC_ADDREF(&wv->std);
        call_user_function(NULL, NULL, &wv->on_event, &ret, 2, args);
        zval_ptr_dtor(&args[0]);
        zval_ptr_dtor(&args[1]);
        zval_ptr_dtor(&ret);
        OBJ_RELEASE(&wv->std);
        break;
    }
    }
}

/* One chunk of every open stream; 1 = some are left. */
static int wv_pump_streams(void)
{
    int left = 0;
    for (vio_webview_object *wv = vio_webviews; wv && !EG(exception); wv = wv->next) {
        vio_wv_stream **pp = &wv->streams;
        while (*pp && !EG(exception)) {
            vio_wv_stream *s = *pp;
            zend_object_iterator *it = s->it;
            if (!s->started) {
                s->started = 1;
                if (it->funcs->rewind) it->funcs->rewind(it);
            } else {
                it->funcs->move_forward(it);
            }
            int done = EG(exception) || it->funcs->valid(it) != SUCCESS;
            if (!done) {
                zval *cur = it->funcs->get_current_data(it);
                if (EG(exception) || !cur) {
                    done = 1;
                } else {
                    zend_string *str = zval_get_string(cur);
                    if (ZSTR_LEN(str)) vio_plat()->webview_reply_data(wv->handle, s->id, ZSTR_VAL(str), ZSTR_LEN(str));
                    zend_string_release(str);
                }
            }
            if (done) {
                vio_plat()->webview_reply_end(wv->handle, s->id);
                *pp = s->next;
                wv_stream_free(s);
                continue;
            }
            left = 1;
            pp = &s->next;
        }
    }
    return left;
}

void vio_webview_dispatch(void)
{
    if (!vio_webviews && !wv_queue_head) return;
    /* only what is queued now: callbacks may make engines queue more. Every
     * webview in the batch is held until the end - a callback may drop its last reference. */
    vio_wv_event *e = wv_queue_head;
    wv_queue_head = wv_queue_tail = NULL;
    size_t n = 0;
    for (vio_wv_event *x = e; x; x = x->next) n++;
    zend_object **held = n ? (zend_object **)emalloc(n * sizeof(zend_object *)) : NULL;
    n = 0;
    for (vio_wv_event *x = e; x; x = x->next) { held[n++] = &x->wv->std; GC_ADDREF(&x->wv->std); }

    while (e) {
        vio_wv_event *next = e->next;
        if (EG(exception)) {   /* an exception stops delivery; the rest waits for the next poll */
            vio_wv_event *last = e;
            while (last->next) last = last->next;
            last->next = wv_queue_head;
            if (!wv_queue_head) wv_queue_tail = last;
            wv_queue_head = e;
            break;
        }
        e->next = NULL;
        if (e->wv->handle) wv_deliver(e);
        wv_event_free(e);
        e = next;
    }
    for (size_t i = 0; i < n; i++) OBJ_RELEASE(held[i]);
    if (held) efree(held);
    if (!EG(exception) && wv_pump_streams()) vio_plat()->wake();
}

/* ── PHP functions ────────────────────────────────────────────────── */

static vio_webview_object *wv_arg(zval *zv, uint32_t arg)
{
    vio_webview_object *wv = Z_VIO_WEBVIEW_P(zv);
    if (!wv->handle) {
        zend_argument_value_error(arg, "is a closed webview");
        return NULL;
    }
    return wv;
}

static int wv_check_text(const char *s, size_t len, uint32_t arg, const char *what)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) {
            zend_argument_value_error(arg, "%s must not contain control characters", what);
            return 0;
        }
    }
    return 1;
}

static int wv_valid_scheme(zend_string *name)
{
    static const char *reserved[] = {"http", "https", "file", "ftp", "about", "data", "blob", "javascript",
                                     "ws", "wss", "mailto", "view-source", NULL};
    size_t n = ZSTR_LEN(name);
    const char *s = ZSTR_VAL(name);
    if (n == 0 || n > 32 || s[0] < 'a' || s[0] > 'z') return 0;
    for (size_t i = 1; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) return 0;
    }
    for (int i = 0; reserved[i]; i++) if (strcmp(s, reserved[i]) == 0) return 0;
    return 1;
}

/* bounds [x, y, width, height]: 1 = parsed, 0 = exception thrown */
static int wv_parse_bounds(zval *v, uint32_t arg, int out[4])
{
    if (Z_TYPE_P(v) != IS_ARRAY || zend_hash_num_elements(Z_ARRVAL_P(v)) != 4) {
        zend_argument_value_error(arg, "bounds must be [x, y, width, height]");
        return 0;
    }
    int i = 0;
    zval *e;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(v), e) {
        if (Z_TYPE_P(e) != IS_LONG && Z_TYPE_P(e) != IS_DOUBLE) {
            zend_argument_value_error(arg, "bounds must be [x, y, width, height] numbers");
            return 0;
        }
        out[i++] = (int)zval_get_double(e);
    } ZEND_HASH_FOREACH_END();
    if (out[2] < 1 || out[3] < 1 || out[2] > 16384 || out[3] > 16384) {
        zend_argument_value_error(arg, "bounds width and height must be between 1 and 16384");
        return 0;
    }
    return 1;
}

/* vio_webview(VioContext|VioWindow $window, array $options = []): VioWebview|false */
ZEND_FUNCTION(vio_webview)
{
    zval *target;
    HashTable *opts = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_OBJECT(target)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_HT(opts)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = NULL;
    if (Z_OBJCE_P(target) == vio_context_ce) {
        ctx = Z_VIO_CONTEXT_P(target);
    } else if (Z_OBJCE_P(target) == vio_window_ce) {
        vio_window_object *w = Z_VIO_WINDOW_P(target);
        if (!w->owner) { zend_argument_value_error(1, "is a closed window"); RETURN_THROWS(); }
        ctx = &w->view;
    } else {
        zend_argument_type_error(1, "must be of type VioContext|VioWindow, %s given", zend_zval_value_name(target));
        RETURN_THROWS();
    }

    vio_webview_desc d;
    memset(&d, 0, sizeof(d));
    zval schemes;
    ZVAL_UNDEF(&schemes);
    const char *names[16];
    if (opts) {
        zval *v;
        if ((v = zend_hash_str_find(opts, "url", sizeof("url") - 1)) && Z_TYPE_P(v) != IS_NULL) {
            if (Z_TYPE_P(v) != IS_STRING) { zend_argument_type_error(2, "option \"url\" must be a string"); RETURN_THROWS(); }
            if (!wv_check_text(Z_STRVAL_P(v), Z_STRLEN_P(v), 2, "option \"url\"")) RETURN_THROWS();
            d.url = Z_STRVAL_P(v);
        }
        if ((v = zend_hash_str_find(opts, "bounds", sizeof("bounds") - 1)) && Z_TYPE_P(v) != IS_NULL) {
            int b[4];
            if (!wv_parse_bounds(v, 2, b)) RETURN_THROWS();
            d.has_bounds = 1;
            d.x = b[0]; d.y = b[1]; d.width = b[2]; d.height = b[3];
        }
        if ((v = zend_hash_str_find(opts, "devtools", sizeof("devtools") - 1))) d.devtools = zend_is_true(v);
        if ((v = zend_hash_str_find(opts, "transparent", sizeof("transparent") - 1))) d.transparent = zend_is_true(v);
        if ((v = zend_hash_str_find(opts, "schemes", sizeof("schemes") - 1)) && Z_TYPE_P(v) != IS_NULL) {
            if (Z_TYPE_P(v) != IS_ARRAY) { zend_argument_type_error(2, "option \"schemes\" must be an array of scheme => callable"); RETURN_THROWS(); }
            if (zend_hash_num_elements(Z_ARRVAL_P(v)) > 16) { zend_argument_value_error(2, "option \"schemes\" takes at most 16 schemes"); RETURN_THROWS(); }
            zend_string *name;
            zval *cb;
            ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(v), name, cb) {
                if (!name || !wv_valid_scheme(name)) {
                    zend_argument_value_error(2, "option \"schemes\": \"%s\" is not a usable custom scheme (lower case, a letter first, not http/https/file/...)",
                                              name ? ZSTR_VAL(name) : "(integer key)");
                    RETURN_THROWS();
                }
                if (!zend_is_callable(cb, 0, NULL)) {
                    zend_argument_type_error(2, "option \"schemes\": the handler of \"%s\" must be a valid callback", ZSTR_VAL(name));
                    RETURN_THROWS();
                }
                names[d.scheme_count++] = ZSTR_VAL(name);
            } ZEND_HASH_FOREACH_END();
            ZVAL_COPY(&schemes, v);
        }
    }
    d.schemes = names;

    if (!vio_webview_available() || !ctx->initialized || !ctx->window) {
        zval_ptr_dtor(&schemes);
        RETURN_FALSE;
    }

    object_init_ex(return_value, vio_webview_ce);
    vio_webview_object *wv = Z_VIO_WEBVIEW_P(return_value);
    ZVAL_COPY_VALUE(&wv->schemes, &schemes);
    if (Z_TYPE(wv->schemes) == IS_UNDEF) array_init(&wv->schemes);
    d.user = wv;
    char *script = wv_shim_script(wv);
    d.script = script;
    wv->handle = vio_plat()->webview_create(ctx->window, &d);
    free(script);
    if (!wv->handle) {
        zval_ptr_dtor(return_value);
        RETURN_FALSE;
    }
    wv->window = ctx->window;
    wv->owner = Z_OBJ_P(target);
    GC_ADDREF(wv->owner);
    wv->next = vio_webviews;
    vio_webviews = wv;
}

ZEND_FUNCTION(vio_webview_navigate)
{
    zval *zv;
    zend_string *url;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(zv, vio_webview_ce)
        Z_PARAM_STR(url)
    ZEND_PARSE_PARAMETERS_END();
    vio_webview_object *wv = wv_arg(zv, 1);
    if (!wv) RETURN_THROWS();
    if (!wv_check_text(ZSTR_VAL(url), ZSTR_LEN(url), 2, "the URL")) RETURN_THROWS();
    RETURN_BOOL(vio_plat()->webview_navigate(wv->handle, ZSTR_VAL(url)));
}

ZEND_FUNCTION(vio_webview_eval)
{
    zval *zv;
    zend_string *js;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(zv, vio_webview_ce)
        Z_PARAM_STR(js)
    ZEND_PARSE_PARAMETERS_END();
    vio_webview_object *wv = wv_arg(zv, 1);
    if (!wv) RETURN_THROWS();
    if (memchr(ZSTR_VAL(js), '\0', ZSTR_LEN(js))) { zend_argument_value_error(2, "must not contain NUL bytes"); RETURN_THROWS(); }
    RETURN_BOOL(vio_plat()->webview_eval(wv->handle, ZSTR_VAL(js)));
}

ZEND_FUNCTION(vio_webview_bounds)
{
    zval *zv, *bounds;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(zv, vio_webview_ce)
        Z_PARAM_ZVAL(bounds)
    ZEND_PARSE_PARAMETERS_END();
    vio_webview_object *wv = wv_arg(zv, 1);
    if (!wv) RETURN_THROWS();
    if (Z_TYPE_P(bounds) == IS_NULL) RETURN_BOOL(vio_plat()->webview_set_bounds(wv->handle, 0, 0, 0, 0, 0));
    int b[4];
    if (!wv_parse_bounds(bounds, 2, b)) RETURN_THROWS();
    RETURN_BOOL(vio_plat()->webview_set_bounds(wv->handle, 1, b[0], b[1], b[2], b[3]));
}

static void wv_set_callback(INTERNAL_FUNCTION_PARAMETERS, int which)
{
    zval *zv, *cb;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(zv, vio_webview_ce)
        Z_PARAM_ZVAL(cb)
    ZEND_PARSE_PARAMETERS_END();
    vio_webview_object *wv = wv_arg(zv, 1);
    if (!wv) RETURN_THROWS();
    if (Z_TYPE_P(cb) != IS_NULL && !zend_is_callable(cb, 0, NULL)) {
        zend_argument_type_error(2, "must be a valid callback or null");
        RETURN_THROWS();
    }
    zval *slot = which ? &wv->on_event : &wv->on_message;
    zval_ptr_dtor(slot);
    ZVAL_UNDEF(slot);
    if (Z_TYPE_P(cb) != IS_NULL) ZVAL_COPY(slot, cb);
}

ZEND_FUNCTION(vio_webview_on_message) { wv_set_callback(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0); }
ZEND_FUNCTION(vio_webview_on_event)   { wv_set_callback(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1); }

ZEND_FUNCTION(vio_webview_destroy)
{
    zval *zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(zv, vio_webview_ce)
    ZEND_PARSE_PARAMETERS_END();
    vio_webview_close(Z_VIO_WEBVIEW_P(zv));
}
