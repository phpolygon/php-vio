/*
 * php-vio - 2D Rendering D3D12 backend state
 */

#ifndef VIO_2D_D3D12_H
#define VIO_2D_D3D12_H

#ifdef HAVE_D3D12

#include <d3d12.h>
#include <d3dcompiler.h>

typedef struct _vio_2d_d3d12_state {
    ID3D12PipelineState *pso_shapes;
    ID3D12PipelineState *pso_sprites;
    ID3D12Resource      *vbo;
    unsigned char       *vbo_mapped;     /* persistently mapped */
    UINT                 vbo_size;
    D3D12_VERTEX_BUFFER_VIEW vbv;
} vio_2d_d3d12_state;

int  vio_2d_d3d12_init(vio_2d_d3d12_state *state);
void vio_2d_d3d12_shutdown(vio_2d_d3d12_state *state);

#endif /* HAVE_D3D12 */
#endif /* VIO_2D_D3D12_H */
