/*
 * php-vio - accessibility tree (DESKTOP-APP-PLAN Phase 5, AccessKit), see vio_a11y.h
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "zend_exceptions.h"
#include "vio_a11y.h"
#include "vio_input.h"
#include "vio_window.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* name, ACCESSKIT_ROLE_ suffix */
#define VIO_A11Y_ROLES(X) \
    X(unknown, UNKNOWN) X(window, WINDOW) X(dialog, DIALOG) X(alert_dialog, ALERT_DIALOG) X(button, BUTTON) \
    X(check_box, CHECK_BOX) X(radio_button, RADIO_BUTTON) X(switch, SWITCH) X(text_input, TEXT_INPUT) \
    X(multiline_text_input, MULTILINE_TEXT_INPUT) X(search_input, SEARCH_INPUT) X(password_input, PASSWORD_INPUT) \
    X(label, LABEL) X(image, IMAGE) X(link, LINK) X(list, LIST) X(list_item, LIST_ITEM) X(list_box, LIST_BOX) \
    X(list_box_option, LIST_BOX_OPTION) X(menu, MENU) X(menu_bar, MENU_BAR) X(menu_item, MENU_ITEM) \
    X(menu_item_check_box, MENU_ITEM_CHECK_BOX) X(menu_item_radio, MENU_ITEM_RADIO) X(tab, TAB) X(tab_list, TAB_LIST) \
    X(tab_panel, TAB_PANEL) X(slider, SLIDER) X(spin_button, SPIN_BUTTON) X(progress_indicator, PROGRESS_INDICATOR) \
    X(combo_box, COMBO_BOX) X(group, GROUP) X(heading, HEADING) X(paragraph, PARAGRAPH) X(scroll_view, SCROLL_VIEW) \
    X(alert, ALERT) X(status, STATUS) X(toolbar, TOOLBAR) X(tooltip, TOOLTIP) X(tree, TREE) X(tree_item, TREE_ITEM) \
    X(table, TABLE) X(row, ROW) X(cell, CELL) X(column_header, COLUMN_HEADER) X(row_header, ROW_HEADER) \
    X(generic, GENERIC_CONTAINER) X(pane, PANE) X(document, DOCUMENT) X(canvas, CANVAS) X(splitter, SPLITTER)

/* name, ACCESSKIT_ACTION_ suffix; bit = index */
#define VIO_A11Y_ACTIONS(X) \
    X(click, CLICK) X(focus, FOCUS) X(blur, BLUR) X(expand, EXPAND) X(collapse, COLLAPSE) X(increment, INCREMENT) \
    X(decrement, DECREMENT) X(set_value, SET_VALUE) X(scroll_into_view, SCROLL_INTO_VIEW) \
    X(show_context_menu, SHOW_CONTEXT_MENU)

#define X(n, e) #n,
static const char *const vio_a11y_role_names[] = { VIO_A11Y_ROLES(X) };
static const char *const vio_a11y_action_names[] = { VIO_A11Y_ACTIONS(X) };
#undef X
#define VIO_A11Y_ROLE_COUNT   ((int)(sizeof(vio_a11y_role_names) / sizeof(vio_a11y_role_names[0])))
#define VIO_A11Y_ACTION_COUNT ((int)(sizeof(vio_a11y_action_names) / sizeof(vio_a11y_action_names[0])))

static int vio_a11y_lookup(const char *const *names, int n, const char *s)
{
    for (int i = 0; i < n; i++) if (strcmp(names[i], s) == 0) return i;
    return -1;
}

/* ── Parsed update (PHP thread, zend memory) ───────────────────────── */

typedef struct {
    zend_string *id;
    int          role;
    zend_string *label, *value, *description;
    int          has_bounds;
    double       bounds[4];
    HashTable   *children;    /* of strings, borrowed from the update array */
    unsigned     actions;
    int          toggled;     /* -1 unset, 0 off, 1 on, 2 mixed */
    int          disabled;
    int          has_numeric;
    double       numeric, numeric_min, numeric_max;
    int          has_range;
} vio_a11y_in_node;

