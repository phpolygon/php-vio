/*
 * php-vio - Input state management
 */

#ifndef VIO_INPUT_H
#define VIO_INPUT_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "../include/vio_constants.h"
#include <stdint.h>

/* Max simultaneous touch points. iPad Pro supports 11; we round up nothing
 * because the slot array is fixed-size and small. Touches beyond this count
 * are dropped silently by the push API. */
#define VIO_MAX_TOUCHES 11

typedef enum {
    VIO_TOUCH_INACTIVE   = 0,
    VIO_TOUCH_BEGAN      = 1,
    VIO_TOUCH_MOVED      = 2,
    VIO_TOUCH_STATIONARY = 3,
    VIO_TOUCH_ENDED      = 4,
    VIO_TOUCH_CANCELLED  = 5,
} vio_touch_phase;

typedef struct _vio_touch {
    /* Platform-stable identifier across frames. On iOS this is the
     * `(uintptr_t)UITouch *` cast; on macOS with NSTouch the equivalent.
     * 0 means "slot is unused" (sentinel; real ids start at 1 - we
     * remap 0 -> 1 internally if a backend hands us a zero id). */
    unsigned long long id;
    double             x, y;
    double             prev_x, prev_y;
    vio_touch_phase    phase;
} vio_touch;

/* ── Gamepads ───────────────────────────────────────────────────────
 *
 * Gamepads are process-global, like GLFW joysticks: the vio_gamepad_* functions
 * take no context. A virtual gamepad in slot N overrides the physical joystick N
 * for every reader until it disconnects. */
#define VIO_GAMEPAD_SLOTS        16  /* GLFW_JOYSTICK_1 .. GLFW_JOYSTICK_LAST */
#define VIO_GAMEPAD_BUTTON_COUNT 15  /* VIO_GAMEPAD_A .. VIO_GAMEPAD_DPAD_LEFT */
#define VIO_GAMEPAD_AXIS_COUNT   6   /* VIO_GAMEPAD_AXIS_LEFT_X .. _RIGHT_TRIGGER */

typedef struct _vio_gamepad_snapshot {
    int           connected;
    char          name[64];
    unsigned char buttons[VIO_GAMEPAD_BUTTON_COUNT];
    float         axes[VIO_GAMEPAD_AXIS_COUNT];
} vio_gamepad_snapshot;

/* ── Recorded input events ─────────────────────────────────────────
 *
 * Field use per type:
 *   KEY            a = key, b = action, c = mods
 *   CHAR           a = codepoint
 *   CURSOR         x, y = raw cursor position
 *   BUTTON         a = mouse button, b = action
 *   SCROLL         x, y = offsets
 *   TOUCH          id, b = phase, x, y
 *   PAD_CONNECT    a = gamepad, name
 *   PAD_DISCONNECT a = gamepad
 *   PAD_BUTTON     a = gamepad, b = button, c = pressed
 *   PAD_AXIS       a = gamepad, b = axis, x = value
 *   END            marks the length of a recording
 * tick counts vio_poll_events calls since the recording started. */
typedef enum {
    VIO_INPUT_EV_KEY = 1,
    VIO_INPUT_EV_CHAR,
    VIO_INPUT_EV_CURSOR,
    VIO_INPUT_EV_BUTTON,
    VIO_INPUT_EV_SCROLL,
    VIO_INPUT_EV_TOUCH,
    VIO_INPUT_EV_PAD_CONNECT,
    VIO_INPUT_EV_PAD_DISCONNECT,
    VIO_INPUT_EV_PAD_BUTTON,
    VIO_INPUT_EV_PAD_AXIS,
    VIO_INPUT_EV_END,
} vio_input_event_type;

typedef struct _vio_input_event {
    uint32_t           tick;
    int                type;
    int                a, b, c;
    unsigned long long id;
    double             x, y;
    zend_string       *name; /* owned, PAD_CONNECT only */
} vio_input_event;

typedef struct _vio_input_event_list {
    vio_input_event *items;
    size_t           count;
    size_t           cap;
} vio_input_event_list;

typedef struct _vio_input_state {
    int    keys[VIO_KEY_LAST + 1];
    int    keys_prev[VIO_KEY_LAST + 1];
    double mouse_x, mouse_y;
    double mouse_prev_x, mouse_prev_y;
    int    mouse_buttons[VIO_MOUSE_LAST + 1];
    double scroll_x, scroll_y;
    zval   on_key_callback;
    zval   on_resize_callback;
    zval   on_char_callback;
    int    has_key_callback;
    int    has_resize_callback;
    int    has_char_callback;
    char   char_buffer[256];
    int    char_buffer_len;
    /* iOS soft-keyboard input. The UIKeyInput view (UIKit main thread) enqueues
     * typed codepoints / bumps the backspace counter; the render thread drains
     * them in vio_poll_events (vio_input_drain_ime), feeding the same char path
     * as the desktop GLFW callback. volatile: cross-thread, lock-free (human
     * typing speed makes the race window negligible, matching the touch model). */
    unsigned int  ime_codepoints[128];
    volatile int  ime_cp_count;
    volatile int  ime_backspaces;

    /* Touch points. Slots with id==0 are inactive. Indexing is not stable
     * across frames - iterate touch_count slots and skip inactive ones,
     * or look up by id. */
    vio_touch touches[VIO_MAX_TOUCHES];
    int       touch_count; /* number of slots currently in use (id != 0) */

    /* Record/replay clock: incremented once per vio_poll_events. */
    uint32_t poll_tick;

    int                  recording;
    uint32_t             record_base;
    vio_input_event_list record;
    vio_gamepad_snapshot record_pads[VIO_GAMEPAD_SLOTS]; /* last recorded pad state */

    int                  replaying;
    uint32_t             replay_base;
    size_t               replay_pos;
    vio_input_event_list replay;
    unsigned int         replay_pads; /* bitmask of virtual pads the replay connected */
} vio_input_state;

