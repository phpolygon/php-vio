/*
 * php-vio - Shared helpers for Direct3D 11 and Direct3D 12 backends
 */

#ifndef VIO_D3D_COMMON_H
#define VIO_D3D_COMMON_H

#if defined(HAVE_D3D11) || defined(HAVE_D3D12)

#include <dxgiformat.h>
#include <dxgi1_6.h>
#include <stdio.h>

/* vio_backend_info (A4): PCI vendor, user-mode driver version and whether the
 * adapter is the software rasterizer (WARP / Microsoft Basic Render Driver). */
static inline void vio_dxgi_adapter_identity(IDXGIAdapter *adapter, uint32_t *vendor_id,
                                             char *driver, size_t driver_size, int *software)
{
    DXGI_ADAPTER_DESC d;
    LARGE_INTEGER umd;
    *vendor_id = 0;
    *software = 0;
    if (driver_size) driver[0] = '\0';
    if (!adapter) return;
    if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &d))) {
        *vendor_id = d.VendorId;
        *software = d.VendorId == 0x1414 && d.DeviceId == 0x8C;
    }
    if (driver_size && SUCCEEDED(IDXGIAdapter_CheckInterfaceSupport(adapter, &IID_IDXGIDevice, &umd)))
        snprintf(driver, driver_size, "%u.%u.%u.%u",
                 (unsigned)HIWORD(umd.HighPart), (unsigned)LOWORD(umd.HighPart),
                 (unsigned)HIWORD(umd.LowPart), (unsigned)LOWORD(umd.LowPart));
}

/* vio_adapters (A6): DXGI adapters in high-performance order (the software
 * adapter last). `probe` adds the API's features and device type and returns
 * 0 when the API can open the adapter. */
typedef int (*vio_dxgi_probe_fn)(IDXGIAdapter1 *adapter, vio_adapter_info *a);
static inline int vio_dxgi_enumerate_adapters(vio_adapter_info *out, int max, vio_dxgi_probe_fn probe)
{
    IDXGIFactory1 *f1 = NULL;
    IDXGIFactory6 *f6 = NULL;
    int n = 0;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f1))) return 0;
    if (FAILED(IDXGIFactory1_QueryInterface(f1, &IID_IDXGIFactory6, (void **)&f6))) f6 = NULL;
    for (UINT i = 0; n < max; i++) {
        IDXGIAdapter1 *ad = NULL;
        HRESULT hr = f6 ? IDXGIFactory6_EnumAdapterByGpuPreference(f6, i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                  &IID_IDXGIAdapter1, (void **)&ad)
                        : IDXGIFactory1_EnumAdapters1(f1, i, &ad);
        if (FAILED(hr) || !ad) break;
        vio_adapter_info *a = &out[n];
        DXGI_ADAPTER_DESC1 d;
        int sw = 0;
        memset(a, 0, sizeof(*a));
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(ad, &d))) {
            if (WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, a->name, (int)sizeof(a->name), NULL, NULL) <= 0)
                a->name[0] = '\0';
            a->device_id = d.DeviceId;
            a->vram_bytes = (uint64_t)d.DedicatedVideoMemory;
        }
        vio_dxgi_adapter_identity((IDXGIAdapter *)ad, &a->vendor_id, a->driver, sizeof(a->driver), &sw);
        a->device_type = sw ? "software" : NULL;
        if (probe(ad, a) == 0) n++;
        IDXGIAdapter1_Release(ad);
    }
    if (f6) IDXGIFactory6_Release(f6);
    IDXGIFactory1_Release(f1);
    return n;
}
#include <windows.h>
#include "../../include/vio_types.h"

