/*
 * php-vio - Input state management implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_input.h"
#include "../include/vio_types.h"
#include <string.h>

void vio_input_init(vio_input_state *state)
{
    memset(state->keys, 0, sizeof(state->keys));
    memset(state->keys_prev, 0, sizeof(state->keys_prev));
    state->mouse_x = 0.0;
    state->mouse_y = 0.0;
    state->mouse_prev_x = 0.0;
    state->mouse_prev_y = 0.0;
    memset(state->mouse_buttons, 0, sizeof(state->mouse_buttons));
    state->scroll_x = 0.0;
    state->scroll_y = 0.0;
    ZVAL_UNDEF(&state->on_key_callback);
    ZVAL_UNDEF(&state->on_resize_callback);
    ZVAL_UNDEF(&state->on_char_callback);
    state->has_key_callback = 0;
    state->has_resize_callback = 0;
    state->has_char_callback = 0;
    state->char_buffer_len = 0;
    state->ime_cp_count = 0;
    state->ime_backspaces = 0;
    memset(state->touches, 0, sizeof(state->touches));
    state->touch_count = 0;

    state->poll_tick   = 0;
    state->recording   = 0;
    state->record_base = 0;
    memset(&state->record, 0, sizeof(state->record));
    memset(state->record_pads, 0, sizeof(state->record_pads));
    state->replaying   = 0;
    state->replay_base = 0;
    state->replay_pos  = 0;
    memset(&state->replay, 0, sizeof(state->replay));
    state->replay_pads = 0;
}

/* Append ev to a running recording, stamped with the current tick. */
static void vio_input_rec(vio_input_state *state, vio_input_event *ev)
{
    if (!state->recording) return;
    ev->tick = state->poll_tick - state->record_base;
    vio_input_event_list_push(&state->record, ev);
}

static int vio_touch_find_slot(vio_input_state *state, unsigned long long id)
{
    for (int i = 0; i < VIO_MAX_TOUCHES; i++) {
        if (state->touches[i].id == id) return i;
    }
    return -1;
}

void vio_input_update(vio_input_state *state)
{
    memcpy(state->keys_prev, state->keys, sizeof(state->keys));
    state->mouse_prev_x = state->mouse_x;
    state->mouse_prev_y = state->mouse_y;
    /* Reset per-frame scroll accumulator */
    state->scroll_x = 0.0;
    state->scroll_y = 0.0;
    /* Reset per-frame char buffer */
    state->char_buffer_len = 0;

    /* Advance touch phases:
     *   BEGAN  -> STATIONARY (consumer had one frame to see the edge)
     *   MOVED  -> STATIONARY (next frame is stationary unless new move arrives)
     *   ENDED, CANCELLED -> slot freed (id = 0, phase = INACTIVE)
     * prev_x/prev_y are snapshotted so MOVED can report a delta. */
    int active = 0;
    for (int i = 0; i < VIO_MAX_TOUCHES; i++) {
        vio_touch *t = &state->touches[i];
        if (t->id == 0) continue;

        switch (t->phase) {
            case VIO_TOUCH_ENDED:
            case VIO_TOUCH_CANCELLED:
                t->id = 0;
                t->phase = VIO_TOUCH_INACTIVE;
                t->x = t->y = t->prev_x = t->prev_y = 0.0;
                break;
            case VIO_TOUCH_BEGAN:
            case VIO_TOUCH_MOVED:
                t->phase = VIO_TOUCH_STATIONARY;
                t->prev_x = t->x;
                t->prev_y = t->y;
                active++;
                break;
            case VIO_TOUCH_STATIONARY:
                t->prev_x = t->x;
                t->prev_y = t->y;
                active++;
                break;
            case VIO_TOUCH_INACTIVE:
                /* Defensive: should not happen since id != 0 implies active */
                t->id = 0;
                break;
        }
    }
    state->touch_count = active;
}

