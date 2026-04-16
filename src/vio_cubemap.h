/*
 * php-vio - Cubemap texture management
 */

#ifndef VIO_CUBEMAP_H
#define VIO_CUBEMAP_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

typedef struct _vio_cubemap_object {
    unsigned int texture_id;    /* GL cubemap texture ID */
    void        *d3d11_texture; /* ID3D11Texture2D* (ArraySize=6) */
    void        *d3d11_srv;     /* ID3D11ShaderResourceView* (TEXTURECUBE) */
    void        *d3d11_sampler; /* ID3D11SamplerState* */
    void        *d3d12_resource;  /* ID3D12Resource* (DepthOrArraySize=6) */
    uint64_t     d3d12_srv_gpu;   /* D3D12_GPU_DESCRIPTOR_HANDLE.ptr */
    uint64_t     d3d12_srv_cpu;   /* D3D12_CPU_DESCRIPTOR_HANDLE.ptr */
    int          backend_type;  /* 0=none, 1=opengl, 2=d3d11, 3=d3d12 */
    int          resolution;
    int          valid;
    zend_object  std;
} vio_cubemap_object;

extern zend_class_entry *vio_cubemap_ce;

void vio_cubemap_register(void);

static inline vio_cubemap_object *vio_cubemap_from_obj(zend_object *obj) {
    return (vio_cubemap_object *)((char *)obj - XtOffsetOf(vio_cubemap_object, std));
}

#define Z_VIO_CUBEMAP_P(zv) vio_cubemap_from_obj(Z_OBJ_P(zv))

#endif /* VIO_CUBEMAP_H */