static int vio_a11y_opt_str(HashTable *h, const char *key, zend_string **out, uint32_t arg, const char *what)
{
    zval *v = zend_hash_str_find(h, key, strlen(key));
    *out = NULL;
    if (!v || Z_TYPE_P(v) == IS_NULL) return 0;
    if (Z_TYPE_P(v) != IS_STRING) {
        zend_argument_type_error(arg, "%s \"%s\" must be a string", what, key);
        return -1;
    }
    *out = Z_STR_P(v);
    return 0;
}

static int vio_a11y_parse_node(zval *zn, vio_a11y_in_node *n, uint32_t arg, zend_long index)
{
    memset(n, 0, sizeof(*n));
    n->toggled = -1;
    if (Z_TYPE_P(zn) != IS_ARRAY) {
        zend_argument_type_error(arg, "node %d must be an array", (int)index);
        return -1;
    }
    HashTable *h = Z_ARRVAL_P(zn);
    zval *v = zend_hash_str_find(h, "id", 2);
    if (!v || Z_TYPE_P(v) != IS_STRING || Z_STRLEN_P(v) == 0) {
        zend_argument_value_error(arg, "node %d needs a non-empty string \"id\"", (int)index);
        return -1;
    }
    n->id = Z_STR_P(v);
    v = zend_hash_str_find(h, "role", 4);
    if (!v || Z_TYPE_P(v) != IS_STRING || (n->role = vio_a11y_lookup(vio_a11y_role_names, VIO_A11Y_ROLE_COUNT, Z_STRVAL_P(v))) < 0) {
        zend_argument_value_error(arg, "node \"%s\" needs a known \"role\"", ZSTR_VAL(n->id));
        return -1;
    }
    if (vio_a11y_opt_str(h, "label", &n->label, arg, "node option") < 0
        || vio_a11y_opt_str(h, "description", &n->description, arg, "node option") < 0) return -1;
    v = zend_hash_str_find(h, "value", 5);
    if (v && Z_TYPE_P(v) != IS_NULL) {
        if (Z_TYPE_P(v) == IS_STRING) n->value = Z_STR_P(v);
        else if (Z_TYPE_P(v) == IS_LONG || Z_TYPE_P(v) == IS_DOUBLE) { n->has_numeric = 1; n->numeric = zval_get_double(v); }
        else { zend_argument_type_error(arg, "node \"%s\" \"value\" must be a string or a number", ZSTR_VAL(n->id)); return -1; }
    }
    v = zend_hash_str_find(h, "range", 5);
    if (v && Z_TYPE_P(v) != IS_NULL) {
        zval *a, *b;
        if (Z_TYPE_P(v) != IS_ARRAY || !(a = zend_hash_index_find(Z_ARRVAL_P(v), 0)) || !(b = zend_hash_index_find(Z_ARRVAL_P(v), 1))) {
            zend_argument_value_error(arg, "node \"%s\" \"range\" must be [min, max]", ZSTR_VAL(n->id));
            return -1;
        }
        n->has_range = 1;
        n->numeric_min = zval_get_double(a);
        n->numeric_max = zval_get_double(b);
    }
    v = zend_hash_str_find(h, "bounds", 6);
    if (v && Z_TYPE_P(v) != IS_NULL) {
        if (Z_TYPE_P(v) != IS_ARRAY || zend_hash_num_elements(Z_ARRVAL_P(v)) != 4) {
            zend_argument_value_error(arg, "node \"%s\" \"bounds\" must be [x, y, width, height]", ZSTR_VAL(n->id));
            return -1;
        }
        for (int i = 0; i < 4; i++) {
            zval *c = zend_hash_index_find(Z_ARRVAL_P(v), i);
            if (!c) { zend_argument_value_error(arg, "node \"%s\" \"bounds\" must be [x, y, width, height]", ZSTR_VAL(n->id)); return -1; }
            n->bounds[i] = zval_get_double(c);
        }
        if (n->bounds[2] < 0 || n->bounds[3] < 0) {
            zend_argument_value_error(arg, "node \"%s\" \"bounds\" must not have a negative size", ZSTR_VAL(n->id));
            return -1;
        }
        n->has_bounds = 1;
    }
    v = zend_hash_str_find(h, "children", 8);
    if (v && Z_TYPE_P(v) != IS_NULL) {
        if (Z_TYPE_P(v) != IS_ARRAY) { zend_argument_type_error(arg, "node \"%s\" \"children\" must be a list of ids", ZSTR_VAL(n->id)); return -1; }
        zval *c;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(v), c) {
            if (Z_TYPE_P(c) != IS_STRING) { zend_argument_type_error(arg, "node \"%s\" \"children\" must be a list of ids", ZSTR_VAL(n->id)); return -1; }
        } ZEND_HASH_FOREACH_END();
        n->children = Z_ARRVAL_P(v);
    }
    v = zend_hash_str_find(h, "actions", 7);
    if (v && Z_TYPE_P(v) != IS_NULL) {
        if (Z_TYPE_P(v) != IS_ARRAY) { zend_argument_type_error(arg, "node \"%s\" \"actions\" must be a list of action names", ZSTR_VAL(n->id)); return -1; }
        zval *c;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(v), c) {
            int a = Z_TYPE_P(c) == IS_STRING ? vio_a11y_lookup(vio_a11y_action_names, VIO_A11Y_ACTION_COUNT, Z_STRVAL_P(c)) : -1;
            if (a < 0) { zend_argument_value_error(arg, "node \"%s\" has an unknown action", ZSTR_VAL(n->id)); return -1; }
            n->actions |= 1u << a;
        } ZEND_HASH_FOREACH_END();
    }
    v = zend_hash_str_find(h, "checked", 7);
    if (v && Z_TYPE_P(v) != IS_NULL) {
        if (Z_TYPE_P(v) == IS_STRING && strcmp(Z_STRVAL_P(v), "mixed") == 0) n->toggled = 2;
        else n->toggled = zend_is_true(v) ? 1 : 0;
    }
    v = zend_hash_str_find(h, "disabled", 8);
    n->disabled = v && zend_is_true(v);
    return 0;
}