void vio_input_shutdown(vio_input_state *state)
{
    vio_input_replay_stop(state);
    state->recording = 0;
    vio_input_event_list_free(&state->record);

    if (state->has_key_callback) {
        zval_ptr_dtor(&state->on_key_callback);
        ZVAL_UNDEF(&state->on_key_callback);
        state->has_key_callback = 0;
    }
    if (state->has_resize_callback) {
        zval_ptr_dtor(&state->on_resize_callback);
        ZVAL_UNDEF(&state->on_resize_callback);
        state->has_resize_callback = 0;
    }
    if (state->has_char_callback) {
        zval_ptr_dtor(&state->on_char_callback);
        ZVAL_UNDEF(&state->on_char_callback);
        state->has_char_callback = 0;
    }
}

/* UTF-8 encode a codepoint. Returns byte count (1-4) or 0 if out of range.
 * Non-guarded: used by both the GLFW char callback and the iOS IME path. */
static int vio_encode_utf8(unsigned int codepoint, char *out)
{
    if (codepoint <= 0x7F) {
        out[0] = (char)codepoint;
        return 1;
    } else if (codepoint <= 0x7FF) {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    } else if (codepoint <= 0xFFFF) {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    } else if (codepoint <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (codepoint >> 18));
        out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[3] = (char)(0x80 | (codepoint & 0x3F));
        return 4;
    }
    return 0;
}

/* Emit one typed codepoint: append its UTF-8 to the per-frame char buffer and
 * fire the on_char PHP callback. The single funnel both the desktop GLFW char
 * callback and the iOS IME drain feed, so the engine's text handling is
 * identical across platforms. Must run on the PHP/render thread (fires a PHP
 * callback). */
void vio_input_emit_char(vio_input_state *state, unsigned int codepoint)
{
    if (!state) return;

    vio_input_event ev = {0};
    ev.type = VIO_INPUT_EV_CHAR;
    ev.a    = (int)codepoint;
    vio_input_rec(state, &ev);

    char encoded[4];
    int len = vio_encode_utf8(codepoint, encoded);
    if (len > 0 && state->char_buffer_len + len < (int)sizeof(state->char_buffer)) {
        memcpy(state->char_buffer + state->char_buffer_len, encoded, (size_t)len);
        state->char_buffer_len += len;
    }

    if (state->has_char_callback) {
        zval retval, args[1];
        ZVAL_LONG(&args[0], (zend_long)codepoint);
        if (call_user_function(NULL, NULL, &state->on_char_callback, &retval, 1, args) == SUCCESS) {
            zval_ptr_dtor(&retval);
        }
    }
}

/* ── Event funnels ──────────────────────────────────────────────────
 *
 * One entry point per event kind, shared by the GLFW callbacks and the
 * vio_inject_* functions. An injected event reaches the game exactly like one
 * from the OS: the same state writes and the same PHP callbacks. That matters
 * for engines that take key edges from vio_on_key rather than from
 * vio_key_just_pressed - PHPolygon does - since an injection that only wrote
 * the key array was invisible to them. PHP/render thread only (fires PHP
 * callbacks). */

void vio_input_key_event(vio_input_state *state, int key, int action, int mods)
{
    if (!state) return;

    vio_input_event ev = {0};
    ev.type = VIO_INPUT_EV_KEY;
    ev.a = key; ev.b = action; ev.c = mods;
    vio_input_rec(state, &ev);

    if (key >= 0 && key <= VIO_KEY_LAST) {
        state->keys[key] = (action != VIO_RELEASE) ? 1 : 0;
    }

    if (state->has_key_callback) {
        zval retval, args[3];
        ZVAL_LONG(&args[0], key);
        ZVAL_LONG(&args[1], action);
        ZVAL_LONG(&args[2], mods);

        if (call_user_function(NULL, NULL, &state->on_key_callback, &retval, 3, args) == SUCCESS) {
            zval_ptr_dtor(&retval);
        }
    }
}

void vio_input_cursor_event(vio_input_state *state, double x, double y)
{
    if (!state) return;

    vio_input_event ev = {0};
    ev.type = VIO_INPUT_EV_CURSOR;
    ev.x = x; ev.y = y;
    vio_input_rec(state, &ev);

    state->mouse_x = x;
    state->mouse_y = y;
}

