/*
 * php-vio - 2D Rendering D3D12 backend
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#ifdef HAVE_D3D12

#define COBJMACROS
#include <d3d12.h>
#include <d3dcompiler.h>
#include <string.h>
#include <stdlib.h>

#include "vio_2d.h"
#include "vio_2d_d3d12.h"
#include "backends/d3d12/vio_d3d12.h"
#include "shaders/shaders_2d.h"

static ID3D12PipelineState *vio_2d_d3d12_create_pso(
    ID3DBlob *vs_blob, ID3DBlob *ps_blob, int has_texture)
{
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(vio_2d_vertex, x), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(vio_2d_vertex, u), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0, offsetof(vio_2d_vertex, r), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {0};
    desc.pRootSignature = vio_d3d12.root_signature;
    desc.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs_blob);
    desc.VS.BytecodeLength  = ID3D10Blob_GetBufferSize(vs_blob);
    desc.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps_blob);
    desc.PS.BytecodeLength  = ID3D10Blob_GetBufferSize(ps_blob);

    desc.InputLayout.pInputElementDescs = layout;
    desc.InputLayout.NumElements = 3;

    /* Blend: alpha blending */
    desc.BlendState.RenderTarget[0].BlendEnable    = TRUE;
    desc.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    /* Rasterizer: no culling, scissor enabled */
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;

    /* Depth: disabled */
    desc.DepthStencilState.DepthEnable    = FALSE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.SampleDesc.Count = 1;

    ID3D12PipelineState *pso = NULL;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(
        vio_d3d12.device, &desc, &IID_ID3D12PipelineState, (void **)&pso);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12 2D: Failed to create PSO (0x%lx)", (unsigned long)hr);
        return NULL;
    }
    return pso;
}

int vio_2d_d3d12_init(vio_2d_d3d12_state *state)
{
    HRESULT hr;
    memset(state, 0, sizeof(vio_2d_d3d12_state));

    /* ── Compile HLSL shaders ────────────────────────────────────── */
    ID3DBlob *vs_blob = NULL, *ps_shapes_blob = NULL, *ps_sprites_blob = NULL;
    ID3DBlob *error_blob = NULL;

    hr = D3DCompile(vio_2d_hlsl_vs, strlen(vio_2d_hlsl_vs), "vio_2d_vs",
                     NULL, NULL, "main", "vs_5_1", 0, 0, &vs_blob, &error_blob);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12 2D VS compile failed: %s",
                          error_blob ? (char *)ID3D10Blob_GetBufferPointer(error_blob) : "unknown");
        if (error_blob) ID3D10Blob_Release(error_blob);
        return -1;
    }

    hr = D3DCompile(vio_2d_hlsl_ps_shapes, strlen(vio_2d_hlsl_ps_shapes), "vio_2d_ps_shapes",
                     NULL, NULL, "main", "ps_5_1", 0, 0, &ps_shapes_blob, &error_blob);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12 2D PS shapes compile failed: %s",
                          error_blob ? (char *)ID3D10Blob_GetBufferPointer(error_blob) : "unknown");
        if (error_blob) ID3D10Blob_Release(error_blob);
        ID3D10Blob_Release(vs_blob);
        return -1;
    }

    hr = D3DCompile(vio_2d_hlsl_ps_sprites, strlen(vio_2d_hlsl_ps_sprites), "vio_2d_ps_sprites",
                     NULL, NULL, "main", "ps_5_1", 0, 0, &ps_sprites_blob, &error_blob);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12 2D PS sprites compile failed: %s",
                          error_blob ? (char *)ID3D10Blob_GetBufferPointer(error_blob) : "unknown");
        if (error_blob) ID3D10Blob_Release(error_blob);
        ID3D10Blob_Release(vs_blob);
        ID3D10Blob_Release(ps_shapes_blob);
        return -1;
    }

    /* ── Create PSOs ─────────────────────────────────────────────── */
    state->pso_shapes  = vio_2d_d3d12_create_pso(vs_blob, ps_shapes_blob, 0);
    state->pso_sprites = vio_2d_d3d12_create_pso(vs_blob, ps_sprites_blob, 1);

    ID3D10Blob_Release(vs_blob);
    ID3D10Blob_Release(ps_shapes_blob);
    ID3D10Blob_Release(ps_sprites_blob);

    if (!state->pso_shapes || !state->pso_sprites) {
        vio_2d_d3d12_shutdown(state);
        return -1;
    }

    /* ── Upload heap vertex buffer (persistently mapped) ─────────── */
    state->vbo_size = sizeof(vio_2d_vertex) * VIO_2D_MAX_VERTICES;

    D3D12_HEAP_PROPERTIES heap_props = {0};
    heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC res_desc = {0};
    res_desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    res_desc.Width              = state->vbo_size;
    res_desc.Height             = 1;
    res_desc.DepthOrArraySize   = 1;
    res_desc.MipLevels          = 1;
    res_desc.SampleDesc.Count   = 1;
    res_desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device,
        &heap_props, D3D12_HEAP_FLAG_NONE, &res_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
        &IID_ID3D12Resource, (void **)&state->vbo);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12 2D: Failed to create VBO");
        vio_2d_d3d12_shutdown(state);
        return -1;
    }

    D3D12_RANGE read_range = {0, 0};
    hr = ID3D12Resource_Map(state->vbo, 0, &read_range, (void **)&state->vbo_mapped);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12 2D: Failed to map VBO");
        vio_2d_d3d12_shutdown(state);
        return -1;
    }

    return 0;
}

void vio_2d_d3d12_shutdown(vio_2d_d3d12_state *state)
{
    if (state->vbo) {
        ID3D12Resource_Unmap(state->vbo, 0, NULL);
        ID3D12Resource_Release(state->vbo);
    }
    if (state->pso_sprites) ID3D12PipelineState_Release(state->pso_sprites);
    if (state->pso_shapes)  ID3D12PipelineState_Release(state->pso_shapes);
    memset(state, 0, sizeof(vio_2d_d3d12_state));
}

#endif /* HAVE_D3D12 */