/* Parse and check an update. Returns the node count, -1 after throwing. */
static int vio_a11y_parse(HashTable *update, uint32_t arg, vio_a11y_in_node **out, zend_string **root, zend_string **focus)
{
    *out = NULL;
    *root = *focus = NULL;
    if (vio_a11y_opt_str(update, "root", root, arg, "option") < 0 || vio_a11y_opt_str(update, "focus", focus, arg, "option") < 0) return -1;
    zval *nodes = zend_hash_str_find(update, "nodes", 5);
    if (nodes && Z_TYPE_P(nodes) != IS_NULL && Z_TYPE_P(nodes) != IS_ARRAY) {
        zend_argument_type_error(arg, "option \"nodes\" must be a list of nodes");
        return -1;
    }
    int count = nodes && Z_TYPE_P(nodes) == IS_ARRAY ? (int)zend_hash_num_elements(Z_ARRVAL_P(nodes)) : 0;
    vio_a11y_in_node *list = count ? ecalloc((size_t)count, sizeof(*list)) : NULL;
    int i = 0;
    if (count) {
        zval *zn;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(nodes), zn) {
            if (vio_a11y_parse_node(zn, &list[i], arg, i) < 0) { efree(list); return -1; }
            for (int j = 0; j < i; j++) {
                if (zend_string_equals(list[j].id, list[i].id)) {
                    zend_argument_value_error(arg, "node id \"%s\" appears twice", ZSTR_VAL(list[i].id));
                    efree(list);
                    return -1;
                }
            }
            i++;
        } ZEND_HASH_FOREACH_END();
    }
    *out = list;
    return count;
}

#ifdef HAVE_ACCESSKIT

#include "accesskit.h"

#ifdef _WIN32
#include <windows.h>
typedef SRWLOCK vio_a11y_mutex;
#define VIO_A11Y_LOCK_INIT(m)   InitializeSRWLock(m)
#define VIO_A11Y_LOCK(m)        AcquireSRWLockExclusive(m)
#define VIO_A11Y_UNLOCK(m)      ReleaseSRWLockExclusive(m)
#define VIO_A11Y_LOCK_FREE(m)   ((void)0)
#else
#include <pthread.h>
typedef pthread_mutex_t vio_a11y_mutex;
#define VIO_A11Y_LOCK_INIT(m)   pthread_mutex_init(m, NULL)
#define VIO_A11Y_LOCK(m)        pthread_mutex_lock(m)
#define VIO_A11Y_UNLOCK(m)      pthread_mutex_unlock(m)
#define VIO_A11Y_LOCK_FREE(m)   pthread_mutex_destroy(m)
#endif

