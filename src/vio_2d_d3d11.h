/*
 * php-vio - 2D Rendering D3D11 backend state
 */

#ifndef VIO_2D_D3D11_H
#define VIO_2D_D3D11_H

#ifdef HAVE_D3D11

#include <d3d11.h>
#include <d3dcompiler.h>

typedef struct _vio_2d_d3d11_state {
    ID3D11Buffer            *vbo;
    ID3D11Buffer            *cb;             /* projection constant buffer */
    ID3D11InputLayout       *input_layout;
    ID3D11VertexShader      *vs;
    ID3D11PixelShader       *ps_shapes;
    ID3D11PixelShader       *ps_sprites;
    ID3D11BlendState        *blend_state;
    ID3D11RasterizerState   *rasterizer_state;
    ID3D11DepthStencilState *depth_stencil_state;
    ID3D11SamplerState      *sampler;
} vio_2d_d3d11_state;

int  vio_2d_d3d11_init(vio_2d_d3d11_state *state);
void vio_2d_d3d11_shutdown(vio_2d_d3d11_state *state);

#endif /* HAVE_D3D11 */
#endif /* VIO_2D_D3D11_H */
