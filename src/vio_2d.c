/*
 * php-vio - 2D Rendering System implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_2d.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef HAVE_GLFW
#include <glad/glad.h>
#include "backends/opengl/vio_opengl.h"
#include "shaders/shaders_2d.h"
#endif

/* ── Orthographic projection matrix ──────────────────────────────── */

static void vio_2d_ortho(float *m, float left, float right, float bottom, float top)
{
    memset(m, 0, sizeof(float) * 16);
    m[0]  =  2.0f / (right - left);
    m[5]  =  2.0f / (top - bottom);
    m[10] = -1.0f;
    m[12] = -(right + left) / (right - left);
    m[13] = -(top + bottom) / (top - bottom);
    m[15] =  1.0f;
}

/* ── Comparison for qsort: z-order then texture ──────────────────── */

static int vio_2d_item_cmp(const void *a, const void *b)
{
    const vio_2d_item *ia = (const vio_2d_item *)a;
    const vio_2d_item *ib = (const vio_2d_item *)b;

    if (ia->z_order < ib->z_order) return -1;
    if (ia->z_order > ib->z_order) return  1;

    /* Same z: group by texture to minimize state changes */
    if (ia->texture_id < ib->texture_id) return -1;
    if (ia->texture_id > ib->texture_id) return  1;

    return 0;
}

/* ── Transform stack ─────────────────────────────────────────────── */

/* Multiply two 2D affine matrices: result = A * B
 * Matrix layout: [a,b,c,d,e,f] where:
 * | a b e |
 * | c d f |
 * | 0 0 1 |
 */
static void vio_2d_mat_mul(const float *A, const float *B, float *out)
{
    out[0] = A[0]*B[0] + A[1]*B[2];
    out[1] = A[0]*B[1] + A[1]*B[3];
    out[2] = A[2]*B[0] + A[3]*B[2];
    out[3] = A[2]*B[1] + A[3]*B[3];
    out[4] = A[4]*B[0] + A[5]*B[2] + B[4];
    out[5] = A[4]*B[1] + A[5]*B[3] + B[5];
}

void vio_2d_push_transform(vio_2d_state *state, float a, float b, float c, float d, float e, float f)
{
    if (state->transform_depth >= VIO_2D_MAX_TRANSFORM_STACK) {
        php_error_docref(NULL, E_WARNING, "Transform stack overflow (max %d)", VIO_2D_MAX_TRANSFORM_STACK);
        return;
    }

    float incoming[6] = {a, b, c, d, e, f};
    float result[6];

    if (state->transform_depth > 0) {
        /* Multiply: incoming * stack (new transform wraps existing ones) */
        vio_2d_mat_mul(incoming, state->transform_stack[state->transform_depth - 1], result);
    } else {
        memcpy(result, incoming, sizeof(float) * 6);
    }

    memcpy(state->transform_stack[state->transform_depth], result, sizeof(float) * 6);
    state->transform_depth++;
}

void vio_2d_pop_transform(vio_2d_state *state)
{
    if (state->transform_depth <= 0) {
        php_error_docref(NULL, E_WARNING, "Transform stack underflow");
        return;
    }
    state->transform_depth--;
}

void vio_2d_apply_transform(vio_2d_state *state, float *x, float *y)
{
    if (state->transform_depth <= 0) return;

    float *m = state->transform_stack[state->transform_depth - 1];
    float ox = *x, oy = *y;
    *x = m[0] * ox + m[1] * oy + m[4];
    *y = m[2] * ox + m[3] * oy + m[5];
}

/* ── Scissor stack ──────────────────────────────────────────────── */

void vio_2d_push_scissor(vio_2d_state *state, float x, float y, float w, float h)
{
    if (state->scissor_depth >= VIO_2D_MAX_SCISSOR_STACK) {
        php_error_docref(NULL, E_WARNING, "Scissor stack overflow (max %d)", VIO_2D_MAX_SCISSOR_STACK);
        return;
    }

    vio_2d_scissor_rect rect = {x, y, w, h, 1};

    /* Intersect with parent scissor if any */
    if (state->scissor_depth > 0) {
        vio_2d_scissor_rect *parent = &state->scissor_stack[state->scissor_depth - 1];
        if (parent->enabled) {
            float ax1 = parent->x, ay1 = parent->y;
            float ax2 = parent->x + parent->w, ay2 = parent->y + parent->h;
            float bx1 = x, by1 = y, bx2 = x + w, by2 = y + h;

            float ix1 = fmaxf(ax1, bx1), iy1 = fmaxf(ay1, by1);
            float ix2 = fminf(ax2, bx2), iy2 = fminf(ay2, by2);

            if (ix1 < ix2 && iy1 < iy2) {
                rect.x = ix1; rect.y = iy1;
                rect.w = ix2 - ix1; rect.h = iy2 - iy1;
            } else {
                /* No intersection - zero-size scissor */
                rect.x = x; rect.y = y;
                rect.w = 0; rect.h = 0;
            }
        }
    }

    state->scissor_stack[state->scissor_depth] = rect;
    state->scissor_depth++;
}