void vio_input_button_event(vio_input_state *state, int button, int action)
{
    if (!state) return;

    vio_input_event ev = {0};
    ev.type = VIO_INPUT_EV_BUTTON;
    ev.a = button; ev.b = action;
    vio_input_rec(state, &ev);

    if (button >= 0 && button <= VIO_MOUSE_LAST) {
        state->mouse_buttons[button] = (action != VIO_RELEASE) ? 1 : 0;
    }
}

void vio_input_scroll_event(vio_input_state *state, double dx, double dy)
{
    if (!state) return;

    vio_input_event ev = {0};
    ev.type = VIO_INPUT_EV_SCROLL;
    ev.x = dx; ev.y = dy;
    vio_input_rec(state, &ev);

    state->scroll_x += dx;
    state->scroll_y += dy;
}

#ifdef HAVE_GLFW

/* While a replay runs it owns the input: OS events are dropped, so a human
 * touching the mouse cannot knock a replayed run off course. */
static vio_input_state *glfw_input_state(GLFWwindow *window)
{
    vio_input_state *state = (vio_input_state *)glfwGetWindowUserPointer(window);
    return (state && !state->replaying) ? state : NULL;
}

static void glfw_key_callback(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    (void)scancode;
    vio_input_key_event(glfw_input_state(window), key, action, mods);
}

static void glfw_char_callback(GLFWwindow *window, unsigned int codepoint)
{
    vio_input_emit_char(glfw_input_state(window), codepoint);
}

static void glfw_cursor_pos_callback(GLFWwindow *window, double xpos, double ypos)
{
    vio_input_cursor_event(glfw_input_state(window), xpos, ypos);
}

static void glfw_mouse_button_callback(GLFWwindow *window, int button, int action, int mods)
{
    (void)mods;
    vio_input_button_event(glfw_input_state(window), button, action);
}

static void glfw_scroll_callback(GLFWwindow *window, double xoffset, double yoffset)
{
    vio_input_scroll_event(glfw_input_state(window), xoffset, yoffset);
}

static void glfw_framebuffer_size_callback(GLFWwindow *window, int width, int height)
{
    vio_input_state *state = (vio_input_state *)glfwGetWindowUserPointer(window);
    if (!state) return;

    /* Fire PHP callback if registered */
    if (state->has_resize_callback) {
        zval retval, args[2];
        ZVAL_LONG(&args[0], width);
        ZVAL_LONG(&args[1], height);

        if (call_user_function(NULL, NULL, &state->on_resize_callback, &retval, 2, args) == SUCCESS) {
            zval_ptr_dtor(&retval);
        }
    }
}

void vio_input_install_callbacks(GLFWwindow *window, vio_input_state *state)
{
    glfwSetWindowUserPointer(window, state);
    glfwSetKeyCallback(window, glfw_key_callback);
    glfwSetCharCallback(window, glfw_char_callback);
    glfwSetCursorPosCallback(window, glfw_cursor_pos_callback);
    glfwSetMouseButtonCallback(window, glfw_mouse_button_callback);
    glfwSetScrollCallback(window, glfw_scroll_callback);
    glfwSetFramebufferSizeCallback(window, glfw_framebuffer_size_callback);
}

#endif /* HAVE_GLFW */

/* ── Touch push API ─────────────────────────────────────────────────
 *
 * Implementation notes:
 *   - The slot array is small (11) so linear search is fine.
 *   - id==0 is the inactive-slot sentinel. Backends that hand us 0 get
 *     remapped to 1, which is fine because real platform ids are pointers
 *     or counters that never collide with 1 in practice.
 *   - We do not deliver PHP callbacks here today. Touch is consumed by
 *     polling vio_touch_count() / vio_touch_get() from PHP. A callback
 *     model can be added later if we need touch-driven events. */

static unsigned long long vio_touch_normalize_id(unsigned long long id)
{
    return id == 0 ? 1 : id;
}

static void vio_input_rec_touch(vio_input_state *state, unsigned long long id, int phase, double x, double y)
{
    vio_input_event ev = {0};
    ev.type = VIO_INPUT_EV_TOUCH;
    ev.id = id; ev.b = phase; ev.x = x; ev.y = y;
    vio_input_rec(state, &ev);
}