#define X(n, e) ACCESSKIT_ROLE_##e,
static const accesskit_role vio_a11y_roles[] = { VIO_A11Y_ROLES(X) };
#undef X
#define X(n, e) ACCESSKIT_ACTION_##e,
static const accesskit_action vio_a11y_actions[] = { VIO_A11Y_ACTIONS(X) };
#undef X

/* A node as AccessKit gets it (malloc'd: read from AccessKit's thread). */
typedef struct {
    char       *id;
    uint64_t    nid;
    int         role;
    char       *label, *value, *description;
    int         has_bounds;
    double      bounds[4];
    uint64_t   *children;
    int         child_count;
    unsigned    actions;
    int         toggled, disabled;
    int         has_numeric, has_range;
    double      numeric, numeric_min, numeric_max;
    int         dirty;
} vio_a11y_node;

typedef struct {
    uint64_t nid;
    int      action;    /* index into vio_a11y_actions, -1 = other */
    int      has_text, has_number;
    char    *text;
    double   number;
} vio_a11y_request;

typedef struct _vio_a11y {
    vio_a11y_mutex  lock;
    vio_a11y_node  *nodes;
    int             count, cap;
    int            *hash;        /* open addressing: node index + 1, 0 = empty */
    int             hash_cap;
    uint64_t        next_nid;
    uint64_t        root, focus;
    int             has_root;
    int             sent_tree;   /* the tree info went out with an update */
    void           *platform_window;
    void           *adapter;
    int             focused;
    vio_a11y_request *queue;     /* actions from assistive technology */
    int             queue_count, queue_cap;
} vio_a11y;

static uint32_t vio_a11y_hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

static int vio_a11y_find(vio_a11y *t, const char *id)
{
    if (!t->hash_cap) return -1;
    uint32_t m = (uint32_t)t->hash_cap - 1, i = vio_a11y_hash_str(id) & m;
    while (t->hash[i]) {
        if (strcmp(t->nodes[t->hash[i] - 1].id, id) == 0) return t->hash[i] - 1;
        i = (i + 1) & m;
    }
    return -1;
}

/* Text inputs with a value get a hidden text run child (AccessKit's text
 * interface reads the runs): its nid is the input's with this bit. */
#define VIO_A11Y_RUN_BIT (1ULL << 62)

static int vio_a11y_is_text_input(int role)
{
    const char *n = vio_a11y_role_names[role];
    return strcmp(n, "text_input") == 0 || strcmp(n, "multiline_text_input") == 0
        || strcmp(n, "search_input") == 0 || strcmp(n, "password_input") == 0;
}

static int vio_a11y_find_nid(vio_a11y *t, uint64_t nid)
{
    nid &= ~VIO_A11Y_RUN_BIT;
    /* nids are handed out in order: node k has nid k + 1 */
    if (nid == 0 || nid > (uint64_t)t->count) return -1;
    return (int)(nid - 1);
}

static void vio_a11y_rehash(vio_a11y *t, int cap)
{
    free(t->hash);
    t->hash = (int *)calloc((size_t)cap, sizeof(int));
    t->hash_cap = cap;
    uint32_t m = (uint32_t)cap - 1;
    for (int k = 0; k < t->count; k++) {
        uint32_t i = vio_a11y_hash_str(t->nodes[k].id) & m;
        while (t->hash[i]) i = (i + 1) & m;
        t->hash[i] = k + 1;
    }
}

static int vio_a11y_add(vio_a11y *t, const char *id)
{
    if (t->count == t->cap) {
        int cap = t->cap ? t->cap * 2 : 32;
        t->nodes = (vio_a11y_node *)realloc(t->nodes, (size_t)cap * sizeof(vio_a11y_node));
        t->cap = cap;
    }
    vio_a11y_node *n = &t->nodes[t->count];
    memset(n, 0, sizeof(*n));
    n->id = strdup(id);
    n->nid = ++t->next_nid;
    n->toggled = -1;
    t->count++;
    if (t->count * 2 > t->hash_cap) vio_a11y_rehash(t, t->hash_cap ? t->hash_cap * 2 : 64);
    else {
        uint32_t m = (uint32_t)t->hash_cap - 1, i = vio_a11y_hash_str(id) & m;
        while (t->hash[i]) i = (i + 1) & m;
        t->hash[i] = t->count;
    }
    return t->count - 1;
}