/* Colour attachment format (vio_pixel_format) -> DXGI. */
static inline DXGI_FORMAT vio_pixel_format_to_dxgi(int f)
{
    switch (f) {
        case VIO_FORMAT_RGBA16F:    return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case VIO_FORMAT_RGBA32F:    return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case VIO_FORMAT_R11G11B10F: return DXGI_FORMAT_R11G11B10_FLOAT;
        case VIO_FORMAT_RG16F:      return DXGI_FORMAT_R16G16_FLOAT;
        case VIO_FORMAT_R16F:       return DXGI_FORMAT_R16_FLOAT;
        case VIO_FORMAT_R32F:       return DXGI_FORMAT_R32_FLOAT;
        case VIO_FORMAT_R8:         return DXGI_FORMAT_R8_UNORM;
        case VIO_FORMAT_RGB10A2:    return DXGI_FORMAT_R10G10B10A2_UNORM;
        case VIO_FORMAT_RGBA8:
        default:                    return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

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

/* VIO_D3D_WARP=<path to d3d10warp.dll>: a WARP from the Microsoft.Direct3D.WARP
 * NuGet package instead of the one in System32 (SM6PLAN 0b). Loaded once, before
 * the first WARP device: D3D11 and D3D12 load "d3d10warp.dll" by name and get the
 * module already in the process. The OS WARP of a CI runner stops at SM 6.2;
 * the NuGet WARP (1.0.18+) has SM 6.9 with DXR 1.2. */
static inline void vio_d3d_load_warp(void)
{
    static int done;
    if (done) return;
    done = 1;
    const char *path = getenv("VIO_D3D_WARP");
    if (!path || !*path) return;
    WCHAR wpath[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH) || !LoadLibraryW(wpath)) {
        php_error_docref(NULL, E_WARNING, "VIO_D3D_WARP: cannot load '%s' (%lu); using the system WARP", path, GetLastError());
    }
}

#endif /* HAVE_D3D11 || HAVE_D3D12 */

/* Stencil state mapping shared by D3D11 / D3D12. Only dxgiformat.h is included
 * here, so the values are the numeric D3D11_/D3D12_COMPARISON_FUNC and
 * D3D11_/D3D12_STENCIL_OP constants (identical in both APIs; callers cast). */
static inline int vio_d3d_compare_func_value(int f)
{
    switch (f) {
        case VIO_CMP_NEVER:    return 1;  /* D3D1x_COMPARISON_NEVER */
        case VIO_CMP_LESS:     return 2;
        case VIO_CMP_EQUAL:    return 3;
        case VIO_CMP_LEQUAL:   return 4;
        case VIO_CMP_GREATER:  return 5;
        case VIO_CMP_NOTEQUAL: return 6;
        case VIO_CMP_GEQUAL:   return 7;
        default:               return 8;  /* ALWAYS */
    }
}
static inline int vio_d3d_stencil_op_value(int op)
{
    switch (op) {
        case VIO_STENCIL_ZERO:      return 2;  /* D3D1x_STENCIL_OP_ZERO */
        case VIO_STENCIL_REPLACE:   return 3;
        case VIO_STENCIL_INCR:      return 4;  /* INCR_SAT */
        case VIO_STENCIL_DECR:      return 5;  /* DECR_SAT */
        case VIO_STENCIL_INVERT:    return 6;
        case VIO_STENCIL_INCR_WRAP: return 7;  /* INCR */
        case VIO_STENCIL_DECR_WRAP: return 8;  /* DECR */
        default:                    return 1;  /* KEEP */
    }
}
#define vio_d3d_compare_func(f)    ((D3D11_COMPARISON_FUNC)vio_d3d_compare_func_value(f))
#define vio_d3d_stencil_op(op)     ((D3D11_STENCIL_OP)vio_d3d_stencil_op_value(op))
#define vio_d3d_compare_func_12(f) ((D3D12_COMPARISON_FUNC)vio_d3d_compare_func_value(f))
#define vio_d3d_stencil_op_12(op)  ((D3D12_STENCIL_OP)vio_d3d_stencil_op_value(op))


/* HDR display detection (GAP-PHASE5 Block 6): is the monitor showing `hwnd` in
 * HDR mode (its output advertises the ST 2084 / BT.2020 colour space)? Both D3D
 * backends call this before choosing the swapchain format; DXGI 1.6 only. */
static inline int vio_d3d_hwnd_output_is_hdr(IDXGIFactory1 *factory, HWND hwnd)
{
    if (!factory || !hwnd) return 0;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    int hdr = 0;
    IDXGIAdapter1 *adapter = NULL;
    for (UINT a = 0; !hdr && IDXGIFactory1_EnumAdapters1(factory, a, &adapter) == S_OK; a++) {
        IDXGIOutput *out = NULL;
        for (UINT o = 0; !hdr && IDXGIAdapter1_EnumOutputs(adapter, o, &out) == S_OK; o++) {
            IDXGIOutput6 *out6 = NULL;
            if (SUCCEEDED(IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput6, (void **)&out6)) && out6) {
                DXGI_OUTPUT_DESC1 d1;
                if (SUCCEEDED(IDXGIOutput6_GetDesc1(out6, &d1)) && d1.Monitor == mon
                    && d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
                    hdr = 1;
                }
                IDXGIOutput6_Release(out6);
            }
            IDXGIOutput_Release(out);
        }
        IDXGIAdapter1_Release(adapter);
    }
    return hdr;
}

#endif /* VIO_D3D_COMMON_H */