int vio_input_touch_began(vio_input_state *state, unsigned long long id, double x, double y)
{
    id = vio_touch_normalize_id(id);
    vio_input_rec_touch(state, id, VIO_TOUCH_BEGAN, x, y);

    /* Reject duplicate id (already active). Backends should not call
     * began twice without an ended in between, but be defensive. */
    if (vio_touch_find_slot(state, id) >= 0) return -1;

    for (int i = 0; i < VIO_MAX_TOUCHES; i++) {
        if (state->touches[i].id == 0) {
            state->touches[i].id     = id;
            state->touches[i].x      = x;
            state->touches[i].y      = y;
            state->touches[i].prev_x = x;
            state->touches[i].prev_y = y;
            state->touches[i].phase  = VIO_TOUCH_BEGAN;
            state->touch_count++;
            /* Touch -> mouse emulation: mirror the touch as the primary
             * mouse button so desktop games that poll vio_mouse_position /
             * vio_mouse_button work unchanged on touch devices. Single-touch
             * model (last finger wins) - fine for pointer-style UIs. */
            state->mouse_x = x;
            state->mouse_y = y;
            state->mouse_buttons[0] = 1;
            return i;
        }
    }
    return -1; /* Array full */
}

void vio_input_touch_moved(vio_input_state *state, unsigned long long id, double x, double y)
{
    id = vio_touch_normalize_id(id);
    vio_input_rec_touch(state, id, VIO_TOUCH_MOVED, x, y);

    int idx = vio_touch_find_slot(state, id);
    if (idx < 0) return;

    /* Don't overwrite ENDED/CANCELLED that haven't been cleared yet — a
     * stray move after end is a backend bug we silently swallow. */
    if (state->touches[idx].phase == VIO_TOUCH_ENDED ||
        state->touches[idx].phase == VIO_TOUCH_CANCELLED) {
        return;
    }

    state->touches[idx].x     = x;
    state->touches[idx].y     = y;
    state->touches[idx].phase = VIO_TOUCH_MOVED;

    /* Touch -> mouse emulation: drag moves the emulated cursor. */
    state->mouse_x = x;
    state->mouse_y = y;
}

void vio_input_touch_ended(vio_input_state *state, unsigned long long id)
{
    id = vio_touch_normalize_id(id);
    vio_input_rec_touch(state, id, VIO_TOUCH_ENDED, 0.0, 0.0);
    int idx = vio_touch_find_slot(state, id);
    if (idx < 0) return;
    state->touches[idx].phase = VIO_TOUCH_ENDED;
    /* Touch -> mouse emulation: finger up = primary mouse button release. */
    state->mouse_buttons[0] = 0;
}

void vio_input_touch_cancelled(vio_input_state *state, unsigned long long id)
{
    id = vio_touch_normalize_id(id);
    vio_input_rec_touch(state, id, VIO_TOUCH_CANCELLED, 0.0, 0.0);
    int idx = vio_touch_find_slot(state, id);
    if (idx < 0) return;
    state->touches[idx].phase = VIO_TOUCH_CANCELLED;
    state->mouse_buttons[0] = 0;
}

/* ── iOS soft-keyboard text input ──────────────────────────────────
 *
 * The UIKeyInput view enqueues codepoints / backspaces from the UIKit main
 * thread; the render thread drains them (vio_input_drain_ime, from
 * vio_poll_events) through the normal char path. Lock-free volatile counters -
 * see the struct comment. */
void vio_input_push_codepoint(vio_input_state *state, unsigned int codepoint)
{
    if (!state) return;
    int n = state->ime_cp_count;
    if (n >= 0 && n < (int)(sizeof(state->ime_codepoints) / sizeof(state->ime_codepoints[0]))) {
        state->ime_codepoints[n] = codepoint;
        state->ime_cp_count = n + 1;
    }
}

void vio_input_drain_ime(vio_input_state *state)
{
    if (!state) return;
    int n = state->ime_cp_count;
    state->ime_cp_count = 0;
    for (int i = 0; i < n; i++) {
        vio_input_emit_char(state, state->ime_codepoints[i]);
    }
}

void vio_input_ime_backspace(vio_input_state *state)
{
    if (state) state->ime_backspaces++;
}

