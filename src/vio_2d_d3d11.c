/*
 * php-vio - 2D Rendering D3D11 backend
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#ifdef HAVE_D3D11

#define COBJMACROS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <string.h>
#include <stdlib.h>

#include "vio_2d.h"
#include "vio_2d_d3d11.h"
#include "backends/d3d11/vio_d3d11.h"
#include "shaders/shaders_2d.h"

int vio_2d_d3d11_init(vio_2d_d3d11_state *state)
{
    HRESULT hr;
    memset(state, 0, sizeof(vio_2d_d3d11_state));

    /* ── Compile HLSL shaders ────────────────────────────────────── */
    ID3DBlob *vs_blob = NULL, *ps_shapes_blob = NULL, *ps_sprites_blob = NULL;
    ID3DBlob *error_blob = NULL;
    UINT flags = 0;
    if (vio_d3d11.debug_enabled) {
        flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    }

    hr = D3DCompile(vio_2d_hlsl_vs, strlen(vio_2d_hlsl_vs), "vio_2d_vs",
                     NULL, NULL, "main", "vs_5_0", flags, 0, &vs_blob, &error_blob);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "2D VS compile failed: %s",
                          error_blob ? (char *)ID3D10Blob_GetBufferPointer(error_blob) : "unknown");
        if (error_blob) ID3D10Blob_Release(error_blob);
        return -1;
    }

    hr = D3DCompile(vio_2d_hlsl_ps_shapes, strlen(vio_2d_hlsl_ps_shapes), "vio_2d_ps_shapes",
                     NULL, NULL, "main", "ps_5_0", flags, 0, &ps_shapes_blob, &error_blob);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "2D PS shapes compile failed: %s",
                          error_blob ? (char *)ID3D10Blob_GetBufferPointer(error_blob) : "unknown");
        if (error_blob) ID3D10Blob_Release(error_blob);
        ID3D10Blob_Release(vs_blob);
        return -1;
    }

    hr = D3DCompile(vio_2d_hlsl_ps_sprites, strlen(vio_2d_hlsl_ps_sprites), "vio_2d_ps_sprites",
                     NULL, NULL, "main", "ps_5_0", flags, 0, &ps_sprites_blob, &error_blob);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "2D PS sprites compile failed: %s",
                          error_blob ? (char *)ID3D10Blob_GetBufferPointer(error_blob) : "unknown");
        if (error_blob) ID3D10Blob_Release(error_blob);
        ID3D10Blob_Release(vs_blob);
        ID3D10Blob_Release(ps_shapes_blob);
        return -1;
    }

    /* ── Create shader objects ───────────────────────────────────── */
    hr = ID3D11Device_CreateVertexShader(vio_d3d11.device,
        ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob),
        NULL, &state->vs);
    if (FAILED(hr)) goto fail;

    hr = ID3D11Device_CreatePixelShader(vio_d3d11.device,
        ID3D10Blob_GetBufferPointer(ps_shapes_blob), ID3D10Blob_GetBufferSize(ps_shapes_blob),
        NULL, &state->ps_shapes);
    if (FAILED(hr)) goto fail;

    hr = ID3D11Device_CreatePixelShader(vio_d3d11.device,
        ID3D10Blob_GetBufferPointer(ps_sprites_blob), ID3D10Blob_GetBufferSize(ps_sprites_blob),
        NULL, &state->ps_sprites);
    if (FAILED(hr)) goto fail;

    /* ── Input layout (matches vio_2d_vertex) ────────────────────── */
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,    0, offsetof(vio_2d_vertex, x), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, offsetof(vio_2d_vertex, u), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(vio_2d_vertex, r), D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    hr = ID3D11Device_CreateInputLayout(vio_d3d11.device, layout, 3,
        ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob),
        &state->input_layout);
    if (FAILED(hr)) goto fail;

    /* ── Dynamic vertex buffer ───────────────────────────────────── */
    D3D11_BUFFER_DESC vb_desc = {0};
    vb_desc.ByteWidth      = sizeof(vio_2d_vertex) * VIO_2D_MAX_VERTICES;
    vb_desc.Usage          = D3D11_USAGE_DYNAMIC;
    vb_desc.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(vio_d3d11.device, &vb_desc, NULL, &state->vbo);
    if (FAILED(hr)) goto fail;

    /* ── Constant buffer for projection matrix ───────────────────── */
    D3D11_BUFFER_DESC cb_desc = {0};
    cb_desc.ByteWidth      = sizeof(float) * 16;  /* 4x4 matrix */
    cb_desc.Usage          = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(vio_d3d11.device, &cb_desc, NULL, &state->cb);
    if (FAILED(hr)) goto fail;

    /* ── Blend state: alpha blending ─────────────────────────────── */
    D3D11_BLEND_DESC blend_desc = {0};
    blend_desc.RenderTarget[0].BlendEnable    = TRUE;
    blend_desc.RenderTarget[0].SrcBlend       = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = ID3D11Device_CreateBlendState(vio_d3d11.device, &blend_desc, &state->blend_state);
    if (FAILED(hr)) goto fail;

    /* ── Rasterizer state: no culling, scissor enabled ───────────── */
    D3D11_RASTERIZER_DESC rast_desc = {0};
    rast_desc.FillMode        = D3D11_FILL_SOLID;
    rast_desc.CullMode        = D3D11_CULL_NONE;
    rast_desc.ScissorEnable   = TRUE;
    rast_desc.DepthClipEnable = TRUE;

    hr = ID3D11Device_CreateRasterizerState(vio_d3d11.device, &rast_desc, &state->rasterizer_state);
    if (FAILED(hr)) goto fail;

    /* ── Depth-stencil state: depth disabled ─────────────────────── */
    D3D11_DEPTH_STENCIL_DESC ds_desc = {0};
    ds_desc.DepthEnable    = FALSE;
    ds_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;

    hr = ID3D11Device_CreateDepthStencilState(vio_d3d11.device, &ds_desc, &state->depth_stencil_state);
    if (FAILED(hr)) goto fail;

    /* ── Sampler for sprites/text ────────────────────────────────── */
    D3D11_SAMPLER_DESC samp_desc = {0};
    samp_desc.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.MaxAnisotropy = 1;
    samp_desc.MaxLOD   = D3D11_FLOAT32_MAX;

    hr = ID3D11Device_CreateSamplerState(vio_d3d11.device, &samp_desc, &state->sampler);
    if (FAILED(hr)) goto fail;

    /* Cleanup blobs */
    ID3D10Blob_Release(vs_blob);
    ID3D10Blob_Release(ps_shapes_blob);
    ID3D10Blob_Release(ps_sprites_blob);
    return 0;