/* Swap previous/current state (call at start of each frame) */
void vio_input_update(vio_input_state *state);

/* Initialize input state */
void vio_input_init(vio_input_state *state);

/* Cleanup input state (release zval callbacks) */
void vio_input_shutdown(vio_input_state *state);

/* ── Touch push API ─────────────────────────────────────────────────
 *
 * Platform backends (iOS UIView, future Android, future trackpad-touch)
 * call these to feed touch events into the input state. The functions
 * never block, never allocate, and are safe to call from the main thread
 * outside of a frame boundary.
 *
 * Identity: touches are tracked by `id`. Within one finger's lifetime
 * (down -> up) the same id is reported. Once a touch ends or cancels,
 * the next frame's vio_input_update() clears the slot, and the id may
 * be reused by a different finger later.
 *
 * id == 0 is reserved as "inactive slot" sentinel - if a backend passes 0,
 * it is remapped to 1 internally. */

/* Touch went down. Returns slot index or -1 if the touch array is full. */
int vio_input_touch_began(vio_input_state *state, unsigned long long id, double x, double y);

/* Touch moved. No-op if id is unknown. */
void vio_input_touch_moved(vio_input_state *state, unsigned long long id, double x, double y);

/* Touch ended cleanly (finger lifted). Marks slot phase=ENDED; cleared next update. */
void vio_input_touch_ended(vio_input_state *state, unsigned long long id);

/* Touch cancelled (system pre-empted - e.g. notification, multitasking gesture).
 * Behaves like ENDED but with cancellation semantics for the consumer. */
void vio_input_touch_cancelled(vio_input_state *state, unsigned long long id);

/* iOS soft-keyboard text entry (UIKit main thread enqueues; render thread
 * drains). push_codepoint queues a typed Unicode codepoint; ime_backspace bumps
 * the backspace counter. drain_ime (called each frame from vio_poll_events on
 * the render thread) emits the queued codepoints through the normal char path
 * (char buffer + on_char callback). take_ime_backspaces reads-and-clears the
 * backspace count. */
void vio_input_push_codepoint(vio_input_state *state, unsigned int codepoint);
void vio_input_drain_ime(vio_input_state *state);
void vio_input_ime_backspace(vio_input_state *state);
int  vio_input_take_ime_backspaces(vio_input_state *state);

/* Emit one typed codepoint into the per-frame char buffer + on_char callback.
 * Shared by the GLFW char callback and the iOS IME drain. PHP/render thread. */
void vio_input_emit_char(vio_input_state *state, unsigned int codepoint);

/* Event funnels shared by the GLFW callbacks and vio_inject_*: an injected
 * event takes the same path as a real one. key_event fires the on_key callback.
 * Coordinates are in the raw cursor space GLFW reports. PHP/render thread. */
void vio_input_key_event(vio_input_state *state, int key, int action, int mods);
void vio_input_cursor_event(vio_input_state *state, double x, double y);
void vio_input_button_event(vio_input_state *state, int button, int action);
void vio_input_scroll_event(vio_input_state *state, double dx, double dy);

/* ── Virtual gamepads (process-global) ─────────────────────────────── */

/* The virtual pad in slot id, or NULL when none is connected there. */
const vio_gamepad_snapshot *vio_virtual_gamepad_get(int id);
/* Connect (or reset) a virtual pad: buttons released, sticks centred,
 * triggers at -1.0 (released, GLFW convention). */
void vio_virtual_gamepad_connect(int id, const char *name);
void vio_virtual_gamepad_disconnect(int id);
/* Return 0 when no virtual pad is connected at id. value is clamped to [-1, 1]. */
int  vio_virtual_gamepad_set_button(int id, int button, int pressed);
int  vio_virtual_gamepad_set_axis(int id, int axis, double value);
/* Disconnect every virtual pad and unhide physical ones (request shutdown). */
void vio_virtual_gamepads_reset(void);
/* Non-zero while a replay runs: physical joysticks are hidden so a human's
 * controller cannot disturb a replayed run. */
int  vio_gamepad_physical_hidden(void);
/* Effective gamepad-layout state of slot id (virtual pad, else physical
 * mapped gamepad). Returns out->connected. */
int  vio_gamepad_read(int id, vio_gamepad_snapshot *out);

/* ── Record / replay ───────────────────────────────────────────────── */

void vio_input_event_list_free(vio_input_event_list *list);
/* Copies ev; takes ownership of ev->name. Returns 0 on overflow. */
int  vio_input_event_list_push(vio_input_event_list *list, const vio_input_event *ev);

/* Brackets vio_poll_events: poll_begin advances the tick, poll_end delivers
 * due replay events and samples gamepads for a running recording. */
void vio_input_poll_begin(vio_input_state *state);
void vio_input_poll_end(vio_input_state *state);

void vio_input_record_start(vio_input_state *state);
/* Ends the recording, appends an END marker and moves the events into out. */
void vio_input_record_stop(vio_input_state *state, vio_input_event_list *out);
/* Takes ownership of events (sorted by tick, stable) and delivers the tick-0
 * ones immediately. Stops a running replay first. */
void vio_input_replay_start(vio_input_state *state, vio_input_event_list *events);
void vio_input_replay_stop(vio_input_state *state);

#ifdef HAVE_GLFW
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

/* Install GLFW callbacks on a window, associating it with an input state */
void vio_input_install_callbacks(GLFWwindow *window, vio_input_state *state);
#endif

#endif /* VIO_INPUT_H */