int vio_input_take_ime_backspaces(vio_input_state *state)
{
    if (!state) return 0;
    int n = state->ime_backspaces;
    state->ime_backspaces = 0;
    return n;
}

/* ── Virtual gamepads ───────────────────────────────────────────────
 *
 * Process-global, like GLFW's joysticks, because the vio_gamepad_* readers
 * take no context. vio_physical_hidden counts running replays. */

static vio_gamepad_snapshot vio_vpads[VIO_GAMEPAD_SLOTS];
static int vio_physical_hidden = 0;

static int vio_pad_id_ok(int id)
{
    return id >= 0 && id < VIO_GAMEPAD_SLOTS;
}

const vio_gamepad_snapshot *vio_virtual_gamepad_get(int id)
{
    if (!vio_pad_id_ok(id) || !vio_vpads[id].connected) return NULL;
    return &vio_vpads[id];
}

void vio_virtual_gamepad_connect(int id, const char *name)
{
    if (!vio_pad_id_ok(id)) return;
    vio_gamepad_snapshot *p = &vio_vpads[id];
    memset(p, 0, sizeof(*p));
    p->connected = 1;
    snprintf(p->name, sizeof(p->name), "%s", name ? name : "Virtual Gamepad");
    p->axes[VIO_GAMEPAD_AXIS_LEFT_TRIGGER]  = -1.0f;
    p->axes[VIO_GAMEPAD_AXIS_RIGHT_TRIGGER] = -1.0f;
}

void vio_virtual_gamepad_disconnect(int id)
{
    if (!vio_pad_id_ok(id)) return;
    memset(&vio_vpads[id], 0, sizeof(vio_vpads[id]));
}

int vio_virtual_gamepad_set_button(int id, int button, int pressed)
{
    if (!vio_virtual_gamepad_get(id) || button < 0 || button >= VIO_GAMEPAD_BUTTON_COUNT) return 0;
    vio_vpads[id].buttons[button] = pressed ? 1 : 0;
    return 1;
}

int vio_virtual_gamepad_set_axis(int id, int axis, double value)
{
    if (!vio_virtual_gamepad_get(id) || axis < 0 || axis >= VIO_GAMEPAD_AXIS_COUNT) return 0;
    if (value < -1.0) value = -1.0;
    if (value > 1.0)  value = 1.0;
    vio_vpads[id].axes[axis] = (float)value;
    return 1;
}

void vio_virtual_gamepads_reset(void)
{
    memset(vio_vpads, 0, sizeof(vio_vpads));
    vio_physical_hidden = 0;
}

int vio_gamepad_physical_hidden(void)
{
    return vio_physical_hidden > 0;
}

int vio_gamepad_read(int id, vio_gamepad_snapshot *out)
{
    memset(out, 0, sizeof(*out));
    if (!vio_pad_id_ok(id)) return 0;

    if (vio_vpads[id].connected) {
        *out = vio_vpads[id];
        return 1;
    }
    if (vio_physical_hidden) return 0;

#ifdef HAVE_GLFW
    GLFWgamepadstate gs;
    if (glfwJoystickPresent(id) && glfwGetGamepadState(id, &gs)) {
        const char *name = glfwGetGamepadName(id);
        out->connected = 1;
        snprintf(out->name, sizeof(out->name), "%s", name ? name : "");
        for (int i = 0; i < VIO_GAMEPAD_BUTTON_COUNT; i++) {
            out->buttons[i] = gs.buttons[i] == GLFW_PRESS;
        }
        for (int i = 0; i < VIO_GAMEPAD_AXIS_COUNT; i++) {
            out->axes[i] = gs.axes[i];
        }
    }
#endif
    return out->connected;
}

/* ── Record / replay ────────────────────────────────────────────────
 *
 * The clock is vio_poll_events: tick N is the Nth poll since the recording
 * (or replay) started, tick 0 everything before the first one. OS events
 * arrive inside vio_poll_events, so a replay that delivers tick N's events at
 * the Nth poll hands the game the same input at the same point of its loop.
 * Gamepads are polled state, not events: a recording samples them once per
 * poll and stores the changes. */