void vio_2d_pop_scissor(vio_2d_state *state)
{
    if (state->scissor_depth <= 0) {
        php_error_docref(NULL, E_WARNING, "Scissor stack underflow");
        return;
    }
    state->scissor_depth--;
}

vio_2d_scissor_rect vio_2d_current_scissor(vio_2d_state *state)
{
    if (state->scissor_depth > 0) {
        return state->scissor_stack[state->scissor_depth - 1];
    }
    vio_2d_scissor_rect none = {0, 0, 0, 0, 0};
    return none;
}

/* ── Public API ──────────────────────────────────────────────────── */

int vio_2d_init(vio_2d_state *state, int width, int height)
{
    memset(state, 0, sizeof(vio_2d_state));

    state->vertex_capacity = VIO_2D_INITIAL_VERTICES;
    state->item_capacity   = VIO_2D_INITIAL_ITEMS;
    state->vertices = emalloc(sizeof(vio_2d_vertex) * state->vertex_capacity);
    state->items    = emalloc(sizeof(vio_2d_item) * state->item_capacity);
    state->width    = width;
    state->height   = height;
    state->fb_width  = width;
    state->fb_height = height;

    vio_2d_ortho(state->projection, 0, (float)width, (float)height, 0);

#ifdef HAVE_GLFW
    if (vio_gl.initialized) {
        /* Compile 2D shaders */
        state->shader_shapes = vio_opengl_compile_shader_source(
            vio_2d_vertex_shader, vio_2d_fragment_shader_shapes);
        state->shader_sprites = vio_opengl_compile_shader_source(
            vio_2d_vertex_shader, vio_2d_fragment_shader_sprites);

        if (!state->shader_shapes || !state->shader_sprites) {
            php_error_docref(NULL, E_WARNING, "Failed to compile 2D shaders");
            return -1;
        }

        /* Create VAO/VBO for dynamic batch */
        glGenVertexArrays(1, &state->vao);
        glGenBuffers(1, &state->vbo);

        glBindVertexArray(state->vao);
        glBindBuffer(GL_ARRAY_BUFFER, state->vbo);
        state->vbo_capacity = state->vertex_capacity;
        glBufferData(GL_ARRAY_BUFFER,
            sizeof(vio_2d_vertex) * state->vbo_capacity, NULL, GL_DYNAMIC_DRAW);

        /* Position: location 0, vec2 */
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(vio_2d_vertex),
            (void *)offsetof(vio_2d_vertex, x));
        glEnableVertexAttribArray(0);

        /* TexCoord: location 1, vec2 */
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vio_2d_vertex),
            (void *)offsetof(vio_2d_vertex, u));
        glEnableVertexAttribArray(1);

        /* Color: location 2, vec4 */
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(vio_2d_vertex),
            (void *)offsetof(vio_2d_vertex, r));
        glEnableVertexAttribArray(2);

        glBindVertexArray(0);
    }
#endif

    state->initialized = 1;
    return 0;
}

void vio_2d_shutdown(vio_2d_state *state)
{
    if (!state->initialized) return;

#ifdef HAVE_GLFW
    if (state->shader_shapes) glDeleteProgram(state->shader_shapes);
    if (state->shader_sprites) glDeleteProgram(state->shader_sprites);
    if (state->vbo) glDeleteBuffers(1, &state->vbo);
    if (state->vao) glDeleteVertexArrays(1, &state->vao);
#endif

    if (state->vertices) efree(state->vertices);
    if (state->items) efree(state->items);

    memset(state, 0, sizeof(vio_2d_state));
}

void vio_2d_begin(vio_2d_state *state)
{
    if (!state->initialized) return;
    state->vertex_count     = 0;
    state->item_count       = 0;
    state->transform_depth  = 0;
    state->scissor_depth    = 0;
}

