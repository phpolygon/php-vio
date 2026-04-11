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

static void glfw_char_callback(GLFWwindow *window, unsigned int codepoint)
{
    vio_input_state *state = (vio_input_state *)glfwGetWindowUserPointer(window);
    if (!state) return;

    /* Append UTF-8 encoded codepoint to per-frame buffer */
    char encoded[4];
    int len = vio_encode_utf8(codepoint, encoded);
    if (len > 0 && state->char_buffer_len + len < (int)sizeof(state->char_buffer)) {
        memcpy(state->char_buffer + state->char_buffer_len, encoded, len);
        state->char_buffer_len += len;
    }

    /* Fire PHP callback if registered */
    if (state->has_char_callback) {
        zval retval, args[1];
        ZVAL_LONG(&args[0], (zend_long)codepoint);

        if (call_user_function(NULL, NULL, &state->on_char_callback, &retval, 1, args) == SUCCESS) {
            zval_ptr_dtor(&retval);
        }
    }
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