static char *vio_a11y_dup(zend_string *s) { return s ? strdup(ZSTR_VAL(s)) : NULL; }

static struct accesskit_node *vio_a11y_build_node(const vio_a11y_node *n)
{
    struct accesskit_node *an = accesskit_node_new(vio_a11y_roles[n->role]);
    if (n->label) accesskit_node_set_label(an, n->label);
    if (n->value) accesskit_node_set_value(an, n->value);
    if (n->description) accesskit_node_set_description(an, n->description);
    if (n->has_numeric) accesskit_node_set_numeric_value(an, n->numeric);
    if (n->has_range) {
        accesskit_node_set_min_numeric_value(an, n->numeric_min);
        accesskit_node_set_max_numeric_value(an, n->numeric_max);
    }
    if (n->has_bounds) {
        struct accesskit_rect r = { n->bounds[0], n->bounds[1], n->bounds[0] + n->bounds[2], n->bounds[1] + n->bounds[3] };
        accesskit_node_set_bounds(an, r);
    }
    for (int i = 0; i < n->child_count; i++) accesskit_node_push_child(an, n->children[i]);
    if (n->value && vio_a11y_is_text_input(n->role)) accesskit_node_push_child(an, n->nid | VIO_A11Y_RUN_BIT);
    for (int a = 0; a < VIO_A11Y_ACTION_COUNT; a++) if (n->actions & (1u << a)) accesskit_node_add_action(an, vio_a11y_actions[a]);
    if (n->toggled >= 0) accesskit_node_set_toggled(an, n->toggled == 2 ? ACCESSKIT_TOGGLED_MIXED : n->toggled ? ACCESSKIT_TOGGLED_TRUE : ACCESSKIT_TOGGLED_FALSE);
    if (n->disabled) accesskit_node_set_disabled(an);
    return an;
}

/* The tree update: everything (activation) or the dirty nodes. Under the lock. */
static struct accesskit_tree_update *vio_a11y_build_update(vio_a11y *t, int full)
{
    struct accesskit_tree_update *u = accesskit_tree_update_with_capacity_and_focus((size_t)t->count, t->focus ? t->focus : t->root);
    if (full || !t->sent_tree) {
        struct accesskit_tree_info *info = accesskit_tree_info_new(t->root);
        accesskit_tree_info_set_toolkit_name(info, "php-vio");
        accesskit_tree_update_set_tree_info(u, info);
        t->sent_tree = 1;
    }
    for (int k = 0; k < t->count; k++) {
        vio_a11y_node *n = &t->nodes[k];
        if (!full && !n->dirty) continue;
        n->dirty = 0;
        accesskit_tree_update_push_node(u, n->nid, vio_a11y_build_node(n));
        if (n->value && vio_a11y_is_text_input(n->role)) {
            struct accesskit_node *run = accesskit_node_new(ACCESSKIT_ROLE_TEXT_RUN);
            accesskit_node_set_value(run, n->value);
            size_t len = strlen(n->value), cps = 0;
            uint8_t *lens = (uint8_t *)malloc(len ? len : 1);
            for (size_t i = 0; i < len; ) {   /* UTF-8 bytes per character */
                unsigned char c = (unsigned char)n->value[i];
                size_t l = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
                if (i + l > len) l = len - i;
                lens[cps++] = (uint8_t)l;
                i += l;
            }
            accesskit_node_set_character_lengths(run, cps, lens);
            free(lens);
            if (n->has_bounds) {
                struct accesskit_rect r = { n->bounds[0], n->bounds[1], n->bounds[0] + n->bounds[2], n->bounds[1] + n->bounds[3] };
                accesskit_node_set_bounds(run, r);
            }
            accesskit_tree_update_push_node(u, n->nid | VIO_A11Y_RUN_BIT, run);
        }
    }
    return u;
}

