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

#ifdef HAVE_D3D11
#define COBJMACROS
#include <d3d11.h>
#include "vio_2d_d3d11.h"
#include "backends/d3d11/vio_d3d11.h"
#endif

#ifdef HAVE_D3D12
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <d3d12.h>
#include "vio_2d_d3d12.h"
#include "backends/d3d12/vio_d3d12.h"
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

    state->vertices = emalloc(sizeof(vio_2d_vertex) * VIO_2D_MAX_VERTICES);
    state->items    = emalloc(sizeof(vio_2d_item) * VIO_2D_MAX_ITEMS);
    state->width    = width;
    state->height   = height;
    state->fb_width  = width;
    state->fb_height = height;
    state->backend   = VIO_2D_BACKEND_NONE;

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
        glBufferData(GL_ARRAY_BUFFER,
            sizeof(vio_2d_vertex) * VIO_2D_MAX_VERTICES, NULL, GL_DYNAMIC_DRAW);

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
        state->backend = VIO_2D_BACKEND_OPENGL;
    }
#endif

#ifdef HAVE_D3D11
    if (state->backend == VIO_2D_BACKEND_NONE && vio_d3d11.initialized) {
        vio_2d_d3d11_state *d3d = calloc(1, sizeof(vio_2d_d3d11_state));
        if (d3d && vio_2d_d3d11_init(d3d) == 0) {
            state->d3d_state = d3d;
            state->backend = VIO_2D_BACKEND_D3D11;
        } else {
            free(d3d);
        }
    }
#endif

#ifdef HAVE_D3D12
    if (state->backend == VIO_2D_BACKEND_NONE && vio_d3d12.initialized) {
        vio_2d_d3d12_state *d3d = calloc(1, sizeof(vio_2d_d3d12_state));
        if (d3d && vio_2d_d3d12_init(d3d) == 0) {
            state->d3d_state = d3d;
            state->backend = VIO_2D_BACKEND_D3D12;
        } else {
            free(d3d);
        }
    }
#endif

    state->initialized = 1;
    return 0;
}

void vio_2d_shutdown(vio_2d_state *state)
{
    if (!state->initialized) return;

#ifdef HAVE_GLFW
    if (state->backend == VIO_2D_BACKEND_OPENGL) {
        if (state->shader_shapes) glDeleteProgram(state->shader_shapes);
        if (state->shader_sprites) glDeleteProgram(state->shader_sprites);
        if (state->vbo) glDeleteBuffers(1, &state->vbo);
        if (state->vao) glDeleteVertexArrays(1, &state->vao);
    }
#endif

#ifdef HAVE_D3D11
    if (state->backend == VIO_2D_BACKEND_D3D11 && state->d3d_state) {
        vio_2d_d3d11_shutdown((vio_2d_d3d11_state *)state->d3d_state);
        free(state->d3d_state);
    }
#endif

#ifdef HAVE_D3D12
    if (state->backend == VIO_2D_BACKEND_D3D12 && state->d3d_state) {
        vio_2d_d3d12_shutdown((vio_2d_d3d12_state *)state->d3d_state);
        free(state->d3d_state);
    }
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
    if (state->vertex_count + count > VIO_2D_MAX_VERTICES) {
        return -1;
    }
    int start = state->vertex_count;
    memcpy(&state->vertices[start], verts, sizeof(vio_2d_vertex) * count);
    state->vertex_count += count;
    return start;
}

void vio_2d_push_item(vio_2d_state *state, vio_2d_item_type type, float z,
                       unsigned int texture_id, void *backend_texture,
                       int vert_start, int vert_count)
{
    if (!state->initialized || state->item_count >= VIO_2D_MAX_ITEMS) return;

    vio_2d_item *item = &state->items[state->item_count++];
    item->type            = type;
    item->z_order         = z;
    item->texture_id      = texture_id;
    item->backend_texture = backend_texture;
    item->vertex_start    = vert_start;
    item->vertex_count    = vert_count;
    item->scissor         = vio_2d_current_scissor(state);
}

