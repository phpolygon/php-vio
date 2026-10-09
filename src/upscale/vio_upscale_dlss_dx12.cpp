/*
 * php-vio - DLSS provider, D3D12 part (TEMPORAL-S4)
 *
 * The backend hands every image over in PIXEL_SHADER_RESOURCE. NGX wants its
 * inputs in NON_PIXEL_SHADER_RESOURCE (read by compute) and the output in
 * UNORDERED_ACCESS, and returns them there: the transitions around the
 * evaluation live here, so the backend contract stays the one of every provider.
 */

#include "vio_upscale_dlss_internal.h"

#ifdef HAVE_D3D12
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstring>
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"

namespace {

NVSDK_NGX_Result dx12_init(const vio_upscale_device *dev, const VioDlssInit *in)
{
    return NVSDK_NGX_D3D12_Init_with_ProjectID(in->project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, in->engine_version,
                                               in->data_path, static_cast<ID3D12Device *>(dev->device),
                                               in->feature_info, NVSDK_NGX_Version_API);
}

NVSDK_NGX_Result dx12_shutdown(const vio_upscale_device *dev)
{
    return NVSDK_NGX_D3D12_Shutdown1(static_cast<ID3D12Device *>(dev->device));
}

NVSDK_NGX_Result dx12_caps(NVSDK_NGX_Parameter **out) { return NVSDK_NGX_D3D12_GetCapabilityParameters(out); }
NVSDK_NGX_Result dx12_alloc(NVSDK_NGX_Parameter **out) { return NVSDK_NGX_D3D12_AllocateParameters(out); }
NVSDK_NGX_Result dx12_free(NVSDK_NGX_Parameter *p) { return NVSDK_NGX_D3D12_DestroyParameters(p); }
NVSDK_NGX_Result dx12_release(NVSDK_NGX_Handle *h) { return NVSDK_NGX_D3D12_ReleaseFeature(h); }

NVSDK_NGX_Result dx12_create(const vio_upscale_device *dev, NVSDK_NGX_Parameter *p,
                             NVSDK_NGX_DLSS_Create_Params *cp, NVSDK_NGX_Handle **out)
{
    return NGX_D3D12_CREATE_DLSS_EXT(static_cast<ID3D12GraphicsCommandList *>(dev->command_list), 1, 1, out, p, cp);
}

/* Transitions of the distinct resources of a dispatch. */
struct Dx12States {
    D3D12_RESOURCE_BARRIER b[8];
    UINT n = 0;
    void add(const vio_upscale_native_image *im, D3D12_RESOURCE_STATES to)
    {
        if (!im->handle) return;
        for (UINT i = 0; i < n; i++) if (b[i].Transition.pResource == im->handle) return;
        D3D12_RESOURCE_BARRIER &x = b[n++];
        memset(&x, 0, sizeof(x));
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition.pResource = static_cast<ID3D12Resource *>(im->handle);
        x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        x.Transition.StateBefore = im->state == VIO_UPSCALE_STATE_GENERAL ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                                                         : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        x.Transition.StateAfter = to;
        if (x.Transition.StateBefore == to) n--;
    }
    void flip()
    {
        for (UINT i = 0; i < n; i++) {
            D3D12_RESOURCE_STATES t = b[i].Transition.StateBefore;
            b[i].Transition.StateBefore = b[i].Transition.StateAfter;
            b[i].Transition.StateAfter = t;
        }
    }
};

NVSDK_NGX_Result dx12_evaluate(const vio_upscale_native_dispatch *nd, NVSDK_NGX_Handle *h, NVSDK_NGX_Parameter *p,
                               float jitter_x, float jitter_y)
{
    const vio_upscale_dispatch_desc *d = nd->desc;
    ID3D12GraphicsCommandList *list = static_cast<ID3D12GraphicsCommandList *>(nd->command_list);
    Dx12States st;
    st.add(&nd->output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const D3D12_RESOURCE_STATES in = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    st.add(&nd->color, in);
    st.add(&nd->depth, in);
    st.add(&nd->motion, in);
    st.add(&nd->reactive, in);
    st.add(&nd->exposure, in);
    if (st.n) list->ResourceBarrier(st.n, st.b);

    NVSDK_NGX_D3D12_DLSS_Eval_Params ev;
    memset(&ev, 0, sizeof(ev));
    ev.Feature.pInColor = static_cast<ID3D12Resource *>(nd->color.handle);
    ev.Feature.pInOutput = static_cast<ID3D12Resource *>(nd->output.handle);
    ev.pInDepth = static_cast<ID3D12Resource *>(nd->depth.handle);
    ev.pInMotionVectors = static_cast<ID3D12Resource *>(nd->motion.handle);
    ev.pInExposureTexture = static_cast<ID3D12Resource *>(nd->exposure.handle);
    ev.pInBiasCurrentColorMask = static_cast<ID3D12Resource *>(nd->reactive.handle);
    ev.InJitterOffsetX = jitter_x;
    ev.InJitterOffsetY = jitter_y;
    ev.InRenderSubrectDimensions.Width = (unsigned int)d->render_width;
    ev.InRenderSubrectDimensions.Height = (unsigned int)d->render_height;
    ev.InReset = d->reset ? 1 : 0;
    ev.InMVScaleX = d->mv_scale_x;
    ev.InMVScaleY = d->mv_scale_y;
    ev.InPreExposure = d->pre_exposure;
    ev.InExposureScale = 1.0f;
    ev.InFrameTimeDeltaInMsec = d->frame_time_ms;
    NVSDK_NGX_Result rc = NGX_D3D12_EVALUATE_DLSS_EXT(list, h, p, &ev);

    st.flip();
    if (st.n) list->ResourceBarrier(st.n, st.b);
    return rc;
}

/* NVIDIA's driver version from the UMD version DXGI reports (a.b.c.d with
 * c = 1x, d = yyzz -> "xyy.zz"). */
void dx12_driver_version(const vio_upscale_device *dev, char *out, size_t out_len)
{
    out[0] = '\0';
    ID3D12Device *device = static_cast<ID3D12Device *>(dev->device);
    LUID luid = device->GetAdapterLuid();
    IDXGIFactory4 *f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void **>(&f))) || !f) return;
    IDXGIAdapter *a = nullptr;
    if (SUCCEEDED(f->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter), reinterpret_cast<void **>(&a))) && a) {
        DXGI_ADAPTER_DESC ad;
        LARGE_INTEGER umd;
        if (SUCCEEDED(a->GetDesc(&ad)) && SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            unsigned c = HIWORD(umd.LowPart), dd = LOWORD(umd.LowPart);
            if (ad.VendorId == 0x10DE) {
                unsigned v = (c % 10) * 10000 + dd;
                snprintf(out, out_len, "%u.%02u", v / 100, v % 100);
            } else {
                snprintf(out, out_len, "%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart), c, dd);
            }
        }
        a->Release();
    }
    f->Release();
}

const VioDlssApi g_api = {
    dx12_init, dx12_shutdown, dx12_caps, dx12_alloc, dx12_free, dx12_create, dx12_evaluate, dx12_release,
    dx12_driver_version, nullptr,
};

} // namespace

const VioDlssApi *const vio_dlss_api_dx12 = &g_api;

#else

const VioDlssApi *const vio_dlss_api_dx12 = nullptr;

#endif