static struct accesskit_tree_update *vio_a11y_activation(void *userdata)
{
    vio_a11y *t = (vio_a11y *)userdata;
    VIO_A11Y_LOCK(&t->lock);
    struct accesskit_tree_update *u = vio_a11y_build_update(t, 1);
    VIO_A11Y_UNLOCK(&t->lock);
    return u;
}

static struct accesskit_tree_update *vio_a11y_changes(void *userdata)
{
    vio_a11y *t = (vio_a11y *)userdata;
    VIO_A11Y_LOCK(&t->lock);
    struct accesskit_tree_update *u = vio_a11y_build_update(t, 0);
    VIO_A11Y_UNLOCK(&t->lock);
    return u;
}

/* Any thread: queue the request for the PHP thread. */
static void vio_a11y_action(struct accesskit_action_request *req, void *userdata)
{
    vio_a11y *t = (vio_a11y *)userdata;
    vio_a11y_request r;
    memset(&r, 0, sizeof(r));
    r.nid = req->target_node;
    r.action = -1;
    for (int a = 0; a < VIO_A11Y_ACTION_COUNT; a++) if (vio_a11y_actions[a] == req->action) r.action = a;
    if (req->data.has_value) {
        if (req->data.value.tag == ACCESSKIT_ACTION_DATA_VALUE && req->data.value.value) {
            r.has_text = 1;
            r.text = strdup(req->data.value.value);
        } else if (req->data.value.tag == ACCESSKIT_ACTION_DATA_NUMERIC_VALUE) {
            r.has_number = 1;
            r.number = req->data.value.numeric_value;
        }
    }
    accesskit_action_request_free(req);
    if (r.action < 0) { free(r.text); return; }
    VIO_A11Y_LOCK(&t->lock);
    if (t->queue_count == t->queue_cap) {
        int cap = t->queue_cap ? t->queue_cap * 2 : 8;
        t->queue = (vio_a11y_request *)realloc(t->queue, (size_t)cap * sizeof(*t->queue));
        t->queue_cap = cap;
    }
    t->queue[t->queue_count++] = r;
    VIO_A11Y_UNLOCK(&t->lock);
    vio_plat()->wake();   /* a vio_wait_events returns to deliver it */
}

static void vio_a11y_deactivation(void *userdata) { (void)userdata; }

/* ── Platform adapters ─────────────────────────────────────────────── */

#if defined(__linux__)
static void vio_a11y_root_bounds(vio_a11y *t)
{
    if (!t->adapter || !t->platform_window) return;
    int x = 0, y = 0, w = 0, h = 0;
    if (vio_plat()->get_window_pos) vio_plat()->get_window_pos(t->platform_window, &x, &y);
    vio_plat()->get_window_size(t->platform_window, &w, &h);
    struct accesskit_rect r = { x, y, x + w, y + h };
    accesskit_unix_adapter_set_root_window_bounds((struct accesskit_unix_adapter *)t->adapter, r, r);
}

static int vio_a11y_adapter_new(vio_a11y *t)
{
    t->adapter = accesskit_unix_adapter_new(vio_a11y_activation, t, vio_a11y_action, t, vio_a11y_deactivation, t);
    if (!t->adapter) return 0;
    vio_a11y_root_bounds(t);
    accesskit_unix_adapter_update_window_focus_state((struct accesskit_unix_adapter *)t->adapter, t->focused);
    return 1;
}
static void vio_a11y_adapter_update(vio_a11y *t)
{
    vio_a11y_root_bounds(t);
    accesskit_unix_adapter_update_if_active((struct accesskit_unix_adapter *)t->adapter, vio_a11y_changes, t);
}
static void vio_a11y_adapter_focus(vio_a11y *t, int focused)
{
    accesskit_unix_adapter_update_window_focus_state((struct accesskit_unix_adapter *)t->adapter, focused != 0);
}
static void vio_a11y_adapter_free(vio_a11y *t)
{
    accesskit_unix_adapter_free((struct accesskit_unix_adapter *)t->adapter);
}
#define VIO_A11Y_HAS_ADAPTER 1
#endif