void vio_2d_flush(vio_2d_state *state)
{
    if (!state->initialized || state->item_count == 0) return;

    /* Sort items by z-order, then texture */
    qsort(state->items, state->item_count, sizeof(vio_2d_item), vio_2d_item_cmp);

#ifdef HAVE_GLFW
    if (state->backend == VIO_2D_BACKEND_OPENGL && vio_gl.initialized) {
        /* Upload vertex data */
        glBindBuffer(GL_ARRAY_BUFFER, state->vbo);
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
#endif

#ifdef HAVE_D3D11
    if (state->backend == VIO_2D_BACKEND_D3D11 && state->d3d_state) {
        vio_2d_d3d11_state *d3d = (vio_2d_d3d11_state *)state->d3d_state;
        ID3D11DeviceContext *ctx = vio_d3d11.context;

        /* Upload vertex data */
        D3D11_MAPPED_SUBRESOURCE mapped = {0};
        HRESULT hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)d3d->vbo,
                                              0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) return;
        memcpy(mapped.pData, state->vertices, sizeof(vio_2d_vertex) * state->vertex_count);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)d3d->vbo, 0);

        /* Upload projection matrix */
        hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)d3d->cb,
                                     0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) return;
        memcpy(mapped.pData, state->projection, sizeof(float) * 16);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)d3d->cb, 0);

        /* Save current pipeline state for restore */
        ID3D11RasterizerState   *prev_rs = NULL;
        ID3D11BlendState        *prev_bs = NULL;
        ID3D11DepthStencilState *prev_ds = NULL;
        FLOAT prev_blend_factor[4];
        UINT prev_sample_mask, prev_stencil_ref;
        ID3D11DeviceContext_RSGetState(ctx, &prev_rs);
        ID3D11DeviceContext_OMGetBlendState(ctx, &prev_bs, prev_blend_factor, &prev_sample_mask);
        ID3D11DeviceContext_OMGetDepthStencilState(ctx, &prev_ds, &prev_stencil_ref);

        /* Set 2D pipeline state */
        FLOAT blend_factor[4] = {0, 0, 0, 0};
        ID3D11DeviceContext_OMSetBlendState(ctx, d3d->blend_state, blend_factor, 0xFFFFFFFF);
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, d3d->depth_stencil_state, 0);
        ID3D11DeviceContext_RSSetState(ctx, d3d->rasterizer_state);

        /* Bind vertex buffer */
        UINT stride = sizeof(vio_2d_vertex);
        UINT offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &d3d->vbo, &stride, &offset);
        ID3D11DeviceContext_IASetInputLayout(ctx, d3d->input_layout);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        /* Bind vertex shader + constant buffer */
        ID3D11DeviceContext_VSSetShader(ctx, d3d->vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &d3d->cb);

        /* Bind sampler */
        ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &d3d->sampler);

        /* Set full-window scissor as default (rasterizer has ScissorEnable=TRUE) */
        D3D11_RECT full_scissor = {0, 0, state->fb_width, state->fb_height};
        ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &full_scissor);

        ID3D11PixelShader *current_ps = NULL;
        void *current_tex = NULL;
        float sc_x = 0, sc_y = 0, sc_w = 0, sc_h = 0;
        int scissor_custom = 0;

        for (int i = 0; i < state->item_count; i++) {
            vio_2d_item *item = &state->items[i];

            /* Scissor */
            if (item->scissor.enabled) {
                if (item->scissor.x != sc_x || item->scissor.y != sc_y ||
                    item->scissor.w != sc_w || item->scissor.h != sc_h || !scissor_custom) {
                    sc_x = item->scissor.x; sc_y = item->scissor.y;
                    sc_w = item->scissor.w; sc_h = item->scissor.h;
                    float sx = (state->width > 0) ? (float)state->fb_width / (float)state->width : 1.0f;
                    float sy = (state->height > 0) ? (float)state->fb_height / (float)state->height : 1.0f;
                    D3D11_RECT sr = {
                        (LONG)(sc_x * sx),
                        (LONG)(sc_y * sy),
                        (LONG)((sc_x + sc_w) * sx),
                        (LONG)((sc_y + sc_h) * sy)
                    };
                    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sr);
                    scissor_custom = 1;
                }
            } else if (scissor_custom) {
                ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &full_scissor);
                scissor_custom = 0;
            }

            /* Select pixel shader */
            ID3D11PixelShader *wanted_ps = (item->backend_texture)
                ? d3d->ps_sprites : d3d->ps_shapes;
            if (wanted_ps != current_ps) {
                current_ps = wanted_ps;
                ID3D11DeviceContext_PSSetShader(ctx, current_ps, NULL, 0);
            }

            /* Bind texture */
            if (item->backend_texture != current_tex) {
                current_tex = item->backend_texture;
                if (current_tex) {
                    vio_d3d11_texture *dtex = (vio_d3d11_texture *)current_tex;
                    if (dtex->srv) {
                        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &dtex->srv);
                    }
                }
            }

            /* Draw */
            ID3D11DeviceContext_Draw(ctx, (UINT)item->vertex_count, (UINT)item->vertex_start);
        }

        /* Restore previous state */
        ID3D11DeviceContext_RSSetState(ctx, prev_rs);
        ID3D11DeviceContext_OMSetBlendState(ctx, prev_bs, prev_blend_factor, prev_sample_mask);
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, prev_ds, prev_stencil_ref);
        if (prev_rs) ID3D11RasterizerState_Release(prev_rs);
        if (prev_bs) ID3D11BlendState_Release(prev_bs);
        if (prev_ds) ID3D11DepthStencilState_Release(prev_ds);
    }
