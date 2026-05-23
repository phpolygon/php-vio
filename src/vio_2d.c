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
#include "backends/opengl/vio_opengl.h"
#include "backends/opengl/vio_2d_opengl.h"
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

#ifdef HAVE_METAL
#include "backends/metal/vio_metal.h"
#endif

#ifdef HAVE_VULKAN
#include <vulkan/vulkan.h>
#include "vio_2d_vulkan.h"
#include "backends/vulkan/vio_vulkan.h"
#include "vio_render_target.h"   /* vio_render_target_object (bound-RT extent) */
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

    /* Apply current 2D transform to the scissor rect so the rect is stored
     * in the same coordinate space as the (transformed) vertex stream.
     * Without this, callers who push a scissor inside a `pushTransform(scale)`
     * (e.g. games that map a logical UI grid onto an HiDPI / 4K framebuffer)
     * see scrollable-list scissors clip at logical-coord pixels instead of
     * the actual on-screen rectangle. Transform the top-left and bottom-right
     * corners separately to handle scale + translate (the only affine forms
     * the 2D path emits today). */
    if (state->transform_depth > 0) {
        float x0 = x,     y0 = y;
        float x1 = x + w, y1 = y + h;
        vio_2d_apply_transform(state, &x0, &y0);
        vio_2d_apply_transform(state, &x1, &y1);
        x = x0; y = y0;
        w = x1 - x0;
        h = y1 - y0;
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
    state->backend   = VIO_2D_BACKEND_NONE;

    vio_2d_ortho(state->projection, 0, (float)width, (float)height, 0);

#ifdef HAVE_GLFW
    if (vio_gl.initialized) {
        vio_2d_opengl_state *gl = calloc(1, sizeof(vio_2d_opengl_state));
        if (gl && vio_2d_opengl_init(gl, state->vertex_capacity) == 0) {
            state->opengl_state = gl;
            state->backend = VIO_2D_BACKEND_OPENGL;
        } else {
            free(gl);
            return -1;
        }
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

#ifdef HAVE_VULKAN
    if (state->backend == VIO_2D_BACKEND_NONE && vio_vk.initialized) {
        vio_2d_vulkan_state *vk = calloc(1, sizeof(vio_2d_vulkan_state));
        if (vk && vio_2d_vulkan_init(vk) == 0) {
            state->vulkan_state = vk;
            state->backend = VIO_2D_BACKEND_VULKAN;
        } else {
            free(vk);
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
    if (state->backend == VIO_2D_BACKEND_OPENGL && state->opengl_state) {
        vio_2d_opengl_shutdown((vio_2d_opengl_state *)state->opengl_state);
        free(state->opengl_state);
        state->opengl_state = NULL;
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

#ifdef HAVE_VULKAN
    if (state->backend == VIO_2D_BACKEND_VULKAN && state->vulkan_state) {
        vio_2d_vulkan_shutdown((vio_2d_vulkan_state *)state->vulkan_state);
        free(state->vulkan_state);
        state->vulkan_state = NULL;
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
                       unsigned int texture_id, void *backend_texture,
                       int vert_start, int vert_count)
{
    if (!state->initialized) return;
    /* Grow buffer if needed */
    if (state->item_count >= state->item_capacity) {
        int new_cap = state->item_capacity * 2;
        state->items = erealloc(state->items, sizeof(vio_2d_item) * new_cap);
        state->item_capacity = new_cap;
    }

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

    /* Sort items by z-order, then texture (shared across all backends) */
    qsort(state->items, state->item_count, sizeof(vio_2d_item), vio_2d_item_cmp);

#ifdef HAVE_METAL
    if (vio_metal_2d_is_active()) {
        vio_metal_2d_flush(state);
        return;
    }
#endif

#ifdef HAVE_GLFW
    if (state->backend == VIO_2D_BACKEND_OPENGL && vio_gl.initialized) {
        vio_2d_opengl_flush(state);
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

        /* Upload vertex data into THIS frame's slice of the persistently-mapped
         * UPLOAD VBO. The resource holds VIO_D3D12_FRAME_COUNT slices; we pick
         * the one indexed by the current backbuffer to avoid stomping vertices
         * still in flight on the GPU from the previous frame. */
        UINT vbo_slice_off = vio_d3d12.frame_index * d3d->vbo_slice_size;
        memcpy(d3d->vbo_mapped + vbo_slice_off, state->vertices, sizeof(vio_2d_vertex) * state->vertex_count);

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

        /* Bind vertex buffer at the current frame's slice */
        D3D12_VERTEX_BUFFER_VIEW vbv = {0};
        vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(d3d->vbo) + vbo_slice_off;
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

#ifdef HAVE_VULKAN
    if (state->backend == VIO_2D_BACKEND_VULKAN && state->vulkan_state
            && vio_vk.initialized && vio_vk.in_frame) {
        vio_2d_vulkan_state *vk = (vio_2d_vulkan_state *)state->vulkan_state;
        VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;

        /* Render area = the currently-bound target's extent. With no offscreen
         * RT bound (the normal frame, current_bound_rt == NULL) this is exactly
         * the swapchain extent as before — byte-identical to the pre-Phase-3
         * path. When an offscreen RT is bound mid-frame (Phase 3), use its
         * extent so the default scissor and the per-item scissor clamp match the
         * offscreen framebuffer (Vulkan errors if a scissor exceeds the bound
         * framebuffer, unlike D3D's silent clamp). */
        uint32_t ra_w = vio_vk.swapchain_extent.width;
        uint32_t ra_h = vio_vk.swapchain_extent.height;
        if (vio_vk.current_bound_rt) {
            vio_render_target_object *brt =
                (vio_render_target_object *)vio_vk.current_bound_rt;
            if (brt->width > 0 && brt->height > 0) {
                ra_w = (uint32_t)brt->width;
                ra_h = (uint32_t)brt->height;
            }
        }
        if (ra_w == 0 || ra_h == 0) return;

        /* Upload vertices into THIS frame's slice of the persistently-mapped
         * ring. The in_flight fence waited in vulkan_begin_frame guarantees the
         * previous use of this slice has completed, so the memcpy is race-free.
         * Clamp to the slice capacity (the CPU batch can grow past it). */
        int vcount = state->vertex_count;
        int max_verts = (int)(vk->vbo_slice_size / sizeof(vio_2d_vertex));
        if (vcount > max_verts) {
            php_error_docref(NULL, E_WARNING,
                "Vulkan 2D: vertex batch %d exceeds slice capacity %d; clamping",
                vcount, max_verts);
            vcount = max_verts;
        }
        VkDeviceSize slice_off = (VkDeviceSize)vio_vk.current_frame * vk->vbo_slice_size;
        if (vcount > 0) {
            memcpy(vk->vbo_mapped + slice_off, state->vertices,
                   sizeof(vio_2d_vertex) * (size_t)vcount);
        }

        /* Bind vertex buffer at the current frame's slice. */
        VkDeviceSize vb_off = slice_off;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vk->vbo, &vb_off);

        /* Projection mat4 via vertex-stage push constant (64 bytes). */
        vkCmdPushConstants(cmd, vk->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(float) * 16, state->projection);

        /* Default scissor = full render area. */
        VkRect2D full_scissor = {0};
        full_scissor.offset = (VkOffset2D){0, 0};
        full_scissor.extent = (VkExtent2D){ra_w, ra_h};
        vkCmdSetScissor(cmd, 0, 1, &full_scissor);

        VkPipeline current_pipeline = VK_NULL_HANDLE;
        float sc_x = 0, sc_y = 0, sc_w = 0, sc_h = 0;
        int   scissor_custom = 0;

        /* Cache the last bound texture -> descriptor set within this frame so a
         * sorted run of items sharing one texture (the common glyph case)
         * allocates / binds exactly one set. The batch is sorted by z then
         * texture_id, so identical textures are adjacent. The sentinel is the
         * address of a local — distinct from NULL and from any heap-allocated
         * backend_texture, so the first textured item always allocates. */
        static char     tex_sentinel;
        void           *current_tex = &tex_sentinel;
        VkDescriptorSet current_set = VK_NULL_HANDLE;

        for (int i = 0; i < state->item_count; i++) {
            vio_2d_item *item = &state->items[i];

            /* Defensive: skip items whose vertices fell outside the clamped
             * slice so vkCmdDraw never reads past the uploaded region. */
            if (item->vertex_start + item->vertex_count > vcount) {
                continue;
            }

            /* A textured item (sprite/glyph) needs a valid Vulkan backend
             * texture. If it has none (e.g. a font atlas that failed to upload),
             * skip it rather than sampling an unbound descriptor. */
            int textured = (item->backend_texture != NULL);

            /* Scissor (scale logical->framebuffer, then CLAMP to render area). */
            if (item->scissor.enabled) {
                if (item->scissor.x != sc_x || item->scissor.y != sc_y ||
                    item->scissor.w != sc_w || item->scissor.h != sc_h || !scissor_custom) {
                    sc_x = item->scissor.x; sc_y = item->scissor.y;
                    sc_w = item->scissor.w; sc_h = item->scissor.h;
                    float sx = (state->width  > 0) ? (float)state->fb_width  / (float)state->width  : 1.0f;
                    float sy = (state->height > 0) ? (float)state->fb_height / (float)state->height : 1.0f;

                    float fx0 = sc_x * sx,           fy0 = sc_y * sy;
                    float fx1 = (sc_x + sc_w) * sx,  fy1 = (sc_y + sc_h) * sy;
                    if (fx0 < 0) fx0 = 0; if (fy0 < 0) fy0 = 0;
                    if (fx1 < fx0) fx1 = fx0; if (fy1 < fy0) fy1 = fy0;
                    if (fx1 > (float)ra_w) fx1 = (float)ra_w;
                    if (fy1 > (float)ra_h) fy1 = (float)ra_h;
                    if (fx0 > (float)ra_w) fx0 = (float)ra_w;
                    if (fy0 > (float)ra_h) fy0 = (float)ra_h;

                    VkRect2D sr = {0};
                    sr.offset.x      = (int32_t)fx0;
                    sr.offset.y      = (int32_t)fy0;
                    sr.extent.width  = (uint32_t)(fx1 - fx0);
                    sr.extent.height = (uint32_t)(fy1 - fy0);
                    vkCmdSetScissor(cmd, 0, 1, &sr);
                    scissor_custom = 1;
                }
            } else if (scissor_custom) {
                vkCmdSetScissor(cmd, 0, 1, &full_scissor);
                scissor_custom = 0;
            }

            /* Pipeline: sprites (samples the bound texture) for textured items,
             * shapes (ignores the descriptor) otherwise. Both share the pipeline
             * layout, so the descriptor set stays bound across a shapes draw —
             * harmless because the shapes shader never samples binding 0. */
            VkPipeline wanted = textured ? vk->pipeline_sprites : vk->pipeline_shapes;
            if (wanted != current_pipeline) {
                current_pipeline = wanted;
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, current_pipeline);
            }

            /* Texture -> descriptor set. Allocate from this frame's pool on a
             * texture change and cache it for the sorted run of same-texture
             * items. On allocation failure (pool exhaustion) skip the draw. */
            if (textured) {
                if (item->backend_texture != current_tex) {
                    current_tex = item->backend_texture;
                    vio_vulkan_texture *vtex = (vio_vulkan_texture *)current_tex;
                    current_set = vio_2d_vulkan_alloc_texture_set(
                        vk, vio_vk.current_frame, vtex->view, vtex->sampler);
                    if (current_set != VK_NULL_HANDLE) {
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                vk->pipeline_layout, 0, 1, &current_set, 0, NULL);
                    }
                }
                if (current_set == VK_NULL_HANDLE) {
                    /* Allocation failed for this texture — don't draw with an
                     * unbound sampler (validation error / undefined sample). */
                    continue;
                }
            }

            vkCmdDraw(cmd, (uint32_t)item->vertex_count, 1, (uint32_t)item->vertex_start, 0);
        }
    }
#endif
}
