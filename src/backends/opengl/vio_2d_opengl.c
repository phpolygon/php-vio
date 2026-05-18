/*
 * php-vio - 2D rendering, OpenGL backend implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#ifdef HAVE_GLFW

#include <glad/glad.h>
#include <stddef.h>

#include "vio_2d_opengl.h"
#include "vio_opengl.h"
#include "../../shaders/shaders_2d.h"

int vio_2d_opengl_init(vio_2d_opengl_state *state, int vertex_capacity)
{
    state->shader_shapes = vio_opengl_compile_shader_source(
        vio_2d_vertex_shader, vio_2d_fragment_shader_shapes);
    state->shader_sprites = vio_opengl_compile_shader_source(
        vio_2d_vertex_shader, vio_2d_fragment_shader_sprites);

    if (!state->shader_shapes || !state->shader_sprites) {
        php_error_docref(NULL, E_WARNING, "Failed to compile 2D shaders");
        return -1;
    }

    glGenVertexArrays(1, &state->vao);
    glGenBuffers(1, &state->vbo);

    glBindVertexArray(state->vao);
    glBindBuffer(GL_ARRAY_BUFFER, state->vbo);
    state->vbo_capacity = vertex_capacity;
    glBufferData(GL_ARRAY_BUFFER,
        sizeof(vio_2d_vertex) * state->vbo_capacity, NULL, GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(vio_2d_vertex),
        (void *)offsetof(vio_2d_vertex, x));
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vio_2d_vertex),
        (void *)offsetof(vio_2d_vertex, u));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(vio_2d_vertex),
        (void *)offsetof(vio_2d_vertex, r));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);
    return 0;
}

void vio_2d_opengl_shutdown(vio_2d_opengl_state *state)
{
    if (state->shader_shapes)  glDeleteProgram(state->shader_shapes);
    if (state->shader_sprites) glDeleteProgram(state->shader_sprites);
    if (state->vbo) glDeleteBuffers(1, &state->vbo);
    if (state->vao) glDeleteVertexArrays(1, &state->vao);
    state->shader_shapes  = 0;
    state->shader_sprites = 0;
    state->vbo            = 0;
    state->vao            = 0;
    state->vbo_capacity   = 0;
}

void vio_2d_opengl_flush(vio_2d_state *state)
{
    vio_2d_opengl_state *gl = (vio_2d_opengl_state *)state->opengl_state;
    if (!gl) return;

    /* Upload vertex data — grow GPU buffer if CPU buffer outgrew it */
    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    if (state->vertex_count > gl->vbo_capacity) {
        gl->vbo_capacity = state->vertex_capacity;
        glBufferData(GL_ARRAY_BUFFER,
            sizeof(vio_2d_vertex) * gl->vbo_capacity, NULL, GL_DYNAMIC_DRAW);
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0,
        sizeof(vio_2d_vertex) * state->vertex_count, state->vertices);

    /* Save GL state */
    GLboolean depth_was_enabled;
    glGetBooleanv(GL_DEPTH_TEST, &depth_was_enabled);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(gl->vao);

    unsigned int current_shader = 0;
    unsigned int current_texture = 0;
    GLint proj_loc;

    int scissor_active = 0;
    float sc_x = 0, sc_y = 0, sc_w = 0, sc_h = 0;

    for (int i = 0; i < state->item_count; i++) {
        vio_2d_item *item = &state->items[i];

        if (item->scissor.enabled) {
            if (!scissor_active ||
                item->scissor.x != sc_x || item->scissor.y != sc_y ||
                item->scissor.w != sc_w || item->scissor.h != sc_h) {
                if (!scissor_active) {
                    glEnable(GL_SCISSOR_TEST);
                    scissor_active = 1;
                }
                sc_x = item->scissor.x; sc_y = item->scissor.y;
                sc_w = item->scissor.w; sc_h = item->scissor.h;
                float sx = (state->width > 0) ? (float)state->fb_width / (float)state->width : 1.0f;
                float sy = (state->height > 0) ? (float)state->fb_height / (float)state->height : 1.0f;
                glScissor((GLint)(sc_x * sx),
                          (GLint)((state->height - sc_y - sc_h) * sy),
                          (GLsizei)(sc_w * sx), (GLsizei)(sc_h * sy));
            }
        } else if (scissor_active) {
            glDisable(GL_SCISSOR_TEST);
            scissor_active = 0;
        }

        unsigned int wanted_shader = (item->texture_id > 0)
            ? gl->shader_sprites : gl->shader_shapes;

        if (wanted_shader != current_shader) {
            current_shader = wanted_shader;
            glUseProgram(current_shader);
            proj_loc = glGetUniformLocation(current_shader, "uProjection");
            glUniformMatrix4fv(proj_loc, 1, GL_FALSE, state->projection);

            if (item->texture_id > 0) {
                GLint tex_loc = glGetUniformLocation(current_shader, "uTexture");
                glUniform1i(tex_loc, 0);
            }
        }

        if (item->texture_id != current_texture) {
            current_texture = item->texture_id;
            if (current_texture > 0) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, current_texture);
            }
        }

        glDrawArrays(GL_TRIANGLES, item->vertex_start, item->vertex_count);
    }

    if (scissor_active) glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(0);
    glUseProgram(0);
    if (depth_was_enabled) glEnable(GL_DEPTH_TEST);
}

#endif /* HAVE_GLFW */