void vio_2d_set_size(vio_2d_state *state, int width, int height)
{
    state->width  = width;
    state->height = height;
    vio_2d_ortho(state->projection, 0, (float)width, (float)height, 0);
}

int vio_2d_push_vertices(vio_2d_state *state, const vio_2d_vertex *verts, int count)
{
    if (!state->initialized) return -1;
    /* Grow buffer if needed */
    while (state->vertex_count + count > state->vertex_capacity) {
        int new_cap = state->vertex_capacity * 2;
        state->vertices = erealloc(state->vertices, sizeof(vio_2d_vertex) * new_cap);
        state->vertex_capacity = new_cap;
    }
    int start = state->vertex_count;
    memcpy(&state->vertices[start], verts, sizeof(vio_2d_vertex) * count);
    state->vertex_count += count;
    return start;
}

void vio_2d_push_item(vio_2d_state *state, vio_2d_item_type type, float z,
                       unsigned int texture_id, int vert_start, int vert_count)
{
    if (!state->initialized) return;
    /* Grow buffer if needed */
    if (state->item_count >= state->item_capacity) {
        int new_cap = state->item_capacity * 2;
        state->items = erealloc(state->items, sizeof(vio_2d_item) * new_cap);
        state->item_capacity = new_cap;
    }

    vio_2d_item *item = &state->items[state->item_count++];
    item->type         = type;
    item->z_order      = z;
    item->texture_id   = texture_id;
    item->vertex_start = vert_start;
    item->vertex_count = vert_count;
    item->scissor      = vio_2d_current_scissor(state);
}

void vio_2d_flush(vio_2d_state *state)
{
    if (!state->initialized || state->item_count == 0) return;

#ifdef HAVE_GLFW
    if (!vio_gl.initialized) return;

    /* Sort items by z-order, then texture */
    qsort(state->items, state->item_count, sizeof(vio_2d_item), vio_2d_item_cmp);

    /* Upload vertex data — grow GPU buffer if CPU buffer outgrew it */
    glBindBuffer(GL_ARRAY_BUFFER, state->vbo);
    if (state->vertex_count > state->vbo_capacity) {
        state->vbo_capacity = state->vertex_capacity;
        glBufferData(GL_ARRAY_BUFFER,
            sizeof(vio_2d_vertex) * state->vbo_capacity, NULL, GL_DYNAMIC_DRAW);
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0,
        sizeof(vio_2d_vertex) * state->vertex_count, state->vertices);

    /* Save GL state */
    GLboolean depth_was_enabled;
    glGetBooleanv(GL_DEPTH_TEST, &depth_was_enabled);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(state->vao);

    unsigned int current_shader = 0;
    unsigned int current_texture = 0;
    GLint proj_loc;

    /* Track scissor state to minimize GL calls */
    int scissor_active = 0;
    float sc_x = 0, sc_y = 0, sc_w = 0, sc_h = 0;

    for (int i = 0; i < state->item_count; i++) {
        vio_2d_item *item = &state->items[i];

        /* Update scissor state */
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
                /* GL scissor operates in framebuffer pixels; scale from logical coords */
                {
                    float sx = (state->width > 0) ? (float)state->fb_width / (float)state->width : 1.0f;
                    float sy = (state->height > 0) ? (float)state->fb_height / (float)state->height : 1.0f;
                    glScissor((GLint)(sc_x * sx),
                              (GLint)((state->height - sc_y - sc_h) * sy),
                              (GLsizei)(sc_w * sx), (GLsizei)(sc_h * sy));
                }
            }
        } else if (scissor_active) {
            glDisable(GL_SCISSOR_TEST);
            scissor_active = 0;
        }

        /* Select shader */
        unsigned int wanted_shader = (item->texture_id > 0)
            ? state->shader_sprites : state->shader_shapes;

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

        /* Bind texture if needed */
        if (item->texture_id != current_texture) {
            current_texture = item->texture_id;
            if (current_texture > 0) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, current_texture);
            }
        }

        /* Draw - all geometry is pre-tessellated as triangles (no GL_LINE_LOOP/GL_LINES
         * since macOS Core Profile caps glLineWidth at 1.0, invisible on Retina) */
        glDrawArrays(GL_TRIANGLES, item->vertex_start, item->vertex_count);
    }

    /* Restore scissor state */
    if (scissor_active) glDisable(GL_SCISSOR_TEST);

    glBindVertexArray(0);
    glUseProgram(0);

    /* Restore depth test */
    if (depth_was_enabled) glEnable(GL_DEPTH_TEST);
#endif
}