#endif

#ifdef HAVE_D3D12
    if (state->backend == VIO_2D_BACKEND_D3D12 && state->d3d_state) {
        vio_2d_d3d12_state *d3d = (vio_2d_d3d12_state *)state->d3d_state;
        ID3D12GraphicsCommandList *cl = vio_d3d12.cmd_list;

        /* Upload vertex data (persistently mapped) */
        memcpy(d3d->vbo_mapped, state->vertices, sizeof(vio_2d_vertex) * state->vertex_count);

        /* Upload projection matrix via cbuffer heap */
        UINT cb_aligned = (sizeof(float) * 16 + 255) & ~255;
        UINT cb_offset = vio_d3d12.cbuffer_heap_offset;
        if (cb_offset + cb_aligned > vio_d3d12.cbuffer_heap_capacity || !vio_d3d12.cbuffer_heap_mapped) {
            return;
        }
        memcpy(vio_d3d12.cbuffer_heap_mapped + cb_offset, state->projection, sizeof(float) * 16);
        D3D12_GPU_VIRTUAL_ADDRESS cb_gpu = vio_d3d12.cbuffer_heap_gpu + cb_offset;
        vio_d3d12.cbuffer_heap_offset = cb_offset + cb_aligned;

        /* Set root signature + cbuffer */
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, vio_d3d12.root_signature);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cl, 0, cb_gpu);

        /* Set descriptor heap (required for SRV table) */
        ID3D12DescriptorHeap *heaps[] = { vio_d3d12.srv_heap.heap };
        ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, heaps);

        /* Bind vertex buffer */
        D3D12_VERTEX_BUFFER_VIEW vbv = {0};
        vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(d3d->vbo);
        vbv.SizeInBytes = sizeof(vio_2d_vertex) * state->vertex_count;
        vbv.StrideInBytes = sizeof(vio_2d_vertex);
        ID3D12GraphicsCommandList_IASetVertexBuffers(cl, 0, 1, &vbv);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        /* Set full-window scissor as default */
        D3D12_RECT full_scissor = {0, 0, state->fb_width, state->fb_height};
        ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &full_scissor);

        /* Set viewport */
        D3D12_VIEWPORT vp = {0, 0, (float)state->fb_width, (float)state->fb_height, 0.0f, 1.0f};
        ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);

        ID3D12PipelineState *current_pso = NULL;
        void *current_tex = NULL;
        float sc_x = 0, sc_y = 0, sc_w = 0, sc_h = 0;
        int scissor_custom = 0;

        for (int i = 0; i < state->item_count; i++) {
            vio_2d_item *item = &state->items[i];

            /* Scissor */
            if (item->scissor.enabled) {
                if (item->scissor.x != sc_x || item->scissor.y != sc_y ||
                    item->scissor.w != sc_w || item->scissor.h != sc_h || !scissor_custom) {
                    sc_x = item->scissor.x; sc_y = item->scissor.y;
                    sc_w = item->scissor.w; sc_h = item->scissor.h;
                    float sx = (state->width > 0) ? (float)state->fb_width / (float)state->width : 1.0f;
                    float sy = (state->height > 0) ? (float)state->fb_height / (float)state->height : 1.0f;
                    D3D12_RECT sr = {
                        (LONG)(sc_x * sx), (LONG)(sc_y * sy),
                        (LONG)((sc_x + sc_w) * sx), (LONG)((sc_y + sc_h) * sy)
                    };
                    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sr);
                    scissor_custom = 1;
                }
            } else if (scissor_custom) {
                ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &full_scissor);
                scissor_custom = 0;
            }

            /* Select PSO */
            ID3D12PipelineState *wanted_pso = (item->backend_texture)
                ? d3d->pso_sprites : d3d->pso_shapes;
            if (wanted_pso != current_pso) {
                current_pso = wanted_pso;
                ID3D12GraphicsCommandList_SetPipelineState(cl, current_pso);
            }

            /* Bind texture via SRV table */
            if (item->backend_texture != current_tex) {
                current_tex = item->backend_texture;
                if (current_tex) {
                    vio_d3d12_texture *dtex = (vio_d3d12_texture *)current_tex;
                    memset(vio_d3d12.pending_srv_valid, 0, sizeof(vio_d3d12.pending_srv_valid));
                    vio_d3d12.pending_srvs[0] = dtex->srv_cpu;
                    vio_d3d12.pending_srv_valid[0] = 1;
                    vio_d3d12_flush_srv_table();
                }
            }

            /* Draw */
            ID3D12GraphicsCommandList_DrawInstanced(cl, (UINT)item->vertex_count, 1,
                                                     (UINT)item->vertex_start, 0);
        }
    }
#endif
}