void vio_input_event_list_free(vio_input_event_list *list)
{
    for (size_t i = 0; i < list->count; i++) {
        if (list->items[i].name) {
            zend_string_release(list->items[i].name);
        }
    }
    if (list->items) {
        efree(list->items);
    }
    memset(list, 0, sizeof(*list));
}

int vio_input_event_list_push(vio_input_event_list *list, const vio_input_event *ev)
{
    if (list->count == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 256;
        if (cap > ((size_t)-1) / sizeof(vio_input_event)) return 0;
        list->items = safe_erealloc(list->items, cap, sizeof(vio_input_event), 0);
        list->cap = cap;
    }
    list->items[list->count++] = *ev;
    return 1;
}

static void vio_input_rec_pad(vio_input_state *state, int type, int pad, int b, int c, double x, zend_string *name)
{
    vio_input_event ev = {0};
    ev.type = type;
    ev.a = pad; ev.b = b; ev.c = c; ev.x = x;
    ev.name = name;
    ev.tick = state->poll_tick - state->record_base;
    vio_input_event_list_push(&state->record, &ev);
}

/* Record how every gamepad changed since the last sample. A pad that appears
 * is recorded as connected in its default state, then diffed against it. */
static void vio_input_record_gamepads(vio_input_state *state)
{
    for (int id = 0; id < VIO_GAMEPAD_SLOTS; id++) {
        vio_gamepad_snapshot now;
        vio_gamepad_snapshot *last = &state->record_pads[id];
        vio_gamepad_read(id, &now);

        if (!now.connected) {
            if (last->connected) {
                vio_input_rec_pad(state, VIO_INPUT_EV_PAD_DISCONNECT, id, 0, 0, 0.0, NULL);
                memset(last, 0, sizeof(*last));
            }
            continue;
        }
        if (!last->connected) {
            vio_input_rec_pad(state, VIO_INPUT_EV_PAD_CONNECT, id, 0, 0, 0.0,
                              zend_string_init(now.name, strlen(now.name), 0));
            memset(last, 0, sizeof(*last));
            last->connected = 1;
            last->axes[VIO_GAMEPAD_AXIS_LEFT_TRIGGER]  = -1.0f;
            last->axes[VIO_GAMEPAD_AXIS_RIGHT_TRIGGER] = -1.0f;
        }
        for (int i = 0; i < VIO_GAMEPAD_BUTTON_COUNT; i++) {
            if (now.buttons[i] != last->buttons[i]) {
                vio_input_rec_pad(state, VIO_INPUT_EV_PAD_BUTTON, id, i, now.buttons[i], 0.0, NULL);
            }
        }
        for (int i = 0; i < VIO_GAMEPAD_AXIS_COUNT; i++) {
            if (now.axes[i] != last->axes[i]) {
                vio_input_rec_pad(state, VIO_INPUT_EV_PAD_AXIS, id, i, 0, (double)now.axes[i], NULL);
            }
        }
        *last = now;
    }
}

void vio_input_record_start(vio_input_state *state)
{
    vio_input_event_list_free(&state->record);
    memset(state->record_pads, 0, sizeof(state->record_pads));
    state->recording   = 1;
    state->record_base = state->poll_tick;
    /* Pads already connected are part of the starting state. */
    vio_input_record_gamepads(state);
}

void vio_input_record_stop(vio_input_state *state, vio_input_event_list *out)
{
    if (state->recording) {
        vio_input_event end = {0};
        end.type = VIO_INPUT_EV_END;
        vio_input_rec(state, &end);
    }
    state->recording = 0;
    *out = state->record;
    memset(&state->record, 0, sizeof(state->record));
}