#ifndef VIO_A11Y_HAS_ADAPTER
static int  vio_a11y_adapter_new(vio_a11y *t) { (void)t; return 0; }
static void vio_a11y_adapter_update(vio_a11y *t) { (void)t; }
static void vio_a11y_adapter_focus(vio_a11y *t, int focused) { (void)t; (void)focused; }
static void vio_a11y_adapter_free(vio_a11y *t) { (void)t; }
#endif

int vio_a11y_available(void)
{
#ifdef VIO_A11Y_HAS_ADAPTER
    return vio_platform_has_windows();
#else
    return 0;
#endif
}

static vio_a11y *vio_a11y_get(vio_input_state *in, void *platform_window)
{
    if (in->a11y) return (vio_a11y *)in->a11y;
    vio_a11y *t = (vio_a11y *)calloc(1, sizeof(*t));
    VIO_A11Y_LOCK_INIT(&t->lock);
    t->platform_window = platform_window;
    t->focused = in->win_focused == 1;
    in->a11y = t;
    return t;
}

int vio_a11y_update(vio_input_state *in, void *platform_window, HashTable *update, uint32_t arg)
{
    vio_a11y_in_node *list;
    zend_string *root, *focus;
    int count = vio_a11y_parse(update, arg, &list, &root, &focus);
    if (count < 0) return -1;
    if (!vio_a11y_available() || !platform_window) { if (list) efree(list); return 0; }

    vio_a11y *t = vio_a11y_get(in, platform_window);
    VIO_A11Y_LOCK(&t->lock);
    /* references must resolve in the tree after this update (AccessKit aborts otherwise) */
    const char *bad = NULL;
    if (!t->has_root && !root && count == 0) bad = "the first update needs nodes";
    zend_string *root_id = root ? root : (!t->has_root && count ? list[0].id : NULL);
    if (!bad && root_id) {
        int known = vio_a11y_find(t, ZSTR_VAL(root_id)) >= 0;
        for (int i = 0; i < count && !known; i++) known = zend_string_equals(list[i].id, root_id);
        if (!known) bad = "\"root\" is not a node of the tree";
    }
    if (!bad && focus) {
        int known = vio_a11y_find(t, ZSTR_VAL(focus)) >= 0;
        for (int i = 0; i < count && !known; i++) known = zend_string_equals(list[i].id, focus);
        if (!known) bad = "\"focus\" is not a node of the tree";
    }
    for (int i = 0; i < count && !bad; i++) {
        if (!list[i].children) continue;
        zval *c;
        ZEND_HASH_FOREACH_VAL(list[i].children, c) {
            int known = vio_a11y_find(t, Z_STRVAL_P(c)) >= 0;
            for (int j = 0; j < count && !known; j++) known = zend_string_equals(list[j].id, Z_STR_P(c));
            if (!known) { bad = "a child id is not a node of the tree"; break; }
        } ZEND_HASH_FOREACH_END();
    }
    if (bad) {
        VIO_A11Y_UNLOCK(&t->lock);
        if (list) efree(list);
        zend_argument_value_error(arg, "%s", bad);
        return -1;
    }

    /* new ids first, so children resolve to nids */
    for (int i = 0; i < count; i++) if (vio_a11y_find(t, ZSTR_VAL(list[i].id)) < 0) vio_a11y_add(t, ZSTR_VAL(list[i].id));
    for (int i = 0; i < count; i++) {
        vio_a11y_in_node *s = &list[i];
        vio_a11y_node *n = &t->nodes[vio_a11y_find(t, ZSTR_VAL(s->id))];
        free(n->label); free(n->value); free(n->description); free(n->children);
        n->role = s->role;
        n->label = vio_a11y_dup(s->label);
        n->value = vio_a11y_dup(s->value);
        n->description = vio_a11y_dup(s->description);
        n->has_bounds = s->has_bounds;
        memcpy(n->bounds, s->bounds, sizeof(n->bounds));
        n->children = NULL;
        n->child_count = 0;
        if (s->children && zend_hash_num_elements(s->children)) {
            n->children = (uint64_t *)malloc(zend_hash_num_elements(s->children) * sizeof(uint64_t));
            zval *c;
            ZEND_HASH_FOREACH_VAL(s->children, c) {
                n->children[n->child_count++] = t->nodes[vio_a11y_find(t, Z_STRVAL_P(c))].nid;
            } ZEND_HASH_FOREACH_END();
        }
        n->actions = s->actions;
        n->toggled = s->toggled;
        n->disabled = s->disabled;
        n->has_numeric = s->has_numeric;
        n->numeric = s->numeric;
        n->has_range = s->has_range;
        n->numeric_min = s->numeric_min;
        n->numeric_max = s->numeric_max;
        n->dirty = 1;
    }
    if (root_id) {
        uint64_t r = t->nodes[vio_a11y_find(t, ZSTR_VAL(root_id))].nid;
        if (t->has_root && r != t->root) t->sent_tree = 0;   /* a new root goes out with the tree info */
        t->root = r;
        t->has_root = 1;
    }
    if (focus) t->focus = t->nodes[vio_a11y_find(t, ZSTR_VAL(focus))].nid;
    VIO_A11Y_UNLOCK(&t->lock);
    if (list) efree(list);

    if (!t->adapter) {
        if (!vio_a11y_adapter_new(t)) return 0;
    } else {
        vio_a11y_adapter_update(t);
    }
    return 1;
}

