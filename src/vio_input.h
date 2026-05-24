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

    /* Touch points. Slots with id==0 are inactive. Indexing is not stable
     * across frames - iterate touch_count slots and skip inactive ones,
     * or look up by id. */
    vio_touch touches[VIO_MAX_TOUCHES];
    int       touch_count; /* number of slots currently in use (id != 0) */
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

#ifdef HAVE_GLFW
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

/* Install GLFW callbacks on a window, associating it with an input state */
void vio_input_install_callbacks(GLFWwindow *window, vio_input_state *state);
#endif

#endif /* VIO_INPUT_H */