fail:
    php_error_docref(NULL, E_WARNING, "Failed to initialize D3D11 2D renderer");
    if (vs_blob) ID3D10Blob_Release(vs_blob);
    if (ps_shapes_blob) ID3D10Blob_Release(ps_shapes_blob);
    if (ps_sprites_blob) ID3D10Blob_Release(ps_sprites_blob);
    vio_2d_d3d11_shutdown(state);
    return -1;
}

void vio_2d_d3d11_shutdown(vio_2d_d3d11_state *state)
{
    if (state->sampler)            ID3D11SamplerState_Release(state->sampler);
    if (state->depth_stencil_state) ID3D11DepthStencilState_Release(state->depth_stencil_state);
    if (state->rasterizer_state)   ID3D11RasterizerState_Release(state->rasterizer_state);
    if (state->blend_state)        ID3D11BlendState_Release(state->blend_state);
    if (state->input_layout)       ID3D11InputLayout_Release(state->input_layout);
    if (state->cb)                 ID3D11Buffer_Release(state->cb);
    if (state->vbo)                ID3D11Buffer_Release(state->vbo);
    if (state->ps_sprites)         ID3D11PixelShader_Release(state->ps_sprites);
    if (state->ps_shapes)          ID3D11PixelShader_Release(state->ps_shapes);
    if (state->vs)                 ID3D11VertexShader_Release(state->vs);
    memset(state, 0, sizeof(vio_2d_d3d11_state));
}

#endif /* HAVE_D3D11 */
