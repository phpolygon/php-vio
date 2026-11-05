/*
 * php-vio - Linux desktop services over the D-Bus session bus
 * (DESKTOP-APP-PLAN Phase 2c): notifications (org.freedesktop.Notifications),
 * the file chooser portal (org.freedesktop.portal.FileChooser) and launcher
 * progress (com.canonical.Unity.LauncherEntry, read by KDE, Dash to Dock,
 * Plank). Shared by the X11, Wayland and GLFW platforms.
 *
 * libdbus-1 is loaded at run time (as libGL / libEGL are), so the module
 * needs no D-Bus headers or library to build. Without it, without a session
 * bus or without the service, the feature is unavailable: false, no warning.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if defined(__linux__)

#include "php.h"
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "vio_desktop_linux.h"
#include "vio_uri_list.h"

/* ── libdbus, the parts used here ─────────────────────────────────── */

typedef struct DBusConnection DBusConnection;
typedef struct DBusMessage DBusMessage;
typedef struct { const char *name, *message; unsigned int bits; void *pad; } DBusError;   /* public layout */
typedef struct { void *p[2]; unsigned int u; int i[8]; int pad; void *pp[2]; } DBusMessageIter;   /* >= libdbus' size */

#define DB_BUS_SESSION 0
#define DB_TYPE_INVALID 0
#define DB_TYPE_BYTE    'y'
#define DB_TYPE_BOOLEAN 'b'
#define DB_TYPE_INT32   'i'
#define DB_TYPE_UINT32  'u'
#define DB_TYPE_DOUBLE  'd'
#define DB_TYPE_STRING  's'
#define DB_TYPE_OBJPATH 'o'
#define DB_TYPE_ARRAY   'a'
#define DB_TYPE_VARIANT 'v'
#define DB_TYPE_STRUCT  'r'
#define DB_TYPE_DICT    'e'

static struct {
    int tried, ok;
    void *lib;
    void (*error_init)(DBusError *);
    void (*error_free)(DBusError *);
    DBusConnection *(*bus_get_private)(int, DBusError *);
    void (*set_exit_on_disconnect)(DBusConnection *, unsigned int);
    const char *(*bus_get_unique_name)(DBusConnection *);
    unsigned int (*bus_name_has_owner)(DBusConnection *, const char *, DBusError *);
    void (*bus_add_match)(DBusConnection *, const char *, DBusError *);
    void (*bus_remove_match)(DBusConnection *, const char *, DBusError *);
    DBusMessage *(*new_method_call)(const char *, const char *, const char *, const char *);
    DBusMessage *(*new_signal)(const char *, const char *, const char *);
    void (*unref)(DBusMessage *);
    void (*iter_init_append)(DBusMessage *, DBusMessageIter *);
    unsigned int (*append_basic)(DBusMessageIter *, int, const void *);
    unsigned int (*open_container)(DBusMessageIter *, int, const char *, DBusMessageIter *);
    unsigned int (*close_container)(DBusMessageIter *, DBusMessageIter *);
    unsigned int (*iter_init)(DBusMessage *, DBusMessageIter *);
    int (*iter_type)(DBusMessageIter *);
    void (*iter_basic)(DBusMessageIter *, void *);
    void (*iter_recurse)(DBusMessageIter *, DBusMessageIter *);
    unsigned int (*iter_next)(DBusMessageIter *);
    DBusMessage *(*send_block)(DBusConnection *, DBusMessage *, int, DBusError *);
    unsigned int (*send)(DBusConnection *, DBusMessage *, uint32_t *);
    void (*flush)(DBusConnection *);
    unsigned int (*read_write)(DBusConnection *, int);
    DBusMessage *(*pop)(DBusConnection *);
    unsigned int (*is_signal)(DBusMessage *, const char *, const char *);
    const char *(*get_path)(DBusMessage *);
    int (*request_name)(DBusConnection *, const char *, unsigned int, DBusError *);
    int (*release_name)(DBusConnection *, const char *, DBusError *);
    DBusMessage *(*new_return)(DBusMessage *);
    DBusMessage *(*new_error)(DBusMessage *, const char *, const char *);
    int (*get_type)(DBusMessage *);
    const char *(*get_interface)(DBusMessage *);
    const char *(*get_member)(DBusMessage *);
    unsigned int (*get_unix_fd)(DBusConnection *, int *);
} db;

static DBusConnection *db_conn;
void vio_linux_handle(DBusMessage *msg);