void vio_a11y_dispatch(vio_input_state *in)
{
    vio_a11y *t = (vio_a11y *)in->a11y;
    if (!t) return;
    VIO_A11Y_LOCK(&t->lock);
    int n = t->queue_count;
    vio_a11y_request *q = t->queue;
    t->queue = NULL;
    t->queue_count = t->queue_cap = 0;
    VIO_A11Y_UNLOCK(&t->lock);
    for (int i = 0; i < n; i++) {
        vio_a11y_request *r = &q[i];
        int k = vio_a11y_find_nid(t, r->nid);
        if (k >= 0 && in->has_window_cb[VIO_WINDOW_CB_A11Y] && !in->blocked) {
            zval args[3], retval;
            ZVAL_STRING(&args[0], t->nodes[k].id);
            ZVAL_STRING(&args[1], vio_a11y_action_names[r->action]);
            if (r->has_text) ZVAL_STRING(&args[2], r->text);
            else if (r->has_number) ZVAL_DOUBLE(&args[2], r->number);
            else ZVAL_NULL(&args[2]);
            ZVAL_UNDEF(&retval);
            call_user_function(NULL, NULL, &in->window_cb[VIO_WINDOW_CB_A11Y], &retval, 3, args);
            zval_ptr_dtor(&retval);
            zval_ptr_dtor(&args[0]);
            zval_ptr_dtor(&args[1]);
            zval_ptr_dtor(&args[2]);
        }
        free(r->text);
    }
    free(q);
}

void vio_a11y_focus(vio_input_state *in, int focused)
{
    vio_a11y *t = (vio_a11y *)in->a11y;
    if (!t) return;
    t->focused = focused != 0;
    if (t->adapter) vio_a11y_adapter_focus(t, focused);
}

void vio_a11y_free(vio_input_state *in)
{
    vio_a11y *t = (vio_a11y *)in->a11y;
    if (!t) return;
    in->a11y = NULL;
    if (t->adapter) vio_a11y_adapter_free(t);   /* joins AccessKit's use of the tree */
    for (int k = 0; k < t->count; k++) {
        vio_a11y_node *n = &t->nodes[k];
        free(n->id); free(n->label); free(n->value); free(n->description); free(n->children);
    }
    for (int i = 0; i < t->queue_count; i++) free(t->queue[i].text);
    free(t->queue);
    free(t->nodes);
    free(t->hash);
    VIO_A11Y_LOCK_FREE(&t->lock);
    free(t);
}

#else /* !HAVE_ACCESSKIT */

int vio_a11y_available(void) { return 0; }

int vio_a11y_update(vio_input_state *in, void *platform_window, HashTable *update, uint32_t arg)
{
    (void)in; (void)platform_window;
    vio_a11y_in_node *list;
    zend_string *root, *focus;
    int count = vio_a11y_parse(update, arg, &list, &root, &focus);
    if (list) efree(list);
    return count < 0 ? -1 : 0;
}

void vio_a11y_dispatch(vio_input_state *in) { (void)in; }
void vio_a11y_focus(vio_input_state *in, int focused) { (void)in; (void)focused; }
void vio_a11y_free(vio_input_state *in) { (void)in; }

#endif
