/*
 * php-vio - Input state management implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_input.h"
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

#ifdef HAVE_GLFW

static void glfw_key_callback(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    vio_input_state *state = (vio_input_state *)glfwGetWindowUserPointer(window);
    if (!state) return;

    (void)scancode;

    if (key >= 0 && key <= VIO_KEY_LAST) {
        state->keys[key] = (action != GLFW_RELEASE) ? 1 : 0;
    }

    /* Fire PHP callback if registered */
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

static void glfw_char_callback(GLFWwindow *window, unsigned int codepoint)
{
    vio_input_state *state = (vio_input_state *)glfwGetWindowUserPointer(window);
    if (!state) return;
    vio_input_emit_char(state, codepoint);
}

static void glfw_cursor_pos_callback(GLFWwindow *window, double xpos, double ypos)
{
    vio_input_state *state = (vio_input_state *)glfwGetWindowUserPointer(window);
    if (!state) return;

    state->mouse_x = xpos;
    state->mouse_y = ypos;
}

static void glfw_mouse_button_callback(GLFWwindow *window, int button, int action, int mods)
{
    vio_input_state *state = (vio_input_state *)glfwGetWindowUserPointer(window);
    if (!state) return;

    (void)mods;

    if (button >= 0 && button <= VIO_MOUSE_LAST) {
        state->mouse_buttons[button] = (action != GLFW_RELEASE) ? 1 : 0;
    }
}

static void glfw_scroll_callback(GLFWwindow *window, double xoffset, double yoffset)
{
    vio_input_state *state = (vio_input_state *)glfwGetWindowUserPointer(window);
    if (!state) return;

    state->scroll_x += xoffset;
    state->scroll_y += yoffset;
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

int vio_input_touch_began(vio_input_state *state, unsigned long long id, double x, double y)
{
    id = vio_touch_normalize_id(id);

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
    int idx = vio_touch_find_slot(state, id);
    if (idx < 0) return;
    state->touches[idx].phase = VIO_TOUCH_ENDED;
    /* Touch -> mouse emulation: finger up = primary mouse button release. */
    state->mouse_buttons[0] = 0;
}

void vio_input_touch_cancelled(vio_input_state *state, unsigned long long id)
{
    id = vio_touch_normalize_id(id);
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