static int db_load(void)
{
    if (db.tried) return db.ok;
    db.tried = 1;
    db.lib = dlopen("libdbus-1.so.3", RTLD_LAZY | RTLD_LOCAL);
    if (!db.lib) return 0;
#define DBL(f, n) if (!(*(void **)&db.f = dlsym(db.lib, n))) return 0
    DBL(error_init, "dbus_error_init"); DBL(error_free, "dbus_error_free");
    DBL(bus_get_private, "dbus_bus_get_private"); DBL(set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect");
    DBL(bus_get_unique_name, "dbus_bus_get_unique_name"); DBL(bus_name_has_owner, "dbus_bus_name_has_owner");
    DBL(bus_add_match, "dbus_bus_add_match"); DBL(bus_remove_match, "dbus_bus_remove_match");
    DBL(new_method_call, "dbus_message_new_method_call"); DBL(new_signal, "dbus_message_new_signal");
    DBL(unref, "dbus_message_unref"); DBL(iter_init_append, "dbus_message_iter_init_append");
    DBL(append_basic, "dbus_message_iter_append_basic"); DBL(open_container, "dbus_message_iter_open_container");
    DBL(close_container, "dbus_message_iter_close_container"); DBL(iter_init, "dbus_message_iter_init");
    DBL(iter_type, "dbus_message_iter_get_arg_type"); DBL(iter_basic, "dbus_message_iter_get_basic");
    DBL(iter_recurse, "dbus_message_iter_recurse"); DBL(iter_next, "dbus_message_iter_next");
    DBL(send_block, "dbus_connection_send_with_reply_and_block"); DBL(send, "dbus_connection_send");
    DBL(flush, "dbus_connection_flush"); DBL(read_write, "dbus_connection_read_write");
    DBL(pop, "dbus_connection_pop_message"); DBL(is_signal, "dbus_message_is_signal");
    DBL(get_path, "dbus_message_get_path");
    DBL(request_name, "dbus_bus_request_name"); DBL(release_name, "dbus_bus_release_name");
    DBL(new_return, "dbus_message_new_method_return"); DBL(new_error, "dbus_message_new_error");
    DBL(get_type, "dbus_message_get_type"); DBL(get_interface, "dbus_message_get_interface");
    DBL(get_member, "dbus_message_get_member"); DBL(get_unix_fd, "dbus_connection_get_unix_fd");
#undef DBL
    db.ok = 1;
    return 1;
}

/* The session bus (a private connection, kept for the process). */
static DBusConnection *db_session(void)
{
    if (db_conn) return db_conn;
    if (!db_load() || !getenv("DBUS_SESSION_BUS_ADDRESS")) return NULL;
    DBusError err;
    db.error_init(&err);
    db_conn = db.bus_get_private(DB_BUS_SESSION, &err);
    if (err.name) db.error_free(&err);
    if (db_conn) db.set_exit_on_disconnect(db_conn, 0);
    return db_conn;
}

static int db_has_owner(const char *name)
{
    DBusConnection *c = db_session();
    if (!c) return 0;
    DBusError err;
    db.error_init(&err);
    int r = (int)db.bus_name_has_owner(c, name, &err);
    if (err.name) { db.error_free(&err); r = 0; }
    return r;
}

/* a{sv} entry helpers */
static void db_dict_entry(DBusMessageIter *dict, const char *key, int type, const char *sig, const void *value)
{
    DBusMessageIter e, v;
    db.open_container(dict, DB_TYPE_DICT, NULL, &e);
    db.append_basic(&e, DB_TYPE_STRING, &key);
    db.open_container(&e, DB_TYPE_VARIANT, sig, &v);
    db.append_basic(&v, type, value);
    db.close_container(&e, &v);
    db.close_container(dict, &e);
}

/* ── Availability ─────────────────────────────────────────────────── */

static const char *vio_app_id(void)
{
    const char *id = zend_ini_string_ex("vio.app_id", sizeof("vio.app_id") - 1, 0, NULL);
    if (!id || !*id) id = getenv("VIO_APP_ID");
    return id && *id ? id : NULL;
}

int vio_linux_available(int feature)
{
    switch (feature) {
        case VIO_PLATFORM_FEATURE_FILE_DIALOG:   return db_has_owner("org.freedesktop.portal.Desktop");
        case VIO_PLATFORM_FEATURE_NOTIFICATIONS: return db_has_owner("org.freedesktop.Notifications");
        case VIO_PLATFORM_FEATURE_PROGRESS:      return db_session() && vio_app_id();
        case VIO_PLATFORM_FEATURE_TRAY:          return db_has_owner("org.kde.StatusNotifierWatcher");
    }
    return 0;
}

/* ── Notifications ────────────────────────────────────────────────── */

/* the server's id per vio id, so a notification with the same id replaces */
#define VIO_NOTIFY_IDS 64
static struct { char key[64]; uint32_t id; } vio_notify_ids[VIO_NOTIFY_IDS];

int vio_linux_notify(const char *title, const char *body, const char *icon, const char *key)
{
    DBusConnection *c = db_session();
    if (!c || !db_has_owner("org.freedesktop.Notifications")) return 0;
    uint32_t replaces = 0;
    int slot = -1;
    if (key) {
        for (int i = 0; i < VIO_NOTIFY_IDS && slot < 0; i++) if (!strcmp(vio_notify_ids[i].key, key)) slot = i;
        for (int i = 0; i < VIO_NOTIFY_IDS && slot < 0; i++) if (!vio_notify_ids[i].key[0]) slot = i;
        if (slot >= 0) replaces = vio_notify_ids[slot].id;
    }
    DBusMessage *m = db.new_method_call("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                        "org.freedesktop.Notifications", "Notify");
    if (!m) return 0;
    const char *app = vio_app_id() ? vio_app_id() : "php-vio";
    const char *ic = icon ? icon : "";
    int32_t expire = -1;
    DBusMessageIter it, arr;
    db.iter_init_append(m, &it);
    db.append_basic(&it, DB_TYPE_STRING, &app);
    db.append_basic(&it, DB_TYPE_UINT32, &replaces);
    db.append_basic(&it, DB_TYPE_STRING, &ic);
    db.append_basic(&it, DB_TYPE_STRING, &title);
    db.append_basic(&it, DB_TYPE_STRING, &body);
    db.open_container(&it, DB_TYPE_ARRAY, "s", &arr);     /* actions */
    db.close_container(&it, &arr);
    db.open_container(&it, DB_TYPE_ARRAY, "{sv}", &arr);  /* hints */
    db.close_container(&it, &arr);
    db.append_basic(&it, DB_TYPE_INT32, &expire);
    DBusError err;
    db.error_init(&err);
    DBusMessage *r = db.send_block(c, m, 2000, &err);
    db.unref(m);
    if (err.name) db.error_free(&err);
    if (!r) return 0;
    uint32_t id = 0;
    if (db.iter_init(r, &it) && db.iter_type(&it) == DB_TYPE_UINT32) db.iter_basic(&it, &id);
    db.unref(r);
    if (slot >= 0) {
        snprintf(vio_notify_ids[slot].key, sizeof(vio_notify_ids[slot].key), "%s", key);
        vio_notify_ids[slot].id = id;
    }
    return 1;
}

/* ── Launcher progress ────────────────────────────────────────────── */

int vio_linux_set_progress(double value, int state)
{
    DBusConnection *c = db_session();
    const char *app = vio_app_id();
    if (!c || !app) return 0;
    char uri[256];
    snprintf(uri, sizeof(uri), "application://%s.desktop", app);
    const char *u = uri;
    DBusMessage *m = db.new_signal("/com/canonical/unity/launcherentry/vio", "com.canonical.Unity.LauncherEntry", "Update");
    if (!m) return 0;
    DBusMessageIter it, dict;
    db.iter_init_append(m, &it);
    db.append_basic(&it, DB_TYPE_STRING, &u);
    db.open_container(&it, DB_TYPE_ARRAY, "{sv}", &dict);
    /* no colours or spinner in the protocol: paused / error / indeterminate show the bar, error also asks for attention */
    unsigned int visible = value >= 0.0 || state == VIO_PLATFORM_PROGRESS_INDETERMINATE;
    double v = value >= 0.0 ? value : 0.0;
    unsigned int urgent = state == VIO_PLATFORM_PROGRESS_ERROR;
    db_dict_entry(&dict, "progress", DB_TYPE_DOUBLE, "d", &v);
    db_dict_entry(&dict, "progress-visible", DB_TYPE_BOOLEAN, "b", &visible);
    db_dict_entry(&dict, "urgent", DB_TYPE_BOOLEAN, "b", &urgent);
    db.close_container(&it, &dict);
    int ok = (int)db.send(c, m, NULL);
    db.flush(c);
    db.unref(m);
    return ok;
}

/* ── File chooser portal ──────────────────────────────────────────── */

/* Request handle path the portal will use for our token (spec: sender
 * without ':' and with '.' -> '_'), subscribed to before the call. */
static void db_request_path(char *out, size_t size, const char *token)
{
    const char *u = db.bus_get_unique_name(db_conn);
    char sender[128];
    size_t k = 0;
    for (const char *p = u && *u == ':' ? u + 1 : (u ? u : ""); *p && k + 1 < sizeof(sender); p++) sender[k++] = *p == '.' ? '_' : *p;
    sender[k] = 0;
    snprintf(out, size, "/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
}

static void db_append_bytes(DBusMessageIter *dict, const char *key, const char *path)
{
    DBusMessageIter e, v, a;
    db.open_container(dict, DB_TYPE_DICT, NULL, &e);
    db.append_basic(&e, DB_TYPE_STRING, &key);
    db.open_container(&e, DB_TYPE_VARIANT, "ay", &v);
    db.open_container(&v, DB_TYPE_ARRAY, "y", &a);
    for (const char *p = path; ; p++) { unsigned char b = (unsigned char)*p; db.append_basic(&a, DB_TYPE_BYTE, &b); if (!*p) break; }   /* NUL-terminated */
    db.close_container(&v, &a);
    db.close_container(&e, &v);
    db.close_container(dict, &e);
}

/* filters: a(sa(us)) - (name, [(0 = glob, "*.png"), ...]) */
static void db_append_filters(DBusMessageIter *dict, const vio_file_dialog_desc *d)
{
    const char *key = "filters";
    DBusMessageIter e, v, list, f, pats, pat;
    db.open_container(dict, DB_TYPE_DICT, NULL, &e);
    db.append_basic(&e, DB_TYPE_STRING, &key);
    db.open_container(&e, DB_TYPE_VARIANT, "a(sa(us))", &v);
    db.open_container(&v, DB_TYPE_ARRAY, "(sa(us))", &list);
    for (int i = 0; i < d->filter_count; i++) {
        db.open_container(&list, DB_TYPE_STRUCT, NULL, &f);
        db.append_basic(&f, DB_TYPE_STRING, &d->filters[i].name);
        db.open_container(&f, DB_TYPE_ARRAY, "(us)", &pats);
        for (int k = 0; k < d->filters[i].count; k++) {
            char glob[96];
            snprintf(glob, sizeof(glob), "*.%s", d->filters[i].extensions[k]);
            const char *g = glob;
            uint32_t kind = 0;
            db.open_container(&pats, DB_TYPE_STRUCT, NULL, &pat);
            db.append_basic(&pat, DB_TYPE_UINT32, &kind);
            db.append_basic(&pat, DB_TYPE_STRING, &g);
            db.close_container(&pats, &pat);
        }
        db.close_container(&f, &pats);
        db.close_container(&list, &f);
    }
    db.close_container(&v, &list);
    db.close_container(&e, &v);
    db.close_container(dict, &e);
}

int vio_linux_file_dialog(const vio_file_dialog_desc *d, char ***out_paths, int *out_count)
{
    *out_paths = NULL;
    *out_count = 0;
    DBusConnection *c = db_session();
    if (!c || !db_has_owner("org.freedesktop.portal.Desktop")) return 0;

    static unsigned counter;
    char token[64], path[256], rule[512];
    snprintf(token, sizeof(token), "vio%d_%u", (int)getpid(), ++counter);
    db_request_path(path, sizeof(path), token);
    snprintf(rule, sizeof(rule), "type='signal',interface='org.freedesktop.portal.Request',member='Response',path='%s'", path);
    DBusError err;
    db.error_init(&err);
    db.bus_add_match(c, rule, &err);
    if (err.name) { db.error_free(&err); return 0; }

    DBusMessage *m = db.new_method_call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                        "org.freedesktop.portal.FileChooser", d->mode == VIO_PLATFORM_DIALOG_SAVE ? "SaveFile" : "OpenFile");
    if (!m) return 0;
    const char *parent = "";
    const char *title = d->title ? d->title : (d->mode == VIO_PLATFORM_DIALOG_SAVE ? "Save" : d->mode == VIO_PLATFORM_DIALOG_FOLDER ? "Choose Folder" : "Open");
    const char *tok = token;
    unsigned int yes = 1;
    DBusMessageIter it, dict;
    db.iter_init_append(m, &it);
    db.append_basic(&it, DB_TYPE_STRING, &parent);
    db.append_basic(&it, DB_TYPE_STRING, &title);
    db.open_container(&it, DB_TYPE_ARRAY, "{sv}", &dict);
    db_dict_entry(&dict, "handle_token", DB_TYPE_STRING, "s", &tok);
    if (d->multiple) db_dict_entry(&dict, "multiple", DB_TYPE_BOOLEAN, "b", &yes);
    if (d->mode == VIO_PLATFORM_DIALOG_FOLDER) db_dict_entry(&dict, "directory", DB_TYPE_BOOLEAN, "b", &yes);
    if (d->filter_count > 0 && d->mode != VIO_PLATFORM_DIALOG_FOLDER) db_append_filters(&dict, d);
    if (d->default_path) {
        /* a folder starts there; for save a file splits into folder + name */
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", d->default_path);
        const char *name = NULL;
        if (access(dir, F_OK) != 0 || d->mode == VIO_PLATFORM_DIALOG_SAVE) {
            char *slash = strrchr(dir, '/');
            if (slash && access(dir, F_OK) != 0) { *slash = 0; name = slash + 1; }
        }
        if (d->mode == VIO_PLATFORM_DIALOG_SAVE && name && *name) db_dict_entry(&dict, "current_name", DB_TYPE_STRING, "s", &name);
        if (*dir) db_append_bytes(&dict, "current_folder", dir);
    }
    db.close_container(&it, &dict);
    db.error_init(&err);
    DBusMessage *r = db.send_block(c, m, 5000, &err);
    db.unref(m);
    if (err.name) db.error_free(&err);
    if (!r) { db.error_init(&err); db.bus_remove_match(c, rule, &err); if (err.name) db.error_free(&err); return 0; }
    db.unref(r);   /* the handle is the path computed above (portal >= 0.9) */

    /* wait for Response(u code, a{sv} results); the dialog is modal for us,
     * other calls (the tray's) are answered meanwhile */
    int ok = 0, done = 0;
    while (!done && db.read_write(c, 200)) {
        DBusMessage *msg;
        while (!done && (msg = db.pop(c))) {
            if (db.is_signal(msg, "org.freedesktop.portal.Request", "Response") && db.get_path(msg) && !strcmp(db.get_path(msg), path)) {
                done = 1;
                ok = 1;
                DBusMessageIter a, dct, ent, var, uris;
                uint32_t code = 2;
                if (db.iter_init(msg, &a) && db.iter_type(&a) == DB_TYPE_UINT32) db.iter_basic(&a, &code);
                if (code == 0 && db.iter_next(&a) && db.iter_type(&a) == DB_TYPE_ARRAY) {
                    db.iter_recurse(&a, &dct);
                    for (; db.iter_type(&dct) == DB_TYPE_DICT; db.iter_next(&dct)) {
                        const char *key = NULL;
                        db.iter_recurse(&dct, &ent);
                        db.iter_basic(&ent, &key);
                        if (!key || strcmp(key, "uris") || !db.iter_next(&ent)) continue;
                        db.iter_recurse(&ent, &var);
                        if (db.iter_type(&var) != DB_TYPE_ARRAY) continue;
                        db.iter_recurse(&var, &uris);
                        /* join to a uri-list and reuse the drop parser */
                        size_t len = 0, cap = 1024;
                        char *list = (char *)malloc(cap);
                        for (; list && db.iter_type(&uris) == DB_TYPE_STRING; db.iter_next(&uris)) {
                            const char *u = NULL;
                            db.iter_basic(&uris, &u);
                            size_t l = u ? strlen(u) : 0;
                            if (len + l + 3 > cap) { char *g = (char *)realloc(list, cap = (len + l + 3) * 2); if (!g) { free(list); list = NULL; break; } list = g; }
                            memcpy(list + len, u, l);
                            len += l;
                            list[len++] = '\r';
                            list[len++] = '\n';
                        }
                        if (list) { *out_count = vio_uri_list_paths(list, len, out_paths); free(list); }
                    }
                }
            }
            else vio_linux_handle(msg);
            db.unref(msg);
        }
    }
    db.error_init(&err);
    db.bus_remove_match(c, rule, &err);
    if (err.name) db.error_free(&err);
    return ok;
}

/* ── Tray: StatusNotifierItem + com.canonical.dbusmenu ─────────────── */

/* A copy of the menu tree as dbusmenu nodes: id = index (0 = root). */
typedef struct { int parent, tag, separator, enabled, checked; char *label; } sni_node;
static struct {
    int        on;
    char       service[96];
    char      *title;
    unsigned char *pixmap;   /* ARGB32 big-endian */
    int        pw, ph;
    sni_node  *nodes;
    int        node_n;
    uint32_t   revision;
} sni;

static void sni_free_nodes(void)
{
    for (int i = 0; i < sni.node_n; i++) free(sni.nodes[i].label);
    free(sni.nodes);
    sni.nodes = NULL;
    sni.node_n = 0;
}

static void sni_add_nodes(const vio_menu_item *items, int n, int parent)
{
    for (int i = 0; i < n; i++) {
        sni_node *grown = (sni_node *)realloc(sni.nodes, (size_t)(sni.node_n + 1) * sizeof(sni_node));
        if (!grown) return;
        sni.nodes = grown;
        int id = sni.node_n++;
        sni_node *nd = &sni.nodes[id];
        memset(nd, 0, sizeof(*nd));
        nd->parent = parent;
        nd->tag = items[i].tag;
        nd->separator = items[i].separator;
        nd->enabled = items[i].enabled;
        nd->checked = items[i].checked;
        nd->label = items[i].label ? strdup(items[i].label) : NULL;
        if (items[i].children) sni_add_nodes(items[i].children, items[i].child_count, id);
    }
}

static void sni_variant_str(DBusMessageIter *it, const char *v)
{
    DBusMessageIter var;
    db.open_container(it, DB_TYPE_VARIANT, "s", &var);
    db.append_basic(&var, DB_TYPE_STRING, &v);
    db.close_container(it, &var);
}

/* a(iiay) */
static void sni_append_pixmaps(DBusMessageIter *it)
{
    DBusMessageIter arr, st, bytes;
    db.open_container(it, DB_TYPE_ARRAY, "(iiay)", &arr);
    if (sni.pixmap) {
        int32_t w = sni.pw, h = sni.ph;
        db.open_container(&arr, DB_TYPE_STRUCT, NULL, &st);
        db.append_basic(&st, DB_TYPE_INT32, &w);
        db.append_basic(&st, DB_TYPE_INT32, &h);
        db.open_container(&st, DB_TYPE_ARRAY, "y", &bytes);
        for (int i = 0; i < w * h * 4; i++) db.append_basic(&bytes, DB_TYPE_BYTE, &sni.pixmap[i]);
        db.close_container(&st, &bytes);
        db.close_container(&arr, &st);
    }
    db.close_container(it, &arr);
}

/* One StatusNotifierItem / dbusmenu property as a variant; 0 = unknown. */
static int sni_append_prop(DBusMessageIter *it, const char *iface, const char *name)
{
    DBusMessageIter var, st, empty;
    const char *s = NULL;
    if (!strcmp(iface, "org.kde.StatusNotifierItem")) {
        if (!strcmp(name, "Category")) s = "ApplicationStatus";
        else if (!strcmp(name, "Id")) s = vio_app_id() ? vio_app_id() : "php-vio";
        else if (!strcmp(name, "Title")) s = sni.title ? sni.title : "";
        else if (!strcmp(name, "Status")) s = "Active";
        else if (!strcmp(name, "IconName")) s = sni.pixmap ? "" : "application-x-executable";
        else if (!strcmp(name, "IconPixmap")) {
            db.open_container(it, DB_TYPE_VARIANT, "a(iiay)", &var);
            sni_append_pixmaps(&var);
            db.close_container(it, &var);
            return 1;
        } else if (!strcmp(name, "ToolTip")) {   /* (s icon, a(iiay) pixmaps, s title, s text) */
            const char *e = "", *t = sni.title ? sni.title : "";
            db.open_container(it, DB_TYPE_VARIANT, "(sa(iiay)ss)", &var);
            db.open_container(&var, DB_TYPE_STRUCT, NULL, &st);
            db.append_basic(&st, DB_TYPE_STRING, &e);
            db.open_container(&st, DB_TYPE_ARRAY, "(iiay)", &empty);
            db.close_container(&st, &empty);
            db.append_basic(&st, DB_TYPE_STRING, &t);
            db.append_basic(&st, DB_TYPE_STRING, &e);
            db.close_container(&var, &st);
            db.close_container(it, &var);
            return 1;
        } else if (!strcmp(name, "ItemIsMenu")) {
            unsigned int b = sni.node_n > 1;
            db.open_container(it, DB_TYPE_VARIANT, "b", &var);
            db.append_basic(&var, DB_TYPE_BOOLEAN, &b);
            db.close_container(it, &var);
            return 1;
        } else if (!strcmp(name, "Menu")) {
            const char *path = sni.node_n > 1 ? "/MenuBar" : "/NO_DBUSMENU";
            db.open_container(it, DB_TYPE_VARIANT, "o", &var);
            db.append_basic(&var, DB_TYPE_OBJPATH, &path);
            db.close_container(it, &var);
            return 1;
        } else return 0;
    } else if (!strcmp(iface, "com.canonical.dbusmenu")) {
        if (!strcmp(name, "Version")) {
            uint32_t v = 3;
            db.open_container(it, DB_TYPE_VARIANT, "u", &var);
            db.append_basic(&var, DB_TYPE_UINT32, &v);
            db.close_container(it, &var);
            return 1;
        }
        if (!strcmp(name, "Status")) s = "normal";
        else if (!strcmp(name, "TextDirection")) s = "ltr";
        else return 0;
    } else return 0;
    sni_variant_str(it, s);
    return 1;
}

static const char *sni_item_props[] = { "Category", "Id", "Title", "Status", "IconName", "IconPixmap", "ToolTip", "ItemIsMenu", "Menu" };
static const char *sni_menu_props[] = { "Version", "Status", "TextDirection" };

/* (i id, a{sv} props, av children) for node id */
static void sni_append_layout(DBusMessageIter *it, int id, int depth)
{
    DBusMessageIter st, props, kids, var;
    int32_t iid = id;
    db.open_container(it, DB_TYPE_STRUCT, NULL, &st);
    db.append_basic(&st, DB_TYPE_INT32, &iid);
    db.open_container(&st, DB_TYPE_ARRAY, "{sv}", &props);
    int has_kids = 0;
    for (int i = 1; i < sni.node_n; i++) if (sni.nodes[i].parent == id) { has_kids = 1; break; }
    if (id == 0 || has_kids) { const char *v = "submenu"; db_dict_entry(&props, "children-display", DB_TYPE_STRING, "s", &v); }
    if (id > 0) {
        sni_node *nd = &sni.nodes[id];
        if (nd->separator) { const char *v = "separator"; db_dict_entry(&props, "type", DB_TYPE_STRING, "s", &v); }
        else {
            const char *l = nd->label ? nd->label : "";
            db_dict_entry(&props, "label", DB_TYPE_STRING, "s", &l);
            unsigned int en = nd->enabled != 0;
            db_dict_entry(&props, "enabled", DB_TYPE_BOOLEAN, "b", &en);
            if (nd->checked) {
                const char *t = "checkmark";
                int32_t st1 = 1;
                db_dict_entry(&props, "toggle-type", DB_TYPE_STRING, "s", &t);
                db_dict_entry(&props, "toggle-state", DB_TYPE_INT32, "i", &st1);
            }
        }
    }
    db.close_container(&st, &props);
    db.open_container(&st, DB_TYPE_ARRAY, "v", &kids);
    if (depth != 0) {
        for (int i = 1; i < sni.node_n; i++) {
            if (sni.nodes[i].parent != id) continue;
            db.open_container(&kids, DB_TYPE_VARIANT, "(ia{sv}av)", &var);
            sni_append_layout(&var, i, depth < 0 ? -1 : depth - 1);
            db.close_container(&kids, &var);
        }
    }
    db.close_container(&st, &kids);
    db.close_container(it, &st);
}

static void sni_menu_event(int32_t id, const char *event)
{
    if (strcmp(event, "clicked") || id <= 0 || id >= sni.node_n || sni.nodes[id].tag < 0 || !sni.nodes[id].enabled) return;
    vio_tray_menu_event(sni.nodes[id].tag);
}

/* Answers a method call for the tray's objects; others are dropped. */
void vio_linux_handle(DBusMessage *msg)
{
    if (!sni.on || db.get_type(msg) != 1 /* method call */) return;
    const char *iface = db.get_interface(msg), *member = db.get_member(msg), *path = db.get_path(msg);
    if (!member || !path) return;
    DBusMessage *r = NULL;
    DBusMessageIter in, out, dict;
    int have_in = db.iter_init(msg, &in);
    if (iface && !strcmp(iface, "org.freedesktop.DBus.Properties") && (!strcmp(member, "Get") || !strcmp(member, "GetAll"))) {
        const char *pi = NULL, *pn = NULL;
        if (have_in && db.iter_type(&in) == DB_TYPE_STRING) { db.iter_basic(&in, &pi); if (db.iter_next(&in) && db.iter_type(&in) == DB_TYPE_STRING) db.iter_basic(&in, &pn); }
        if (!pi) pi = "";
        r = db.new_return(msg);
        db.iter_init_append(r, &out);
        if (!strcmp(member, "Get")) {
            if (!pn || !sni_append_prop(&out, pi, pn)) { db.unref(r); r = db.new_error(msg, "org.freedesktop.DBus.Error.UnknownProperty", pn ? pn : ""); }
        } else {
            const char **names = !strcmp(pi, "com.canonical.dbusmenu") ? sni_menu_props : sni_item_props;
            int n = names == sni_menu_props ? 3 : 9;
            db.open_container(&out, DB_TYPE_ARRAY, "{sv}", &dict);
            for (int i = 0; i < n; i++) {
                DBusMessageIter e;
                db.open_container(&dict, DB_TYPE_DICT, NULL, &e);
                db.append_basic(&e, DB_TYPE_STRING, &names[i]);
                sni_append_prop(&e, pi, names[i]);
                db.close_container(&dict, &e);
            }
            db.close_container(&out, &dict);
        }
    } else if (iface && !strcmp(iface, "org.kde.StatusNotifierItem")) {
        if (!strcmp(member, "Activate")) vio_tray_click_event();   /* ContextMenu / SecondaryActivate / Scroll: nothing */
        r = db.new_return(msg);
    } else if (iface && !strcmp(iface, "com.canonical.dbusmenu")) {
        r = db.new_return(msg);
        db.iter_init_append(r, &out);
        if (!strcmp(member, "GetLayout")) {
            int32_t parent = 0, depth = -1;
            if (have_in && db.iter_type(&in) == DB_TYPE_INT32) { db.iter_basic(&in, &parent); if (db.iter_next(&in) && db.iter_type(&in) == DB_TYPE_INT32) db.iter_basic(&in, &depth); }
            if (parent < 0 || parent >= (sni.node_n ? sni.node_n : 1)) parent = 0;
            db.append_basic(&out, DB_TYPE_UINT32, &sni.revision);
            sni_append_layout(&out, parent, depth);
        } else if (!strcmp(member, "Event")) {
            int32_t id = 0;
            const char *ev = "";
            if (have_in && db.iter_type(&in) == DB_TYPE_INT32) { db.iter_basic(&in, &id); if (db.iter_next(&in) && db.iter_type(&in) == DB_TYPE_STRING) db.iter_basic(&in, &ev); }
            sni_menu_event(id, ev);
        } else if (!strcmp(member, "EventGroup")) {   /* a(isvu) -> ai (ids not found) */
            DBusMessageIter arr, st, none;
            if (have_in && db.iter_type(&in) == DB_TYPE_ARRAY) {
                db.iter_recurse(&in, &arr);
                for (; db.iter_type(&arr) == DB_TYPE_STRUCT; db.iter_next(&arr)) {
                    int32_t id = 0;
                    const char *ev = "";
                    db.iter_recurse(&arr, &st);
                    db.iter_basic(&st, &id);
                    if (db.iter_next(&st)) db.iter_basic(&st, &ev);
                    sni_menu_event(id, ev);
                }
            }
            db.open_container(&out, DB_TYPE_ARRAY, "i", &none);
            db.close_container(&out, &none);
        } else if (!strcmp(member, "AboutToShow")) {
            unsigned int no = 0;
            db.append_basic(&out, DB_TYPE_BOOLEAN, &no);
        } else if (!strcmp(member, "AboutToShowGroup")) {
            DBusMessageIter a1, a2;
            db.open_container(&out, DB_TYPE_ARRAY, "i", &a1); db.close_container(&out, &a1);
            db.open_container(&out, DB_TYPE_ARRAY, "i", &a2); db.close_container(&out, &a2);
        } else if (!strcmp(member, "GetGroupProperties")) {   /* ai, as -> a(ia{sv}) with each id's layout props */
            DBusMessageIter ids, arr, st, props;
            db.open_container(&out, DB_TYPE_ARRAY, "(ia{sv})", &arr);
            if (have_in && db.iter_type(&in) == DB_TYPE_ARRAY) {
                db.iter_recurse(&in, &ids);
                for (; db.iter_type(&ids) == DB_TYPE_INT32; db.iter_next(&ids)) {
                    int32_t id = 0;
                    db.iter_basic(&ids, &id);
                    if (id <= 0 || id >= sni.node_n) continue;
                    db.open_container(&arr, DB_TYPE_STRUCT, NULL, &st);
                    db.append_basic(&st, DB_TYPE_INT32, &id);
                    db.open_container(&st, DB_TYPE_ARRAY, "{sv}", &props);
                    const char *l = sni.nodes[id].label ? sni.nodes[id].label : "";
                    db_dict_entry(&props, "label", DB_TYPE_STRING, "s", &l);
                    db.close_container(&st, &props);
                    db.close_container(&arr, &st);
                }
            }
            db.close_container(&out, &arr);
        } else {
            db.unref(r);
            r = db.new_error(msg, "org.freedesktop.DBus.Error.UnknownMethod", member);
        }
    } else if (iface && !strcmp(iface, "org.freedesktop.DBus.Introspectable")) {
        static const char *xml =
            "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
            "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\"><node>"
            "<interface name=\"org.kde.StatusNotifierItem\"><method name=\"Activate\"><arg type=\"i\"/><arg type=\"i\"/></method></interface>"
            "<interface name=\"com.canonical.dbusmenu\"><method name=\"GetLayout\"><arg type=\"i\" direction=\"in\"/>"
            "<arg type=\"i\" direction=\"in\"/><arg type=\"as\" direction=\"in\"/><arg type=\"u\" direction=\"out\"/>"
            "<arg type=\"(ia{sv}av)\" direction=\"out\"/></method></interface></node>";
        r = db.new_return(msg);
        db.iter_init_append(r, &out);
        db.append_basic(&out, DB_TYPE_STRING, &xml);
    } else {
        r = db.new_error(msg, "org.freedesktop.DBus.Error.UnknownMethod", member);
    }
    if (r) { db.send(db_conn, r, NULL); db.unref(r); db.flush(db_conn); }
}

void vio_linux_dispatch(void)
{
    if (!db_conn) return;
    db.read_write(db_conn, 0);
    DBusMessage *m;
    while ((m = db.pop(db_conn))) { vio_linux_handle(m); db.unref(m); }
}

int vio_linux_dbus_fd(void)
{
    int fd = -1;
    if (!db_conn || !sni.on || !db.get_unix_fd(db_conn, &fd)) return -1;
    return fd;
}

static void sni_signal(const char *path, const char *iface, const char *name)
{
    DBusMessage *m = db.new_signal(path, iface, name);
    if (!m) return;
    if (!strcmp(name, "LayoutUpdated")) {
        DBusMessageIter it;
        int32_t parent = 0;
        db.iter_init_append(m, &it);
        db.append_basic(&it, DB_TYPE_UINT32, &sni.revision);
        db.append_basic(&it, DB_TYPE_INT32, &parent);
    }
    db.send(db_conn, m, NULL);
    db.unref(m);
}

int vio_linux_set_tray(const vio_tray_desc *d)
{
    DBusConnection *c = db_session();
    if (!c) return 0;
    DBusError err;
    if (!d) {
        if (sni.on) {
            db.error_init(&err);
            db.release_name(c, sni.service, &err);   /* the watcher drops the item when its name goes */
            if (err.name) db.error_free(&err);
            db.flush(c);
        }
        sni.on = 0;
        free(sni.title); sni.title = NULL;
        free(sni.pixmap); sni.pixmap = NULL;
        sni_free_nodes();
        return 1;
    }
    if (!db_has_owner("org.kde.StatusNotifierWatcher")) return 0;
    free(sni.title);
    sni.title = d->tooltip ? strdup(d->tooltip) : NULL;
    free(sni.pixmap);
    sni.pixmap = NULL;
    if (d->image_count > 0) {   /* the largest image, as ARGB32 in network order */
        int best = 0;
        for (int i = 1; i < d->image_count; i++) if (d->images[i].width > d->images[best].width) best = i;
        const vio_platform_image *im = &d->images[best];
        sni.pixmap = (unsigned char *)malloc((size_t)im->width * (size_t)im->height * 4);
        if (sni.pixmap) {
            sni.pw = im->width;
            sni.ph = im->height;
            for (int i = 0; i < im->width * im->height; i++) {
                sni.pixmap[i * 4 + 0] = im->rgba[i * 4 + 3];
                sni.pixmap[i * 4 + 1] = im->rgba[i * 4 + 0];
                sni.pixmap[i * 4 + 2] = im->rgba[i * 4 + 1];
                sni.pixmap[i * 4 + 3] = im->rgba[i * 4 + 2];
            }
        }
    }
    sni_free_nodes();
    sni.nodes = (sni_node *)calloc(1, sizeof(sni_node));   /* the root */
    if (sni.nodes) { sni.node_n = 1; sni.nodes[0].parent = -1; sni.nodes[0].tag = -1; }
    if (d->menu_count > 0) sni_add_nodes(d->menu, d->menu_count, 0);
    sni.revision++;
    if (!sni.on) {
        snprintf(sni.service, sizeof(sni.service), "org.kde.StatusNotifierItem-%d-1", (int)getpid());
        db.error_init(&err);
        int r = db.request_name(c, sni.service, 4 /* DO_NOT_QUEUE */, &err);
        if (err.name) db.error_free(&err);
        if (r != 1 && r != 4) return 0;   /* primary owner / already owner */
        DBusMessage *m = db.new_method_call("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                                            "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem");
        if (!m) return 0;
        DBusMessageIter it;
        const char *svc = sni.service;
        db.iter_init_append(m, &it);
        db.append_basic(&it, DB_TYPE_STRING, &svc);
        sni.on = 1;   /* the watcher may ask for properties before it answers */
        db.send(c, m, NULL);
        db.unref(m);
        db.flush(c);
    } else {
        sni_signal("/StatusNotifierItem", "org.kde.StatusNotifierItem", "NewTitle");
        sni_signal("/StatusNotifierItem", "org.kde.StatusNotifierItem", "NewIcon");
        sni_signal("/StatusNotifierItem", "org.kde.StatusNotifierItem", "NewToolTip");
        sni_signal("/MenuBar", "com.canonical.dbusmenu", "LayoutUpdated");
        db.flush(c);
    }
    return 1;
}

#endif /* __linux__ */
