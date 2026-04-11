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
} vio_input_state;

/* Swap previous/current state (call at start of each frame) */
void vio_input_update(vio_input_state *state);

/* Initialize input state */
void vio_input_init(vio_input_state *state);

/* Cleanup input state (release zval callbacks) */
void vio_input_shutdown(vio_input_state *state);

#ifdef HAVE_GLFW
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

/* Install GLFW callbacks on a window, associating it with an input state */
void vio_input_install_callbacks(GLFWwindow *window, vio_input_state *state);
#endif

#endif /* VIO_INPUT_H */
