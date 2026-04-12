/*
 * php-vio - Shared helpers for Direct3D 11 and Direct3D 12 backends
 */

#ifndef VIO_D3D_COMMON_H
#define VIO_D3D_COMMON_H

#if defined(HAVE_D3D11) || defined(HAVE_D3D12)

#include <dxgiformat.h>
#include "../../include/vio_types.h"

static inline DXGI_FORMAT vio_format_to_dxgi(vio_format f)
{
    switch (f) {
        case VIO_FLOAT1: return DXGI_FORMAT_R32_FLOAT;
        case VIO_FLOAT2: return DXGI_FORMAT_R32G32_FLOAT;
        case VIO_FLOAT3: return DXGI_FORMAT_R32G32B32_FLOAT;
        case VIO_FLOAT4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case VIO_INT1:   return DXGI_FORMAT_R32_SINT;
        case VIO_INT2:   return DXGI_FORMAT_R32G32_SINT;
        case VIO_INT3:   return DXGI_FORMAT_R32G32B32_SINT;
        case VIO_INT4:   return DXGI_FORMAT_R32G32B32A32_SINT;
        case VIO_UINT1:  return DXGI_FORMAT_R32_UINT;
        case VIO_UINT2:  return DXGI_FORMAT_R32G32_UINT;
        case VIO_UINT3:  return DXGI_FORMAT_R32G32B32_UINT;
        case VIO_UINT4:  return DXGI_FORMAT_R32G32B32A32_UINT;
        default:         return DXGI_FORMAT_R32G32B32A32_FLOAT;
    }
}

static inline UINT vio_format_byte_size(vio_format f)
{
    switch (f) {
        case VIO_FLOAT1: case VIO_INT1: case VIO_UINT1: return 4;
        case VIO_FLOAT2: case VIO_INT2: case VIO_UINT2: return 8;
        case VIO_FLOAT3: case VIO_INT3: case VIO_UINT3: return 12;
        case VIO_FLOAT4: case VIO_INT4: case VIO_UINT4: return 16;
        default: return 16;
    }
}

static inline const char *vio_usage_to_semantic(vio_usage u)
{
    switch (u) {
        case VIO_POSITION: return "POSITION";
        case VIO_COLOR:    return "COLOR";
        case VIO_TEXCOORD: return "TEXCOORD";
        case VIO_NORMAL:   return "NORMAL";
        case VIO_TANGENT:  return "TANGENT";
        default:           return "TEXCOORD";
    }
}

#endif /* HAVE_D3D11 || HAVE_D3D12 */
#endif /* VIO_D3D_COMMON_H */