static void vio_input_deliver(vio_input_state *state, const vio_input_event *ev)
{
    switch (ev->type) {
        case VIO_INPUT_EV_KEY:    vio_input_key_event(state, ev->a, ev->b, ev->c); break;
        case VIO_INPUT_EV_CHAR:   vio_input_emit_char(state, (unsigned int)ev->a); break;
        case VIO_INPUT_EV_CURSOR: vio_input_cursor_event(state, ev->x, ev->y); break;
        case VIO_INPUT_EV_BUTTON: vio_input_button_event(state, ev->a, ev->b); break;
        case VIO_INPUT_EV_SCROLL: vio_input_scroll_event(state, ev->x, ev->y); break;
        case VIO_INPUT_EV_TOUCH:
            switch (ev->b) {
                case VIO_TOUCH_BEGAN:     vio_input_touch_began(state, ev->id, ev->x, ev->y); break;
                case VIO_TOUCH_MOVED:     vio_input_touch_moved(state, ev->id, ev->x, ev->y); break;
                case VIO_TOUCH_ENDED:     vio_input_touch_ended(state, ev->id); break;
                case VIO_TOUCH_CANCELLED: vio_input_touch_cancelled(state, ev->id); break;
                default: break;
            }
            break;
        case VIO_INPUT_EV_PAD_CONNECT:
            vio_virtual_gamepad_connect(ev->a, ev->name ? ZSTR_VAL(ev->name) : NULL);
            if (vio_pad_id_ok(ev->a)) state->replay_pads |= 1u << ev->a;
            break;
        case VIO_INPUT_EV_PAD_DISCONNECT:
            vio_virtual_gamepad_disconnect(ev->a);
            if (vio_pad_id_ok(ev->a)) state->replay_pads &= ~(1u << ev->a);
            break;
        case VIO_INPUT_EV_PAD_BUTTON: vio_virtual_gamepad_set_button(ev->a, ev->b, ev->c); break;
        case VIO_INPUT_EV_PAD_AXIS:   vio_virtual_gamepad_set_axis(ev->a, ev->b, ev->x); break;
        default: break;
    }
}

/* Deliver every replay event due at the current tick. The event is copied
 * first: a PHP callback it fires may stop or restart the replay, which frees
 * the list underneath. */
static void vio_input_replay_due(vio_input_state *state)
{
    while (state->replaying && state->replay_pos < state->replay.count) {
        uint32_t now = state->poll_tick - state->replay_base;
        if (state->replay.items[state->replay_pos].tick > now) break;
        vio_input_event ev = state->replay.items[state->replay_pos++];
        vio_input_deliver(state, &ev);
    }
}

/* Stable sort by tick: events of one tick keep their order, which matters
 * (press before release). Insertion sort - recordings are already sorted and
 * hand-written scripts are short or nearly sorted. */
static void vio_input_sort_events(vio_input_event_list *list)
{
    for (size_t i = 1; i < list->count; i++) {
        vio_input_event k = list->items[i];
        size_t j = i;
        while (j > 0 && list->items[j - 1].tick > k.tick) {
            list->items[j] = list->items[j - 1];
            j--;
        }
        list->items[j] = k;
    }
}

void vio_input_replay_start(vio_input_state *state, vio_input_event_list *events)
{
    vio_input_replay_stop(state);

    vio_input_sort_events(events);
    state->replay      = *events;
    memset(events, 0, sizeof(*events));
    state->replaying   = 1;
    state->replay_base = state->poll_tick;
    state->replay_pos  = 0;
    state->replay_pads = 0;
    vio_physical_hidden++;

    vio_input_replay_due(state);
}

void vio_input_replay_stop(vio_input_state *state)
{
    if (!state->replaying) return;
    state->replaying = 0;
    for (int id = 0; id < VIO_GAMEPAD_SLOTS; id++) {
        if (state->replay_pads & (1u << id)) {
            vio_virtual_gamepad_disconnect(id);
        }
    }
    state->replay_pads = 0;
    if (vio_physical_hidden > 0) vio_physical_hidden--;
    vio_input_event_list_free(&state->replay);
    state->replay_pos = 0;
}

void vio_input_poll_begin(vio_input_state *state)
{
    state->poll_tick++;
}

void vio_input_poll_end(vio_input_state *state)
{
    if (state->replaying) {
        /* A finished replay ends one poll after its last events, so the
         * final input stays visible for a frame before the OS takes over. */
        if (state->replay_pos >= state->replay.count) {
            vio_input_replay_stop(state);
        } else {
            vio_input_replay_due(state);
        }
    }
    if (state->recording) {
        vio_input_record_gamepads(state);
    }
}