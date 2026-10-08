/*
 * php-vio - Direct3D 12 Backend implementation
 *
 * Uses D3D12 with explicit resource management, command lists, and fence synchronization.
 * Double-buffered swapchain with per-frame command allocators.
 * Shaders: GLSL -> SPIR-V -> HLSL (SM 5.1) via SPIRV-Cross -> DXBC via D3DCompile.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#ifdef HAVE_D3D12

#define COBJMACROS
#define INITGUID
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>

/* Work graphs need the Windows 11 24H2 SDK (10.0.26100) headers. */
#if defined(__ID3D12WorkGraphProperties_INTERFACE_DEFINED__) && defined(__ID3D12GraphicsCommandList10_INTERFACE_DEFINED__) \
    && defined(__ID3D12StateObjectProperties1_INTERFACE_DEFINED__)
#define VIO_D3D12_HAS_WORK_GRAPHS 1
#endif

#include "../../../include/vio_platform.h"

#include "vio_d3d12.h"
#include "../../vio_cubemap.h"   /* bindless cube slots */
#include "../vio_d3d_common.h"
#include "../vio_d3d_shader_check.h"
#include "../../vio_texfmt.h"
#include "../../vio_shader_cache.h"
#include "../../vio_render_target.h"

/* DXC front end (vio_dxc.cpp, GAP-PHASE5 Block 7). */
int  vio_dxc_available(void);
void vio_dxc_set_dir(const char *dir);
int  vio_dxc_highest_minor(int max_minor);
int  vio_dxc_compile(const char *hlsl, const char *entry, const char *profile, int debug,
                     int enable_16bit, void **out_bytes, size_t *out_len, char **out_error);
#include "../../vio_shader_reflect.h"   /* vio_spirv_reflect — data-driven compute register mapping */
#include "../../vio_shader_compiler.h"  /* vio_compile_glsl_stage_to_spirv — geometry / tessellation stages */
#include "../../vio_tess_hlsl.h"

static HRESULT d3d12_compile_cached(const char *src, const char *entry_tag, const char *profile, UINT flags, ID3DBlob **out);
static int d3d12_hlsl_target(void);
static const char *d3d12_profile(const char *profile, char *buf, size_t n);
#include "../../vio_texture.h"          /* vio_texture_object — storage-image binds */
#include "../../vio_font.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

vio_d3d12_state vio_d3d12 = {0};

/* Currently bound pipeline (for vertex stride in draw calls) */
static vio_d3d12_pipeline *d3d12_current_pipeline = NULL;
static void d3d12_compute_wait(void);   /* defined with the compute path; used by the readback helper */

/* Pull pending validation messages out of the D3D12 InfoQueue and forward them
 * to PHP's error log. No-op when the debug layer is not active. Called after
 * any operation that might have triggered validation errors (resource creation
 * failures, present(), etc.) so the underlying cause shows up next to the
 * symptomatic failure rather than scrolling past in the Windows event log. */
static void d3d12_drain_info_queue(const char *context)
{
    /* The InfoQueue is resolved ONCE, at init (vio_d3d12.info_queue), and is NULL
     * whenever the debug layer is inactive — which is every release run.
     *
     * This used to QueryInterface(IID_ID3D12InfoQueue) on the device on every
     * call. begin_frame() calls this every frame, so a release build paid a COM
     * QI (that was guaranteed to FAIL) once per frame, forever, to learn
     * something already known at startup. Now: one pointer test. */
    ID3D12InfoQueue *iq = vio_d3d12.info_queue;
    if (!iq) return;

    UINT64 count = ID3D12InfoQueue_GetNumStoredMessagesAllowedByRetrievalFilter(iq);
    for (UINT64 i = 0; i < count; i++) {
        SIZE_T size = 0;
        ID3D12InfoQueue_GetMessage(iq, i, NULL, &size);
        if (size == 0) continue;
        D3D12_MESSAGE *msg = (D3D12_MESSAGE *)malloc(size);
        if (!msg) continue;
        if (SUCCEEDED(ID3D12InfoQueue_GetMessage(iq, i, msg, &size))) {
            const char *sev = "INFO";
            switch (msg->Severity) {
                case D3D12_MESSAGE_SEVERITY_CORRUPTION: sev = "CORRUPTION"; break;
                case D3D12_MESSAGE_SEVERITY_ERROR:      sev = "ERROR";      break;
                case D3D12_MESSAGE_SEVERITY_WARNING:    sev = "WARNING";    break;
                case D3D12_MESSAGE_SEVERITY_INFO:       sev = "INFO";       break;
                case D3D12_MESSAGE_SEVERITY_MESSAGE:    sev = "MESSAGE";    break;
            }
            /* Emit via DIRECT fprintf(stderr) — not php_error_docref — because the
             * PHP error channel can be swallowed when called from inside a frame
             * callback even with display_errors=stderr. fprintf reliably surfaces. */
            fprintf(stderr, "D3D12[%s] [%s] id=%d: %s\n",
                    context ? context : "?", sev, (int)msg->ID,
                    msg->pDescription ? msg->pDescription : "");
            fflush(stderr);
        }
        free(msg);
    }
    ID3D12InfoQueue_ClearStoredMessages(iq);
    /* No Release here — vio_d3d12.info_queue owns the reference for the device's
     * lifetime and is released in d3d12_shutdown(). */
}

/* Map a DRED auto-breadcrumb op enum to a short readable name. Covers the ops
 * vio actually issues; anything else is printed by numeric value. */
static const char *d3d12_dred_op_name(D3D12_AUTO_BREADCRUMB_OP op)
{
    switch (op) {
        case D3D12_AUTO_BREADCRUMB_OP_SETMARKER:                 return "SetMarker";
        case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT:               return "BeginEvent";
        case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT:                 return "EndEvent";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED:            return "DrawInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED:     return "DrawIndexedInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT:          return "ExecuteIndirect";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCH:                 return "Dispatch";
        case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION:         return "CopyBufferRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION:        return "CopyTextureRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE:             return "CopyResource";
        case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE:       return "ResolveSubresource";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW:    return "ClearRenderTargetView";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUnorderedAccessView";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW:    return "ClearDepthStencilView";
        case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:          return "ResourceBarrier";
        case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE:            return "ExecuteBundle";
        case D3D12_AUTO_BREADCRUMB_OP_PRESENT:                  return "Present";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS:            return "DispatchRays";
        default:                                                return "Op";
    }
}

/* Dump DRED (Device Removed Extended Data) after a device-removed event:
 * the GPU auto-breadcrumb history (last ops the GPU actually executed before
 * it hung) and the page-fault VA (the address the GPU faulted on). This is the
 * payload that tells us WHICH command/resource hung. Written via direct
 * fprintf(stderr)+fflush so PHP notice-suppression can never swallow it.
 *
 * Safe to call even when DRED was not enabled or the device does not implement
 * the interface — it just prints that DRED data is unavailable. */
static void d3d12_dump_dred(const char *context)
{
    if (!vio_d3d12.device) return;

    ID3D12DeviceRemovedExtendedData *dred = NULL;
    HRESULT hr = ID3D12Device_QueryInterface(vio_d3d12.device,
                     &IID_ID3D12DeviceRemovedExtendedData, (void **)&dred);
    if (FAILED(hr) || !dred) {
        fprintf(stderr, "D3D12-DRED[%s]: data UNAVAILABLE "
                        "(DRED not enabled at init? set VIO_D3D12_DRED=1)\n",
                context ? context : "?");
        fflush(stderr);
        return;
    }

    /* Auto-breadcrumbs: a linked list of command-list breadcrumb nodes, each
     * with a ring of op markers. The LAST completed value vs the command count
     * tells us where the GPU stopped — the first NOT-yet-completed op is the
     * one that hung. */
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc = {0};
    if (SUCCEEDED(ID3D12DeviceRemovedExtendedData_GetAutoBreadcrumbsOutput(dred, &bc))) {
        const D3D12_AUTO_BREADCRUMB_NODE *node = bc.pHeadAutoBreadcrumbNode;
        int node_idx = 0;
        if (!node) {
            fprintf(stderr, "D3D12-DRED[%s]: no breadcrumb nodes "
                            "(GPU may not have started executing the failing list)\n",
                    context ? context : "?");
        }
        while (node) {
            UINT last_done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            fprintf(stderr, "D3D12-DRED[%s]: breadcrumb node %d cmdlist='%ls' queue='%ls' "
                            "ops=%u lastCompleted=%u%s\n",
                    context ? context : "?", node_idx,
                    node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"(unnamed)",
                    node->pCommandQueueDebugNameW ? node->pCommandQueueDebugNameW : L"(unnamed)",
                    node->BreadcrumbCount, last_done,
                    (last_done < node->BreadcrumbCount) ? "  <-- HUNG HERE" : " (completed)");

            /* Print a window of ops around the failure point so the dump stays
             * readable: a few before lastCompleted through the first unfinished. */
            UINT count = node->BreadcrumbCount;
            UINT start = (last_done > 8) ? last_done - 8 : 0;
            UINT end   = (last_done + 2 < count) ? last_done + 2 : count;
            for (UINT i = start; i < end; i++) {
                D3D12_AUTO_BREADCRUMB_OP op = node->pCommandHistory[i];
                const char *mark = (i == last_done) ? "  >>> first NOT executed (suspected hang)"
                                 : (i <  last_done) ? "      done"
                                 :                    "      pending";
                fprintf(stderr, "D3D12-DRED[%s]:     [%u] %s (op=%d)%s\n",
                        context ? context : "?", i,
                        d3d12_dred_op_name(op), (int)op, mark);
            }
            node = node->pNext;
            node_idx++;
            if (node_idx > 32) { /* guard against an unexpectedly long list */
                fprintf(stderr, "D3D12-DRED[%s]: ... (more nodes truncated)\n",
                        context ? context : "?");
                break;
            }
        }
    } else {
        fprintf(stderr, "D3D12-DRED[%s]: GetAutoBreadcrumbsOutput failed\n",
                context ? context : "?");
    }

    /* Page fault: the GPU virtual address the device faulted on, plus the lists
     * of resource allocations (live and recently-freed) that occupied that VA.
     * A VA that matches a RECENTLY-FREED allocation is the classic "used after
     * free" — a resource released while still referenced by an in-flight list
     * (exactly the root-CBV / RT-lifetime family of bugs). */
    D3D12_DRED_PAGE_FAULT_OUTPUT pf = {0};
    if (SUCCEEDED(ID3D12DeviceRemovedExtendedData_GetPageFaultAllocationOutput(dred, &pf))) {
        if (pf.PageFaultVA != 0) {
            fprintf(stderr, "D3D12-DRED[%s]: PAGE FAULT VA = 0x%llx\n",
                    context ? context : "?", (unsigned long long)pf.PageFaultVA);
            const D3D12_DRED_ALLOCATION_NODE *an = pf.pHeadExistingAllocationNode;
            int n = 0;
            while (an && n < 32) {
                fprintf(stderr, "D3D12-DRED[%s]:   existing alloc: '%ls' type=%d\n",
                        context ? context : "?",
                        an->ObjectNameW ? an->ObjectNameW : L"(unnamed)",
                        (int)an->AllocationType);
                an = an->pNext; n++;
            }
            an = pf.pHeadRecentFreedAllocationNode; n = 0;
            while (an && n < 32) {
                fprintf(stderr, "D3D12-DRED[%s]:   RECENTLY FREED alloc (use-after-free suspect): "
                                "'%ls' type=%d\n",
                        context ? context : "?",
                        an->ObjectNameW ? an->ObjectNameW : L"(unnamed)",
                        (int)an->AllocationType);
                an = an->pNext; n++;
            }
        } else {
            fprintf(stderr, "D3D12-DRED[%s]: no page fault recorded "
                            "(hang was likely a long-running/infinite shader, not a bad VA)\n",
                    context ? context : "?");
        }
    } else {
        fprintf(stderr, "D3D12-DRED[%s]: GetPageFaultAllocationOutput failed\n",
                context ? context : "?");
    }

    fflush(stderr);
    ID3D12DeviceRemovedExtendedData_Release(dred);
}

/* Called from any path that detects a FAILED HRESULT which may be a device
 * loss. Logs the removal reason + full DRED breadcrumbs/page-fault EXACTLY
 * ONCE, then latches vio_d3d12.device_lost so subsequent frames stop trying to
 * Present (which would just re-fail and spam the log until it fills up). */
static void d3d12_handle_device_removed(const char *context, HRESULT present_hr)
{
    if (vio_d3d12.device_lost) return;   /* already reported once */
    vio_d3d12.device_lost = 1;

    HRESULT reason = vio_d3d12.device
        ? ID3D12Device_GetDeviceRemovedReason(vio_d3d12.device)
        : present_hr;

    fprintf(stderr,
        "\n==================== D3D12 DEVICE REMOVED ====================\n"
        "D3D12[%s]: device removed. present_hr=0x%08lx GetDeviceRemovedReason=0x%08lx\n"
        "  0x887A0006 = DXGI_ERROR_DEVICE_HUNG\n"
        "  0x887A0005 = DXGI_ERROR_DEVICE_REMOVED\n"
        "  0x887A0007 = DXGI_ERROR_DEVICE_RESET\n"
        "  0x887A0020 = DXGI_ERROR_DRIVER_INTERNAL_ERROR\n",
        context ? context : "?", (unsigned long)present_hr, (unsigned long)reason);
    fflush(stderr);

    /* Surface any pending validation messages first, then the DRED payload. */
    d3d12_drain_info_queue(context);
    d3d12_dump_dred(context);

    fprintf(stderr,
        "D3D12[%s]: rendering halted after first device-removal "
        "(further Present calls suppressed to keep this log readable).\n"
        "=============================================================\n\n",
        context ? context : "?");
    fflush(stderr);
}

/* Forward declarations */
extern char *vio_spirv_to_hlsl(const uint32_t *spirv, size_t spirv_size,
                                int shader_model, char **error_msg);
extern uint32_t *vio_compile_glsl_to_spirv(const char *source, int stage,
                                            size_t *out_size, char **error_msg);
extern uint32_t *vio_compile_glsl_compute_to_spirv(const char *source,
                                                   size_t *out_size, char **error_msg);

/* ── Helpers ──────────────────────────────────────────────────────── */
/* vio_format_to_dxgi, vio_format_byte_size, vio_usage_to_semantic from vio_d3d_common.h */

static D3D12_PRIMITIVE_TOPOLOGY vio_topology_to_d3d12(vio_topology t, int patch_vertices)
{
    switch (t) {
        case VIO_TRIANGLES:      return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case VIO_TRIANGLE_STRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case VIO_LINES:          return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case VIO_LINE_STRIP:     return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case VIO_POINTS:         return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        case VIO_LINES_ADJACENCY:          return D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
        case VIO_LINE_STRIP_ADJACENCY:     return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ;
        case VIO_TRIANGLES_ADJACENCY:      return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ;
        case VIO_TRIANGLE_STRIP_ADJACENCY: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
        case VIO_TRIANGLE_FAN:   return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case VIO_PATCHES: {
            /* N_CONTROL_POINT_PATCHLIST values are contiguous from 1 (=33). */
            int n = patch_vertices > 0 ? patch_vertices : 3;
            if (n > 32) n = 32;
            return (D3D12_PRIMITIVE_TOPOLOGY)(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + (n - 1));
        }
        default:                 return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

static D3D12_PRIMITIVE_TOPOLOGY_TYPE vio_topology_to_d3d12_type(vio_topology t)
{
    switch (t) {
        case VIO_TRIANGLES:
        case VIO_TRIANGLE_STRIP:
        case VIO_TRIANGLE_FAN:   return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        case VIO_LINES:
        case VIO_LINE_STRIP:     return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case VIO_POINTS:         return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case VIO_LINES_ADJACENCY:
        case VIO_LINE_STRIP_ADJACENCY:     return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case VIO_TRIANGLES_ADJACENCY:
        case VIO_TRIANGLE_STRIP_ADJACENCY: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        case VIO_PATCHES:        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        default:                 return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

/* ── GPU Synchronization ──────────────────────────────────────────── */

void vio_d3d12_wait_for_gpu(void)
{
    if (!vio_d3d12.cmd_queue || !vio_d3d12.fence) return;

    vio_d3d12.fence_value++;
    ID3D12CommandQueue_Signal(vio_d3d12.cmd_queue, vio_d3d12.fence, vio_d3d12.fence_value);

    if (ID3D12Fence_GetCompletedValue(vio_d3d12.fence) < vio_d3d12.fence_value) {
        ID3D12Fence_SetEventOnCompletion(vio_d3d12.fence, vio_d3d12.fence_value,
                                          vio_d3d12.fence_event);
        WaitForSingleObject(vio_d3d12.fence_event, INFINITE);
    }
}

static void d3d12_wait_for_frame(UINT frame_idx)
{
    vio_d3d12_frame *frame = &vio_d3d12.frames[frame_idx];

    if (ID3D12Fence_GetCompletedValue(vio_d3d12.fence) < frame->fence_value) {
        ID3D12Fence_SetEventOnCompletion(vio_d3d12.fence, frame->fence_value,
                                          vio_d3d12.fence_event);
        WaitForSingleObject(vio_d3d12.fence_event, INFINITE);
    }
}

/* ── Descriptor heap helpers ──────────────────────────────────────── */

/* VIO_SHADING_RATE_* -> D3D12_SHADING_RATE (log2 width << 2 | log2 height). */
static D3D12_SHADING_RATE d3d12_shading_rate_value(int rate)
{
    switch (rate) {
        case VIO_SHADING_RATE_1X2: return D3D12_SHADING_RATE_1X2;
        case VIO_SHADING_RATE_2X1: return D3D12_SHADING_RATE_2X1;
        case VIO_SHADING_RATE_2X2: return D3D12_SHADING_RATE_2X2;
        case VIO_SHADING_RATE_4X4: return D3D12_SHADING_RATE_4X4;
        default:                   return D3D12_SHADING_RATE_1X1;
    }
}

/* Variable rate shading (GAP-PHASE5 Block 12): RSSetShadingRate is command-list
 * state, so the sticky rate is re-armed after every Reset of the frame list. */
static void d3d12_apply_shading_rate(void)
{
    if (!vio_d3d12.cmd_list || vio_d3d12.vrs_tier <= 0) return;
    if (!vio_d3d12.cmd_list5 &&
        FAILED(ID3D12GraphicsCommandList_QueryInterface(vio_d3d12.cmd_list, &IID_ID3D12GraphicsCommandList5, (void **)&vio_d3d12.cmd_list5))) {
        vio_d3d12.cmd_list5 = NULL;
        vio_d3d12.vrs_tier = 0;   /* runtime too old for the interface: report honestly */
        return;
    }
    D3D12_SHADING_RATE r = d3d12_shading_rate_value(vio_d3d12.shading_rate);
    /* Tier 2: a pipeline whose vertex stage writes SV_ShadingRate overrides the
     * set rate with the per-primitive one; every other pipeline keeps it. The
     * shading-rate image then combines by MAX (the coarser rate wins). */
    if (vio_d3d12.vrs_tier >= 2) {
        int image = vio_d3d12.vrs_image_active && vio_d3d12.vrs_image;
        D3D12_SHADING_RATE_COMBINER comb[2] = {
            (d3d12_current_pipeline && d3d12_current_pipeline->writes_shading_rate)
                ? D3D12_SHADING_RATE_COMBINER_OVERRIDE : D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
            image ? D3D12_SHADING_RATE_COMBINER_MAX : D3D12_SHADING_RATE_COMBINER_PASSTHROUGH };
        ID3D12GraphicsCommandList5_RSSetShadingRate(vio_d3d12.cmd_list5, r, comb);
        ID3D12GraphicsCommandList5_RSSetShadingRateImage(vio_d3d12.cmd_list5, image ? vio_d3d12.vrs_image : NULL);
        return;
    }
    ID3D12GraphicsCommandList5_RSSetShadingRate(vio_d3d12.cmd_list5, r, NULL);
}

static int d3d12_upload_subresources(ID3D12Resource *dst, const D3D12_RESOURCE_DESC *rd,
                                     UINT first_sub, UINT num_sub,
                                     const void *const *src, const UINT *src_row_pitch,
                                     const UINT *src_rows, const UINT *src_slices,
                                     D3D12_RESOURCE_STATES state_before,
                                     D3D12_RESOURCE_STATES state_after,
                                     UINT dst_x, UINT dst_y, UINT dst_z);
static void d3d12_retire_later(ID3D12Resource *res, UINT64 fence);
static void d3d12_retire_object_later(IUnknown *obj, UINT64 fence);
static void d3d12_release_parked(UINT slot);
static void d3d12_release_parked_all(void);
/* Recorded draw sequences (BUNDLE-PLAN phase 3), defined with the draw code. */
typedef struct _vio_d3d12_bundle vio_d3d12_bundle;
static vio_d3d12_bundle *d3d12_brec;   /* the bundle being recorded, NULL otherwise */
static void d3d12_brec_fail(void);
static int  d3d12_brec_sampler_table(const int *combos, D3D12_GPU_DESCRIPTOR_HANDLE *out);
static void d3d12_bundles_sweep(void);
static void d3d12_bundle_graves_collect(int force);
/* The top of the sampler heap holds the sampler tables of bundles (one per
 * distinct set, kept for the device's life); the frame rings share the rest. */
#define VIO_D3D12_BUNDLE_SAMPLER_SETS    32
#define VIO_D3D12_BUNDLE_SAMPLER_RESERVE (VIO_D3D12_BUNDLE_SAMPLER_SETS * VIO_D3D12_SAMPLER_TABLE_SIZE)

/* Shading-rate image (VIO_FEATURE_SHADING_RATE_IMAGE, Tier 2): an R8_UINT
 * texture with one D3D12_SHADING_RATE per tile. A new tile count recreates it -
 * outside a frame after a GPU drain, inside one the old texture stays alive to
 * the end of the context (the recorded list may still reference it); the same
 * tile count only re-uploads, ordered by the single DIRECT queue. */
static int d3d12_set_shading_rate_image(const unsigned char *rates, int tiles_x, int tiles_y)
{
    if (vio_d3d12.vrs_tier < 2 || vio_d3d12.vrs_tile_size <= 0) return -1;
    if (!rates) {
        vio_d3d12.vrs_image_active = 0;
        if (vio_d3d12.in_frame && vio_d3d12.cmd_list) d3d12_apply_shading_rate();
        return 0;
    }
    if (tiles_x <= 0 || tiles_y <= 0) return -1;
    size_t n = (size_t)tiles_x * (size_t)tiles_y;
    unsigned char *texels = (unsigned char *)malloc(n);
    if (!texels) return -1;
    for (size_t i = 0; i < n; i++) {
        if (rates[i] == VIO_SHADING_RATE_4X4 && !vio_d3d12.vrs_additional_rates) { free(texels); return -1; }
        texels[i] = (unsigned char)d3d12_shading_rate_value(rates[i]);
    }
    if (vio_d3d12.vrs_image && (vio_d3d12.vrs_image_w != tiles_x || vio_d3d12.vrs_image_h != tiles_y)) {
        if (vio_d3d12.in_frame) {
            d3d12_retire_later(vio_d3d12.vrs_image, UINT64_MAX);   /* released at shutdown */
        } else {
            vio_d3d12_wait_for_gpu();
            ID3D12Resource_Release(vio_d3d12.vrs_image);
        }
        vio_d3d12.vrs_image = NULL;
    }
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)tiles_x;
    rd.Height = (UINT)tiles_y;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8_UINT;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (!vio_d3d12.vrs_image) {
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(ID3D12Device_CreateCommittedResource(vio_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                       D3D12_RESOURCE_STATE_COMMON, NULL,
                                                       &IID_ID3D12Resource, (void **)&vio_d3d12.vrs_image))) {
            vio_d3d12.vrs_image = NULL;
            free(texels);
            return -1;
        }
        vio_d3d12.vrs_image_w = tiles_x;
        vio_d3d12.vrs_image_h = tiles_y;
        vio_d3d12.vrs_image_in_source = 0;
    }
    const void *src = texels;
    UINT pitch = (UINT)tiles_x, rows = (UINT)tiles_y, slices = 1;
    int rc = d3d12_upload_subresources(vio_d3d12.vrs_image, &rd, 0, 1, &src, &pitch, &rows, &slices,
                                       vio_d3d12.vrs_image_in_source ? D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE
                                                                     : D3D12_RESOURCE_STATE_COMMON,
                                       D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE, 0, 0, 0);
    free(texels);
    if (rc != 0) return -1;
    vio_d3d12.vrs_image_in_source = 1;
    vio_d3d12.vrs_image_active = 1;
    if (vio_d3d12.in_frame && vio_d3d12.cmd_list) d3d12_apply_shading_rate();
    return 0;
}

static int d3d12_shading_rate_tile_size(void)
{
    return vio_d3d12.vrs_tier >= 2 ? vio_d3d12.vrs_tile_size : 0;
}

static int d3d12_set_shading_rate(int rate)
{
    if (vio_d3d12.vrs_tier <= 0) return -1;
    if (rate == VIO_SHADING_RATE_4X4 && !vio_d3d12.vrs_additional_rates) return -1;
    vio_d3d12.shading_rate = rate;
    if (vio_d3d12.in_frame && vio_d3d12.cmd_list) d3d12_apply_shading_rate();
    return vio_d3d12.vrs_tier > 0 ? 0 : -1;
}

static int d3d12_create_descriptor_heap(ID3D12DescriptorHeap **out, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                         UINT count, D3D12_DESCRIPTOR_HEAP_FLAGS flags)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc = {0};
    desc.Type = type;
    desc.NumDescriptors = count;
    desc.Flags = flags;
    desc.NodeMask = 0;

    HRESULT hr = ID3D12Device_CreateDescriptorHeap(vio_d3d12.device, &desc,
                                                    &IID_ID3D12DescriptorHeap, (void **)out);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create descriptor heap (0x%08lx)", hr);
        return -1;
    }
    return 0;
}

/* Allocate a descriptor from the SRV heap, returns index or UINT_MAX on overflow.
 *
 * Static SRVs grow DOWNWARD from the top of the heap (index = capacity-1, then
 * capacity-2, …). The per-frame allocator (see d3d12_begin_frame /
 * vio_d3d12_flush_srv_table) grows UPWARD from index 0. The two regions never
 * overlap until the heap is full, so a texture created mid-frame can never
 * land in any frame's per-frame region — fixing the previous bug where lazy
 * texture loads (e.g. menu icons) had their freshly-created SRVs immediately
 * stomped by flush_srv_table's null-init sweep on the very next bound draw. */
static UINT d3d12_alloc_srv_descriptor(D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu,
                                        D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu)
{
    if (vio_d3d12.srv_heap.count >= vio_d3d12.srv_heap.capacity) {
        php_error_docref(NULL, E_WARNING, "D3D12: SRV descriptor heap full (%u/%u)",
                          vio_d3d12.srv_heap.count, vio_d3d12.srv_heap.capacity);
        memset(out_cpu, 0, sizeof(*out_cpu));
        memset(out_gpu, 0, sizeof(*out_gpu));
        return UINT_MAX;
    }
    UINT idx = vio_d3d12.srv_heap.capacity - 1 - vio_d3d12.srv_heap.count;
    vio_d3d12.srv_heap.count++;

    /* CPU handle into the staging (non-shader-visible) heap — this is the one
     * CreateShaderResourceView writes to and that flush_srv_table reads from.
     * The GPU handle points at the matching slot in the shader-visible heap
     * so callers that want to bind without per-frame copying still have a
     * valid GPU descriptor at the same index. */
    D3D12_CPU_DESCRIPTOR_HANDLE staging_start;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.srv_staging_heap, &staging_start);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(vio_d3d12.srv_heap.heap, &gpu_start);

    out_cpu->ptr = staging_start.ptr + (SIZE_T)(idx * vio_d3d12.srv_heap.descriptor_size);
    out_gpu->ptr = gpu_start.ptr + (UINT64)(idx * vio_d3d12.srv_heap.descriptor_size);
    return idx;
}

/* ── Root Signature ───────────────────────────────────────────────── */

/* The bindless table's samplers, s1..s4 in space 1 (BINDLESS-PLAN): vio_sampler
 * (linear, repeat), vio_sampler_nearest, vio_sampler_clamp, vio_sampler_nearest_clamp.
 * Graphics and compute root signatures carry the same four. */
static void d3d12_bindless_static_samplers(D3D12_STATIC_SAMPLER_DESC out[4])
{
    for (int v = 0; v < 4; v++) {
        D3D12_STATIC_SAMPLER_DESC *s = &out[v];
        D3D12_TEXTURE_ADDRESS_MODE am = (v & 2) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        memset(s, 0, sizeof(*s));
        s->Filter = (v & 1) ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        s->AddressU = s->AddressV = s->AddressW = am;
        s->ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        s->MaxLOD = D3D12_FLOAT32_MAX;
        s->ShaderRegister = 1 + (UINT)v;
        s->RegisterSpace = 1;
        s->ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
}

/* The bindless table's SRV ranges: Texture2D[] (space 1), TextureCube[] (space 3)
 * and Texture2DArray[] (space 4), all unbounded over the same descriptors. */
static void d3d12_bindless_ranges(D3D12_DESCRIPTOR_RANGE out[3])
{
    memset(out, 0, sizeof(D3D12_DESCRIPTOR_RANGE) * 3);
    for (int r = 0; r < 3; r++) {
        out[r].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        out[r].NumDescriptors = UINT_MAX;   /* unbounded */
        out[r].BaseShaderRegister = 0;
        out[r].RegisterSpace = r == 0 ? 1 : 2 + (UINT)r;
        out[r].OffsetInDescriptorsFromTableStart = 0;
    }
}

static int d3d12_build_root_signature(int mesh, ID3D12RootSignature **out)
{
    /*
     * Root signature layout:
     *   [0] CBV (b0) — vertex stage constants (model, view, projection, etc.)
     *   [1] CBV (b0) — pixel stage constants (lights, material, etc.)
     *   [2] Descriptor table: SRV (t0..t15) — textures (regular t0-t3, shadow t4-t7)
     *   Static samplers: s0 (linear wrap), s1 (comparison for shadow)
     *
     * VS and PS each have their own cbuffer at b0 (different data, same register).
     * Separate root params with per-stage visibility allow independent binding.
     *
     *   [3] SRV (t0) — VERTEX visibility: a storage buffer the vertex shader
     *       reads per-instance (Path B, VIO_FEATURE_VERTEX_STORAGE). Overlaps
     *       the PS SRV table at t0 but with different (VERTEX vs PIXEL)
     *       visibility, which D3D12 permits. A root SRV (raw/structured buffer)
     *       set via SetGraphicsRootShaderResourceView; unused by shaders that
     *       don't declare the SSBO, so normal draws leave it unset.
     *
     *   [5..7]   CBV b0 for the GEOMETRY / HULL / DOMAIN stage (vio_shader
     *            'geometry' / 'tess_control' / 'tess_eval' constant blocks).
     *   [8..10]  SRV table t0..t15 with GEOMETRY / HULL / DOMAIN visibility -
     *            the per-draw SRV block bound at [2] is re-pointed here when
     *            the bound pipeline has the stage (vio_d3d12_flush_srv_table).
     *   [11..13] Sampler table s0..s7, same replication as the SRV table.
     *   Indices: VIO_D3D12_RP_* in vio_d3d12.h. Root cost: 5 CBVs (10 DWORDs)
     *   + 1 root SRV (2) + 8 tables (8) = 20 of 64 DWORDs.
     */
    D3D12_ROOT_PARAMETER params[VIO_D3D12_RP_COUNT] = {0};

    /* [0] CBV b0 — vertex shader only */
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = mesh ? D3D12_SHADER_VISIBILITY_MESH : D3D12_SHADER_VISIBILITY_VERTEX;

    /* [1] CBV b0 — pixel shader only */
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].Descriptor.RegisterSpace = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    /* [2] SRV table t0..t(N-1). Regular textures occupy t0-t7, shadow/depth
     * samplers t8-t11 (SPIRV-Cross assigns depth samplers from register 8 — see
     * vio_shader_reflect.c). Sized to VIO_D3D12_SRV_TABLE_SIZE so high shadow
     * registers are always covered by the table and never dropped at bind. */
    D3D12_DESCRIPTOR_RANGE srv_range = {0};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = VIO_D3D12_SRV_TABLE_SIZE;
    srv_range.BaseShaderRegister = 0;
    srv_range.RegisterSpace = 0;
    srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &srv_range;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    /* [3] Root SRV t0 — vertex-stage storage buffer (Path B). */
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].Descriptor.ShaderRegister = 0;
    /* Mesh variant: space 2 (vio_shader_reflect.c moves mesh / task storage
     * buffers there), since the mesh stage also sees the SRV table at t0. */
    params[3].Descriptor.RegisterSpace = 2;   /* vertex / mesh storage buffers live in space 2 (A28) */
    params[3].ShaderVisibility = mesh ? D3D12_SHADER_VISIBILITY_MESH : D3D12_SHADER_VISIBILITY_VERTEX;

    /* [4] Sampler table s0..s7 — one per regular texture register (GAP-PLAN
     * Phase 1). Filled per draw from each bound texture's sampler combo, so
     * filter / wrap / anisotropy are honoured like on D3D11. */
    D3D12_DESCRIPTOR_RANGE sampler_range = {0};
    sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    sampler_range.NumDescriptors = VIO_D3D12_SAMPLER_TABLE_SIZE;
    sampler_range.BaseShaderRegister = 0;
    sampler_range.RegisterSpace = 0;
    sampler_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 1;
    params[4].DescriptorTable.pDescriptorRanges = &sampler_range;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    /* [5..13] Optional-stage mirrors: CBV b0, SRV table, sampler table per
     * GEOMETRY / HULL / DOMAIN visibility. Same register ranges as the PS /
     * VS ones - D3D12 allows the overlap because the visibilities differ. */
    {
        const D3D12_SHADER_VISIBILITY vis[3] = {
            D3D12_SHADER_VISIBILITY_GEOMETRY, D3D12_SHADER_VISIBILITY_HULL, D3D12_SHADER_VISIBILITY_DOMAIN
        };
        for (int s = 0; s < 3; s++) {
            D3D12_ROOT_PARAMETER *cbv = &params[VIO_D3D12_RP_GS_CBV + s];
            cbv->ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            cbv->Descriptor.ShaderRegister = 0;
            cbv->Descriptor.RegisterSpace = 0;
            cbv->ShaderVisibility = vis[s];
            /* Mesh variant (mesh pipelines have no GS / HS / DS): the GS CBV
             * slot carries the vertex-stage block to the amplification stage. */
            if (mesh && s == 0) cbv->ShaderVisibility = D3D12_SHADER_VISIBILITY_AMPLIFICATION;

            D3D12_ROOT_PARAMETER *srv = &params[VIO_D3D12_RP_GS_SRV + s];
            srv->ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            srv->DescriptorTable.NumDescriptorRanges = 1;
            srv->DescriptorTable.pDescriptorRanges = &srv_range;
            srv->ShaderVisibility = vis[s];

            D3D12_ROOT_PARAMETER *smp = &params[VIO_D3D12_RP_GS_SAMPLER + s];
            smp->ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            smp->DescriptorTable.NumDescriptorRanges = 1;
            smp->DescriptorTable.pDescriptorRanges = &sampler_range;
            smp->ShaderVisibility = vis[s];
            /* Mesh variant: the GS tables serve the amplification stage, the HS
             * tables the mesh stage (textures in mesh / task, OPEN-ITEMS-PLAN A30). */
            if (mesh && s < 2) {
                D3D12_SHADER_VISIBILITY mv = s == 0 ? D3D12_SHADER_VISIBILITY_AMPLIFICATION : D3D12_SHADER_VISIBILITY_MESH;
                srv->ShaderVisibility = mv;
                smp->ShaderVisibility = mv;
            }
        }
    }

    /* Static samplers: s8-s11 = comparison (shadow maps with sampler2DShadow).
     * SPIRV-Cross assigns shadow samplers to s8+ and regular to s0+ (see
     * vio_shader_reflect.c — the base is 8 so up to 8 regular samplers fit in
     * the table above). Static and table samplers may share a root signature as
     * long as their registers do not overlap. */
    D3D12_STATIC_SAMPLER_DESC static_samplers[4] = {0};
    for (int s = 0; s < 4; s++) {
        static_samplers[s].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        static_samplers[s].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        static_samplers[s].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        static_samplers[s].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        static_samplers[s].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        static_samplers[s].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        static_samplers[s].MaxLOD = D3D12_FLOAT32_MAX;
        static_samplers[s].ShaderRegister = 8 + s;
        static_samplers[s].RegisterSpace = 0;
        static_samplers[s].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;   /* shadow samplers in every stage */
    }

    /* [14] root SRV t0, space9: the ray-query acceleration structure (vio_shader_reflect.c
     * moves every RaytracingAccelerationStructure there), visible to every stage. */
    params[VIO_D3D12_RP_ACCEL].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[VIO_D3D12_RP_ACCEL].Descriptor.ShaderRegister = 0;
    params[VIO_D3D12_RP_ACCEL].Descriptor.RegisterSpace = 9;
    params[VIO_D3D12_RP_ACCEL].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    /* [15] Bindless table (vio_texture_index, BINDLESS-PLAN.md): an unbounded
     * SRV range t0.. in register space 1 for every stage, and its sampler as a
     * static s1 / space1 (linear, repeat) - the GLSL contract's Set 1 binding 0
     * / 1. Only with Resource Binding Tier 2+ (unbounded ranges). */
    /* The same descriptors, also as TextureCube[] (space 3, vio_cubes) and
     * Texture2DArray[] (space 4, vio_texture_arrays): each slot is read through
     * the array of its kind (BINDLESS-PLAN 4b). */
    D3D12_DESCRIPTOR_RANGE bindless_ranges[3];
    d3d12_bindless_ranges(bindless_ranges);
    params[VIO_D3D12_RP_BINDLESS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[VIO_D3D12_RP_BINDLESS].DescriptorTable.NumDescriptorRanges = 3;
    params[VIO_D3D12_RP_BINDLESS].DescriptorTable.pDescriptorRanges = bindless_ranges;
    params[VIO_D3D12_RP_BINDLESS].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    /* The bindless table's samplers, s1..s4 in space 1 (BINDLESS-PLAN): vio_sampler
     * (linear, repeat), vio_sampler_nearest, vio_sampler_clamp, vio_sampler_nearest_clamp. */
    D3D12_STATIC_SAMPLER_DESC all_samplers[8];
    memcpy(all_samplers, static_samplers, sizeof(static_samplers));
    d3d12_bindless_static_samplers(&all_samplers[4]);

    /* [20] / [21] Vertex textures (A28): the SRV and sampler tables for the
     * vertex stage (its storage buffer moved to t0, space2 for that). */
    params[VIO_D3D12_RP_VS_SRV].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[VIO_D3D12_RP_VS_SRV].DescriptorTable.NumDescriptorRanges = 1;
    params[VIO_D3D12_RP_VS_SRV].DescriptorTable.pDescriptorRanges = &srv_range;
    params[VIO_D3D12_RP_VS_SRV].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[VIO_D3D12_RP_VS_SAMPLER].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[VIO_D3D12_RP_VS_SAMPLER].DescriptorTable.NumDescriptorRanges = 1;
    params[VIO_D3D12_RP_VS_SAMPLER].DescriptorTable.pDescriptorRanges = &sampler_range;
    params[VIO_D3D12_RP_VS_SAMPLER].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    /* [16..19] Fragment storage buffers (A15): root UAVs u4..u7 for the pixel
     * stage (vio_shader_reflect.c moves fragment SSBO binding b to u(b + 4)). */
    for (int i = 0; i < VIO_MAX_FRAGMENT_STORAGE; i++) {
        params[VIO_D3D12_RP_PS_UAV + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[VIO_D3D12_RP_PS_UAV + i].Descriptor.ShaderRegister = (UINT)(4 + i);
        params[VIO_D3D12_RP_PS_UAV + i].Descriptor.RegisterSpace = 0;
        params[VIO_D3D12_RP_PS_UAV + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    /* [15] Draw parameters (OPEN-ITEMS-PLAN A11): gl_BaseVertex / gl_BaseInstance as
     * two root constants at b13 for vertex stages below SM 6.8, set per draw
     * (0 for direct draws, from the copied records for indirect ones). */
    params[VIO_D3D12_RP_DRAW_PARAMS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[VIO_D3D12_RP_DRAW_PARAMS].Constants.ShaderRegister = 13;
    params[VIO_D3D12_RP_DRAW_PARAMS].Constants.RegisterSpace = 0;
    params[VIO_D3D12_RP_DRAW_PARAMS].Constants.Num32BitValues = 2;
    params[VIO_D3D12_RP_DRAW_PARAMS].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    /* [17] Sampler feedback map (VIO_FEATURE_SAMPLER_FEEDBACK): one UAV u0 in
     * register space 2 for the pixel stage - the FeedbackTexture2D of an
     * 'hlsl' => ['fragment' => ...] override. Only when the feature is on, which
     * implies the bindless table, so the optional parameters stay trailing. */
    D3D12_DESCRIPTOR_RANGE feedback_range = {0};
    feedback_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    feedback_range.NumDescriptors = 1;
    feedback_range.BaseShaderRegister = 0;
    feedback_range.RegisterSpace = 2;
    feedback_range.OffsetInDescriptorsFromTableStart = 0;
    params[VIO_D3D12_RP_FEEDBACK].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[VIO_D3D12_RP_FEEDBACK].DescriptorTable.NumDescriptorRanges = 1;
    params[VIO_D3D12_RP_FEEDBACK].DescriptorTable.pDescriptorRanges = &feedback_range;
    params[VIO_D3D12_RP_FEEDBACK].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc = {0};
    /* VS CBV, PS CBV, PS SRV table, VS storage SRV, PS sampler table, GS/HS/DS mirrors, accel
     * (+ bindless table (+ feedback table)) */
    rs_desc.NumParameters = vio_d3d12.sampler_feedback ? VIO_D3D12_RP_COUNT
                          : vio_d3d12.bindless ? VIO_D3D12_RP_FEEDBACK : VIO_D3D12_RP_BINDLESS;
    rs_desc.pParameters = params;
    rs_desc.NumStaticSamplers = vio_d3d12.bindless ? 8 : 4;
    rs_desc.pStaticSamplers = all_samplers;
    rs_desc.Flags = mesh ? D3D12_ROOT_SIGNATURE_FLAG_NONE : D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob *signature_blob = NULL;
    ID3DBlob *error_blob = NULL;
    HRESULT hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              &signature_blob, &error_blob);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to serialize root signature: %s",
                          error_blob ? (char *)ID3D10Blob_GetBufferPointer(error_blob) : "unknown");
        if (error_blob) ID3D10Blob_Release(error_blob);
        return -1;
    }

    hr = ID3D12Device_CreateRootSignature(vio_d3d12.device, 0,
                                           ID3D10Blob_GetBufferPointer(signature_blob),
                                           ID3D10Blob_GetBufferSize(signature_blob),
                                           &IID_ID3D12RootSignature,
                                           (void **)out);
    ID3D10Blob_Release(signature_blob);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create root signature (0x%08lx)", hr);
        return -1;
    }

    return 0;
}

static int d3d12_create_root_signature(void)
{
    return d3d12_build_root_signature(0, &vio_d3d12.root_signature);
}

/* Root signature of the bound pipeline: mesh pipelines use the variant whose
 * vertex-stage parameters are MESH-visible (same indices, so every root
 * argument push stays the same). */
static ID3D12RootSignature *d3d12_graphics_root_signature(void);

/* ── Render target views ──────────────────────────────────────────── */

static int d3d12_create_render_targets(void)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.rtv_heap, &rtv_handle);

    for (UINT i = 0; i < vio_d3d12.frame_count; i++) {
        HRESULT hr = IDXGISwapChain3_GetBuffer(vio_d3d12.swapchain, i,
                                                &IID_ID3D12Resource,
                                                (void **)&vio_d3d12.frames[i].render_target);
        if (FAILED(hr)) {
            php_error_docref(NULL, E_WARNING, "D3D12: Failed to get swapchain buffer %u (0x%08lx)", i, hr);
            return -1;
        }

        vio_d3d12.frames[i].rtv_handle = rtv_handle;
        ID3D12Device_CreateRenderTargetView(vio_d3d12.device,
                                             vio_d3d12.frames[i].render_target,
                                             NULL, rtv_handle);
        rtv_handle.ptr += vio_d3d12.rtv_descriptor_size;
    }

    return 0;
}

static void d3d12_release_render_targets(void)
{
    for (UINT i = 0; i < vio_d3d12.frame_count; i++) {
        if (vio_d3d12.frames[i].render_target) {
            ID3D12Resource_Release(vio_d3d12.frames[i].render_target);
            vio_d3d12.frames[i].render_target = NULL;
        }
    }
}

/* ── Depth buffer ─────────────────────────────────────────────────── */

static int d3d12_create_depth_buffer(int width, int height)
{
    D3D12_HEAP_PROPERTIES heap_props = {0};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depth_desc = {0};
    depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth_desc.Width = width;
    depth_desc.Height = height;
    depth_desc.DepthOrArraySize = 1;
    depth_desc.MipLevels = 1;
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear_value = {0};
    clear_value.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    clear_value.DepthStencil.Depth = 1.0f;
    clear_value.DepthStencil.Stencil = 0;

    HRESULT hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device,
                                                       &heap_props,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &depth_desc,
                                                       D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                       &clear_value,
                                                       &IID_ID3D12Resource,
                                                       (void **)&vio_d3d12.depth_buffer);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create depth buffer (0x%08lx)", hr);
        return -1;
    }

    /* Create DSV */
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {0};
    dsv_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.dsv_heap, &dsv_handle);
    ID3D12Device_CreateDepthStencilView(vio_d3d12.device, vio_d3d12.depth_buffer,
                                         &dsv_desc, dsv_handle);

    return 0;
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

static void d3d12_shutdown(void);
static void d3d12_retire_uploads(int force);   /* upload queue, defined with the texture helpers */
static int  d3d12_upload_buffer_region(ID3D12Resource *dst, const void *data, size_t size);
static int  d3d12_upload_buffer_at(ID3D12Resource *dst, UINT64 offset, const void *data, size_t size);

/* ── Agility SDK (vio_create(['agility_sdk' => dir])) ─────────────── */

/* An application opts into the Agility SDK by exporting D3D12SDKVersion /
 * D3D12SDKPath from its .exe - a PHP extension cannot. Instead the device comes
 * from an independent device factory of the SDK's D3D12Core.dll:
 * D3D12GetInterface(CLSID_D3D12SDKConfiguration) ->
 * ID3D12SDKConfiguration1::CreateDeviceFactory(version, path) ->
 * ID3D12DeviceFactory::CreateDevice. Needs a d3d12.dll that knows
 * ID3D12SDKConfiguration1 (Windows 10 with the 2023 updates / Windows 11). */
#if defined(__ID3D12SDKConfiguration1_INTERFACE_DEFINED__) && defined(__ID3D12DeviceFactory_INTERFACE_DEFINED__)
#define VIO_D3D12_HAS_AGILITY 1

/* `dir` as the runtime wants it: UTF-8, backslashes, a trailing backslash; a
 * relative directory stays relative (the runtime resolves it against the
 * executable's directory), an absolute one inside the executable's directory
 * is made relative too. */
static void d3d12_agility_path(const char *dir, char *out, size_t n)
{
    char tmp[512];
    size_t len = strlen(dir);
    if (len >= sizeof(tmp) - 2) len = sizeof(tmp) - 2;
    for (size_t i = 0; i < len; i++) tmp[i] = dir[i] == '/' ? '\\' : dir[i];
    if (len == 0 || tmp[len - 1] != '\\') tmp[len++] = '\\';
    tmp[len] = '\0';

    char exe[MAX_PATH];
    DWORD el = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
    if (el > 0 && el < sizeof(exe)) {
        char *slash = strrchr(exe, '\\');
        if (slash) {
            slash[1] = '\0';
            size_t pl = strlen(exe);
            if (_strnicmp(tmp, exe, pl) == 0) {
                snprintf(out, n, ".\\%s", tmp + pl);
                return;
            }
        }
    }
    snprintf(out, n, "%s", tmp);
}

/* D3D12SDKVersion as D3D12Core.dll in `dir` exports it; 0 when unreadable. */
static UINT d3d12_agility_dll_version(const char *dir)
{
    char path[600];
    snprintf(path, sizeof(path), "%s%sD3D12Core.dll", dir,
             (dir[0] && dir[strlen(dir) - 1] != '\\' && dir[strlen(dir) - 1] != '/') ? "\\" : "");
    for (char *c = path; *c; c++) if (*c == '/') *c = '\\';
    if (path[0] == '.' || !(path[1] == ':' || (path[0] == '\\' && path[1] == '\\'))) {
        /* relative: against the executable's directory, like the runtime */
        char exe[MAX_PATH];
        DWORD el = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
        char *slash = (el > 0 && el < sizeof(exe)) ? strrchr(exe, '\\') : NULL;
        if (slash) {
            char abs[MAX_PATH + 600];
            slash[1] = '\0';
            snprintf(abs, sizeof(abs), "%s%s", exe, path);
            snprintf(path, sizeof(path), "%s", abs);
        }
    }
    HMODULE core = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!core) return 0;
    UINT version = 0;
    const UINT *exported = (const UINT *)(void *)GetProcAddress(core, "D3D12SDKVersion");
    if (exported) version = *exported;
    FreeLibrary(core);
    return version;
}

typedef HRESULT (WINAPI *vio_pfn_d3d12_get_interface)(REFCLSID, REFIID, void **);

/* Device on `adapter` from the Agility runtime in cfg->agility_sdk; S_OK and
 * vio_d3d12.device set, or a failure the caller answers with the OS runtime. */
static HRESULT d3d12_agility_create_device(vio_config *cfg, IDXGIAdapter1 *adapter)
{
    HMODULE d3d12 = GetModuleHandleA("d3d12.dll");
    vio_pfn_d3d12_get_interface get_interface = d3d12
        ? (vio_pfn_d3d12_get_interface)(void *)GetProcAddress(d3d12, "D3D12GetInterface") : NULL;
    if (!get_interface) {
        php_error_docref(NULL, E_WARNING, "D3D12: agility_sdk: this d3d12.dll has no D3D12GetInterface; using the OS runtime");
        return E_NOINTERFACE;
    }
    UINT version = cfg->agility_sdk_version > 0 ? (UINT)cfg->agility_sdk_version : d3d12_agility_dll_version(cfg->agility_sdk);
    if (version == 0) {
        php_error_docref(NULL, E_WARNING, "D3D12: agility_sdk: no D3D12Core.dll with D3D12SDKVersion in '%s' "
                         "(pass agility_sdk_version); using the OS runtime", cfg->agility_sdk);
        return E_INVALIDARG;
    }
    char path[600];
    d3d12_agility_path(cfg->agility_sdk, path, sizeof(path));

    ID3D12SDKConfiguration1 *config = NULL;
    HRESULT hr = get_interface(&CLSID_D3D12SDKConfiguration, &IID_ID3D12SDKConfiguration1, (void **)&config);
    if (FAILED(hr) || !config) {
        php_error_docref(NULL, E_WARNING, "D3D12: agility_sdk: ID3D12SDKConfiguration1 unavailable (0x%08lx, OS too old); "
                         "using the OS runtime", hr);
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    ID3D12DeviceFactory *factory = NULL;
    hr = ID3D12SDKConfiguration1_CreateDeviceFactory(config, version, path, &IID_ID3D12DeviceFactory, (void **)&factory);
    if (FAILED(hr) || !factory) {
        php_error_docref(NULL, E_WARNING, "D3D12: agility_sdk: CreateDeviceFactory(%u, '%s') failed (0x%08lx); "
                         "using the OS runtime", version, path, hr);
        ID3D12SDKConfiguration1_Release(config);
        return FAILED(hr) ? hr : E_FAIL;
    }
    /* The debug layer of THIS runtime (D3D12GetDebugInterface reached the OS one). */
    if (cfg->debug) {
        ID3D12Debug *dbg = NULL;
        if (SUCCEEDED(ID3D12DeviceFactory_GetConfigurationInterface(factory, &CLSID_D3D12Debug, &IID_ID3D12Debug, (void **)&dbg)) && dbg) {
            ID3D12Debug_EnableDebugLayer(dbg);
            ID3D12Debug_Release(dbg);
        }
    }
    hr = ID3D12DeviceFactory_CreateDevice(factory, (IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0,
                                          &IID_ID3D12Device, (void **)&vio_d3d12.device);
    if (FAILED(hr) || !vio_d3d12.device) {
        php_error_docref(NULL, E_WARNING, "D3D12: agility_sdk: CreateDevice on the SDK %u runtime failed (0x%08lx); "
                         "using the OS runtime", version, hr);
        vio_d3d12.device = NULL;
        ID3D12DeviceFactory_Release(factory);
        ID3D12SDKConfiguration1_Release(config);
        return FAILED(hr) ? hr : E_FAIL;
    }
    vio_d3d12.agility_sdk = (int)version;
    vio_d3d12.agility_config = (IUnknown *)config;
    vio_d3d12.agility_factory = (IUnknown *)factory;
    return S_OK;
}
#endif

static int d3d12_init(vio_config *cfg)
{
    HRESULT hr;

    /* Resolve the in-flight frame count FIRST: the heap slices divide by it, so a
     * zero here would be a divide-by-zero rather than a wrong number. 0 (or any
     * out-of-range value) means "use the default" — we never trust the caller to
     * have clamped, because this arrives straight from a PHP array. */
    {
        int requested = cfg ? cfg->frame_count : 0;
        if (requested <= 0) {
            requested = VIO_D3D12_FRAME_COUNT_DEFAULT;
        } else if (requested < VIO_D3D12_MIN_FRAME_COUNT) {
            requested = VIO_D3D12_MIN_FRAME_COUNT;
        } else if (requested > VIO_D3D12_MAX_FRAME_COUNT) {
            requested = VIO_D3D12_MAX_FRAME_COUNT;
        }
        vio_d3d12.frame_count = (UINT)requested;
    }

    /* Enable DRED (Device Removed Extended Data) BEFORE device creation.
     *
     * DRED gives us, on a device-removed event, the GPU "auto-breadcrumbs"
     * (the last render operations the GPU actually executed before it hung)
     * and the page-fault virtual address (the resource the GPU faulted on).
     * This is the only practical way to learn WHICH command/resource hung on
     * an intermittent, non-reproducible DEVICE_HUNG in the field.
     *
     * Crucially, DRED does NOT require the debug layer or the "Graphics Tools"
     * optional feature — ID3D12DeviceRemovedExtendedDataSettings is a
     * pre-device global toggle that works on retail drivers. It does add a
     * small per-command-list overhead (breadcrumb writes), so we only force it
     * on when explicitly requested via the VIO_D3D12_DRED env var, OR whenever
     * the debug layer is already on. Default: off (zero overhead). */
    {
        int dred_requested = 0;
        const char *dred_env = getenv("VIO_D3D12_DRED");
        if (dred_env && dred_env[0] && dred_env[0] != '0') dred_requested = 1;
        if (cfg->debug) dred_requested = 1;

        if (dred_requested) {
            ID3D12DeviceRemovedExtendedDataSettings *dred_settings = NULL;
            if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12DeviceRemovedExtendedDataSettings,
                                                 (void **)&dred_settings)) && dred_settings) {
                ID3D12DeviceRemovedExtendedDataSettings_SetAutoBreadcrumbsEnablement(
                    dred_settings, D3D12_DRED_ENABLEMENT_FORCED_ON);
                ID3D12DeviceRemovedExtendedDataSettings_SetPageFaultEnablement(
                    dred_settings, D3D12_DRED_ENABLEMENT_FORCED_ON);
                ID3D12DeviceRemovedExtendedDataSettings_Release(dred_settings);
                vio_d3d12.dred_enabled = 1;
                fprintf(stderr, "[d3d12] DRED: AutoBreadcrumbs + PageFault FORCED_ON\n");
            } else {
                fprintf(stderr, "[d3d12] DRED: settings interface UNAVAILABLE "
                                "(very old SDK/OS?) — device-removed dumps will be reason-only\n");
            }
            fflush(stderr);
        }
    }

    /* Enable debug layer */
    if (cfg->debug) {
        ID3D12Debug *debug_controller = NULL;
        if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&debug_controller))) {
            ID3D12Debug_EnableDebugLayer(debug_controller);
            ID3D12Debug_Release(debug_controller);
            fprintf(stderr, "[d3d12] debug layer: EnableDebugLayer OK\n");
        } else {
            fprintf(stderr, "[d3d12] debug layer: D3D12GetDebugInterface FAILED "
                            "(Graphics Tools not registered / reboot needed?)\n");
        }
        fflush(stderr);
        vio_d3d12.debug_enabled = 1;
    }

    /* Create DXGI factory */
    UINT factory_flags = vio_d3d12.debug_enabled ? DXGI_CREATE_FACTORY_DEBUG : 0;
    hr = CreateDXGIFactory2(factory_flags, &IID_IDXGIFactory4, (void **)&vio_d3d12.factory);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create DXGI factory (0x%08lx)", hr);
        goto init_fail;
    }

    /* Probe DXGI_FEATURE_PRESENT_ALLOW_TEARING once. Requires IDXGIFactory5
     * (Windows 10 1511+); on older DXGI the QI simply fails and we keep the
     * vsync-throttled behaviour. Never assume the feature — a driver/OS without
     * it will fail Present() outright if the flag is passed. */
    {
        IDXGIFactory5 *factory5 = NULL;
        if (SUCCEEDED(IDXGIFactory4_QueryInterface(vio_d3d12.factory,
                                                   &IID_IDXGIFactory5,
                                                   (void **)&factory5))) {
            BOOL allow_tearing = FALSE;
            HRESULT thr = IDXGIFactory5_CheckFeatureSupport(
                factory5,
                DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                &allow_tearing,
                sizeof(allow_tearing));
            /* CheckFeatureSupport can succeed and still leave the out-param
             * untouched on some drivers — require both. */
            vio_d3d12.tearing_supported = (SUCCEEDED(thr) && allow_tearing) ? 1 : 0;
            IDXGIFactory5_Release(factory5);
        }
    }

    /* Select adapter (WARP for headless, hardware otherwise).
     * VIO_D3D_HEADLESS_HARDWARE=1 keeps the GPU for headless contexts, so the
     * headless test suite can cover hardware-only paths (mesh shaders, DXR,
     * sampler feedback, VRS tier 2). */
    IDXGIAdapter1 *adapter = NULL;
    const char *hw_env = getenv("VIO_D3D_HEADLESS_HARDWARE");
    if (cfg->headless && !cfg->headless_hardware && !(hw_env && *hw_env && strcmp(hw_env, "0") != 0)) {
        vio_d3d_load_warp();
        hr = IDXGIFactory4_EnumWarpAdapter(vio_d3d12.factory, &IID_IDXGIAdapter1, (void **)&adapter);
        if (FAILED(hr)) {
            php_error_docref(NULL, E_WARNING, "D3D12: WARP adapter not available (0x%08lx)", hr);
            goto init_fail;
        }
    } else {
        /* Pick first hardware adapter that supports D3D12 */
        for (UINT i = 0; IDXGIFactory4_EnumAdapters1(vio_d3d12.factory, i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc;
            IDXGIAdapter1_GetDesc1(adapter, &desc);

            /* Skip software adapters */
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                IDXGIAdapter1_Release(adapter);
                adapter = NULL;
                continue;
            }

            /* Check if adapter supports D3D12 */
            if (SUCCEEDED(D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0,
                                             &IID_ID3D12Device, NULL))) {
                break;
            }

            IDXGIAdapter1_Release(adapter);
            adapter = NULL;
        }
    }

    if (!adapter) {
        php_error_docref(NULL, E_WARNING, "D3D12: No suitable adapter found");
        goto init_fail;
    }

    /* Capture the SELECTED adapter's description for vio_gpu_info(). This is the
     * exact adapter we are about to create the device on, so we never need to
     * re-enumerate later. Description is WCHAR[128]; convert to UTF-8. On WARP
     * (headless) this still works but reports the software adapter name and a
     * DedicatedVideoMemory of 0. */
    {
        DXGI_ADAPTER_DESC1 sel_desc;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &sel_desc))) {
            vio_d3d12.vram_bytes = (uint64_t)sel_desc.DedicatedVideoMemory;
            vio_dxgi_adapter_identity((IDXGIAdapter *)adapter, &vio_d3d12.vendor_id, vio_d3d12.driver,
                                      sizeof(vio_d3d12.driver), &vio_d3d12.software_adapter);
            int n = WideCharToMultiByte(CP_UTF8, 0, sel_desc.Description, -1,
                                        vio_d3d12.gpu_name, (int)sizeof(vio_d3d12.gpu_name),
                                        NULL, NULL);
            if (n <= 0) {
                /* Conversion failed — leave name empty rather than garbage. */
                vio_d3d12.gpu_name[0] = '\0';
            }
        }
    }

    /* Create device: from the Agility SDK runtime when asked for (falls back to
     * the OS runtime with a warning), else from the OS runtime. */
    vio_d3d12.agility_sdk = 0;
    hr = E_FAIL;
#ifdef VIO_D3D12_HAS_AGILITY
    if (cfg->agility_sdk[0]) hr = d3d12_agility_create_device(cfg, adapter);
#else
    if (cfg->agility_sdk[0])
        php_error_docref(NULL, E_WARNING, "D3D12: agility_sdk: built against a Windows SDK without ID3D12SDKConfiguration1; using the OS runtime");
#endif
    if (FAILED(hr))
        hr = D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0,
                               &IID_ID3D12Device, (void **)&vio_d3d12.device);
    IDXGIAdapter1_Release(adapter);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create device (0x%08lx)", hr);
        goto init_fail;
    }

    /* Silence two benign perf-HINT messages from the InfoQueue when the debug
     * layer is active. vio is a generic renderer: it creates swapchain
     * backbuffers and offscreen render targets WITHOUT an optimized
     * D3D12_CLEAR_VALUE, because it cannot know the app's clear color at
     * resource-creation time. The debug layer then emits these on every
     * ClearRenderTargetView / ClearDepthStencilView whose color differs from
     * the (absent / mismatched) optimized clear value:
     *   id=820 CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE
     *   id=821 CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE
     * The clear still works correctly — these are pure perf hints — so we add a
     * DENY *storage* filter for exactly these two IDs. The layer never queues
     * them, so d3d12_drain_info_queue() never sees them. Every other message
     * (all severities, every other ID — including the real validation errors
     * the drain exists to surface) is unaffected and still stored + emitted. */
    if (cfg->debug) {
        /* Resolve the InfoQueue ONCE and keep the reference for the device's
         * lifetime (released in d3d12_shutdown). d3d12_drain_info_queue() reads
         * vio_d3d12.info_queue directly, so the per-frame drain no longer does a
         * QueryInterface. When debug is off, info_queue stays NULL and the drain
         * is a single pointer test. */
        ID3D12InfoQueue *iq = NULL;
        if (SUCCEEDED(ID3D12Device_QueryInterface(vio_d3d12.device, &IID_ID3D12InfoQueue,
                                                   (void **)&iq)) && iq) {
            D3D12_MESSAGE_ID deny_ids[] = {
                D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
            };
            D3D12_INFO_QUEUE_FILTER filter = {0};
            filter.DenyList.NumIDs = (UINT)(sizeof(deny_ids) / sizeof(deny_ids[0]));
            filter.DenyList.pIDList = deny_ids;
            /* AddStorageFilterEntries: do not store messages matching the deny
             * list. No category/severity entries → only these exact IDs match. */
            ID3D12InfoQueue_AddStorageFilterEntries(iq, &filter);
            vio_d3d12.info_queue = iq;   /* keep the reference; do NOT Release */
            fprintf(stderr, "[d3d12] InfoQueue available — validation messages will print to stderr\n");
        } else {
            fprintf(stderr, "[d3d12] InfoQueue UNAVAILABLE after device create — debug layer not live (reboot after Graphics Tools install?)\n");
        }
        fflush(stderr);
    }

    /* Create command queue */
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    hr = ID3D12Device_CreateCommandQueue(vio_d3d12.device, &queue_desc,
                                          &IID_ID3D12CommandQueue,
                                          (void **)&vio_d3d12.cmd_queue);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create command queue (0x%08lx)", hr);
        goto init_fail;
    }

    /* Create per-frame command allocators */
    for (UINT i = 0; i < vio_d3d12.frame_count; i++) {
        hr = ID3D12Device_CreateCommandAllocator(vio_d3d12.device,
                                                  D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  &IID_ID3D12CommandAllocator,
                                                  (void **)&vio_d3d12.frames[i].cmd_allocator);
        if (FAILED(hr)) {
            php_error_docref(NULL, E_WARNING, "D3D12: Failed to create command allocator %u (0x%08lx)", i, hr);
            goto init_fail;
        }
    }

    /* Create command list (initially closed) */
    hr = ID3D12Device_CreateCommandList(vio_d3d12.device, 0,
                                         D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         vio_d3d12.frames[0].cmd_allocator,
                                         NULL, /* initial PSO */
                                         &IID_ID3D12GraphicsCommandList,
                                         (void **)&vio_d3d12.cmd_list);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create command list (0x%08lx)", hr);
        goto init_fail;
    }
    /* Close it immediately — will be reset in begin_frame */
    ID3D12GraphicsCommandList_Close(vio_d3d12.cmd_list);

    /* Create fence */
    hr = ID3D12Device_CreateFence(vio_d3d12.device, 0, D3D12_FENCE_FLAG_NONE,
                                   &IID_ID3D12Fence, (void **)&vio_d3d12.fence);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create fence (0x%08lx)", hr);
        goto init_fail;
    }
    vio_d3d12.fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    vio_d3d12.fence_value = 0;

    /* Shader model (GAP-PHASE5 Block 7): SM 6 only when asked for, the device
     * reports it and DXC (+ dxil.dll for signing) can be loaded. */
    vio_d3d12.shader_model = 5;
    vio_d3d12.shader_model_version = 51;
    vio_d3d12.wave_ops = 0;
    vio_d3d12.barycentrics = 0;
    vio_d3d12.int64_ops = 0;
    vio_d3d12.native16 = 0;
    vio_d3d12.view_instancing = 0;
    vio_d3d12.mesh_tier = 0;
    vio_d3d12.raytracing_tier = 0;
    vio_d3d12.work_graphs_tier = 0;
    /* Variable rate shading capability (GAP-PHASE5 Block 12). */
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6 = {0};
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(vio_d3d12.device, D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6)))) {
            vio_d3d12.vrs_tier = (int)o6.VariableShadingRateTier;
            vio_d3d12.vrs_additional_rates = o6.AdditionalShadingRatesSupported ? 1 : 0;
            vio_d3d12.vrs_tile_size = (int)o6.ShadingRateImageTileSize;
        }
        vio_d3d12.shading_rate = VIO_SHADING_RATE_1X1;
        vio_d3d12.vrs_image = NULL;
        vio_d3d12.vrs_image_w = vio_d3d12.vrs_image_h = 0;
        vio_d3d12.vrs_image_active = 0;
        vio_d3d12.vrs_image_in_source = 0;
    }

    vio_hlsl_set_16bit_types(0);   /* FXC / SM < 6.2: min16float as before */
    /* 6 = highest 6.x; 60..69 (major * 10 + minor) pins the profile, values above
     * the device / DXC maximum clamp to it. Without the option,
     * VIO_D3D12_SHADER_MODEL (same encoding) applies. */
    int sm_req = cfg->shader_model;
    if (sm_req == 0) {
        /* Win32 block, not the CRT copy: PHP's putenv() updates the former. */
        char env[16];
        DWORD n = GetEnvironmentVariableA("VIO_D3D12_SHADER_MODEL", env, sizeof(env));
        if (n > 0 && n < sizeof(env)) sm_req = atoi(env);
    }
    /* 50..59 = FXC 5.1 explicitly; other values >= 6 (pre-pinning callers passed
     * 6) take DXC, capped at the pinned minor. */
    int sm_cap_minor = (sm_req >= 60 && sm_req <= 69) ? sm_req - 60 : 9;
    if (sm_req >= 6 && !(sm_req >= 50 && sm_req <= 59)) {
        if (cfg->dxc_dir[0]) vio_dxc_set_dir(cfg->dxc_dir);
        /* The runtime rejects HighestShaderModel values it does not know
         * (E_INVALIDARG), so walk down from 6.9 until it answers; it then
         * lowers the value to what the device supports. D3D_SHADER_MODEL
         * values are 0xMm, written as numbers so older SDK headers work. */
        int device_minor = -1;
        for (int v = 0x69; v >= 0x60 && device_minor < 0; v--) {
            D3D12_FEATURE_DATA_SHADER_MODEL sm = { (D3D_SHADER_MODEL)v };
            if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(vio_d3d12.device, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
                if ((int)sm.HighestShaderModel >= 0x60) device_minor = (int)sm.HighestShaderModel & 0xF;
                break;
            }
        }
        if (device_minor > sm_cap_minor) device_minor = sm_cap_minor;
        int dxc_minor = (device_minor >= 0 && vio_dxc_available()) ? vio_dxc_highest_minor(device_minor) : -1;
        if (dxc_minor >= 0) {
            vio_d3d12.shader_model = 6;
            vio_d3d12.shader_model_version = 60 + dxc_minor;
            D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1 = {0};
            if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(vio_d3d12.device, D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1))))
                vio_d3d12.wave_ops = o1.WaveOps ? 1 : 0;
            vio_d3d12.int64_ops = o1.Int64ShaderOps ? 1 : 0;
            /* Native 16-bit shader ops (SM 6.2): SPIRV-Cross emits `half`, DXC gets
             * -enable-16bit-types; the cache key includes the HLSL, so blobs of the
             * two modes never mix. */
            D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4 = {0};
            if (vio_d3d12.shader_model_version >= 62
                && SUCCEEDED(ID3D12Device_CheckFeatureSupport(vio_d3d12.device, D3D12_FEATURE_D3D12_OPTIONS4, &o4, sizeof(o4))))
                vio_d3d12.native16 = o4.Native16BitShaderOpsSupported ? 1 : 0;
            D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3 = {0};
            if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(vio_d3d12.device, D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3))))
                vio_d3d12.barycentrics = o3.BarycentricsSupported ? 1 : 0;
            if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(vio_d3d12.device, D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3))))
                vio_d3d12.view_instancing = (int)o3.ViewInstancingTier;
            /* Mesh / amplification shaders (SM 6.5). */
            D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {0};
            if (vio_d3d12.shader_model_version >= 65
                && SUCCEEDED(ID3D12Device_CheckFeatureSupport(vio_d3d12.device, D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7))))
                vio_d3d12.mesh_tier = (int)o7.MeshShaderTier;
#ifdef VIO_D3D12_HAS_WORK_GRAPHS
            /* Work graphs (SM 6.8 node shaders); the OS runtime usually needs the
             * Agility SDK (agility_sdk) to report a tier. */
            D3D12_FEATURE_DATA_D3D12_OPTIONS21 o21;
            memset(&o21, 0, sizeof(o21));
            if (vio_d3d12.shader_model_version >= 68
                && SUCCEEDED(ID3D12Device_CheckFeatureSupport(vio_d3d12.device, D3D12_FEATURE_D3D12_OPTIONS21, &o21, sizeof(o21))))
                vio_d3d12.work_graphs_tier = (int)o21.WorkGraphsTier;
#endif
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5;
            memset(&o5, 0, sizeof(o5));
            if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(vio_d3d12.device, D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))))
                vio_d3d12.raytracing_tier = (int)o5.RaytracingTier;
            vio_hlsl_set_16bit_types(vio_d3d12.native16);
        } else {
            php_error_docref(NULL, E_NOTICE, "D3D12: shader_model 6 requested but %s; using FXC (SM 5.1)",
                             device_minor < 0 ? "the device lacks SM 6.0"
                             : vio_dxc_available() ? "DXC / dxil.dll cannot compile cs_6_0"
                             : "dxcompiler.dll / dxil.dll not loadable");
        }
    }

    /* GPU timestamps (GAP-PHASE5 Block 3) — optional; a failure just leaves the
     * feature off. */
    vio_d3d12.last_gpu_ms = -1.0;
    memset(vio_d3d12.ts_marks, 0, sizeof(vio_d3d12.ts_marks));
    vio_d3d12.ts_result_valid = 0;
    {
        D3D12_QUERY_HEAP_DESC qh = {0};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = VIO_GPU_TS_PER_FRAME * VIO_D3D12_MAX_FRAME_COUNT;
        ID3D12QueryHeap *heap = NULL;
        if (SUCCEEDED(ID3D12Device_CreateQueryHeap(vio_d3d12.device, &qh, &IID_ID3D12QueryHeap, (void **)&heap)) && heap) {
            D3D12_HEAP_PROPERTIES hp = {0};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rb = {0};
            rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rb.Width = sizeof(UINT64) * VIO_GPU_TS_PER_FRAME * VIO_D3D12_MAX_FRAME_COUNT;
            rb.Height = 1; rb.DepthOrArraySize = 1; rb.MipLevels = 1;
            rb.SampleDesc.Count = 1;
            rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource *readback = NULL;
            if (SUCCEEDED(ID3D12Device_CreateCommittedResource(vio_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &rb,
                    D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&readback)) && readback) {
                vio_d3d12.ts_heap = heap;
                vio_d3d12.ts_readback = readback;
                ID3D12CommandQueue_GetTimestampFrequency(vio_d3d12.cmd_queue, &vio_d3d12.ts_frequency);
            } else {
                ID3D12QueryHeap_Release(heap);
            }
        }
    }

    /* Create descriptor heaps */
    if (d3d12_create_descriptor_heap(&vio_d3d12.rtv_heap,
                                      D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                                      vio_d3d12.frame_count,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_NONE) != 0) {
        goto init_fail;
    }
    vio_d3d12.rtv_descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(
        vio_d3d12.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    vio_d3d12.dsv_descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(
        vio_d3d12.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    if (d3d12_create_descriptor_heap(&vio_d3d12.dsv_heap,
                                      D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_NONE) != 0) {
        goto init_fail;
    }

    /* Bindless texture table (vio_texture_index) needs unbounded descriptor
     * tables: Resource Binding Tier 2 (the root signature below adds the table
     * only then). */
    vio_d3d12.bindless = 0;
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS o0 = {0};
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(vio_d3d12.device, D3D12_FEATURE_D3D12_OPTIONS, &o0, sizeof(o0))))
            vio_d3d12.bindless = o0.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_2;
    }

    /* Sampler feedback: WriteSamplerFeedback is SM 6.5 (DXC), the feedback map
     * needs ID3D12Device8::CreateCommittedResource2 and Tier 0.9; root
     * parameter [16] sits behind the bindless table. */
    vio_d3d12.sampler_feedback = 0;
    vio_d3d12.fb_bound = NULL;
    vio_d3d12.fb_null_gpu.ptr = 0;
    if (vio_d3d12.bindless && vio_d3d12.shader_model == 6 && vio_d3d12.shader_model_version >= 65) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7;
        memset(&o7, 0, sizeof(o7));
        ID3D12Device8 *dev8 = NULL;
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(vio_d3d12.device, D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7)))
            && o7.SamplerFeedbackTier >= D3D12_SAMPLER_FEEDBACK_TIER_0_9
            && SUCCEEDED(ID3D12Device_QueryInterface(vio_d3d12.device, &IID_ID3D12Device8, (void **)&dev8)) && dev8) {
            vio_d3d12.sampler_feedback = 1;
        }
        if (dev8) ID3D12Device8_Release(dev8);
    }

    /* GPU-visible SRV/CBV/UAV heap */
    if (d3d12_create_descriptor_heap(&vio_d3d12.srv_heap.heap,
                                      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                      VIO_D3D12_MAX_SRV_DESCRIPTORS,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
        goto init_fail;
    }
    vio_d3d12.srv_heap.descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(
        vio_d3d12.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    vio_d3d12.srv_heap.capacity = VIO_D3D12_MAX_SRV_DESCRIPTORS;
    vio_d3d12.srv_heap.count = 0;
    if (vio_d3d12.bindless) {
        /* The top VIO_BINDLESS_MAX descriptors are the bindless table: static
         * SRVs grow down below them, the per-frame region stays under those.
         * Null Texture2D SRVs until vio_texture_index fills a slot. */
        vio_d3d12.bindless_base = VIO_D3D12_MAX_SRV_DESCRIPTORS - VIO_BINDLESS_MAX;
        vio_d3d12.srv_heap.count = VIO_BINDLESS_MAX;
        D3D12_SHADER_RESOURCE_VIEW_DESC nd = {0};
        nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.srv_heap.heap, &h);
        for (UINT i = 0; i < VIO_BINDLESS_MAX; i++) {
            D3D12_CPU_DESCRIPTOR_HANDLE d = { h.ptr + (SIZE_T)(vio_d3d12.bindless_base + i) * vio_d3d12.srv_heap.descriptor_size };
            ID3D12Device_CreateShaderResourceView(vio_d3d12.device, NULL, &nd, d);
        }
        /* The CPU-only mirror starts with the same null SRVs. */
        vio_d3d12.bindless_cpu_heap = NULL;
        if (d3d12_create_descriptor_heap(&vio_d3d12.bindless_cpu_heap, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                         VIO_BINDLESS_MAX, D3D12_DESCRIPTOR_HEAP_FLAG_NONE) == 0) {
            D3D12_CPU_DESCRIPTOR_HANDLE m;
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.bindless_cpu_heap, &m);
            for (UINT i = 0; i < VIO_BINDLESS_MAX; i++) {
                D3D12_CPU_DESCRIPTOR_HANDLE d = { m.ptr + (SIZE_T)i * vio_d3d12.srv_heap.descriptor_size };
                ID3D12Device_CreateShaderResourceView(vio_d3d12.device, NULL, &nd, d);
            }
        }
    }

    /* CPU-only staging mirror — texture SRVs live here so they can serve as
     * the source of CopyDescriptorsSimple into the per-frame shader-visible
     * region. Same capacity as srv_heap so we can use matching indices. */
    if (d3d12_create_descriptor_heap(&vio_d3d12.srv_staging_heap,
                                      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                      VIO_D3D12_MAX_SRV_DESCRIPTORS,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_NONE) != 0) {
        goto init_fail;
    }
    /* Per-frame allocator partitions itself dynamically against srv_heap.count
     * each begin_frame, so static SRVs (textures, render targets) can grow
     * arbitrarily without colliding with per-frame descriptor writes. */
    vio_d3d12.srv_frame_capacity = 0;
    vio_d3d12.srv_frame_base = 0;
    vio_d3d12.srv_frame_offset = 0;

    /* Pre-built null-SRV block (GAP-PLAN 4.3). Lives at the TOP of the static
     * region so it never collides with the per-frame ring; flush_srv_table
     * copies it in one call instead of 16 CreateShaderResourceView(NULL). */
    {
        D3D12_CPU_DESCRIPTOR_HANDLE first_cpu; D3D12_GPU_DESCRIPTOR_HANDLE first_gpu;
        vio_d3d12.null_srv_block_valid = 0;
        if (d3d12_alloc_srv_descriptor(&first_cpu, &first_gpu) != UINT_MAX) {
            /* Static descriptors grow downward, so allocate the remaining 15 and
             * keep the LOWEST handle as the block start. */
            D3D12_CPU_DESCRIPTOR_HANDLE lowest = first_cpu;
            int ok = 1;
            for (int i = 1; i < VIO_D3D12_SRV_TABLE_SIZE; i++) {
                D3D12_CPU_DESCRIPTOR_HANDLE c; D3D12_GPU_DESCRIPTOR_HANDLE g;
                if (d3d12_alloc_srv_descriptor(&c, &g) == UINT_MAX) { ok = 0; break; }
                if (c.ptr < lowest.ptr) lowest = c;
            }
            if (ok) {
                D3D12_SHADER_RESOURCE_VIEW_DESC null_srv = {0};
                null_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                null_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                null_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                null_srv.Texture2D.MipLevels = 1;
                for (int i = 0; i < VIO_D3D12_SRV_TABLE_SIZE; i++) {
                    D3D12_CPU_DESCRIPTOR_HANDLE h = { lowest.ptr + (SIZE_T)i * vio_d3d12.srv_heap.descriptor_size };
                    ID3D12Device_CreateShaderResourceView(vio_d3d12.device, NULL, &null_srv, h);
                }
                vio_d3d12.null_srv_block = lowest;
                vio_d3d12.null_srv_block_valid = 1;
            }
        }
    }

    /* Sampler heaps (GAP-PLAN Phase 1): the CPU-only combo heap with every
     * filter x wrap x anisotropy sampler, and the shader-visible per-frame ring
     * the per-draw 8-sampler blocks are copied into. */
    if (d3d12_create_descriptor_heap(&vio_d3d12.sampler_combo_heap,
                                      D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                      VIO_D3D12_SAMPLER_COMBOS,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_NONE) != 0) {
        goto init_fail;
    }
    if (d3d12_create_descriptor_heap(&vio_d3d12.sampler_heap,
                                      D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                      VIO_D3D12_SAMPLER_HEAP_CAPACITY,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
        goto init_fail;
    }
    vio_d3d12.sampler_descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(
        vio_d3d12.device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    {
        static const int aniso_levels[VIO_D3D12_SAMPLER_ANISO_LEVELS] = {1, 2, 4, 8, 16};
        D3D12_CPU_DESCRIPTOR_HANDLE base;
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.sampler_combo_heap, &base);
        for (int a = 0; a < VIO_D3D12_SAMPLER_ANISO_LEVELS; a++) {
            for (int w = 0; w < 3; w++) {
                for (int f = 0; f < 2; f++) {
                    /* f: 0 = LINEAR, 1 = NEAREST (so combo 0 is the legacy LINEAR/WRAP). */
                    D3D12_SAMPLER_DESC sd = {0};
                    if (f == 1)                    sd.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                    else if (aniso_levels[a] > 1)  sd.Filter = D3D12_FILTER_ANISOTROPIC;
                    else                           sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                    D3D12_TEXTURE_ADDRESS_MODE am = w == 1 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
                                                  : w == 2 ? D3D12_TEXTURE_ADDRESS_MODE_MIRROR
                                                           : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                    sd.AddressU = sd.AddressV = sd.AddressW = am;
                    sd.MaxAnisotropy = (UINT)(f == 1 ? 1 : aniso_levels[a]);
                    sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
                    sd.MaxLOD = D3D12_FLOAT32_MAX;
                    int idx = (a * 3 + w) * 2 + f;
                    D3D12_CPU_DESCRIPTOR_HANDLE h = { base.ptr + (SIZE_T)idx * vio_d3d12.sampler_descriptor_size };
                    ID3D12Device_CreateSampler(vio_d3d12.device, &sd, h);
                }
            }
        }
    }
    vio_d3d12.sampler_frame_capacity = 0;
    vio_d3d12.sampler_frame_base = 0;
    vio_d3d12.sampler_frame_offset = 0;
    vio_d3d12.sampler_set_count = 0;
    vio_d3d12.sampler_table_bound = 0;

    /* Root signature */
    if (d3d12_create_root_signature() != 0) {
        goto init_fail;
    }

    /* Per-frame linear cbuffer allocator: persistently mapped UPLOAD heap.
     * Each draw call allocates a 256-byte-aligned slice for its cbuffer data.
     *
     * The heap is split into vio_d3d12.frame_count equal per-frame slices
     * (begin_frame rebases the offset to this frame's slice). A single shared
     * offset reset to 0 every frame would let frame N+1 overwrite the very
     * addresses frame N's in-flight root CBVs still read — invisible while
     * consecutive frames upload near-identical uniform sequences, but a sudden
     * draw-count swing (look at the sky, look back down) shifts the layout and
     * the overlap renders one frame of garbage lighting/transforms.
     *
     * Sized generously (32MB total = 16MB per slice at FRAME_COUNT 2) so the
     * dynamic-grow path is rarely hit. Growing the heap mid-execution requires
     * a full GPU sync (see d3d12_begin_frame) because root CBV lifetime is NOT
     * tracked by the runtime — releasing the old heap while another frame in
     * flight references it via GPU VA causes use-after-free flicker. */
    {
        UINT heap_size = 32 * 1024 * 1024; /* 32MB total, FRAME_COUNT slices */
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = heap_size;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(ID3D12Device_CreateCommittedResource(vio_d3d12.device,
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                NULL, &IID_ID3D12Resource, (void **)&vio_d3d12.cbuffer_heap))) {
            vio_d3d12.cbuffer_heap_gpu = ID3D12Resource_GetGPUVirtualAddress(vio_d3d12.cbuffer_heap);
            vio_d3d12.cbuffer_heap_capacity = heap_size;
            vio_d3d12.cbuffer_frame_base = 0;
            vio_d3d12.cbuffer_frame_end = heap_size / vio_d3d12.frame_count;
            vio_d3d12.cbuffer_heap_offset = 0;
            /* Persistently map (never unmap — valid for UPLOAD heaps in D3D12) */
            D3D12_RANGE rr = {0, 0};
            ID3D12Resource_Map(vio_d3d12.cbuffer_heap, 0, &rr, (void **)&vio_d3d12.cbuffer_heap_mapped);
        }
    }

    /* Per-frame linear instance-data allocator: persistently mapped UPLOAD heap.
     * Mirrors the cbuffer heap exactly. Each vio_draw_instanced gets a 256-byte-
     * aligned slice holding its mat4 instance array; vbvs[1] points at that
     * slice's GPU VA. The heap is split into vio_d3d12.frame_count equal slices
     * (begin_frame rebases the offset to this frame's slice) so the CPU never
     * overwrites instance matrices another in-flight frame's command list still
     * reads. Growing mid-execution requires a full GPU sync (see d3d12_begin_frame)
     * because the slot-1 VBV references the heap via raw GPU VA, which the runtime
     * does NOT track — releasing the old heap while a frame in flight reads it is
     * use-after-free. Sized 32MB total = 16MB per slice at FRAME_COUNT 2:
     * ~262k mat4 instances per frame before the grow path is hit. */
    {
        UINT heap_size = 32 * 1024 * 1024; /* 32MB total, FRAME_COUNT slices */
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = heap_size;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(ID3D12Device_CreateCommittedResource(vio_d3d12.device,
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                NULL, &IID_ID3D12Resource, (void **)&vio_d3d12.instance_heap))) {
            vio_d3d12.instance_heap_gpu = ID3D12Resource_GetGPUVirtualAddress(vio_d3d12.instance_heap);
            vio_d3d12.instance_heap_capacity = heap_size;
            vio_d3d12.instance_frame_base = 0;
            vio_d3d12.instance_frame_end = heap_size / vio_d3d12.frame_count;
            vio_d3d12.instance_heap_offset = 0;
            /* Persistently map (never unmap — valid for UPLOAD heaps in D3D12) */
            D3D12_RANGE rr = {0, 0};
            ID3D12Resource_Map(vio_d3d12.instance_heap, 0, &rr, (void **)&vio_d3d12.instance_heap_mapped);
        }
    }

    /* Identity instance buffer (single mat4 identity for non-instanced draws on slot 1) */
    {
        float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = sizeof(identity);
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(ID3D12Device_CreateCommittedResource(vio_d3d12.device,
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                NULL, &IID_ID3D12Resource, (void **)&vio_d3d12.identity_instance_buf))) {
            vio_d3d12.identity_instance_gpu = ID3D12Resource_GetGPUVirtualAddress(vio_d3d12.identity_instance_buf);
            void *mapped = NULL;
            D3D12_RANGE rr = {0, 0};
            if (SUCCEEDED(ID3D12Resource_Map(vio_d3d12.identity_instance_buf, 0, &rr, &mapped))) {
                memcpy(mapped, identity, sizeof(identity));
                ID3D12Resource_Unmap(vio_d3d12.identity_instance_buf, 0, NULL);
            }
        }
    }

    vio_d3d12.vsync = cfg->vsync;
    vio_d3d12.initialized = 1;
    return 0;

init_fail:
    /* Clean up anything already created */
    vio_d3d12.initialized = 1; /* allow shutdown to run */
    d3d12_shutdown();
    return -1;
}

static void d3d12_shutdown(void)
{
    if (!vio_d3d12.initialized) return;

    /* Wait for GPU to finish all work */
    vio_d3d12_wait_for_gpu();
    d3d12_bundles_sweep();

    if (vio_d3d12.vrs_image) { ID3D12Resource_Release(vio_d3d12.vrs_image); vio_d3d12.vrs_image = NULL; }
    vio_d3d12.vrs_image_active = 0;

    /* Upload queue: every submission is complete now, release all staging. */
    d3d12_retire_uploads(1);
    free(vio_d3d12.upload_retire);
    vio_d3d12.upload_retire = NULL;
    vio_d3d12.upload_retire_count = vio_d3d12.upload_retire_cap = 0;
    if (vio_d3d12.upload_list) { ID3D12GraphicsCommandList_Release(vio_d3d12.upload_list); vio_d3d12.upload_list = NULL; }
    for (int i = 0; i < VIO_D3D12_UPLOAD_ALLOCATORS; i++) {
        if (vio_d3d12.upload_allocs[i]) { ID3D12CommandAllocator_Release(vio_d3d12.upload_allocs[i]); vio_d3d12.upload_allocs[i] = NULL; }
    }

    d3d12_release_render_targets();

    if (vio_d3d12.cbuffer_heap) {
        ID3D12Resource_Unmap(vio_d3d12.cbuffer_heap, 0, NULL);
        ID3D12Resource_Release(vio_d3d12.cbuffer_heap);
        vio_d3d12.cbuffer_heap = NULL;
        vio_d3d12.cbuffer_heap_mapped = NULL;
    }

    if (vio_d3d12.instance_heap) {
        ID3D12Resource_Unmap(vio_d3d12.instance_heap, 0, NULL);
        ID3D12Resource_Release(vio_d3d12.instance_heap);
        vio_d3d12.instance_heap = NULL;
        vio_d3d12.instance_heap_mapped = NULL;
    }

    if (vio_d3d12.identity_instance_buf) {
        ID3D12Resource_Release(vio_d3d12.identity_instance_buf);
        vio_d3d12.identity_instance_buf = NULL;
    }

    if (vio_d3d12.depth_buffer) {
        ID3D12Resource_Release(vio_d3d12.depth_buffer);
    }

    for (UINT i = 0; i < vio_d3d12.frame_count; i++) {
        if (vio_d3d12.frames[i].cmd_allocator) {
            ID3D12CommandAllocator_Release(vio_d3d12.frames[i].cmd_allocator);
        }
    }

    if (vio_d3d12.cmd_list)       ID3D12GraphicsCommandList_Release(vio_d3d12.cmd_list);
    if (vio_d3d12.root_signature) ID3D12RootSignature_Release(vio_d3d12.root_signature);
    if (vio_d3d12.compute_root_signature) ID3D12RootSignature_Release(vio_d3d12.compute_root_signature);
    if (vio_d3d12.bindless_cpu_heap) { ID3D12DescriptorHeap_Release(vio_d3d12.bindless_cpu_heap); vio_d3d12.bindless_cpu_heap = NULL; }
    if (vio_d3d12.compute_srv_heap) ID3D12DescriptorHeap_Release(vio_d3d12.compute_srv_heap);
    if (vio_d3d12.cmdsig_indexed) ID3D12CommandSignature_Release(vio_d3d12.cmdsig_indexed);
    if (vio_d3d12.cmdsig_plain)   ID3D12CommandSignature_Release(vio_d3d12.cmdsig_plain);
    if (vio_d3d12.cmdsig_indexed_dp) ID3D12CommandSignature_Release(vio_d3d12.cmdsig_indexed_dp);
    if (vio_d3d12.cmdsig_plain_dp)   ID3D12CommandSignature_Release(vio_d3d12.cmdsig_plain_dp);
    vio_d3d12.cmdsig_indexed_dp = vio_d3d12.cmdsig_plain_dp = NULL;
    for (int i = 0; i < VIO_D3D12_MAX_FRAME_COUNT; i++) {
        if (vio_d3d12.dp_buf[i]) ID3D12Resource_Release(vio_d3d12.dp_buf[i]);
        vio_d3d12.dp_buf[i] = NULL;
        vio_d3d12.dp_cap[i] = vio_d3d12.dp_used[i] = 0;
    }
    d3d12_release_parked_all();
    if (vio_d3d12.dmip_pso)       { ID3D12PipelineState_Release(vio_d3d12.dmip_pso); vio_d3d12.dmip_pso = NULL; }
    if (vio_d3d12.dmip_resolve_pso) { ID3D12PipelineState_Release(vio_d3d12.dmip_resolve_pso); vio_d3d12.dmip_resolve_pso = NULL; }
    if (vio_d3d12.dmip_rs)        { ID3D12RootSignature_Release(vio_d3d12.dmip_rs); vio_d3d12.dmip_rs = NULL; }
    if (vio_d3d12.mipgen_pso)     ID3D12PipelineState_Release(vio_d3d12.mipgen_pso);
    if (vio_d3d12.mipgen_rs)      ID3D12RootSignature_Release(vio_d3d12.mipgen_rs);
    if (vio_d3d12.mipgen_heap)    ID3D12DescriptorHeap_Release(vio_d3d12.mipgen_heap);
    if (vio_d3d12.cmd_list5)      ID3D12GraphicsCommandList5_Release(vio_d3d12.cmd_list5);
    if (vio_d3d12.cmd_list1)      ID3D12GraphicsCommandList1_Release(vio_d3d12.cmd_list1);
    vio_d3d12.cmd_list1 = NULL;
    if (vio_d3d12.cmd_list6)      ID3D12GraphicsCommandList6_Release(vio_d3d12.cmd_list6);
    vio_d3d12.cmd_list6 = NULL;
    if (vio_d3d12.cmdsig_mesh)    ID3D12CommandSignature_Release(vio_d3d12.cmdsig_mesh);
    vio_d3d12.cmdsig_mesh = NULL;
    if (vio_d3d12.mesh_root_signature) ID3D12RootSignature_Release(vio_d3d12.mesh_root_signature);
    vio_d3d12.mesh_root_signature = NULL;
    if (vio_d3d12.ts_readback)    ID3D12Resource_Release(vio_d3d12.ts_readback);
    if (vio_d3d12.ts_heap)        ID3D12QueryHeap_Release(vio_d3d12.ts_heap);
    if (vio_d3d12.fence)          ID3D12Fence_Release(vio_d3d12.fence);
    if (vio_d3d12.fence_event)    CloseHandle(vio_d3d12.fence_event);
    if (vio_d3d12.rtv_heap)       ID3D12DescriptorHeap_Release(vio_d3d12.rtv_heap);
    if (vio_d3d12.dsv_heap)       ID3D12DescriptorHeap_Release(vio_d3d12.dsv_heap);
    if (vio_d3d12.srv_heap.heap)  ID3D12DescriptorHeap_Release(vio_d3d12.srv_heap.heap);
    if (vio_d3d12.srv_staging_heap) ID3D12DescriptorHeap_Release(vio_d3d12.srv_staging_heap);
    if (vio_d3d12.sampler_combo_heap) ID3D12DescriptorHeap_Release(vio_d3d12.sampler_combo_heap);
    if (vio_d3d12.sampler_heap)   ID3D12DescriptorHeap_Release(vio_d3d12.sampler_heap);
    if (vio_d3d12.frame_latency_waitable) CloseHandle(vio_d3d12.frame_latency_waitable);
    if (vio_d3d12.fb_null_map)    ID3D12Resource_Release(vio_d3d12.fb_null_map);
    if (vio_d3d12.fb_null_tex)    ID3D12Resource_Release(vio_d3d12.fb_null_tex);
    if (vio_d3d12.swapchain)      IDXGISwapChain3_Release(vio_d3d12.swapchain);
    if (vio_d3d12.cmd_queue)      ID3D12CommandQueue_Release(vio_d3d12.cmd_queue);
    if (vio_d3d12.factory)        IDXGIFactory4_Release(vio_d3d12.factory);
    /* Cached debug-layer InfoQueue — must go before the device it was QI'd from. */
    if (vio_d3d12.info_queue)     ID3D12InfoQueue_Release(vio_d3d12.info_queue);
    if (vio_d3d12.device)         ID3D12Device_Release(vio_d3d12.device);
    if (vio_d3d12.agility_factory) IUnknown_Release(vio_d3d12.agility_factory);
    if (vio_d3d12.agility_config)  IUnknown_Release(vio_d3d12.agility_config);

    d3d12_current_pipeline = NULL;
    vio_d3d12.fb_bound = NULL;
    vio_d3d12.fb_null_gpu.ptr = 0;
    memset(&vio_d3d12, 0, sizeof(vio_d3d12));
}

/* ── Surface & Window ─────────────────────────────────────────────── */

static void *d3d12_create_surface(vio_config *cfg)
{
    /* The platform window's HWND carries the swapchain. */
    HWND hwnd = vio_d3d12.platform_window ? (HWND)vio_plat()->native_handle(vio_d3d12.platform_window, VIO_NATIVE_HWND) : NULL;
    if (!hwnd) {
        php_error_docref(NULL, E_WARNING, "D3D12: no window to present into");
        return NULL;
    }

    /* HDR10 (GAP-PHASE5 Block 6): 10-bit backbuffer when asked for and the
     * window's display is in HDR mode (or forced); colour space set below. */
    int want_hdr = cfg->hdr_output == 2
        || (cfg->hdr_output == 1 && vio_d3d_hwnd_output_is_hdr((IDXGIFactory1 *)vio_d3d12.factory, hwnd));
    vio_d3d12.swapchain_format = want_hdr ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    vio_d3d12.hdr_paper_white = cfg->hdr_paper_white > 0.0f ? cfg->hdr_paper_white : 200.0f;
    vio_d3d12.hdr_output = 0;

    DXGI_SWAP_CHAIN_DESC1 sc_desc = {0};
    sc_desc.Width = cfg->width;
    sc_desc.Height = cfg->height;
    sc_desc.Format = vio_d3d12.swapchain_format;
    sc_desc.Stereo = FALSE;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = vio_d3d12.frame_count;
    sc_desc.Scaling = DXGI_SCALING_STRETCH;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

    /* ALLOW_TEARING only when the factory reported support. Setting it blindly makes
     * CreateSwapChainForHwnd fail with DXGI_ERROR_INVALID_CALL on systems that lack
     * the feature. Cache the exact flag set: d3d12_resize() must pass the identical
     * value to ResizeBuffers or the swapchain silently loses its tearing capability
     * and every later Present with DXGI_PRESENT_ALLOW_TEARING fails. */
    vio_d3d12.swapchain_flags = vio_d3d12.tearing_supported
        ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
        : 0u;
    /* Waitable swapchain (GAP-PHASE5 Block 5): windowed FLIP swapchains may carry
     * the frame-latency waitable object; begin_frame waits on it. */
    if (cfg->frame_latency > 0) {
        vio_d3d12.swapchain_flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    }
    sc_desc.Flags = vio_d3d12.swapchain_flags;

    IDXGISwapChain1 *swapchain1 = NULL;
    HRESULT hr = IDXGIFactory4_CreateSwapChainForHwnd(
        vio_d3d12.factory,
        (IUnknown *)vio_d3d12.cmd_queue,  /* D3D12 uses command queue, not device */
        hwnd,
        &sc_desc,
        NULL, NULL,
        &swapchain1
    );
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create swapchain (0x%08lx)", hr);
        return NULL;
    }

    /* QI for IDXGISwapChain3 (needed for GetCurrentBackBufferIndex) */
    hr = IDXGISwapChain1_QueryInterface(swapchain1, &IID_IDXGISwapChain3,
                                         (void **)&vio_d3d12.swapchain);
    IDXGISwapChain1_Release(swapchain1);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: SwapChain3 not supported (0x%08lx)", hr);
        return NULL;
    }
    if (want_hdr) {
        /* ST 2084 / BT.2020 when the swapchain can present it; a 10-bit
         * swapchain on an SDR desktop (forced) stays sRGB-interpreted. */
        UINT support = 0;
        if (SUCCEEDED(IDXGISwapChain3_CheckColorSpaceSupport(vio_d3d12.swapchain, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &support))
            && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)
            && SUCCEEDED(IDXGISwapChain3_SetColorSpace1(vio_d3d12.swapchain, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020))) {
            vio_d3d12.hdr_output = 1;
        } else if (cfg->hdr_output == 2) {
            vio_d3d12.hdr_output = 1;   /* forced: encode PQ anyway (test path) */
        }
    }
    if (cfg->frame_latency > 0) {
        /* IDXGISwapChain3 derives from IDXGISwapChain2: set the cap and grab the
         * waitable object once; ResizeBuffers keeps it valid. */
        IDXGISwapChain3_SetMaximumFrameLatency(vio_d3d12.swapchain, (UINT)cfg->frame_latency);
        vio_d3d12.frame_latency_waitable = IDXGISwapChain3_GetFrameLatencyWaitableObject(vio_d3d12.swapchain);
        vio_d3d12.frame_latency = vio_d3d12.frame_latency_waitable ? cfg->frame_latency : 0;
    }

    /* Disable ALT+Enter */
    IDXGIFactory4_MakeWindowAssociation(vio_d3d12.factory, hwnd, DXGI_MWA_NO_ALT_ENTER);

    vio_d3d12.width = cfg->width;
    vio_d3d12.height = cfg->height;
    vio_d3d12.frame_index = IDXGISwapChain3_GetCurrentBackBufferIndex(vio_d3d12.swapchain);

    /* Create render target views for each frame */
    if (d3d12_create_render_targets() != 0) {
        return NULL;
    }

    /* Create depth buffer */
    if (d3d12_create_depth_buffer(cfg->width, cfg->height) != 0) {
        return NULL;
    }

    return vio_d3d12.swapchain;
}

static void d3d12_destroy_surface(void *surface)
{
    (void)surface;
    vio_d3d12_wait_for_gpu();

    d3d12_release_render_targets();

    if (vio_d3d12.depth_buffer) {
        ID3D12Resource_Release(vio_d3d12.depth_buffer);
        vio_d3d12.depth_buffer = NULL;
    }

    if (vio_d3d12.swapchain) {
        IDXGISwapChain3_Release(vio_d3d12.swapchain);
        vio_d3d12.swapchain = NULL;
    }
}

static void d3d12_resize(int width, int height)
{
    /* Skip if nothing to do, no swapchain yet, or the window is minimised
     * (0x0). ResizeBuffers to a zero dimension fails, and a minimised window
     * has no client area to draw to — keep the last valid swapchain size and
     * pick the change up again when the window is restored. */
    if (!vio_d3d12.swapchain || width <= 0 || height <= 0 ||
        (width == vio_d3d12.width && height == vio_d3d12.height)) {
        return;
    }

    /* The window is being resized live (maximise / fullscreen / drag), so the
     * resize MUST NOT run while a frame's command list is open and recording
     * into the soon-to-be-released backbuffer. Callers (vio_begin) invoke this
     * before begin_frame(), i.e. while in_frame==0 and the command list is in
     * the Closed state. Bail out defensively if that contract is violated. */
    if (vio_d3d12.in_frame) {
        php_error_docref(NULL, E_WARNING,
            "D3D12: resize requested mid-frame; ignoring to avoid releasing a "
            "backbuffer the open command list still references");
        return;
    }

    /* GPU must be idle before we release any backbuffer references. */
    vio_d3d12_wait_for_gpu();

    /* CRITICAL for FLIP_DISCARD swapchains: IDXGISwapChain::ResizeBuffers fails
     * with DXGI_ERROR_INVALID_CALL if ANY outstanding reference to a backbuffer
     * remains. d3d12_release_render_targets() drops our COM refs, but the
     * command list recorded last frame (OMSetRenderTargets + the
     * RENDER_TARGET->PRESENT barrier in end_frame) is Closed-but-not-Reset and
     * still internally references frames[*].render_target. Reset it here against
     * the current frame's allocator (safe now the GPU is idle) so those
     * references are dropped. begin_frame() Resets it again next frame, so this
     * leaves the list in the same Closed/clean state the rest of the code
     * expects between frames.
     *
     * Without this, ResizeBuffers silently fails and the old (e.g. 16:9)
     * backbuffer keeps being stretched onto the new (e.g. 32:9) client area —
     * the exact maximise/fullscreen stretch this function exists to prevent. */
    {
        vio_d3d12_frame *frame = &vio_d3d12.frames[vio_d3d12.frame_index];
        ID3D12CommandAllocator_Reset(frame->cmd_allocator);
        ID3D12GraphicsCommandList_Reset(vio_d3d12.cmd_list, frame->cmd_allocator, NULL);
        ID3D12GraphicsCommandList_Close(vio_d3d12.cmd_list);
    }

    d3d12_release_render_targets();
    if (vio_d3d12.depth_buffer) {
        ID3D12Resource_Release(vio_d3d12.depth_buffer);
        vio_d3d12.depth_buffer = NULL;
    }

    /* CRITICAL: the flag set passed here must be identical to the one the swapchain
     * was created with (vio_d3d12.swapchain_flags). ResizeBuffers does NOT preserve
     * flags the way this call preserves BufferCount and Format — it REPLACES them.
     * Passing 0 on a swapchain created with ALLOW_TEARING silently strips the
     * tearing capability, and every subsequent Present() with
     * DXGI_PRESENT_ALLOW_TEARING then fails with DXGI_ERROR_INVALID_CALL — i.e.
     * frames stop reaching the screen after the first window resize. */
    HRESULT hr = IDXGISwapChain3_ResizeBuffers(vio_d3d12.swapchain,
                                                vio_d3d12.frame_count,
                                                width, height,
                                                vio_d3d12.swapchain_format,
                                                vio_d3d12.swapchain_flags);
    if (FAILED(hr)) {
        HRESULT removed = ID3D12Device_GetDeviceRemovedReason(vio_d3d12.device);
        php_error_docref(NULL, E_WARNING,
            "D3D12: Failed to resize buffers to %dx%d (0x%08lx) device_removed=0x%08lx",
            width, height, hr, removed);
        d3d12_drain_info_queue("resize_fail");
        /* Recreate RTVs/depth at the OLD size so the swapchain stays usable and
         * the next frame doesn't draw into freed render targets. */
        d3d12_create_render_targets();
        d3d12_create_depth_buffer(vio_d3d12.width, vio_d3d12.height);
        return;
    }

    vio_d3d12.width = width;
    vio_d3d12.height = height;
    vio_d3d12.frame_index = IDXGISwapChain3_GetCurrentBackBufferIndex(vio_d3d12.swapchain);
    vio_d3d12.last_presented_frame_idx = 0;

    /* Cached backbuffer RTV/DSV handles and dimensions are now stale; the
     * recreated handles below replace them, and begin_frame() rebinds them.
     * Drop any cached "currently bound RT == backbuffer" tracking so the next
     * frame transitions from the correct (recreated) resource. */
    vio_d3d12.current_bound_rt = NULL;

    if (d3d12_create_render_targets() != 0) {
        php_error_docref(NULL, E_WARNING, "D3D12: failed to recreate render targets after resize");
        return;
    }
    if (d3d12_create_depth_buffer(width, height) != 0) {
        php_error_docref(NULL, E_WARNING, "D3D12: failed to recreate depth buffer after resize");
        return;
    }
}

/* ── Pipeline ─────────────────────────────────────────────────────── */

/* Fill one render-target blend description from a vio blend mode + VIO_COLOR_*
 * write mask. Used for RenderTarget[0] (the classic single state, replicated to
 * every attachment by the runtime) and, with IndependentBlendEnable, per
 * attachment when the pipeline carries 'attachment_blend' / 'attachment_color_mask'. */
static void d3d12_fill_rt_blend(D3D12_RENDER_TARGET_BLEND_DESC *b, int blend, int cm)
{
    UINT8 wm = 0; /* cm is taken literally: 0 = write nothing (masked-off attachment) */
    if (cm & VIO_COLOR_R) wm |= D3D12_COLOR_WRITE_ENABLE_RED;
    if (cm & VIO_COLOR_G) wm |= D3D12_COLOR_WRITE_ENABLE_GREEN;
    if (cm & VIO_COLOR_B) wm |= D3D12_COLOR_WRITE_ENABLE_BLUE;
    if (cm & VIO_COLOR_A) wm |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
    b->RenderTargetWriteMask = wm;
    b->BlendEnable = FALSE;
    b->BlendOp = b->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b->SrcBlend = D3D12_BLEND_ONE; b->DestBlend = D3D12_BLEND_ZERO;
    b->SrcBlendAlpha = D3D12_BLEND_ONE; b->DestBlendAlpha = D3D12_BLEND_ZERO;
    switch (blend) {
        case VIO_BLEND_ALPHA:
            b->BlendEnable = TRUE;
            b->SrcBlend = D3D12_BLEND_SRC_ALPHA; b->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            b->SrcBlendAlpha = D3D12_BLEND_ONE;  b->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; break;
        case VIO_BLEND_ADDITIVE:
            b->BlendEnable = TRUE;
            b->SrcBlend = D3D12_BLEND_SRC_ALPHA; b->DestBlend = D3D12_BLEND_ONE;
            b->SrcBlendAlpha = D3D12_BLEND_ONE;  b->DestBlendAlpha = D3D12_BLEND_ONE; break;
        case VIO_BLEND_PREMULTIPLIED:
            b->BlendEnable = TRUE;
            b->SrcBlend = D3D12_BLEND_ONE;       b->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            b->SrcBlendAlpha = D3D12_BLEND_ONE;  b->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; break;
        case VIO_BLEND_MULTIPLY:
            b->BlendEnable = TRUE;
            b->SrcBlend = D3D12_BLEND_DEST_COLOR; b->DestBlend = D3D12_BLEND_ZERO;
            b->SrcBlendAlpha = D3D12_BLEND_DEST_ALPHA; b->DestBlendAlpha = D3D12_BLEND_ZERO; break;
        case VIO_BLEND_SCREEN:
            b->BlendEnable = TRUE;
            b->SrcBlend = D3D12_BLEND_ONE;       b->DestBlend = D3D12_BLEND_INV_SRC_COLOR;
            b->SrcBlendAlpha = D3D12_BLEND_ONE;  b->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; break;
        case VIO_BLEND_MIN:
            b->BlendEnable = TRUE;
            b->BlendOp = b->BlendOpAlpha = D3D12_BLEND_OP_MIN;
            b->SrcBlend = b->DestBlend = b->SrcBlendAlpha = b->DestBlendAlpha = D3D12_BLEND_ONE; break;
        case VIO_BLEND_MAX:
            b->BlendEnable = TRUE;
            b->BlendOp = b->BlendOpAlpha = D3D12_BLEND_OP_MAX;
            b->SrcBlend = b->DestBlend = b->SrcBlendAlpha = b->DestBlendAlpha = D3D12_BLEND_ONE; break;
        default: break;
    }
}

/* ── Inline ray tracing (VIO_FEATURE_RAY_QUERY) ─────────────────────
 * DXR 1.1 acceleration structures, built synchronously on a dedicated command
 * list: one bottom-level structure per geometry (positions R32G32B32_FLOAT,
 * uint32 indices, opaque), one top-level structure over the instances. The
 * shaders read the bound top level through root SRV t0, space9
 * (VIO_D3D12_RP_ACCEL / compute root parameter 3). */
typedef struct _vio_d3d12_as {
    ID3D12Resource  *tlas;
    ID3D12Resource **blas;
    int              blas_count;
    /* Kept for vio_acceleration_structure_update (A14): the top level is built
     * with ALLOW_UPDATE, its scratch and instance descriptors stay. */
    UINT64           tlas_size, scratch_size;
    ID3D12Resource  *tlas_scratch;
    ID3D12Resource  *instances;      /* upload buffer, instance_cap descriptors */
    int              instance_cap, instance_count;
} vio_d3d12_as;
static vio_d3d12_as *d3d12_bound_as = NULL;

static ID3D12Resource *d3d12_as_buffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                                       D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size > 0 ? size : 256;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    /* Buffers start in COMMON whatever is asked (the debug layer warns); scratch
     * buffers promote to UNORDERED_ACCESS on first use. */
    if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) state = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource *r = NULL;
    if (FAILED(ID3D12Device_CreateCommittedResource(vio_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL,
                                                    &IID_ID3D12Resource, (void **)&r))) return NULL;
    return r;
}

static ID3D12Resource *d3d12_as_upload(const void *data, size_t size)
{
    ID3D12Resource *r = d3d12_as_buffer((UINT64)size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!r) return NULL;
    void *p = NULL;
    D3D12_RANGE none = { 0, 0 };
    if (FAILED(ID3D12Resource_Map(r, 0, &none, &p)) || !p) { ID3D12Resource_Release(r); return NULL; }
    memcpy(p, data, size);
    ID3D12Resource_Unmap(r, 0, NULL);
    return r;
}

static void d3d12_as_free(vio_d3d12_as *as)
{
    if (!as) return;
    if (as->tlas) ID3D12Resource_Release(as->tlas);
    if (as->tlas_scratch) ID3D12Resource_Release(as->tlas_scratch);
    if (as->instances) ID3D12Resource_Release(as->instances);
    for (int i = 0; i < as->blas_count; i++) if (as->blas[i]) ID3D12Resource_Release(as->blas[i]);
    free(as->blas);
    free(as);
}

/* Record the top-level build over `count` instances (A14). Built with
 * ALLOW_UPDATE: refit = 1 (same count and geometries) updates it in place, a
 * rebuild reuses the result / scratch resources while they are big enough.
 * The GPU must not be using the structure (create, or after a drain). */
static int d3d12_as_record_tlas(ID3D12Device5 *dev5, ID3D12GraphicsCommandList4 *list4, vio_d3d12_as *as,
                                const vio_as_instance *inst, int count, int refit)
{
    if (count > as->instance_cap) {
        if (as->instances) ID3D12Resource_Release(as->instances);
        as->instances = d3d12_as_buffer(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * (UINT64)count, D3D12_HEAP_TYPE_UPLOAD,
                                        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        as->instance_cap = as->instances ? count : 0;
        if (!as->instances) return -1;
    }
    D3D12_RAYTRACING_INSTANCE_DESC *ids = NULL;
    D3D12_RANGE none = { 0, 0 };
    if (FAILED(ID3D12Resource_Map(as->instances, 0, &none, (void **)&ids)) || !ids) return -1;
    for (int i = 0; i < count; i++) {
        const vio_as_instance *src = &inst[i];
        if (src->geometry < 0 || src->geometry >= as->blas_count) { ID3D12Resource_Unmap(as->instances, 0, NULL); return -1; }
        memset(&ids[i], 0, sizeof(ids[i]));
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++) ids[i].Transform[r][c] = src->transform[r * 4 + c];
        ids[i].InstanceID = (UINT)i;
        ids[i].InstanceMask = (UINT)(src->mask & 0xFF);
        ids[i].InstanceContributionToHitGroupIndex = (UINT)src->hit_group;
        ids[i].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
        ids[i].AccelerationStructure = ID3D12Resource_GetGPUVirtualAddress(as->blas[src->geometry]);
    }
    ID3D12Resource_Unmap(as->instances, 0, NULL);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in;
    memset(&in, 0, sizeof(in));
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
             | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    in.NumDescs = (UINT)count;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.InstanceDescs = ID3D12Resource_GetGPUVirtualAddress(as->instances);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre;
    memset(&pre, 0, sizeof(pre));
    ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(dev5, &in, &pre);
    UINT64 scratch = refit ? pre.UpdateScratchDataSizeInBytes : pre.ScratchDataSizeInBytes;
    if (refit && !as->tlas) return -1;
    if (!refit && (!as->tlas || pre.ResultDataMaxSizeInBytes > as->tlas_size)) {
        if (as->tlas) ID3D12Resource_Release(as->tlas);
        as->tlas = d3d12_as_buffer(pre.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        as->tlas_size = as->tlas ? pre.ResultDataMaxSizeInBytes : 0;
        if (!as->tlas) return -1;
    }
    if (!as->tlas_scratch || scratch > as->scratch_size) {
        if (as->tlas_scratch) ID3D12Resource_Release(as->tlas_scratch);
        as->tlas_scratch = d3d12_as_buffer(scratch, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        as->scratch_size = as->tlas_scratch ? scratch : 0;
        if (!as->tlas_scratch) return -1;
    }
    if (refit) in.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd;
    memset(&bd, 0, sizeof(bd));
    bd.Inputs = in;
    bd.DestAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(as->tlas);
    if (refit) bd.SourceAccelerationStructureData = bd.DestAccelerationStructureData;
    bd.ScratchAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(as->tlas_scratch);
    ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(list4, &bd, 0, NULL);
    as->instance_count = count;
    return 0;
}

static void *d3d12_create_acceleration_structure(const vio_as_desc *desc)
{
    if (!desc || desc->geometry_count < 1 || desc->instance_count < 1 || !vio_d3d12.device) return NULL;
    ID3D12Device5 *dev5 = NULL;
    if (FAILED(ID3D12Device_QueryInterface(vio_d3d12.device, &IID_ID3D12Device5, (void **)&dev5)) || !dev5) return NULL;
    vio_d3d12_as *as = calloc(1, sizeof(vio_d3d12_as));
    ID3D12Resource **tmp = calloc((size_t)desc->geometry_count * 3 + 4, sizeof(ID3D12Resource *));
    int tmp_count = 0;
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    ID3D12GraphicsCommandList4 *list4 = NULL;
    if (!as || !tmp) goto fail;
    as->blas = calloc((size_t)desc->geometry_count, sizeof(ID3D12Resource *));
    if (!as->blas) goto fail;
    if (FAILED(ID3D12Device_CreateCommandAllocator(vio_d3d12.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   &IID_ID3D12CommandAllocator, (void **)&alloc))) goto fail;
    if (FAILED(ID3D12Device_CreateCommandList(vio_d3d12.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                                              &IID_ID3D12GraphicsCommandList, (void **)&list))) goto fail;
    if (FAILED(ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList4, (void **)&list4))) goto fail;

    for (int g = 0; g < desc->geometry_count; g++) {
        const vio_as_geometry *geo = &desc->geometries[g];
        ID3D12Resource *vb = d3d12_as_upload(geo->positions, (size_t)geo->vertex_count * 12);
        if (!vb) goto fail;
        tmp[tmp_count++] = vb;
        D3D12_RAYTRACING_GEOMETRY_DESC gd;
        memset(&gd, 0, sizeof(gd));
        gd.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        gd.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        gd.Triangles.VertexBuffer.StartAddress = ID3D12Resource_GetGPUVirtualAddress(vb);
        gd.Triangles.VertexBuffer.StrideInBytes = 12;
        gd.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        gd.Triangles.VertexCount = (UINT)geo->vertex_count;
        if (geo->indices && geo->index_count >= 3) {
            ID3D12Resource *ib = d3d12_as_upload(geo->indices, (size_t)geo->index_count * 4);
            if (!ib) goto fail;
            tmp[tmp_count++] = ib;
            gd.Triangles.IndexBuffer = ID3D12Resource_GetGPUVirtualAddress(ib);
            gd.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            gd.Triangles.IndexCount = (UINT)geo->index_count;
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in;
        memset(&in, 0, sizeof(in));
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        in.NumDescs = 1;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.pGeometryDescs = &gd;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre;
        memset(&pre, 0, sizeof(pre));
        ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(dev5, &in, &pre);
        ID3D12Resource *blas = d3d12_as_buffer(pre.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                               D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        ID3D12Resource *scratch = d3d12_as_buffer(pre.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!blas || !scratch) { if (blas) ID3D12Resource_Release(blas); if (scratch) ID3D12Resource_Release(scratch); goto fail; }
        as->blas[as->blas_count++] = blas;
        tmp[tmp_count++] = scratch;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd;
        memset(&bd, 0, sizeof(bd));
        bd.Inputs = in;
        bd.DestAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(blas);
        bd.ScratchAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(scratch);
        ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(list4, &bd, 0, NULL);
    }
    /* The top level reads the finished bottom levels. */
    {
        D3D12_RESOURCE_BARRIER uav;
        memset(&uav, 0, sizeof(uav));
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = NULL;   /* all UAV accesses */
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &uav);
    }
    if (d3d12_as_record_tlas(dev5, list4, as, desc->instances, desc->instance_count, 0) != 0) goto fail;
    if (FAILED(ID3D12GraphicsCommandList_Close(list))) goto fail;
    {
        ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
        ID3D12CommandQueue_ExecuteCommandLists(vio_d3d12.cmd_queue, 1, lists);
        vio_d3d12_wait_for_gpu();   /* the temporaries below are freed right after */
    }
    for (int i = 0; i < tmp_count; i++) ID3D12Resource_Release(tmp[i]);
    free(tmp);
    ID3D12GraphicsCommandList4_Release(list4);
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(alloc);
    ID3D12Device5_Release(dev5);
    return as;

fail:
    php_error_docref(NULL, E_WARNING, "D3D12: acceleration structure build failed");
    if (tmp) { for (int i = 0; i < tmp_count; i++) ID3D12Resource_Release(tmp[i]); free(tmp); }
    if (list4) ID3D12GraphicsCommandList4_Release(list4);
    if (list) ID3D12GraphicsCommandList_Release(list);
    if (alloc) ID3D12CommandAllocator_Release(alloc);
    d3d12_as_free(as);
    ID3D12Device5_Release(dev5);
    return NULL;
}

static int d3d12_update_acceleration_structure(void *ptr, const vio_as_instance *inst, int count, int refit)
{
    vio_d3d12_as *as = (vio_d3d12_as *)ptr;
    if (!as || !inst || count < 1 || !vio_d3d12.device || vio_d3d12.in_frame) return -1;
    if (refit && count != as->instance_count) return -1;
    ID3D12Device5 *dev5 = NULL;
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    ID3D12GraphicsCommandList4 *list4 = NULL;
    int rc = -1;
    if (FAILED(ID3D12Device_QueryInterface(vio_d3d12.device, &IID_ID3D12Device5, (void **)&dev5)) || !dev5) return -1;
    /* Frames in flight may still read the structure and its instance buffer. */
    vio_d3d12_wait_for_gpu();
    if (FAILED(ID3D12Device_CreateCommandAllocator(vio_d3d12.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   &IID_ID3D12CommandAllocator, (void **)&alloc))) goto done;
    if (FAILED(ID3D12Device_CreateCommandList(vio_d3d12.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                                              &IID_ID3D12GraphicsCommandList, (void **)&list))) goto done;
    if (FAILED(ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList4, (void **)&list4))) goto done;
    if (d3d12_as_record_tlas(dev5, list4, as, inst, count, refit) != 0) goto done;
    if (FAILED(ID3D12GraphicsCommandList_Close(list))) goto done;
    {
        ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
        ID3D12CommandQueue_ExecuteCommandLists(vio_d3d12.cmd_queue, 1, lists);
        vio_d3d12_wait_for_gpu();
    }
    rc = 0;
done:
    if (list4) ID3D12GraphicsCommandList4_Release(list4);
    if (list) ID3D12GraphicsCommandList_Release(list);
    if (alloc) ID3D12CommandAllocator_Release(alloc);
    ID3D12Device5_Release(dev5);
    return rc;
}

static void d3d12_destroy_acceleration_structure(void *ptr)
{
    vio_d3d12_as *as = (vio_d3d12_as *)ptr;
    if (!as) return;
    if (d3d12_bound_as == as) d3d12_bound_as = NULL;
    /* A frame in flight may still read it: drain before releasing (rare). */
    vio_d3d12_wait_for_gpu();
    d3d12_as_free(as);
}

static void d3d12_bind_acceleration_structure(void *ptr, int binding)
{
    (void)binding;   /* the shader's structure always sits at t0, space9 */
    d3d12_bound_as = (vio_d3d12_as *)ptr;
}

/* Graphics draws: the bound top level at root SRV [14] (root arguments reset
 * with every SetGraphicsRootSignature, so every draw re-arms it). */
/* Fragment storage buffers (A15): root UAVs [16..19], re-armed on every draw
 * like the acceleration structure; the buffers are marked for readback. */
static vio_d3d12_buffer *d3d12_fs_storage[VIO_MAX_FRAGMENT_STORAGE];

static int d3d12_bind_fragment_storage(void *backend_buffer, int binding)
{
    if (binding < 0 || binding >= VIO_MAX_FRAGMENT_STORAGE) return -1;
    vio_d3d12_buffer *buf = (vio_d3d12_buffer *)backend_buffer;
    if (buf && !buf->resource) return -1;
    d3d12_fs_storage[binding] = buf;
    return 0;
}

/* What an unbound slot points at: a pixel shader that writes a declared but
 * unbound root UAV removes the device on hardware (WARP shrugs it off). */
static ID3D12Resource *d3d12_fs_scratch;
static ID3D12Device   *d3d12_fs_scratch_dev;

static D3D12_GPU_VIRTUAL_ADDRESS d3d12_fs_scratch_va(void)
{
    if (!d3d12_fs_scratch || d3d12_fs_scratch_dev != vio_d3d12.device) {
        if (d3d12_fs_scratch) ID3D12Resource_Release(d3d12_fs_scratch);
        d3d12_fs_scratch = d3d12_as_buffer(65536, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                           D3D12_RESOURCE_STATE_COMMON);
        d3d12_fs_scratch_dev = vio_d3d12.device;
    }
    return d3d12_fs_scratch ? ID3D12Resource_GetGPUVirtualAddress(d3d12_fs_scratch) : 0;
}

static void d3d12_apply_fs_storage(void)
{
    if (!vio_d3d12.cmd_list) return;
    for (int i = 0; i < VIO_MAX_FRAGMENT_STORAGE; i++) {
        vio_d3d12_buffer *buf = d3d12_fs_storage[i];
        if (!buf || !buf->resource) {
            D3D12_GPU_VIRTUAL_ADDRESS va = d3d12_fs_scratch_va();
            if (va) ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(vio_d3d12.cmd_list, (UINT)(VIO_D3D12_RP_PS_UAV + i), va);
            continue;
        }
        ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(vio_d3d12.cmd_list, (UINT)(VIO_D3D12_RP_PS_UAV + i),
                                                                    ID3D12Resource_GetGPUVirtualAddress(buf->resource));
        buf->fs_dirty = 1;
        buf->uav_live_serial = vio_d3d12.frame_serial;   /* UNORDERED_ACCESS on this frame's list */
    }
}

static void d3d12_apply_accel(void)
{
    d3d12_apply_fs_storage();
    if (!d3d12_bound_as || !d3d12_bound_as->tlas || !vio_d3d12.cmd_list) return;
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(vio_d3d12.cmd_list, VIO_D3D12_RP_ACCEL,
        ID3D12Resource_GetGPUVirtualAddress(d3d12_bound_as->tlas));
}

/* Multiview (VIO_FEATURE_MULTIVIEW): view instancing exists only in the
 * pipeline-state-stream API (ID3D12Device2::CreatePipelineState). The stream is
 * the graphics description subobject by subobject plus VIEW_INSTANCING; every
 * subobject starts pointer-aligned with its value after the 4-byte type at the
 * value's own alignment (the CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT layout). */
static void d3d12_pss_add(unsigned char *buf, size_t *off, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type,
                          const void *value, size_t size, size_t align)
{
    size_t o = (*off + 7) & ~(size_t)7;
    memcpy(buf + o, &type, sizeof(type));
    size_t vo = (o + sizeof(type) + align - 1) & ~(align - 1);
    memcpy(buf + vo, value, size);
    *off = (vo + size + 7) & ~(size_t)7;
}

static HRESULT d3d12_create_pso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d, int view_count,
                                const vio_d3d12_pipeline *mesh, ID3D12PipelineState **out)
{
    if (view_count <= 1 && !mesh)
        return ID3D12Device_CreateGraphicsPipelineState(vio_d3d12.device, d, &IID_ID3D12PipelineState, (void **)out);
    ID3D12Device2 *dev2 = NULL;
    if (FAILED(ID3D12Device_QueryInterface(vio_d3d12.device, &IID_ID3D12Device2, (void **)&dev2)) || !dev2)
        return E_NOINTERFACE;
    /* View v renders into slice v of the all-slices RTV / DSV (VIO_RT_ALL_LAYERS). */
    D3D12_VIEW_INSTANCE_LOCATION loc[4];
    for (int v = 0; v < 4; v++) { loc[v].ViewportArrayIndex = 0; loc[v].RenderTargetArrayIndex = (UINT)v; }
    D3D12_VIEW_INSTANCING_DESC vi;
    vi.ViewInstanceCount      = (UINT)view_count;
    vi.pViewInstanceLocations = loc;
    vi.Flags                  = D3D12_VIEW_INSTANCING_FLAG_NONE;
    struct D3D12_RT_FORMAT_ARRAY rtf;   /* d3d12.h declares it without a typedef */
    memset(&rtf, 0, sizeof(rtf));
    rtf.NumRenderTargets = d->NumRenderTargets;
    memcpy(rtf.RTFormats, d->RTVFormats, sizeof(rtf.RTFormats));
    ID3D12RootSignature *rs = d->pRootSignature;
    UINT sample_mask = d->SampleMask, node_mask = d->NodeMask;
    UINT64 storage[160];
    unsigned char *buf = (unsigned char *)storage;
    memset(storage, 0, sizeof(storage));
    size_t off = 0;
#define VIO_PSS(T, V, A) d3d12_pss_add(buf, &off, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##T, &(V), sizeof(V), (A))
    VIO_PSS(ROOT_SIGNATURE, rs, sizeof(void *));
    if (mesh) {
        /* Mesh pipeline: AS + MS instead of the vertex input stages. */
        if (mesh->as_blob) {
            D3D12_SHADER_BYTECODE as;
            as.pShaderBytecode = ID3D10Blob_GetBufferPointer(mesh->as_blob);
            as.BytecodeLength  = ID3D10Blob_GetBufferSize(mesh->as_blob);
            VIO_PSS(AS, as, sizeof(void *));
        }
        VIO_PSS(MS, d->VS, sizeof(void *));
        VIO_PSS(PS, d->PS, sizeof(void *));
        VIO_PSS(BLEND, d->BlendState, 4);
        VIO_PSS(SAMPLE_MASK, sample_mask, 4);
        VIO_PSS(RASTERIZER, d->RasterizerState, 4);
        VIO_PSS(DEPTH_STENCIL, d->DepthStencilState, 4);
        VIO_PSS(RENDER_TARGET_FORMATS, rtf, 4);
        VIO_PSS(DEPTH_STENCIL_FORMAT, d->DSVFormat, 4);
        VIO_PSS(SAMPLE_DESC, d->SampleDesc, 4);
        VIO_PSS(NODE_MASK, node_mask, 4);
        VIO_PSS(FLAGS, d->Flags, 4);
        if (view_count > 1) VIO_PSS(VIEW_INSTANCING, vi, sizeof(void *));
        goto stream_done;
    }
    if (d->VS.pShaderBytecode) VIO_PSS(VS, d->VS, sizeof(void *));
    if (d->PS.pShaderBytecode) VIO_PSS(PS, d->PS, sizeof(void *));
    if (d->DS.pShaderBytecode) VIO_PSS(DS, d->DS, sizeof(void *));
    if (d->HS.pShaderBytecode) VIO_PSS(HS, d->HS, sizeof(void *));
    if (d->GS.pShaderBytecode) VIO_PSS(GS, d->GS, sizeof(void *));
    VIO_PSS(BLEND, d->BlendState, 4);
    VIO_PSS(SAMPLE_MASK, sample_mask, 4);
    VIO_PSS(RASTERIZER, d->RasterizerState, 4);
    VIO_PSS(DEPTH_STENCIL, d->DepthStencilState, 4);
    VIO_PSS(INPUT_LAYOUT, d->InputLayout, sizeof(void *));
    VIO_PSS(IB_STRIP_CUT_VALUE, d->IBStripCutValue, 4);
    VIO_PSS(PRIMITIVE_TOPOLOGY, d->PrimitiveTopologyType, 4);
    VIO_PSS(RENDER_TARGET_FORMATS, rtf, 4);
    VIO_PSS(DEPTH_STENCIL_FORMAT, d->DSVFormat, 4);
    VIO_PSS(SAMPLE_DESC, d->SampleDesc, 4);
    VIO_PSS(NODE_MASK, node_mask, 4);
    VIO_PSS(FLAGS, d->Flags, 4);
    VIO_PSS(VIEW_INSTANCING, vi, sizeof(void *));
stream_done:
#undef VIO_PSS
    D3D12_PIPELINE_STATE_STREAM_DESC sd;
    sd.SizeInBytes                   = off;
    sd.pPipelineStateSubobjectStream = buf;
    HRESULT hr = ID3D12Device2_CreatePipelineState(dev2, &sd, &IID_ID3D12PipelineState, (void **)out);
    ID3D12Device2_Release(dev2);
    return hr;
}

static void *d3d12_create_pipeline(vio_pipeline_desc *desc)
{
    vio_d3d12_pipeline *pipeline = calloc(1, sizeof(vio_d3d12_pipeline));
    if (!pipeline) return NULL;

    vio_d3d12_shader *shader = (vio_d3d12_shader *)desc->shader;
    if (!shader) { free(pipeline); return NULL; }

    /* A hull stage only accepts control-point patch lists (PSO type PATCH). */
    vio_topology topo = shader->hs_blob ? VIO_PATCHES : desc->topology;
    pipeline->topology = vio_topology_to_d3d12(topo, desc->patch_vertices);
    pipeline->has_gs = shader->gs_blob != NULL;
    pipeline->writes_shading_rate = shader->writes_shading_rate;
    pipeline->uses_bindless = shader->uses_bindless;
    pipeline->uses_draw_params = shader->uses_draw_params;
    pipeline->uses_feedback = shader->uses_feedback;
    pipeline->has_hs = shader->hs_blob != NULL;
    pipeline->has_ds = shader->ds_blob != NULL;
    pipeline->is_mesh = shader->is_mesh;
    pipeline->as_blob = shader->as_blob;
    if (shader->is_mesh && !vio_d3d12.mesh_root_signature &&
        d3d12_build_root_signature(1, &vio_d3d12.mesh_root_signature) != 0) {
        vio_d3d12.mesh_root_signature = NULL;
        free(pipeline);
        return NULL;
    }

    /* Build input layout */
    D3D12_INPUT_ELEMENT_DESC *elements = NULL;
    char (*sem_names)[24] = NULL;
    UINT vertex_stride = 0;
    if (desc->vertex_attrib_count > 0 && desc->vertex_layout) {
        elements = calloc(desc->vertex_attrib_count, sizeof(D3D12_INPUT_ELEMENT_DESC));
        /* Matrix columns: SPIRV-Cross names them TEXCOORD{base}_{column}
         * (semantic "TEXCOORD3_" index 0..3), see the D3D11 twin. */
        sem_names = calloc(desc->vertex_attrib_count, sizeof(*sem_names));
        pipeline->input_locations = calloc(desc->vertex_attrib_count, sizeof(int));
        pipeline->input_count = desc->vertex_attrib_count;
        UINT vertex_offset = 0;
        for (int i = 0; i < desc->vertex_attrib_count; i++) {
            int loc = desc->vertex_layout[i].location;
            if (pipeline->input_locations) pipeline->input_locations[i] = loc;
            elements[i].SemanticName = vio_usage_to_semantic(desc->vertex_layout[i].usage);
            elements[i].SemanticIndex = loc;
            if (sem_names && desc->vertex_layout[i].matrix_columns > 1) {
                int base = loc - desc->vertex_layout[i].matrix_column;
                snprintf(sem_names[i], sizeof(sem_names[i]), "%s%d_", elements[i].SemanticName, base);
                elements[i].SemanticName = sem_names[i];
                elements[i].SemanticIndex = (UINT)desc->vertex_layout[i].matrix_column;
            }
            elements[i].Format = vio_format_to_dxgi(desc->vertex_layout[i].format);

            if (loc >= 3 && loc <= 6) {
                /* Per-instance attribute (mat4 columns) — InputSlot 1 */
                elements[i].InputSlot = 1;
                elements[i].AlignedByteOffset = (loc - 3) * 16;
                elements[i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
                elements[i].InstanceDataStepRate = 1;
            } else {
                /* Per-vertex attribute — InputSlot 0 */
                elements[i].InputSlot = 0;
                elements[i].AlignedByteOffset = vertex_offset;
                elements[i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                elements[i].InstanceDataStepRate = 0;
                vertex_offset += vio_format_byte_size(desc->vertex_layout[i].format);
            }
        }
        vertex_stride = vertex_offset;
    }
    pipeline->vertex_stride = vertex_stride;

    /* PSO description */
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {0};
    pso_desc.pRootSignature = shader->is_mesh ? vio_d3d12.mesh_root_signature : vio_d3d12.root_signature;

    /* Shaders */
    pso_desc.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(shader->vs_blob);
    pso_desc.VS.BytecodeLength = ID3D10Blob_GetBufferSize(shader->vs_blob);
    pso_desc.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(shader->ps_blob);
    pso_desc.PS.BytecodeLength = ID3D10Blob_GetBufferSize(shader->ps_blob);
    if (shader->gs_blob) {
        pso_desc.GS.pShaderBytecode = ID3D10Blob_GetBufferPointer(shader->gs_blob);
        pso_desc.GS.BytecodeLength = ID3D10Blob_GetBufferSize(shader->gs_blob);
    }
    /* The hull shader's InputPatch size has to match the draw's patch size;
     * vio_shader built it for layout(vertices = N), other sizes get a variant. */
    if (shader->tess_tcs && desc->patch_vertices > 0 && (uint32_t)desc->patch_vertices != shader->hs_input_points) {
        vio_tess_hlsl_desc td = { shader->tess_tcs, shader->tess_tcs_size, shader->tess_tes, shader->tess_tes_size,
                                  (uint32_t)desc->patch_vertices, d3d12_hlsl_target(), 0 };
        char *err = NULL;
        char *hlsl = vio_tess_to_hlsl(VIO_STAGE_TESS_CONTROL, &td, &err);
        if (!hlsl) php_error_docref(NULL, E_WARNING, "D3D12: hull shader for %d control points: %s", desc->patch_vertices, err ? err : "unknown");
        else if (FAILED(d3d12_compile_cached(hlsl, "HS", "hs_5_1", shader->compile_flags, &pipeline->hs_variant))) pipeline->hs_variant = NULL;
        free(hlsl);
        free(err);
    }
    ID3DBlob *hs_blob = pipeline->hs_variant ? pipeline->hs_variant : shader->hs_blob;
    if (hs_blob) {
        pso_desc.HS.pShaderBytecode = ID3D10Blob_GetBufferPointer(hs_blob);
        pso_desc.HS.BytecodeLength = ID3D10Blob_GetBufferSize(hs_blob);
    }
    if (shader->ds_blob) {
        pso_desc.DS.pShaderBytecode = ID3D10Blob_GetBufferPointer(shader->ds_blob);
        pso_desc.DS.BytecodeLength = ID3D10Blob_GetBufferSize(shader->ds_blob);
    }

    /* Input layout */
    pso_desc.InputLayout.pInputElementDescs = elements;
    pso_desc.InputLayout.NumElements = desc->vertex_attrib_count;

    /* Rasterizer */
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    switch (desc->cull_mode) {
        case VIO_CULL_NONE:  pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; break;
        case VIO_CULL_BACK:  pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK; break;
        case VIO_CULL_FRONT: pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT; break;
    }
    pso_desc.RasterizerState.FrontCounterClockwise = TRUE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.RasterizerState.DepthBias = (INT)desc->depth_bias;
    pso_desc.RasterizerState.SlopeScaledDepthBias = desc->slope_scaled_depth_bias;
    pso_desc.RasterizerState.DepthBiasClamp = 0.0f;

    /* Depth-stencil */
    pso_desc.DepthStencilState.DepthEnable = desc->depth_test ? TRUE : FALSE;
    pso_desc.DepthStencilState.DepthWriteMask = (desc->depth_test && desc->depth_write)
        ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pso_desc.DepthStencilState.DepthFunc = (desc->depth_func == VIO_DEPTH_LEQUAL)
        ? D3D12_COMPARISON_FUNC_LESS_EQUAL
        : D3D12_COMPARISON_FUNC_LESS;
    pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    /* Stencil (VIO_FEATURE_STENCIL): every depth attachment is D24S8. */
    if (desc->stencil_enable) {
        pso_desc.DepthStencilState.StencilEnable = TRUE;
        pso_desc.DepthStencilState.StencilReadMask = (UINT8)desc->stencil_read_mask;
        pso_desc.DepthStencilState.StencilWriteMask = (UINT8)desc->stencil_write_mask;
        pso_desc.DepthStencilState.FrontFace.StencilFunc = vio_d3d_compare_func_12(desc->stencil_func);
        pso_desc.DepthStencilState.FrontFace.StencilPassOp = vio_d3d_stencil_op_12(desc->stencil_pass_op);
        pso_desc.DepthStencilState.FrontFace.StencilFailOp = vio_d3d_stencil_op_12(desc->stencil_fail_op);
        pso_desc.DepthStencilState.FrontFace.StencilDepthFailOp = vio_d3d_stencil_op_12(desc->stencil_depth_fail_op);
        pso_desc.DepthStencilState.BackFace = pso_desc.DepthStencilState.FrontFace;
    }
    pipeline->stencil_ref = (UINT)desc->stencil_ref;

    /* Blend: one state for every colour attachment (the D3D default, IndependentBlend
     * off), or - with 'attachment_blend' / 'attachment_color_mask' - one per attachment so
     * an MRT transparent pass can alpha-blend colour while the data attachment stays. */
    if (desc->per_attachment) {
        pso_desc.BlendState.IndependentBlendEnable = TRUE;
        for (int ai = 0; ai < VIO_MAX_COLOR_ATTACHMENTS; ai++) {
            d3d12_fill_rt_blend(&pso_desc.BlendState.RenderTarget[ai], desc->attachment_blend[ai], desc->attachment_mask[ai]);
        }
    } else {
        d3d12_fill_rt_blend(&pso_desc.BlendState.RenderTarget[0], (int)desc->blend, desc->color_mask ? desc->color_mask : VIO_COLOR_RGBA);
    }

    /* Topology type */
    pso_desc.PrimitiveTopologyType = vio_topology_to_d3d12_type(topo);

    /* Render target format. Must match the format of the render target bound at
     * draw time (D3D12 hard rule), else DrawIndexedInstanced is dropped with
     * "render target format ... does not match that specified by the current
     * pipeline state". Default is R8G8B8A8_UNORM (swapchain + every LDR offscreen
     * target); desc->hdr_output selects R16G16B16A16_FLOAT to match a render
     * target created with hdr=true (e.g. the SSAO G-buffer). */
    if (desc->color_count > 0) {
        /* MRT: vio_pipeline(['attachments' => [...]]) declares the formats of
         * the target this pipeline draws into (must equal the RT's list). */
        int n = desc->color_count > VIO_MAX_COLOR_ATTACHMENTS ? VIO_MAX_COLOR_ATTACHMENTS : desc->color_count;
        pso_desc.NumRenderTargets = (UINT)n;
        for (int i = 0; i < n; i++) pso_desc.RTVFormats[i] = vio_pixel_format_to_dxgi(desc->color_formats[i]);
    } else {
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = desc->hdr_output
            ? DXGI_FORMAT_R16G16B16A16_FLOAT
            : DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    /* MSAA */
    pso_desc.SampleDesc.Count = 1;
    pso_desc.SampleMask = UINT_MAX;

    pipeline->view_count = desc->view_count > 1 ? desc->view_count : 0;
    HRESULT hr = d3d12_create_pso(&pso_desc, pipeline->view_count, pipeline->is_mesh ? pipeline : NULL, &pipeline->pso);
    if (FAILED(hr)) {
        d3d12_drain_info_queue("create_pso_fail");
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create PSO (0x%08lx)", hr);
        if (elements) free(elements);
        if (sem_names) free(sem_names);
        free(pipeline);
        return NULL;
    }
    /* Keep the description (and the arrays it points into) for the MSAA
     * variants; the shader blobs stay alive through the VioShader the PHP
     * pipeline object holds. */
    pipeline->pso_desc = pso_desc;
    pipeline->input_elements = elements;
    pipeline->sem_names = sem_names;

    return pipeline;
}

/* The PSO variant for the bound target: `samples` (1 = single-sample) and its
 * colour format (the swapchain may be RGB10A2 while the PSO was declared for
 * RGBA8, GAP-PHASE5 Block 6). Variants are created on first use and live as
 * long as the pipeline. Format variants only apply to single-target PSOs whose
 * declared format is the RGBA8 default (an explicit FP16 / MRT declaration is
 * kept as is). */
static ID3D12PipelineState *d3d12_pipeline_pso_for_target(vio_d3d12_pipeline *p, int samples, DXGI_FORMAT fmt)
{
    if (!p) return NULL;
    UINT want_samples = samples <= 1 ? 1u : (samples >= 8 ? 8u : (samples >= 4 ? 4u : 2u));
    DXGI_FORMAT want_fmt = p->pso_desc.RTVFormats[0];
    if (fmt != DXGI_FORMAT_UNKNOWN && p->pso_desc.NumRenderTargets == 1
        && p->pso_desc.RTVFormats[0] == DXGI_FORMAT_R8G8B8A8_UNORM && fmt != DXGI_FORMAT_R8G8B8A8_UNORM) {
        want_fmt = fmt;
    }
    /* The mesh's own attribute offsets (OPEN-ITEMS-PLAN A31). */
    const vio_mesh_layout *ml = &vio_d3d12.mesh_layout;
    uint32_t want_layout = (p->input_elements && p->input_locations && !p->is_mesh) ? ml->key : 0;
    if (want_samples == 1 && want_fmt == p->pso_desc.RTVFormats[0] && want_layout == 0) return p->pso;
    for (int i = 0; i < p->pso_variant_count; i++) {
        if (p->pso_variants[i].fmt == want_fmt && p->pso_variants[i].samples == want_samples
            && p->pso_variants[i].layout == want_layout) return p->pso_variants[i].pso;
    }
    if (p->pso_variant_count >= 16) return p->pso;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = p->pso_desc;
    d.SampleDesc.Count = want_samples;
    d.SampleDesc.Quality = 0;
    d.RTVFormats[0] = want_fmt;
    D3D12_INPUT_ELEMENT_DESC *moved = NULL;
    if (want_layout) {
        moved = malloc(sizeof(*moved) * (size_t)p->input_count);
        if (!moved) return p->pso;
        memcpy(moved, p->input_elements, sizeof(*moved) * (size_t)p->input_count);
        for (int i = 0; i < p->input_count; i++)
            if (moved[i].InputSlot == 0)
                moved[i].AlignedByteOffset = (UINT)vio_mesh_layout_offset(ml, p->input_locations[i], (int)moved[i].AlignedByteOffset);
        d.InputLayout.pInputElementDescs = moved;
    }
    ID3D12PipelineState *pso = NULL;
    HRESULT hr = d3d12_create_pso(&d, p->view_count, p->is_mesh ? p : NULL, &pso);
    free(moved);   /* the PSO keeps its own copy */
    if (FAILED(hr) || !pso) {
        d3d12_drain_info_queue("create_pso_variant_fail");
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create PSO variant (samples %u, format %d) (0x%08lx)", want_samples, (int)want_fmt, hr);
        return p->pso;
    }
    p->pso_variants[p->pso_variant_count].fmt = want_fmt;
    p->pso_variants[p->pso_variant_count].samples = want_samples;
    p->pso_variants[p->pso_variant_count].layout = want_layout;
    p->pso_variants[p->pso_variant_count].pso = pso;
    p->pso_variant_count++;
    return pso;
}

/* PSOs freed while a frame is recording may still be referenced by that
 * frame's command list. Park them in the slot of the frame being recorded and
 * release them when d3d12_begin_frame has waited for that slot's fence (i.e.
 * the GPU is provably done with the list that used them). */
#define VIO_D3D12_PENDING_PSO_MAX 64
static ID3D12PipelineState *d3d12_pending_pso[3][VIO_D3D12_PENDING_PSO_MAX];
static int                  d3d12_pending_pso_count[3];

static void d3d12_release_pending_psos(UINT slot)
{
    if (slot >= 3) return;
    for (int i = 0; i < d3d12_pending_pso_count[slot]; i++) {
        if (d3d12_pending_pso[slot][i]) ID3D12PipelineState_Release(d3d12_pending_pso[slot][i]);
        d3d12_pending_pso[slot][i] = NULL;
    }
    d3d12_pending_pso_count[slot] = 0;
}

static void d3d12_destroy_pipeline(void *pipeline_ptr)
{
    vio_d3d12_pipeline *p = (vio_d3d12_pipeline *)pipeline_ptr;
    if (!p) return;
    if (d3d12_current_pipeline == p) d3d12_current_pipeline = NULL;
    for (int v = -1; v < p->pso_variant_count; v++) {
        ID3D12PipelineState *pso = v < 0 ? p->pso : p->pso_variants[v].pso;
        if (!pso) continue;
        UINT slot = vio_d3d12.frame_index < 3 ? vio_d3d12.frame_index : 0;
        if (vio_d3d12.in_frame && d3d12_pending_pso_count[slot] < VIO_D3D12_PENDING_PSO_MAX) {
            d3d12_pending_pso[slot][d3d12_pending_pso_count[slot]++] = pso;
        } else {
            /* Between frames the last submitted frame may still run on the GPU
             * (a pipeline freed right after vio_end): release once everything
             * signalled so far has completed. The debug layer ends the process
             * on a PSO deleted while still in use. */
            if (vio_d3d12.device && vio_d3d12.fence) d3d12_retire_object_later((IUnknown *)pso, vio_d3d12.fence_value);
            else ID3D12PipelineState_Release(pso);   /* context gone: nothing in flight */
        }
    }
    if (p->input_elements) free(p->input_elements);
    free(p->input_locations);
    if (p->sem_names) free(p->sem_names);
    if (p->hs_variant) ID3D10Blob_Release(p->hs_variant);
    free(p);
}

/* Re-issue the bound pipeline's PSO for the sample count of the (new) bound
 * target — called after render-target binds / unbinds so "bind pipeline, then
 * bind target" orders pick the right variant too. */
/* Multiview: enable every view of the bound PSO (views 0..N-1 -> slices
 * 0..N-1). The mask is command-list state that a Reset clears, so every draw
 * re-arms it. */
/* Bindless table (vio_texture_index): root table [14] at the reserved block
 * for pipelines whose shader reads it. Root arguments do not survive
 * SetGraphicsRootSignature, so every draw re-points it (cheap). */
static void d3d12_apply_feedback(void);

static void d3d12_apply_bindless(void)
{
    d3d12_apply_feedback();   /* same call sites: root tables reset with the signature */
    vio_d3d12_pipeline *p = d3d12_current_pipeline;
    if (!p || !p->uses_bindless || !vio_d3d12.bindless || !vio_d3d12.cmd_list) return;
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(vio_d3d12.srv_heap.heap, &g);
    g.ptr += (UINT64)vio_d3d12.bindless_base * vio_d3d12.srv_heap.descriptor_size;
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(vio_d3d12.cmd_list, VIO_D3D12_RP_BINDLESS, g);
}

/* Index of the bindless block in the compute heap: behind the dispatch blocks. */
#define VIO_D3D12_COMPUTE_BINDLESS_BASE (VIO_D3D12_COMPUTE_MAX_BINDINGS * 2 * VIO_D3D12_COMPUTE_HEAP_BLOCKS)

/* Copy slot `slot` of the CPU mirror into the shader-visible blocks: the
 * graphics heap's and, once it exists, the compute heap's. */
static void d3d12_bindless_publish(int slot)
{
    UINT inc = vio_d3d12.srv_heap.descriptor_size;
    D3D12_CPU_DESCRIPTOR_HANDLE m, g;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.bindless_cpu_heap, &m);
    m.ptr += (SIZE_T)slot * inc;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.srv_heap.heap, &g);
    g.ptr += (SIZE_T)(vio_d3d12.bindless_base + (UINT)slot) * inc;
    ID3D12Device_CopyDescriptorsSimple(vio_d3d12.device, 1, g, m, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (vio_d3d12.compute_srv_heap) {
        D3D12_CPU_DESCRIPTOR_HANDLE c;
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.compute_srv_heap, &c);
        c.ptr += (SIZE_T)(VIO_D3D12_COMPUTE_BINDLESS_BASE + (UINT)slot) * vio_d3d12.compute_srv_descriptor_size;
        ID3D12Device_CopyDescriptorsSimple(vio_d3d12.device, 1, c, m, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
}

static int d3d12_bindless_set(int slot, void *backend_texture, int kind)
{
    if (!vio_d3d12.bindless || !vio_d3d12.bindless_cpu_heap || slot < 0 || slot >= VIO_BINDLESS_MAX) return -1;
    D3D12_CPU_DESCRIPTOR_HANDLE m;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.bindless_cpu_heap, &m);
    m.ptr += (SIZE_T)slot * vio_d3d12.srv_heap.descriptor_size;
    if (!backend_texture) {
        /* Released: back to the null SRV the table starts with. */
        D3D12_SHADER_RESOURCE_VIEW_DESC nd = {0};
        nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nd.Texture2D.MipLevels = 1;
        ID3D12Device_CreateShaderResourceView(vio_d3d12.device, NULL, &nd, m);
    } else if (kind == VIO_BINDLESS_KIND_CUBE) {
        vio_cubemap_object *cm = (vio_cubemap_object *)backend_texture;
        if (!cm->d3d12_srv_cpu) return -1;
        D3D12_CPU_DESCRIPTOR_HANDLE s = { (SIZE_T)cm->d3d12_srv_cpu };
        ID3D12Device_CopyDescriptorsSimple(vio_d3d12.device, 1, m, s, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    } else {
        vio_d3d12_texture *t = (vio_d3d12_texture *)backend_texture;
        if (!t->resource || !t->srv_cpu.ptr || t->depth > 0 || (t->layers > 1) != (kind == VIO_BINDLESS_KIND_ARRAY)) return -1;
        /* From the texture's staging (CPU-only) SRV. */
        ID3D12Device_CopyDescriptorsSimple(vio_d3d12.device, 1, m, t->srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    d3d12_bindless_publish(slot);
    return 0;
}

/* Direct draws: gl_BaseVertex / gl_BaseInstance are 0 (vio_draw has no base
 * vertex or first instance). Root arguments are lost with every root-signature
 * change and after ExecuteIndirect, so every draw site sets them. */
static void d3d12_apply_draw_params_zero(void)
{
    vio_d3d12_pipeline *p = d3d12_current_pipeline;
    if (!p || !p->uses_draw_params || p->is_mesh || !vio_d3d12.cmd_list) return;
    static const UINT zero[2] = { 0, 0 };
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(vio_d3d12.cmd_list, VIO_D3D12_RP_DRAW_PARAMS, 2, zero, 0);
}

static void d3d12_apply_view_mask(void)
{
    d3d12_apply_accel();   /* every draw site calls this helper first */
    d3d12_apply_draw_params_zero();
    vio_d3d12_pipeline *p = d3d12_current_pipeline;
    if (!p || p->view_count <= 1 || !vio_d3d12.cmd_list) return;
    if (!vio_d3d12.cmd_list1 &&
        FAILED(ID3D12GraphicsCommandList_QueryInterface(vio_d3d12.cmd_list, &IID_ID3D12GraphicsCommandList1, (void **)&vio_d3d12.cmd_list1)))
        vio_d3d12.cmd_list1 = NULL;
    if (vio_d3d12.cmd_list1) ID3D12GraphicsCommandList1_SetViewInstanceMask(vio_d3d12.cmd_list1, (1u << p->view_count) - 1u);
}

static ID3D12RootSignature *d3d12_graphics_root_signature(void)
{
    if (d3d12_current_pipeline && d3d12_current_pipeline->is_mesh && vio_d3d12.mesh_root_signature)
        return vio_d3d12.mesh_root_signature;
    return vio_d3d12.root_signature;
}

static void d3d12_rearm_pso_for_target(void)
{
    if (!d3d12_current_pipeline || !vio_d3d12.cmd_list || !vio_d3d12.in_frame) return;
    ID3D12PipelineState *pso = d3d12_pipeline_pso_for_target(d3d12_current_pipeline, vio_d3d12.current_rt_samples, vio_d3d12.current_rt_format);
    if (pso) ID3D12GraphicsCommandList_SetPipelineState(vio_d3d12.cmd_list, pso);
    d3d12_apply_view_mask();
    d3d12_apply_bindless();
}

/* The mesh about to be drawn: select the PSO variant with its attribute
 * offsets. Meshes without a declared layout keep the PSO untouched. */
static void d3d12_apply_mesh_layout(const vio_mesh_layout *ml)
{
    uint32_t key = ml ? ml->key : 0;
    uint32_t prev = vio_d3d12.applied_layout_key;
    if (ml) vio_d3d12.mesh_layout = *ml;
    else vio_d3d12.mesh_layout.key = 0;
    vio_d3d12.applied_layout_key = key;
    if (key == 0 && prev == 0) return;
    if (!d3d12_current_pipeline || !vio_d3d12.cmd_list || !vio_d3d12.in_frame) return;
    ID3D12PipelineState *pso = d3d12_pipeline_pso_for_target(d3d12_current_pipeline, vio_d3d12.current_rt_samples, vio_d3d12.current_rt_format);
    if (pso) ID3D12GraphicsCommandList_SetPipelineState(vio_d3d12.cmd_list, pso);
}

static void d3d12_bind_pipeline(void *pipeline_ptr)
{
    vio_d3d12_pipeline *p = (vio_d3d12_pipeline *)pipeline_ptr;
    if (!p) return;

    d3d12_current_pipeline = p;
    /* A bundle inherits the shading rate and cannot set the view mask; a
     * pipeline that needs either (or sampler feedback) is replayed instead. */
    if (d3d12_brec && (p->view_count > 1 || p->writes_shading_rate || p->uses_feedback || p->is_mesh)) d3d12_brec_fail();
    ID3D12GraphicsCommandList_SetPipelineState(vio_d3d12.cmd_list,
        d3d12_pipeline_pso_for_target(p, vio_d3d12.current_rt_samples, vio_d3d12.current_rt_format));
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(vio_d3d12.cmd_list,
                                                        d3d12_graphics_root_signature());
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(vio_d3d12.cmd_list, p->topology);
    ID3D12GraphicsCommandList_OMSetStencilRef(vio_d3d12.cmd_list, p->stencil_ref);
    d3d12_apply_view_mask();   /* also covers vio_draw_instanced's draw in php_vio.c */
    if (vio_d3d12.vrs_tier >= 2 && !d3d12_brec) d3d12_apply_shading_rate();   /* the combiner follows the pipeline */

    /* Bind SRV + sampler heaps. Also invalidates the cached root arguments for
     * params 2 / 4 (SetGraphicsRootSignature / SetDescriptorHeaps may reset
     * them), so the next flush rebuilds + re-points rather than reusing. */
    vio_d3d12_bind_graphics_heaps(vio_d3d12.cmd_list);
    d3d12_apply_bindless();   /* after the heaps: a table must point into the bound heap */
}

/* ── Sampler combos + graphics heap binding (GAP-PLAN Phase 1) ─────── */

int vio_d3d12_sampler_combo(int filter, int wrap, int anisotropy)
{
    int f = (filter == VIO_FILTER_NEAREST) ? 1 : 0;
    int w = (wrap == VIO_WRAP_CLAMP) ? 1 : (wrap == VIO_WRAP_MIRROR) ? 2 : 0;
    int a = 0;
    if (anisotropy >= 16)     a = 4;
    else if (anisotropy >= 8) a = 3;
    else if (anisotropy >= 4) a = 2;
    else if (anisotropy >= 2) a = 1;
    return (a * 3 + w) * 2 + f;
}

void vio_d3d12_bind_srv_slot(D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu, int slot, int sampler_index)
{
    if (slot < 0 || slot >= VIO_D3D12_SRV_TABLE_SIZE) return;
    vio_d3d12.pending_srvs[slot] = srv_cpu;
    vio_d3d12.pending_srv_valid[slot] = 1;
    if (slot < VIO_D3D12_SAMPLER_TABLE_SIZE) {
        if (sampler_index < 0 || sampler_index >= VIO_D3D12_SAMPLER_COMBOS) sampler_index = 0;
        vio_d3d12.pending_samplers[slot] = sampler_index;
    }
}

void vio_d3d12_bind_graphics_heaps(ID3D12GraphicsCommandList *list)
{
    if (!list) return;
    ID3D12DescriptorHeap *heaps[2];
    UINT n = 0;
    if (vio_d3d12.srv_heap.heap) heaps[n++] = vio_d3d12.srv_heap.heap;
    if (vio_d3d12.sampler_heap)  heaps[n++] = vio_d3d12.sampler_heap;
    if (n) ID3D12GraphicsCommandList_SetDescriptorHeaps(list, n, heaps);
    vio_d3d12.srv_table_bound = 0;
    vio_d3d12.sampler_table_bound = 0;
}

/* ── Resources: Buffers ───────────────────────────────────────────── */

static void *d3d12_create_buffer(vio_buffer_desc *desc)
{
    vio_d3d12_buffer *buf = calloc(1, sizeof(vio_d3d12_buffer));
    if (!buf) return NULL;

    buf->type = desc->type;
    buf->size = desc->size;
    buf->binding = desc->binding;
    buf->stride = desc->stride;

    D3D12_HEAP_PROPERTIES heap_props = {0};
    D3D12_RESOURCE_DESC res_desc = {0};
    D3D12_RESOURCE_STATES initial_state;

    res_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    res_desc.Width = desc->size;
    res_desc.Height = 1;
    res_desc.DepthOrArraySize = 1;
    res_desc.MipLevels = 1;
    res_desc.SampleDesc.Count = 1;
    res_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (desc->type == VIO_BUFFER_UNIFORM) {
        /* Uniform buffers: upload heap for frequent CPU writes */
        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
        initial_state = D3D12_RESOURCE_STATE_GENERIC_READ;
        /* CB size must be 256-byte aligned */
        res_desc.Width = (res_desc.Width + 255) & ~255;
    } else if (desc->type == VIO_BUFFER_STORAGE) {
        /* Compute storage: DEFAULT heap with the UAV flag in every case, so one
         * buffer is a valid kernel input (SRV) AND output (UAV), and CPU writes
         * (the 'data' seed, vio_update_buffer) go through a staging copy on the
         * upload queue. A seeded buffer used to live on an UPLOAD heap, which
         * forbids the UAV flag: binding it for writing removed the device.
         * Buffers have no layout, so the runtime creates them in COMMON; COMMON
         * is implicitly promoted to SRV / UAV / COPY_DEST / INDIRECT_ARGUMENT. */
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
        initial_state = D3D12_RESOURCE_STATE_COMMON;
        res_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        buf->default_heap = 1;
    } else if (desc->data) {
        /* Static vertex / index data (vio_mesh): GPU-local DEFAULT heap, filled
         * through a staging copy on the upload queue (GAP-PLAN 4.2). An UPLOAD
         * heap buffer is system memory read over PCIe on every draw on a
         * discrete GPU. Buffers are created in COMMON; the copy and the later
         * vertex/index reads promote implicitly, so no barriers are needed. */
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
        initial_state = D3D12_RESOURCE_STATE_COMMON;
        buf->default_heap = 1;
    } else {
        /* Vertex/index without initial data: upload heap (CPU-writable). */
        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
        initial_state = D3D12_RESOURCE_STATE_GENERIC_READ;
    }

    HRESULT hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device,
                                                       &heap_props,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &res_desc,
                                                       initial_state,
                                                       NULL,
                                                       &IID_ID3D12Resource,
                                                       (void **)&buf->resource);
    if (FAILED(hr)) {
        HRESULT removed = ID3D12Device_GetDeviceRemovedReason(vio_d3d12.device);
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create buffer (0x%08lx) size=%llu type=%d device_removed_reason=0x%08lx",
                         hr, (unsigned long long)desc->size, desc->type, removed);
        d3d12_drain_info_queue("create_buffer");
        free(buf);
        return NULL;
    }

    buf->gpu_address = ID3D12Resource_GetGPUVirtualAddress(buf->resource);

    /* Upload initial data */
    if (desc->data && buf->default_heap) {
        if (d3d12_upload_buffer_region(buf->resource, desc->data, desc->size) != 0) {
            ID3D12Resource_Release(buf->resource);
            free(buf);
            return NULL;
        }
    } else if (desc->data && heap_props.Type == D3D12_HEAP_TYPE_UPLOAD) {
        void *mapped = NULL;
        D3D12_RANGE read_range = {0, 0}; /* We don't read */
        hr = ID3D12Resource_Map(buf->resource, 0, &read_range, &mapped);
        if (SUCCEEDED(hr)) {
            memcpy(mapped, desc->data, desc->size);
            ID3D12Resource_Unmap(buf->resource, 0, NULL);
        }
    }

    return buf;
}

static void d3d12_update_buffer(void *buffer_ptr, const void *data, size_t size, size_t offset)
{
    vio_d3d12_buffer *buf = (vio_d3d12_buffer *)buffer_ptr;
    if (!buf || !buf->resource || !data || offset >= buf->size) return;
    if (size > buf->size - offset) size = buf->size - offset;
    buf->fs_dirty = 1;   /* the next read_buffer copies it again instead of an old readback */

    if (buf->default_heap) {
        /* GPU-local buffer: staging copy on the upload queue (ordered before
         * the next frame list on the same queue). */
        d3d12_upload_buffer_at(buf->resource, (UINT64)offset, data, size);
        return;
    }

    void *mapped = NULL;
    D3D12_RANGE read_range = {0, 0};
    HRESULT hr = ID3D12Resource_Map(buf->resource, 0, &read_range, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy((char *)mapped + offset, data, size);
        ID3D12Resource_Unmap(buf->resource, 0, NULL);
    }
}

static void d3d12_destroy_buffer(void *buffer_ptr)
{
    vio_d3d12_buffer *buf = (vio_d3d12_buffer *)buffer_ptr;
    if (!buf) return;
    if (buf->readback_resource) ID3D12Resource_Release(buf->readback_resource);
    if (buf->upload_resource) ID3D12Resource_Release(buf->upload_resource);
    if (buf->resource) ID3D12Resource_Release(buf->resource);
    free(buf);
}

/* ── Upload helpers ─────────────────────────────────────────────────
 *
 * Every texture-like upload (2D / 3D textures, cubemap faces, mip levels,
 * vio_texture_update regions) goes through d3d12_upload_subresources: one
 * UPLOAD-heap staging buffer laid out by GetCopyableFootprints, one
 * CopyTextureRegion per subresource, then the COPY_DEST -> state_after
 * barrier. Submission goes through d3d12_submit_upload(), which is the single
 * place the GAP-PLAN Phase 4.1 upload queue replaces the execute-and-wait. */

/* Upload queue (GAP-PLAN 4.1).
 *
 * Every resource upload used to create a fresh allocator + list, execute it
 * and vio_d3d12_wait_for_gpu() — a full pipeline drain per texture, cubemap or
 * render-target clear (a hundred textures at load time = a hundred drains, and
 * a mid-frame vio_texture_update drained the frame being built).
 *
 * Now a ring of VIO_D3D12_UPLOAD_ALLOCATORS allocators shares one command
 * list; a submit signals the shared fence and returns WITHOUT waiting. GPU
 * ordering is guaranteed by the single DIRECT queue: the upload list is
 * executed before any frame list submitted after it, so even a texture
 * created between vio_begin and vio_draw is complete when the draw runs.
 * Staging buffers go onto a retire list tagged with the signalled fence value
 * and are released in begin_frame once the fence has passed. An allocator is
 * only reset after its own previous submission has retired. */
static void d3d12_retire_uploads(int force)
{
    if (!vio_d3d12.upload_retire) return;
    UINT64 done = vio_d3d12.fence ? ID3D12Fence_GetCompletedValue(vio_d3d12.fence) : UINT64_MAX;
    int kept = 0;
    for (int i = 0; i < vio_d3d12.upload_retire_count; i++) {
        if (force || vio_d3d12.upload_retire[i].fence <= done) {
            IUnknown_Release(vio_d3d12.upload_retire[i].res);
        } else {
            vio_d3d12.upload_retire[kept++] = vio_d3d12.upload_retire[i];
        }
    }
    vio_d3d12.upload_retire_count = kept;
}

static void d3d12_retire_object_later(IUnknown *obj, UINT64 fence)
{
    if (!obj) return;
    if (vio_d3d12.upload_retire_count >= vio_d3d12.upload_retire_cap) {
        int cap = vio_d3d12.upload_retire_cap ? vio_d3d12.upload_retire_cap * 2 : 32;
        void *grown = realloc(vio_d3d12.upload_retire, (size_t)cap * sizeof(*vio_d3d12.upload_retire));
        if (!grown) {
            /* Out of memory for bookkeeping: fall back to the old stall. */
            vio_d3d12_wait_for_gpu();
            IUnknown_Release(obj);
            return;
        }
        vio_d3d12.upload_retire = grown;
        vio_d3d12.upload_retire_cap = cap;
    }
    vio_d3d12.upload_retire[vio_d3d12.upload_retire_count].res = obj;
    vio_d3d12.upload_retire[vio_d3d12.upload_retire_count].fence = fence;
    vio_d3d12.upload_retire_count++;
}

static void d3d12_retire_later(ID3D12Resource *res, UINT64 fence)
{
    d3d12_retire_object_later((IUnknown *)res, fence);
}

/* Record + submit an upload list. Returns 0 on success; the submission's
 * fence value is left in vio_d3d12.upload_last_fence for retire tagging. */
static int d3d12_submit_upload(void (*record)(ID3D12GraphicsCommandList *, void *), void *user)
{
    HRESULT hr;
    int slot = vio_d3d12.upload_alloc_idx;
    ID3D12CommandAllocator *alloc = vio_d3d12.upload_allocs[slot];
    if (!alloc) {
        hr = ID3D12Device_CreateCommandAllocator(vio_d3d12.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 &IID_ID3D12CommandAllocator, (void **)&alloc);
        if (FAILED(hr)) {
            php_error_docref(NULL, E_WARNING, "D3D12: upload allocator creation failed (0x%08lx)", hr);
            return -1;
        }
        vio_d3d12.upload_allocs[slot] = alloc;
    } else if (vio_d3d12.fence &&
               ID3D12Fence_GetCompletedValue(vio_d3d12.fence) < vio_d3d12.upload_alloc_fence[slot]) {
        /* This allocator's previous upload is still executing (more than
         * VIO_D3D12_UPLOAD_ALLOCATORS uploads in flight): wait for that one only. */
        ID3D12Fence_SetEventOnCompletion(vio_d3d12.fence, vio_d3d12.upload_alloc_fence[slot], vio_d3d12.fence_event);
        WaitForSingleObject(vio_d3d12.fence_event, INFINITE);
    }
    ID3D12CommandAllocator_Reset(alloc);

    if (!vio_d3d12.upload_list) {
        hr = ID3D12Device_CreateCommandList(vio_d3d12.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                                            &IID_ID3D12GraphicsCommandList, (void **)&vio_d3d12.upload_list);
        if (FAILED(hr)) {
            php_error_docref(NULL, E_WARNING, "D3D12: upload command list creation failed (0x%08lx)", hr);
            return -1;
        }
    } else {
        ID3D12GraphicsCommandList_Reset(vio_d3d12.upload_list, alloc, NULL);
    }

    record(vio_d3d12.upload_list, user);
    ID3D12GraphicsCommandList_Close(vio_d3d12.upload_list);
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)vio_d3d12.upload_list };
    ID3D12CommandQueue_ExecuteCommandLists(vio_d3d12.cmd_queue, 1, lists);

    vio_d3d12.fence_value++;
    ID3D12CommandQueue_Signal(vio_d3d12.cmd_queue, vio_d3d12.fence, vio_d3d12.fence_value);
    vio_d3d12.upload_alloc_fence[slot] = vio_d3d12.fence_value;
    vio_d3d12.upload_last_fence = vio_d3d12.fence_value;
    vio_d3d12.upload_alloc_idx = (slot + 1) % VIO_D3D12_UPLOAD_ALLOCATORS;

    /* Opportunistically free staging buffers whose uploads have completed. */
    d3d12_retire_uploads(0);
    return 0;
}

typedef struct _d3d12_upload_job {
    ID3D12Resource                     *dst;
    ID3D12Resource                     *staging;
    UINT                                first_sub;
    UINT                                num_sub;
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fp;
    D3D12_RESOURCE_STATES               state_before;
    D3D12_RESOURCE_STATES               state_after;
    /* Optional destination offset (vio_texture_update region uploads). */
    UINT                                dst_x, dst_y, dst_z;
} d3d12_upload_job;

static void d3d12_record_upload_job(ID3D12GraphicsCommandList *list, void *user)
{
    d3d12_upload_job *job = (d3d12_upload_job *)user;
    if (job->state_before != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = job->dst;
        b.Transition.StateBefore = job->state_before;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
    }
    for (UINT i = 0; i < job->num_sub; i++) {
        D3D12_TEXTURE_COPY_LOCATION dst_loc = {0};
        dst_loc.pResource = job->dst;
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_loc.SubresourceIndex = job->first_sub + i;
        D3D12_TEXTURE_COPY_LOCATION src_loc = {0};
        src_loc.pResource = job->staging;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_loc.PlacedFootprint = job->fp[i];
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst_loc, job->dst_x, job->dst_y, job->dst_z, &src_loc, NULL);
    }
    if (job->state_after != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = job->dst;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = job->state_after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
    }
}

/* Upload num_sub consecutive subresources of `dst` (described by `rd`, which
 * may be a sub-rectangle description for region updates — see
 * d3d12_update_texture). src[i] is tightly packed: src_row_pitch[i] bytes per
 * row, src_rows[i] rows per slice, src_slices[i] slices. The resource is
 * transitioned state_before -> COPY_DEST -> state_after. Returns 0 on success. */
static int d3d12_upload_subresources(ID3D12Resource *dst, const D3D12_RESOURCE_DESC *rd,
                                     UINT first_sub, UINT num_sub,
                                     const void *const *src, const UINT *src_row_pitch,
                                     const UINT *src_rows, const UINT *src_slices,
                                     D3D12_RESOURCE_STATES state_before,
                                     D3D12_RESOURCE_STATES state_after,
                                     UINT dst_x, UINT dst_y, UINT dst_z)
{
    if (!dst || num_sub == 0 || num_sub > 64) return -1;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[64];
    UINT   num_rows[64];
    UINT64 row_sizes[64];
    UINT64 total = 0;
    ID3D12Device_GetCopyableFootprints(vio_d3d12.device, rd, first_sub, num_sub, 0,
                                        fp, num_rows, row_sizes, &total);

    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource *staging = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&staging);
    if (FAILED(hr) || !staging) {
        php_error_docref(NULL, E_WARNING, "D3D12: upload staging buffer failed (0x%08lx)", hr);
        return -1;
    }

    void *mapped = NULL;
    D3D12_RANGE none = {0, 0};
    if (FAILED(ID3D12Resource_Map(staging, 0, &none, &mapped)) || !mapped) {
        ID3D12Resource_Release(staging);
        return -1;
    }
    for (UINT i = 0; i < num_sub; i++) {
        const uint8_t *s = (const uint8_t *)src[i];
        uint8_t *d = (uint8_t *)mapped + fp[i].Offset;
        UINT rows = src_rows[i] < num_rows[i] ? src_rows[i] : num_rows[i];
        UINT64 row_bytes = (UINT64)src_row_pitch[i] < row_sizes[i] ? src_row_pitch[i] : row_sizes[i];
        UINT64 dst_slice = (UINT64)fp[i].Footprint.RowPitch * num_rows[i];
        UINT64 src_slice = (UINT64)src_row_pitch[i] * src_rows[i];
        for (UINT z = 0; z < src_slices[i]; z++) {
            for (UINT y = 0; y < rows; y++) {
                memcpy(d + z * dst_slice + (UINT64)y * fp[i].Footprint.RowPitch,
                       s + z * src_slice + (UINT64)y * src_row_pitch[i], (size_t)row_bytes);
            }
        }
    }
    ID3D12Resource_Unmap(staging, 0, NULL);

    d3d12_upload_job job = {0};
    job.dst = dst; job.staging = staging;
    job.first_sub = first_sub; job.num_sub = num_sub; job.fp = fp;
    job.state_before = state_before; job.state_after = state_after;
    job.dst_x = dst_x; job.dst_y = dst_y; job.dst_z = dst_z;
    int rc = d3d12_submit_upload(d3d12_record_upload_job, &job);
    if (rc == 0) {
        /* The copy may still be executing: release the staging buffer once
         * the fence signalled for this submission has passed. */
        d3d12_retire_later(staging, vio_d3d12.upload_last_fence);
    } else {
        ID3D12Resource_Release(staging);
    }
    return rc;
}

/* Buffer upload through the queue: staging UPLOAD buffer + CopyBufferRegion
 * into a DEFAULT-heap buffer (COMMON state — the copy promotes it implicitly). */
typedef struct _d3d12_buffer_upload_job {
    ID3D12Resource *dst;
    ID3D12Resource *staging;
    UINT64          size;
    UINT64          dst_offset;
} d3d12_buffer_upload_job;

static void d3d12_record_buffer_upload(ID3D12GraphicsCommandList *list, void *user)
{
    d3d12_buffer_upload_job *job = (d3d12_buffer_upload_job *)user;
    ID3D12GraphicsCommandList_CopyBufferRegion(list, job->dst, job->dst_offset, job->staging, 0, job->size);
}

static int d3d12_upload_buffer_region(ID3D12Resource *dst, const void *data, size_t size)
{
    return d3d12_upload_buffer_at(dst, 0, data, size);
}

/* Staging copy of `data` into `dst` at byte `offset`, on the upload queue. */
static int d3d12_upload_buffer_at(ID3D12Resource *dst, UINT64 offset, const void *data, size_t size)
{
    if (!dst || !data || size == 0) return -1;
    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = size;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *staging = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&staging);
    if (FAILED(hr) || !staging) {
        php_error_docref(NULL, E_WARNING, "D3D12: buffer staging allocation failed (0x%08lx)", hr);
        return -1;
    }
    void *mapped = NULL;
    D3D12_RANGE none = {0, 0};
    if (FAILED(ID3D12Resource_Map(staging, 0, &none, &mapped)) || !mapped) {
        ID3D12Resource_Release(staging);
        return -1;
    }
    memcpy(mapped, data, size);
    ID3D12Resource_Unmap(staging, 0, NULL);

    d3d12_buffer_upload_job job = { dst, staging, (UINT64)size, offset };
    int rc = d3d12_submit_upload(d3d12_record_buffer_upload, &job);
    if (rc == 0) d3d12_retire_later(staging, vio_d3d12.upload_last_fence);
    else ID3D12Resource_Release(staging);
    return rc;
}

/* 2x2 box filter of an 8-bit `channels`-per-pixel image (odd sizes clamp the
 * last column / row). Output is max(w/2,1) x max(h/2,1). */
static void d3d12_box_downsample(const uint8_t *src, int w, int h, int channels, uint8_t *dst)
{
    int dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
    for (int y = 0; y < dh; y++) {
        int y0 = y * 2, y1 = (y0 + 1 < h) ? y0 + 1 : y0;
        for (int x = 0; x < dw; x++) {
            int x0 = x * 2, x1 = (x0 + 1 < w) ? x0 + 1 : x0;
            for (int c = 0; c < channels; c++) {
                int sum = src[(y0 * w + x0) * channels + c] + src[(y0 * w + x1) * channels + c]
                        + src[(y1 * w + x0) * channels + c] + src[(y1 * w + x1) * channels + c];
                dst[(y * dw + x) * channels + c] = (uint8_t)((sum + 2) / 4);
            }
        }
    }
}

static int d3d12_full_mip_count(int w, int h)
{
    int levels = 1;
    while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; levels++; }
    return levels;
}

/* Upload level 0 (+ a CPU-generated box-filtered chain when levels > 1) of a
 * 2D texture / one array slice. `sub_base` is the first subresource index of
 * the slice (slice * mip_levels). */
static int d3d12_upload_mip_chain(ID3D12Resource *res, const D3D12_RESOURCE_DESC *rd, UINT sub_base,
                                  const uint8_t *level0, int w, int h, int channels, int levels,
                                  D3D12_RESOURCE_STATES state_before, D3D12_RESOURCE_STATES state_after)
{
    if (levels < 1) levels = 1;
    if (levels > 16) levels = 16;
    const void *srcs[16]; UINT pitches[16], rows[16], slices[16];
    uint8_t *generated[16] = {0};
    int lw = w, lh = h;
    const uint8_t *prev = level0;
    for (int l = 0; l < levels; l++) {
        if (l > 0) {
            int nw = lw > 1 ? lw / 2 : 1, nh = lh > 1 ? lh / 2 : 1;
            generated[l] = (uint8_t *)malloc((size_t)nw * nh * channels);
            if (!generated[l]) { levels = l; break; }
            d3d12_box_downsample(prev, lw, lh, channels, generated[l]);
            prev = generated[l]; lw = nw; lh = nh;
        }
        srcs[l] = prev; pitches[l] = (UINT)lw * channels; rows[l] = (UINT)lh; slices[l] = 1;
    }
    int rc = d3d12_upload_subresources(res, rd, sub_base, (UINT)levels, srcs, pitches, rows, slices,
                                       state_before, state_after, 0, 0, 0);
    for (int l = 1; l < 16; l++) free(generated[l]);
    return rc;
}

/* ── Resources: Textures ──────────────────────────────────────────── */

/* Typed UAV load-free store support for a format: the precondition for creating
 * a mipmapped resource with ALLOW_UNORDERED_ACCESS so vio_generate_mipmaps can
 * downsample it on the GPU (GAP-PHASE5 Block 11). */
static int d3d12_format_supports_uav(DXGI_FORMAT fmt)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {0};
    fs.Format = fmt;
    if (!vio_d3d12.device) return 0;
    if (FAILED(ID3D12Device_CheckFeatureSupport(vio_d3d12.device, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs)))) return 0;
    return (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
           (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) ? 1 : 0;
}

static DXGI_FORMAT d3d12_texfmt(int fmt)
{
    switch (fmt) {
        case VIO_FORMAT_BC1: return DXGI_FORMAT_BC1_UNORM;
        case VIO_FORMAT_BC3: return DXGI_FORMAT_BC3_UNORM;
        case VIO_FORMAT_BC4: return DXGI_FORMAT_BC4_UNORM;
        case VIO_FORMAT_BC5: return DXGI_FORMAT_BC5_UNORM;
        case VIO_FORMAT_BC7: return DXGI_FORMAT_BC7_UNORM;
        case VIO_FORMAT_R8:  return DXGI_FORMAT_R8_UNORM;
        default:             return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

static int d3d12_generate_mips(ID3D12Resource *res, int slices, int levels, int w, int h, int channels,
                               D3D12_RESOURCE_STATES state);

/* Texture arrays / block-compressed data / explicit mip chains (GAP-PHASE5
 * Block 9). Subresource index = layer * mips + mip; the level-major payload is
 * scattered accordingly and uploaded in chunks of <= 64 subresources (the
 * resource stays COPY_DEST between chunks, the last one lands in
 * PIXEL_SHADER_RESOURCE). A single uncompressed level with `mipmaps` gets the
 * full chain through the compute downsample (CPU fallback inside). */
static void *d3d12_create_texture_ex(vio_texture_desc *desc)
{
    int layers = desc->layers > 1 ? desc->layers : 1;
    int levels = desc->mip_levels > 1 ? desc->mip_levels : 1;
    int compressed = vio_texfmt_is_compressed(desc->format);
    int gen = !compressed && desc->mipmaps && levels == 1;
    int mips = gen ? vio_texfmt_full_mip_count(desc->width, desc->height) : levels;
    if (!desc->data) return NULL;

    vio_d3d12_texture *tex = calloc(1, sizeof(vio_d3d12_texture));
    if (!tex) return NULL;
    tex->width = desc->width;
    tex->height = desc->height;
    tex->channels = vio_texfmt_channels(desc->format);
    tex->mip_levels = mips;
    tex->layers = layers;
    tex->compressed = compressed;
    tex->sampler_index = vio_d3d12_sampler_combo(desc->filter, desc->wrap, desc->anisotropy);

    D3D12_HEAP_PROPERTIES heap_props = {0};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC res_desc = {0};
    res_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    res_desc.Width = (UINT64)desc->width;
    res_desc.Height = (UINT)desc->height;
    res_desc.DepthOrArraySize = (UINT16)layers;
    res_desc.MipLevels = (UINT16)mips;
    res_desc.Format = d3d12_texfmt(desc->format);
    res_desc.SampleDesc.Count = 1;
    res_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (!compressed && mips > 1 && d3d12_format_supports_uav(res_desc.Format)) res_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HRESULT hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &heap_props, D3D12_HEAP_FLAG_NONE,
        &res_desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&tex->resource);
    if (FAILED(hr)) {
        free(tex);
        return NULL;
    }

    /* Scatter the level-major payload into subresource order. */
    int count = layers * levels;
    const void **srcs = calloc((size_t)count, sizeof(*srcs));
    UINT *pitches = calloc((size_t)count, sizeof(*pitches));
    UINT *rows = calloc((size_t)count, sizeof(*rows));
    UINT *slcs = calloc((size_t)count, sizeof(*slcs));
    if (!srcs || !pitches || !rows || !slcs) {
        free(srcs); free(pitches); free(rows); free(slcs);
        ID3D12Resource_Release(tex->resource);
        free(tex);
        return NULL;
    }
    {
        const uint8_t *p = (const uint8_t *)desc->data;
        int lw = desc->width, lh = desc->height;
        for (int l = 0; l < levels; l++) {
            UINT pitch = (UINT)vio_texfmt_row_pitch(desc->format, lw);
            UINT nrows = (UINT)vio_texfmt_rows(desc->format, lh);
            size_t image = vio_texfmt_image_size(desc->format, lw, lh);
            for (int a = 0; a < layers; a++) {
                int i = a * levels + l;
                srcs[i] = p; pitches[i] = pitch; rows[i] = nrows; slcs[i] = 1;
                p += image;
            }
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
        }
    }
    int rc = 0;
    if (gen) {
        /* Only mip 0 of every layer is present: one upload per layer. */
        for (int a = 0; a < layers && rc == 0; a++) {
            rc = d3d12_upload_subresources(tex->resource, &res_desc, (UINT)(a * mips), 1, &srcs[a], &pitches[a], &rows[a], &slcs[a],
                                           D3D12_RESOURCE_STATE_COPY_DEST,
                                           a == layers - 1 ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST,
                                           0, 0, 0);
        }
    } else {
        for (int first = 0; first < count && rc == 0; first += 64) {
            int n = count - first < 64 ? count - first : 64;
            rc = d3d12_upload_subresources(tex->resource, &res_desc, (UINT)first, (UINT)n, &srcs[first], &pitches[first], &rows[first], &slcs[first],
                                           D3D12_RESOURCE_STATE_COPY_DEST,
                                           first + n >= count ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST,
                                           0, 0, 0);
        }
    }
    free(srcs); free(pitches); free(rows); free(slcs);
    if (rc != 0) {
        ID3D12Resource_Release(tex->resource);
        free(tex);
        return NULL;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
    srv_desc.Format = res_desc.Format;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (layers > 1) {
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv_desc.Texture2DArray.MipLevels = (UINT)mips;
        srv_desc.Texture2DArray.ArraySize = (UINT)layers;
    } else {
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = (UINT)mips;
    }
    d3d12_alloc_srv_descriptor(&tex->srv_cpu, &tex->srv_gpu);
    ID3D12Device_CreateShaderResourceView(vio_d3d12.device, tex->resource, &srv_desc, tex->srv_cpu);

    if (gen && mips > 1) {
        d3d12_generate_mips(tex->resource, layers, mips, desc->width, desc->height, tex->channels,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    return tex;
}

static void *d3d12_create_texture(vio_texture_desc *desc)
{
    if (desc->layers > 1 || desc->mip_levels > 1 || vio_texfmt_is_compressed(desc->format)) {
        return d3d12_create_texture_ex(desc);
    }
    vio_d3d12_texture *tex = calloc(1, sizeof(vio_d3d12_texture));
    if (!tex) return NULL;

    tex->width = desc->width;
    tex->height = desc->height;
    tex->channels = desc->single_channel ? 1 : 4;
    tex->mip_levels = desc->mipmaps ? d3d12_full_mip_count(desc->width, desc->height) : 1;
    tex->sampler_index = vio_d3d12_sampler_combo(desc->filter, desc->wrap, desc->anisotropy);

    D3D12_HEAP_PROPERTIES heap_props = {0};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC res_desc = {0};
    res_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    res_desc.Width = desc->width;
    res_desc.Height = desc->height;
    res_desc.DepthOrArraySize = 1;
    res_desc.MipLevels = (UINT16)tex->mip_levels;
    res_desc.Format = desc->single_channel ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    res_desc.SampleDesc.Count = 1;
    res_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (desc->storage) res_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    /* Mip chains are regenerated on the GPU (compute downsample) when the
     * format takes typed UAV stores; otherwise the CPU box filter stays. */
    if (tex->mip_levels > 1 && d3d12_format_supports_uav(res_desc.Format)) res_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    /* Textures without initial data (storage images) start directly in the
     * sampling state the rest of the backend assumes; data uploads transition
     * COPY_DEST -> PIXEL_SHADER_RESOURCE inside the upload. */
    HRESULT hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &heap_props, D3D12_HEAP_FLAG_NONE,
        &res_desc, desc->data ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        NULL, &IID_ID3D12Resource, (void **)&tex->resource);
    if (FAILED(hr)) {
        free(tex);
        return NULL;
    }

    if (desc->data) {
        if (d3d12_upload_mip_chain(tex->resource, &res_desc, 0, (const uint8_t *)desc->data,
                                   desc->width, desc->height, tex->channels, tex->mip_levels,
                                   D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0) {
            ID3D12Resource_Release(tex->resource);
            free(tex);
            return NULL;
        }
    }

    /* Create SRV. For an R8 glyph atlas, swizzle the single channel so the
     * texture reads as (1,1,1,R) — white RGB, coverage in alpha — exactly what
     * the shared sprite shader expects, so no separate text shader is needed
     * (unlike D3D11, whose SRVs have no component mapping). */
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
    srv_desc.Format = res_desc.Format;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = desc->single_channel
        ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
              D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1,
              D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1,
              D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1,
              D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0)
        : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = (UINT)tex->mip_levels;

    d3d12_alloc_srv_descriptor(&tex->srv_cpu, &tex->srv_gpu);
    ID3D12Device_CreateShaderResourceView(vio_d3d12.device, tex->resource,
                                           &srv_desc, tex->srv_cpu);

    return tex;
}

/* Create a 3D / volume texture (Fieldtracing SDF). Mirrors d3d12_create_texture
 * but with a TEXTURE3D resource (one subresource whose footprint spans all
 * Depth slices) and a TEXTURE3D SRV. The bind path (d3d12_bind_texture) is
 * unchanged — it binds the SRV descriptor, which is dimension-agnostic. */
static void *d3d12_create_texture_3d(vio_texture_desc *desc)
{
    if (desc->depth <= 0) return NULL;

    vio_d3d12_texture *tex = calloc(1, sizeof(vio_d3d12_texture));
    if (!tex) return NULL;

    tex->width = desc->width;
    tex->height = desc->height;
    tex->depth = desc->depth;
    tex->channels = 4;
    tex->mip_levels = 1;
    tex->sampler_index = vio_d3d12_sampler_combo(desc->filter, desc->wrap, desc->anisotropy);

    D3D12_HEAP_PROPERTIES heap_props = {0};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC res_desc = {0};
    res_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    res_desc.Width = desc->width;
    res_desc.Height = desc->height;
    res_desc.DepthOrArraySize = (UINT16)desc->depth;
    res_desc.MipLevels = 1;
    res_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    res_desc.SampleDesc.Count = 1;
    res_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (desc->storage) res_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HRESULT hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &heap_props, D3D12_HEAP_FLAG_NONE,
        &res_desc, desc->data ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        NULL, &IID_ID3D12Resource, (void **)&tex->resource);
    if (FAILED(hr)) {
        free(tex);
        return NULL;
    }

    if (desc->data) {
        const void *src = desc->data;
        UINT pitch = (UINT)desc->width * 4, rows = (UINT)desc->height, slices = (UINT)desc->depth;
        if (d3d12_upload_subresources(tex->resource, &res_desc, 0, 1, &src, &pitch, &rows, &slices,
                                      D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                      0, 0, 0) != 0) {
            ID3D12Resource_Release(tex->resource);
            free(tex);
            return NULL;
        }
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture3D.MipLevels = 1;

    d3d12_alloc_srv_descriptor(&tex->srv_cpu, &tex->srv_gpu);
    ID3D12Device_CreateShaderResourceView(vio_d3d12.device, tex->resource,
                                           &srv_desc, tex->srv_cpu);

    return tex;
}

static void d3d12_destroy_texture(void *texture_ptr)
{
    vio_d3d12_texture *tex = (vio_d3d12_texture *)texture_ptr;
    if (!tex) return;
    if (vio_d3d12.fb_bound == tex) vio_d3d12.fb_bound = NULL;
    if (tex->fb_map) ID3D12Resource_Release(tex->fb_map);
    if (tex->fb_decoded) ID3D12Resource_Release(tex->fb_decoded);
    if (tex->upload_resource) ID3D12Resource_Release(tex->upload_resource);
    if (tex->resource) ID3D12Resource_Release(tex->resource);
    /* Note: descriptor in SRV heap is leaked (linear allocator doesn't support free).
     * A proper free-list allocator would reclaim the slot. */
    free(tex);
}

/* Object destructors invoked from Zend free_object handlers; mirror the
 * destroy_mesh / destroy_cubemap slots in the vtable. */

#include "../../vio_cubemap.h"
#include "../../vio_font.h"
#include "../../vio_render_target.h"
#include "../../vio_mesh.h"  /* vio_mesh_object — draw_instanced_from_storage reads mesh->backend_vb/stride/index_count */

/* Cubemap upload: a 6-slice Texture2D array (RGBA8), each face uploaded with
 * its CPU-generated mip chain when cm->mipmaps is set (textureLod usable), then
 * a TEXTURECUBE SRV in the staging heap (same allocator as 2D textures). Face
 * order +X,-X,+Y,-Y,+Z,-Z == slices 0..5. */
static int d3d12_upload_cubemap(void *cm_obj, int width, int height, const void *face_rgba[6])
{
    vio_cubemap_object *cm = (vio_cubemap_object *)cm_obj;
    if (!cm || !vio_d3d12.device || width <= 0 || height != width) return -1;

    int levels = cm->mipmaps ? d3d12_full_mip_count(width, height) : 1;

    D3D12_HEAP_PROPERTIES heap_props = {0};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC res_desc = {0};
    res_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    res_desc.Width = (UINT64)width;
    res_desc.Height = (UINT)height;
    res_desc.DepthOrArraySize = 6;
    res_desc.MipLevels = (UINT16)levels;
    res_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    res_desc.SampleDesc.Count = 1;
    res_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (levels > 1 && d3d12_format_supports_uav(res_desc.Format)) res_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;   /* GPU mip generation */

    ID3D12Resource *res = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &heap_props, D3D12_HEAP_FLAG_NONE,
        &res_desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&res);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: cubemap resource creation failed (0x%08lx)", hr);
        return -1;
    }

    /* One upload per face (each carries its own mip chain); the resource stays
     * in COPY_DEST between faces and moves to PIXEL_SHADER_RESOURCE with the
     * last one. */
    static const uint8_t black[4] = {0, 0, 0, 255};
    for (int f = 0; f < 6; f++) {
        const uint8_t *src = face_rgba[f] ? (const uint8_t *)face_rgba[f] : NULL;
        uint8_t *fallback = NULL;
        if (!src) {
            fallback = (uint8_t *)malloc((size_t)width * height * 4);
            if (!fallback) { ID3D12Resource_Release(res); return -1; }
            for (size_t i = 0; i < (size_t)width * height; i++) memcpy(fallback + i * 4, black, 4);
            src = fallback;
        }
        int rc = d3d12_upload_mip_chain(res, &res_desc, (UINT)(f * levels), src, width, height, 4, levels,
                                        D3D12_RESOURCE_STATE_COPY_DEST,
                                        f == 5 ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST);
        free(fallback);
        if (rc != 0) {
            ID3D12Resource_Release(res);
            php_error_docref(NULL, E_WARNING, "D3D12: cubemap face %d upload failed", f);
            return -1;
        }
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.TextureCube.MipLevels = (UINT)levels;
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu;
    d3d12_alloc_srv_descriptor(&srv_cpu, &srv_gpu);
    ID3D12Device_CreateShaderResourceView(vio_d3d12.device, res, &srv_desc, srv_cpu);

    cm->d3d12_resource = res;
    cm->d3d12_srv_cpu  = srv_cpu.ptr;
    cm->d3d12_srv_gpu  = srv_gpu.ptr;
    cm->resolution     = width;
    cm->mipmaps        = levels > 1;
    cm->backend_type   = 3;
    return 0;
}

/* ── Sampler feedback (VIO_FEATURE_SAMPLER_FEEDBACK) ─────────────────── */

/* The CPU handle in the shader-visible heap at the index of a staging handle
 * from d3d12_alloc_srv_descriptor (both heaps use the same indices). */
static D3D12_CPU_DESCRIPTOR_HANDLE d3d12_visible_cpu_of(D3D12_CPU_DESCRIPTOR_HANDLE staging)
{
    D3D12_CPU_DESCRIPTOR_HANDLE s0, v0;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.srv_staging_heap, &s0);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.srv_heap.heap, &v0);
    v0.ptr += staging.ptr - s0.ptr;
    return v0;
}

/* Create the texture's MinMip feedback map on first use. One region per
 * standard 64 KB tile of a 32-bit texture (128 x 128 texels), smaller for
 * small textures: the largest power of two <= half the shorter side, >= 4. */
static int d3d12_feedback_ensure(vio_d3d12_texture *t)
{
    if (t->fb_map) return 0;
    if (!vio_d3d12.sampler_feedback || !t->resource || t->depth > 0 || t->layers > 1 || t->width < 8 || t->height < 8)
        return -1;
    ID3D12Device8 *dev8 = NULL;
    if (FAILED(ID3D12Device_QueryInterface(vio_d3d12.device, &IID_ID3D12Device8, (void **)&dev8)) || !dev8) return -1;

    int shorter = t->width < t->height ? t->width : t->height;
    int region = 4;
    while (region < 128 && region * 2 <= shorter / 2) region *= 2;
    int rx = (t->width + region - 1) / region, ry = (t->height + region - 1) / region;

    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC1 d = {0};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = (UINT64)t->width;
    d.Height = (UINT)t->height;
    d.DepthOrArraySize = 1;
    d.MipLevels = (UINT16)(t->mip_levels > 0 ? t->mip_levels : 1);   /* must match the paired texture */
    d.Format = DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    d.SamplerFeedbackMipRegion.Width = (UINT)region;
    d.SamplerFeedbackMipRegion.Height = (UINT)region;
    d.SamplerFeedbackMipRegion.Depth = 1;
    ID3D12Resource *map = NULL;
    HRESULT hr = ID3D12Device8_CreateCommittedResource2(dev8, &hp, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, NULL, &IID_ID3D12Resource, (void **)&map);
    if (FAILED(hr) || !map) {
        php_error_docref(NULL, E_WARNING, "D3D12: sampler feedback map creation failed (0x%08lx)", hr);
        ID3D12Device8_Release(dev8);
        return -1;
    }

    /* Decode target: one R8_UINT texel per region (the lowest mip, 0xFF = none). */
    D3D12_RESOURCE_DESC dd = {0};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = (UINT64)rx;
    dd.Height = (UINT)ry;
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_R8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ID3D12Resource *decoded = NULL;
    hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &dd,
        D3D12_RESOURCE_STATE_RESOLVE_DEST, NULL, &IID_ID3D12Resource, (void **)&decoded);
    if (FAILED(hr) || !decoded) {
        php_error_docref(NULL, E_WARNING, "D3D12: sampler feedback decode target creation failed (0x%08lx)", hr);
        ID3D12Resource_Release(map);
        ID3D12Device8_Release(dev8);
        return -1;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    if (d3d12_alloc_srv_descriptor(&cpu, &gpu) == UINT_MAX) {
        ID3D12Resource_Release(decoded);
        ID3D12Resource_Release(map);
        ID3D12Device8_Release(dev8);
        return -1;
    }
    /* The staging copy is the CPU handle of ClearUnorderedAccessViewUint, the
     * shader-visible one is what root parameter [16] points at. */
    ID3D12Device8_CreateSamplerFeedbackUnorderedAccessView(dev8, t->resource, map, cpu);
    ID3D12Device8_CreateSamplerFeedbackUnorderedAccessView(dev8, t->resource, map, d3d12_visible_cpu_of(cpu));
    ID3D12Device8_Release(dev8);

    t->fb_map = map;
    t->fb_decoded = decoded;
    t->fb_uav_cpu = cpu;
    t->fb_uav_gpu = gpu;
    t->fb_region = region;
    t->fb_rx = rx;
    t->fb_ry = ry;
    return 0;
}

/* Null feedback UAV for a feedback shader drawn without a bound map: its
 * writes are dropped instead of hitting an unset root table. */
static int d3d12_feedback_null(D3D12_GPU_DESCRIPTOR_HANDLE *out)
{
    if (!vio_d3d12.fb_null_gpu.ptr) {
        /* D3D12 has no null feedback UAV: CreateSamplerFeedbackUnorderedAccessView
         * with NULL resources and a null UAV in the opaque feedback format are
         * both invalid calls that remove the device at the next submit. The
         * writes go to a private 8x8 texture / map pair instead. */
        ID3D12Device8 *dev8 = NULL;
        if (FAILED(ID3D12Device_QueryInterface(vio_d3d12.device, &IID_ID3D12Device8, (void **)&dev8)) || !dev8) return -1;
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 8;
        td.Height = 8;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        ID3D12Resource *tex = NULL, *map = NULL;
        HRESULT hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&tex);
        if (SUCCEEDED(hr) && tex) {
            D3D12_RESOURCE_DESC1 md = {0};
            md.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            md.Width = 8;
            md.Height = 8;
            md.DepthOrArraySize = 1;
            md.MipLevels = 1;
            md.Format = DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE;
            md.SampleDesc.Count = 1;
            md.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            md.SamplerFeedbackMipRegion.Width = 4;
            md.SamplerFeedbackMipRegion.Height = 4;
            md.SamplerFeedbackMipRegion.Depth = 1;
            hr = ID3D12Device8_CreateCommittedResource2(dev8, &hp, D3D12_HEAP_FLAG_NONE, &md,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, NULL, &IID_ID3D12Resource, (void **)&map);
        }
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        if (FAILED(hr) || !tex || !map || d3d12_alloc_srv_descriptor(&cpu, &gpu) == UINT_MAX) {
            if (map) ID3D12Resource_Release(map);
            if (tex) ID3D12Resource_Release(tex);
            ID3D12Device8_Release(dev8);
            return -1;
        }
        ID3D12Device8_CreateSamplerFeedbackUnorderedAccessView(dev8, tex, map, d3d12_visible_cpu_of(cpu));
        ID3D12Device8_Release(dev8);
        vio_d3d12.fb_null_tex = tex;
        vio_d3d12.fb_null_map = map;
        vio_d3d12.fb_null_gpu = gpu;
    }
    *out = vio_d3d12.fb_null_gpu;
    return 0;
}

/* Root parameter [16] for pipelines whose pixel stage writes feedback; runs
 * with d3d12_apply_bindless at every draw site and pipeline bind. */
static void d3d12_apply_feedback(void)
{
    vio_d3d12_pipeline *p = d3d12_current_pipeline;
    if (!p || !p->uses_feedback || !vio_d3d12.sampler_feedback || !vio_d3d12.cmd_list || !vio_d3d12.in_frame) return;
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    if (vio_d3d12.fb_bound && vio_d3d12.fb_bound->fb_map) g = vio_d3d12.fb_bound->fb_uav_gpu;
    else if (d3d12_feedback_null(&g) != 0) return;
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(vio_d3d12.cmd_list, VIO_D3D12_RP_FEEDBACK, g);
}

static int d3d12_sampler_feedback_bind(void *backend_texture)
{
    vio_d3d12_texture *t = (vio_d3d12_texture *)backend_texture;
    if (!vio_d3d12.sampler_feedback) return -1;
    if (t && d3d12_feedback_ensure(t) != 0) return -1;
    vio_d3d12.fb_bound = t;
    d3d12_apply_feedback();
    return 0;
}

static void d3d12_record_feedback_clear(ID3D12GraphicsCommandList *list, void *user)
{
    vio_d3d12_texture *t = (vio_d3d12_texture *)user;
    /* The GPU handle must lie in the heap bound on the list. */
    vio_d3d12_bind_graphics_heaps(list);
    static const UINT zero[4] = {0, 0, 0, 0};   /* opaque format: any value clears to "not sampled" */
    ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, t->fb_uav_gpu, t->fb_uav_cpu, t->fb_map, zero, 0, NULL);
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = t->fb_map;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

static int d3d12_sampler_feedback_clear(void *backend_texture)
{
    vio_d3d12_texture *t = (vio_d3d12_texture *)backend_texture;
    if (!t || d3d12_feedback_ensure(t) != 0) return -1;
    if (vio_d3d12.in_frame && vio_d3d12.cmd_list) {
        d3d12_record_feedback_clear(vio_d3d12.cmd_list, t);
        d3d12_apply_bindless();   /* the heap rebind dropped the root tables */
        return 0;
    }
    return d3d12_submit_upload(d3d12_record_feedback_clear, t);
}

/* MinMip map -> R8_UINT decode target (ResolveSubresourceRegion with
 * DECODE_SAMPLER_FEEDBACK; all mips of the map are subresource UINT_MAX). */
static void d3d12_record_feedback_resolve(ID3D12GraphicsCommandList *list, void *user)
{
    vio_d3d12_texture *t = (vio_d3d12_texture *)user;
    ID3D12GraphicsCommandList1 *list1 = NULL;
    if (FAILED(ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList1, (void **)&list1)) || !list1)
        return;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t->fb_map;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
    ID3D12GraphicsCommandList1_ResolveSubresourceRegion(list1, t->fb_decoded, 0, 0, 0, t->fb_map, UINT_MAX, NULL,
                                                        DXGI_FORMAT_R8_UINT, D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
    ID3D12GraphicsCommandList1_Release(list1);
}

static unsigned char *d3d12_readback_subresource(ID3D12Resource *src, UINT subresource,
                                                 D3D12_RESOURCE_STATES state, UINT *out_pitch);

static int d3d12_sampler_feedback_read(void *backend_texture, unsigned char **out,
                                       int *regions_x, int *regions_y, int *region_px)
{
    vio_d3d12_texture *t = (vio_d3d12_texture *)backend_texture;
    *out = NULL;
    if (!t || !t->fb_map) return -1;
    if (vio_d3d12.in_frame && vio_d3d12.cmd_list) d3d12_record_feedback_resolve(vio_d3d12.cmd_list, t);
    else if (d3d12_submit_upload(d3d12_record_feedback_resolve, t) != 0) return -1;
    /* Same queue: the copy below runs after the resolve (mid-frame on the
     * frame list, which the helper executes and reopens). */
    UINT pitch = 0;
    unsigned char *raw = d3d12_readback_subresource(t->fb_decoded, 0, D3D12_RESOURCE_STATE_RESOLVE_DEST, &pitch);
    if (!raw) return -1;
    unsigned char *mips = (unsigned char *)malloc((size_t)t->fb_rx * t->fb_ry);
    if (!mips) { free(raw); return -1; }
    for (int y = 0; y < t->fb_ry; y++) memcpy(mips + (size_t)y * t->fb_rx, raw + (size_t)y * pitch, (size_t)t->fb_rx);
    free(raw);
    *out = mips;
    *regions_x = t->fb_rx;
    *regions_y = t->fb_ry;
    *region_px = t->fb_region;
    return 0;
}

/* Copy one subresource of a texture into CPU memory (row pitch = out_pitch).
 * Mid-frame the copy is recorded on the live frame list which is then
 * executed, waited on and reopened (the vio_d3d12_capture_frame pattern);
 * otherwise a transient list is used. The resource is returned to `state`. */
static unsigned char *d3d12_readback_subresource(ID3D12Resource *src, UINT subresource,
                                                 D3D12_RESOURCE_STATES state, UINT *out_pitch)
{
    D3D12_RESOURCE_DESC sd;
    ID3D12Resource_GetDesc(src, &sd);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {0};
    UINT64 total = 0;
    ID3D12Device_GetCopyableFootprints(vio_d3d12.device, &sd, subresource, 1, 0, &fp, NULL, NULL, &total);

    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb = {0};
    rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb.Width = total; rb.Height = 1; rb.DepthOrArraySize = 1; rb.MipLevels = 1;
    rb.SampleDesc.Count = 1;
    rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *readback = NULL;
    if (FAILED(ID3D12Device_CreateCommittedResource(vio_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &rb,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&readback)) || !readback) {
        return NULL;
    }

    D3D12_TEXTURE_COPY_LOCATION src_loc = {0};
    src_loc.pResource = src;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = subresource;
    D3D12_TEXTURE_COPY_LOCATION dst_loc = {0};
    dst_loc.pResource = readback;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint = fp;
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = src;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    if (vio_d3d12.in_frame && vio_d3d12.cmd_list) {
        ID3D12GraphicsCommandList *list = vio_d3d12.cmd_list;
        barrier.Transition.StateBefore = state;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst_loc, 0, 0, 0, &src_loc, NULL);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter  = state;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
        /* Execute + wait + reopen with the bound target / pipeline re-armed. */
        vio_d3d12.compute_async_pending = 1;
        d3d12_compute_wait();
    } else {
        ID3D12CommandAllocator *alloc = NULL;
        ID3D12GraphicsCommandList *list = NULL;
        HRESULT hr = ID3D12Device_CreateCommandAllocator(vio_d3d12.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         &IID_ID3D12CommandAllocator, (void **)&alloc);
        if (SUCCEEDED(hr)) {
            hr = ID3D12Device_CreateCommandList(vio_d3d12.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                                                &IID_ID3D12GraphicsCommandList, (void **)&list);
        }
        if (FAILED(hr)) {
            if (alloc) ID3D12CommandAllocator_Release(alloc);
            ID3D12Resource_Release(readback);
            return NULL;
        }
        barrier.Transition.StateBefore = state;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst_loc, 0, 0, 0, &src_loc, NULL);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter  = state;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
        ID3D12GraphicsCommandList_Close(list);
        ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
        ID3D12CommandQueue_ExecuteCommandLists(vio_d3d12.cmd_queue, 1, lists);
        vio_d3d12_wait_for_gpu();
        ID3D12GraphicsCommandList_Release(list);
        ID3D12CommandAllocator_Release(alloc);
    }

    D3D12_RANGE rr = {0, (SIZE_T)total};
    void *mapped = NULL;
    if (FAILED(ID3D12Resource_Map(readback, 0, &rr, &mapped)) || !mapped) {
        ID3D12Resource_Release(readback);
        return NULL;
    }
    unsigned char *out = (unsigned char *)malloc((size_t)total);
    if (out) memcpy(out, mapped, (size_t)total);
    ID3D12Resource_Unmap(readback, 0, NULL);
    ID3D12Resource_Release(readback);
    if (out_pitch) *out_pitch = fp.Footprint.RowPitch;
    return out;
}

static int d3d12_rt_mips(const vio_render_target_object *rt);

/* vio_read_render_target on D3D12: colour attachment (any vio_pixel_format,
 * converted to RGBA8 by the shared converter) or the depth buffer
 * (R24G8_TYPELESS -> grey ramp). Cube targets are not available on D3D12. */
static int d3d12_read_render_target(void *rt_ptr, int face, int attachment, void *out_rgba)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!rt || rt->backend_type != VIO_RT_BACKEND_D3D12 || !vio_d3d12.device) return -1;
    int n = rt->attachment_count > 0 ? rt->attachment_count : 1;
    if (attachment < 0 || attachment >= n) return -1;
    /* Cube / array targets: subresource = layer * mip_levels (mip 0 of that
     * slice); the depth resource has a single level. */
    UINT subresource = 0;
    if (rt->is_cube || rt->layers > 1) {
        int f = face >= 0 ? face : (rt->bound_face >= 0 ? rt->bound_face : 0);
        subresource = (UINT)(f * (rt->depth_only ? 1 : d3d12_rt_mips(rt)));
    }

    ID3D12Resource *src;
    D3D12_RESOURCE_STATES state;
    if (rt->depth_only) {
        src = (ID3D12Resource *)rt->d3d12_depth_resource;
        state = rt->d3d12_depth_is_srv ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_DEPTH_WRITE;
    } else {
        src = attachment == 0 ? (ID3D12Resource *)rt->d3d12_color_resource
                              : (ID3D12Resource *)rt->d3d12_color_resources[attachment];
        state = rt->d3d12_color_is_srv ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (!src) return -1;

    UINT pitch = 0;
    unsigned char *raw = d3d12_readback_subresource(src, subresource, state, &pitch);
    if (!raw) return -1;
    int w = rt->width, h = rt->height;
    unsigned char *out = (unsigned char *)out_rgba;
    if (rt->depth_only) {
        for (int y = 0; y < h; y++) {
            const unsigned char *row = raw + (size_t)y * pitch;
            unsigned char *dst = out + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) {
                uint32_t v; memcpy(&v, row + x * 4, 4); v &= 0x00FFFFFFu;
                unsigned char g = (unsigned char)((v * 255ULL) / 0x00FFFFFFu);
                dst[x*4+0] = dst[x*4+1] = dst[x*4+2] = g; dst[x*4+3] = 255;
            }
        }
    } else {
        vio_rt_convert_to_rgba8(rt->formats[attachment], 0, raw, (size_t)pitch, w, h, out);
    }
    free(raw);
    return 0;
}

static void d3d12_destroy_cubemap(void *cm_ptr)
{
    vio_cubemap_object *cm = (vio_cubemap_object *)cm_ptr;
    /* vio_render_target_cubemap wrapper: the RT owns the resource. */
    if (cm->borrowed) {
        cm->d3d12_resource = NULL;
        return;
    }
    if (cm->d3d12_resource) {
        ID3D12Resource_Release((ID3D12Resource *)cm->d3d12_resource);
        cm->d3d12_resource = NULL;
    }
}

static void d3d12_destroy_font_atlas(void *font_ptr)
{
    vio_font_object *font = (vio_font_object *)font_ptr;
    if (font->atlas_backend_texture) {
        d3d12_destroy_texture(font->atlas_backend_texture);
        font->atlas_backend_texture = NULL;
    }
}

static void d3d12_destroy_render_target(void *rt_ptr)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (rt->backend_type != VIO_RT_BACKEND_D3D12) return;

    /* Drop any dangling references to this target so a later bind/unbind/begin
     * can't dereference freed memory if the RT is destroyed while still tracked
     * (e.g. released without an intervening in-frame unbind). */
    if (vio_d3d12.current_bound_rt == rt) vio_d3d12.current_bound_rt = NULL;
    if (vio_d3d12.pending_bound_rt == rt) vio_d3d12.pending_bound_rt = NULL;

    /* Ensure the GPU is finished with these resources before releasing them.
     * An offscreen target rendered to in a present-skipped frame (warm-render
     * path) has no Present to implicitly throttle the CPU, so its command list
     * may still be in flight here — releasing now would be a use-after-free
     * (device-removed / crash). wait_for_gpu() is a no-op once the queue/fence
     * are gone (shutdown), and destroy is rare, so the stall is harmless. */
    vio_d3d12_wait_for_gpu();

    /* Cached backend-texture wrappers — descriptors are borrowed from the
     * RT's heap, so we just free the wrapper struct itself. */
    for (int i = 0; i < 2; i++) {
        vio_d3d12_texture **slot = i == 0
            ? (vio_d3d12_texture **)&rt->d3d12_color_backend_texture
            : (vio_d3d12_texture **)&rt->d3d12_depth_backend_texture;
        if (*slot) { free(*slot); *slot = NULL; }
    }
    /* MRT attachments 1..n (index 0 is the scalar below). */
    for (int i = 1; i < VIO_MAX_COLOR_ATTACHMENTS; i++) {
        if (rt->d3d12_color_backend_textures[i]) { free(rt->d3d12_color_backend_textures[i]); rt->d3d12_color_backend_textures[i] = NULL; }
        if (rt->d3d12_color_resources[i]) {
            ID3D12Resource_Release((ID3D12Resource *)rt->d3d12_color_resources[i]);
            rt->d3d12_color_resources[i] = NULL;
        }
    }
    rt->d3d12_color_backend_textures[0] = NULL;
    rt->d3d12_color_resources[0] = NULL;
    for (int i = 0; i < VIO_MAX_COLOR_ATTACHMENTS; i++) {
        if (rt->d3d12_msaa_color_resources[i]) {
            ID3D12Resource_Release((ID3D12Resource *)rt->d3d12_msaa_color_resources[i]);
            rt->d3d12_msaa_color_resources[i] = NULL;
        }
    }
    if (rt->d3d12_msaa_depth_resource) { ID3D12Resource_Release((ID3D12Resource *)rt->d3d12_msaa_depth_resource); rt->d3d12_msaa_depth_resource = NULL; }
    if (rt->d3d12_msaa_rtv_heap) { ID3D12DescriptorHeap_Release((ID3D12DescriptorHeap *)rt->d3d12_msaa_rtv_heap); rt->d3d12_msaa_rtv_heap = NULL; }
    if (rt->d3d12_msaa_dsv_heap) { ID3D12DescriptorHeap_Release((ID3D12DescriptorHeap *)rt->d3d12_msaa_dsv_heap); rt->d3d12_msaa_dsv_heap = NULL; }
    rt->d3d12_msaa_layered = 0;
    rt->d3d12_msaa_depth_only = 0;
    if (rt->d3d12_color_resource) {
        ID3D12Resource_Release((ID3D12Resource *)rt->d3d12_color_resource);
        rt->d3d12_color_resource = NULL;
    }
    if (rt->d3d12_depth_resource) {
        ID3D12Resource_Release((ID3D12Resource *)rt->d3d12_depth_resource);
        rt->d3d12_depth_resource = NULL;
    }
    if (rt->d3d12_rtv_heap) {
        ID3D12DescriptorHeap_Release((ID3D12DescriptorHeap *)rt->d3d12_rtv_heap);
        rt->d3d12_rtv_heap = NULL;
    }
    if (rt->d3d12_dsv_heap) {
        ID3D12DescriptorHeap_Release((ID3D12DescriptorHeap *)rt->d3d12_dsv_heap);
        rt->d3d12_dsv_heap = NULL;
    }
}

/* ── Render targets (GAP-PLAN Phase 2: moved out of php_vio.c) ──────
 *
 * Everything here records onto vio_d3d12.cmd_list, which only accepts commands
 * between d3d12_begin_frame()'s Reset() and d3d12_end_frame()'s Close(). A bind
 * requested while the list is closed (before vio_begin) is deferred in
 * pending_bound_rt and applied by vio_begin() through
 * vio_d3d12_apply_pending_render_target(). */

static void d3d12_rt_set_viewport_scissor(int w, int h)
{
    D3D12_VIEWPORT vp = {0};
    vp.Width = (float)w;
    vp.Height = (float)h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D12GraphicsCommandList_RSSetViewports(vio_d3d12.cmd_list, 1, &vp);
    D3D12_RECT scissor = {0, 0, w, h};
    ID3D12GraphicsCommandList_RSSetScissorRects(vio_d3d12.cmd_list, 1, &scissor);
}

static void d3d12_rt_barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *res,
                             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

static int d3d12_rt_mips(const vio_render_target_object *rt)
{
    return rt->is_cube && rt->mip_levels > 0 ? rt->mip_levels : 1;
}

/* Resolve a multisampled target into its single-sample resolve resources so
 * the SRVs / readback see the final image (D3D11 twin: d3d11_rt_resolve_msaa).
 * Afterwards the resolve resources sit in PIXEL_SHADER_RESOURCE (color_is_srv)
 * and the multisampled ones are back in RENDER_TARGET for the next bind. */
static int d3d12_resolve_depth_msaa(vio_render_target_object *rt);

static void d3d12_rt_resolve_msaa(vio_render_target_object *rt)
{
    if (rt && rt->d3d12_msaa_depth_only) {   /* depth_only MSAA (A24) */
        if (rt->d3d12_msaa_dirty && vio_d3d12.cmd_list) {
            d3d12_resolve_depth_msaa(rt);
            rt->d3d12_msaa_dirty = 0;
        }
        return;
    }
    if (rt && rt->d3d12_msaa_layered && vio_d3d12.cmd_list) {
        /* Cube / array (A24): the drawn layer(s) into mip 0 of the face; the whole
         * single-sample resource ends readable (one state flag covers it). */
        ID3D12Resource *dst = (ID3D12Resource *)rt->d3d12_color_resource;
        ID3D12Resource *ms = (ID3D12Resource *)rt->d3d12_msaa_color_resources[0];
        if (!dst || !ms) return;
        D3D12_RESOURCE_STATES cur = rt->d3d12_color_is_srv ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (rt->d3d12_msaa_dirty) {
            int layers = vio_rt_layer_count(rt), mips = d3d12_rt_mips(rt);
            int first = rt->d3d12_msaa_layer < 0 ? 0 : rt->d3d12_msaa_layer;
            int last = rt->d3d12_msaa_layer < 0 ? layers - 1 : rt->d3d12_msaa_layer;
            d3d12_rt_barrier(vio_d3d12.cmd_list, ms, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            d3d12_rt_barrier(vio_d3d12.cmd_list, dst, cur, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            for (int l = first; l <= last; l++)
                ID3D12GraphicsCommandList_ResolveSubresource(vio_d3d12.cmd_list, dst, (UINT)(l * mips), ms, (UINT)l,
                                                             vio_pixel_format_to_dxgi(rt->formats[0]));
            d3d12_rt_barrier(vio_d3d12.cmd_list, ms, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            d3d12_rt_barrier(vio_d3d12.cmd_list, dst, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            rt->d3d12_msaa_dirty = 0;
        } else if (!rt->d3d12_color_is_srv) {
            d3d12_rt_barrier(vio_d3d12.cmd_list, dst, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        rt->d3d12_color_is_srv = 1;
        return;
    }
    if (!rt || !rt->d3d12_msaa_color_resources[0] || !rt->d3d12_msaa_dirty || !vio_d3d12.cmd_list) return;
    int n = rt->attachment_count > 0 ? rt->attachment_count : 1;
    if (n > VIO_MAX_COLOR_ATTACHMENTS) n = VIO_MAX_COLOR_ATTACHMENTS;
    for (int ai = 0; ai < n; ai++) {
        ID3D12Resource *ms  = (ID3D12Resource *)rt->d3d12_msaa_color_resources[ai];
        ID3D12Resource *dst = ai == 0 ? (ID3D12Resource *)rt->d3d12_color_resource
                                      : (ID3D12Resource *)rt->d3d12_color_resources[ai];
        if (!ms || !dst) continue;
        DXGI_FORMAT fmt = vio_pixel_format_to_dxgi(rt->formats[ai]);
        d3d12_rt_barrier(vio_d3d12.cmd_list, ms, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        d3d12_rt_barrier(vio_d3d12.cmd_list, dst,
                         rt->d3d12_color_is_srv ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET,
                         D3D12_RESOURCE_STATE_RESOLVE_DEST);
        ID3D12GraphicsCommandList_ResolveSubresource(vio_d3d12.cmd_list, dst, 0, ms, 0, fmt);
        d3d12_rt_barrier(vio_d3d12.cmd_list, dst, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        d3d12_rt_barrier(vio_d3d12.cmd_list, ms, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    rt->d3d12_color_is_srv = 1;
    rt->d3d12_msaa_dirty = 0;
}

/* Record the commands that make `rt` the active target (colour attachment
 * face/level for cube targets). Caller guarantees vio_d3d12.in_frame. */
static void d3d12_record_bind_render_target(vio_render_target_object *rt, int face, int level)
{
    /* Transition the OUTGOING target's depth back to a samplable state before
     * binding the new one, so a chain of depth-only binds (CSM cascades) with a
     * single unbind at the end leaves every cascade readable. */
    if (vio_d3d12.current_bound_rt && vio_d3d12.current_bound_rt != rt) {
        vio_render_target_object *prev = (vio_render_target_object *)vio_d3d12.current_bound_rt;
        if (prev->d3d12_depth_resource && prev->depth_only && !prev->d3d12_depth_is_srv) {
            d3d12_rt_barrier(vio_d3d12.cmd_list, (ID3D12Resource *)prev->d3d12_depth_resource,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            prev->d3d12_depth_is_srv = 1;
        }
        /* An outgoing multisampled target replaced without an unbind still gets
         * its resolve, so sampling it later sees what was drawn. */
        d3d12_rt_resolve_msaa(prev);
    }

    /* Colour attachments used as SRVs since the last bind go back to RENDER_TARGET
     * (one flag covers the whole MRT set / every cube face). MSAA targets render
     * into their multisampled resources, which always stay RENDER_TARGET; the
     * resolve targets keep their SRV state until the next resolve. */
    if (rt->d3d12_msaa_layered) {
        /* handled per face below */
    } else if (rt->d3d12_msaa_color_resources[0]) {
        rt->d3d12_msaa_dirty = 1;
    } else if (rt->d3d12_color_resource && rt->d3d12_color_is_srv) {
        int n = rt->attachment_count > 0 ? rt->attachment_count : 1;
        for (int ai = 0; ai < n && ai < VIO_MAX_COLOR_ATTACHMENTS; ai++) {
            ID3D12Resource *res = ai == 0 ? (ID3D12Resource *)rt->d3d12_color_resource
                                          : (ID3D12Resource *)rt->d3d12_color_resources[ai];
            if (res) d3d12_rt_barrier(vio_d3d12.cmd_list, res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        rt->d3d12_color_is_srv = 0;
    }
    if (rt->d3d12_depth_resource && rt->d3d12_depth_is_srv) {
        d3d12_rt_barrier(vio_d3d12.cmd_list, (ID3D12Resource *)rt->d3d12_depth_resource,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        rt->d3d12_depth_is_srv = 0;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart((ID3D12DescriptorHeap *)rt->d3d12_dsv_heap, &dsv_handle);
    int w = rt->width, h = rt->height;
    int layers = vio_rt_layer_count(rt);
    int all = layers > 1 && face == VIO_RT_ALL_LAYERS;
    if (layers > 1) {
        /* Cube / array: one DSV per layer (heap entry = layer); entry `layers`
         * spans every slice (VIO_RT_ALL_LAYERS, SV_RenderTargetArrayIndex picks). */
        if (!all && (face < 0 || face >= layers)) face = 0;
        dsv_handle.ptr += (SIZE_T)(all ? layers : face) * vio_d3d12.dsv_descriptor_size;
    }

    if (rt->depth_only) {
        if (rt->d3d12_msaa_depth_only) {
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart((ID3D12DescriptorHeap *)rt->d3d12_msaa_dsv_heap, &dsv_handle);
            rt->d3d12_msaa_dirty = 1;
        }
        ID3D12GraphicsCommandList_OMSetRenderTargets(vio_d3d12.cmd_list, 0, NULL, FALSE, &dsv_handle);
        vio_d3d12.current_has_rtv = 0;
        vio_d3d12.current_rtv_count = 0;
        vio_d3d12.current_dsv = dsv_handle;
        if (layers > 1) { rt->bound_face = all ? VIO_RT_ALL_LAYERS : face; rt->bound_level = 0; }
    } else if (layers > 1) {
        int mips = d3d12_rt_mips(rt);
        if (level < 0 || level >= mips) level = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_base;
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart((ID3D12DescriptorHeap *)rt->d3d12_rtv_heap, &rtv_base);
        if (all) level = 0;
        int rtv_index = all ? layers * mips : face * mips + level;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = { rtv_base.ptr + (SIZE_T)rtv_index * vio_d3d12.rtv_descriptor_size };
        if (rt->d3d12_msaa_layered) {
            /* MSAA (A24): level 0 renders into the layer of the MS array; leaving a
             * layer or going to a smaller level resolves first, smaller levels render
             * single-sampled into the face itself. */
            int ms_layer = all ? -1 : face;
            if (rt->d3d12_msaa_dirty && (level != 0 || rt->d3d12_msaa_layer != ms_layer)) d3d12_rt_resolve_msaa(rt);
            if (level == 0) {
                D3D12_CPU_DESCRIPTOR_HANDLE r0, d0;
                ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart((ID3D12DescriptorHeap *)rt->d3d12_msaa_rtv_heap, &r0);
                ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart((ID3D12DescriptorHeap *)rt->d3d12_msaa_dsv_heap, &d0);
                rtv.ptr = r0.ptr + (SIZE_T)(all ? layers : face) * vio_d3d12.rtv_descriptor_size;
                dsv_handle.ptr = d0.ptr + (SIZE_T)(all ? layers : face) * vio_d3d12.dsv_descriptor_size;
                rt->d3d12_msaa_layer = ms_layer;
                rt->d3d12_msaa_dirty = 1;
            } else if (rt->d3d12_color_is_srv) {
                d3d12_rt_barrier(vio_d3d12.cmd_list, (ID3D12Resource *)rt->d3d12_color_resource,
                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                rt->d3d12_color_is_srv = 0;
            }
        }
        /* The layer's depth slice matches level 0 only (GL / Metal contract). */
        ID3D12GraphicsCommandList_OMSetRenderTargets(vio_d3d12.cmd_list, 1, &rtv, FALSE, level == 0 ? &dsv_handle : NULL);
        vio_d3d12.current_rtv = rtv;
        vio_d3d12.current_rtvs[0] = rtv;
        vio_d3d12.current_rtv_count = 1;
        vio_d3d12.current_has_rtv = 1;
        vio_d3d12.current_dsv = dsv_handle;
        w = (rt->width >> level) > 0 ? (rt->width >> level) : 1;
        h = (rt->height >> level) > 0 ? (rt->height >> level) : 1;
        rt->bound_face = all ? VIO_RT_ALL_LAYERS : face;
        rt->bound_level = level;
    } else {
        int n = rt->attachment_count > 0 ? rt->attachment_count : 1;
        if (n > VIO_MAX_COLOR_ATTACHMENTS) n = VIO_MAX_COLOR_ATTACHMENTS;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handles[VIO_MAX_COLOR_ATTACHMENTS];
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_base;
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart((ID3D12DescriptorHeap *)rt->d3d12_rtv_heap, &rtv_base);
        for (int ai = 0; ai < n; ai++) {
            rtv_handles[ai].ptr = rtv_base.ptr + (SIZE_T)ai * vio_d3d12.rtv_descriptor_size;
            vio_d3d12.current_rtvs[ai] = rtv_handles[ai];
        }
        ID3D12GraphicsCommandList_OMSetRenderTargets(vio_d3d12.cmd_list, (UINT)n, rtv_handles, FALSE, &dsv_handle);
        vio_d3d12.current_rtv = rtv_handles[0];
        vio_d3d12.current_rtv_count = n;
        vio_d3d12.current_has_rtv = 1;
        vio_d3d12.current_dsv = dsv_handle;
    }
    vio_d3d12.current_rt_width = w;
    vio_d3d12.current_rt_height = h;
    d3d12_rt_set_viewport_scissor(w, h);
    vio_d3d12.current_bound_rt = rt;
    /* A layered MSAA target renders single-sampled at levels > 0. */
    vio_d3d12.current_rt_samples = (rt->samples > 1 && !(rt->d3d12_msaa_layered && rt->bound_level > 0)) ? rt->samples : 1;
    vio_d3d12.current_rt_format = rt->depth_only ? DXGI_FORMAT_UNKNOWN : vio_pixel_format_to_dxgi(rt->formats[0]);
    d3d12_rearm_pso_for_target();
}

void vio_d3d12_apply_pending_render_target(void)
{
    if (!vio_d3d12.initialized || !vio_d3d12.pending_bound_rt || !vio_d3d12.in_frame) return;
    vio_render_target_object *prt = (vio_render_target_object *)vio_d3d12.pending_bound_rt;
    vio_d3d12.pending_bound_rt = NULL;
    if (prt->valid && prt->backend_type == VIO_RT_BACKEND_D3D12) {
        d3d12_record_bind_render_target(prt, 0, 0);
    }
}

static void d3d12_bind_render_target(void *rt_ptr)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!rt || !vio_d3d12.initialized || rt->backend_type != VIO_RT_BACKEND_D3D12) return;
    if (vio_d3d12.in_frame) {
        d3d12_record_bind_render_target(rt, 0, 0);
    } else {
        /* Command list closed (before vio_begin): recording would be an error;
         * vio_begin() applies the pending bind once the list is open. This is
         * what makes the warm-render "bind then begin" order work. */
        vio_d3d12.pending_bound_rt = rt;
    }
}

static int d3d12_bind_render_target_face(void *rt_ptr, int face, int level)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!rt || !vio_d3d12.initialized || rt->backend_type != VIO_RT_BACKEND_D3D12) return -1;
    if ((!rt->is_cube && rt->layers <= 1) || level < 0 || level >= d3d12_rt_mips(rt)) return -1;
    if (face == VIO_RT_ALL_LAYERS ? level != 0 : (face < 0 || face >= vio_rt_layer_count(rt))) return -1;
    if (!vio_d3d12.in_frame) {
        php_error_docref(NULL, E_WARNING, "D3D12: cube face / array layer binds are only valid between vio_begin and vio_end");
        return -1;
    }
    d3d12_record_bind_render_target(rt, face, level);
    return 0;
}

static void d3d12_unbind_render_target(unsigned int default_fbo, int width, int height)
{
    (void)default_fbo; (void)width; (void)height;
    if (!vio_d3d12.initialized) return;

    if (!vio_d3d12.in_frame) {
        /* Command list closed (unbind after vio_end — the warm-render path).
         * The next d3d12_begin_frame() rebinds the swapchain anyway; just drop
         * the tracked + pending binding. The colour stays in RENDER_TARGET state
         * (no SRV transition recorded) — fine for the only out-of-frame caller,
         * which discards the target without sampling it. */
        vio_d3d12.pending_bound_rt = NULL;
        vio_d3d12.current_bound_rt = NULL;
        return;
    }

    /* Transition the bound target so it can be sampled: depth-only -> SRV
     * (unless a later bind already did it), colour attachments -> SRV. */
    if (vio_d3d12.current_bound_rt) {
        vio_render_target_object *bound_rt = (vio_render_target_object *)vio_d3d12.current_bound_rt;
        if (bound_rt->d3d12_depth_resource && bound_rt->depth_only && !bound_rt->d3d12_depth_is_srv) {
            d3d12_rt_barrier(vio_d3d12.cmd_list, (ID3D12Resource *)bound_rt->d3d12_depth_resource,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            bound_rt->d3d12_depth_is_srv = 1;
        }
        if (bound_rt->d3d12_msaa_color_resources[0] || bound_rt->d3d12_msaa_depth_only) {
            d3d12_rt_resolve_msaa(bound_rt);
        } else if (bound_rt->d3d12_color_resource && !bound_rt->depth_only && !bound_rt->d3d12_color_is_srv) {
            int n = bound_rt->attachment_count > 0 ? bound_rt->attachment_count : 1;
            for (int ai = 0; ai < n && ai < VIO_MAX_COLOR_ATTACHMENTS; ai++) {
                ID3D12Resource *res = ai == 0 ? (ID3D12Resource *)bound_rt->d3d12_color_resource
                                              : (ID3D12Resource *)bound_rt->d3d12_color_resources[ai];
                if (res) d3d12_rt_barrier(vio_d3d12.cmd_list, res, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            bound_rt->d3d12_color_is_srv = 1;
        }
        vio_d3d12.current_bound_rt = NULL;
    }

    /* Restore the swapchain target */
    vio_d3d12_frame *frame = &vio_d3d12.frames[vio_d3d12.frame_index];
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.dsv_heap, &dsv_handle);
    ID3D12GraphicsCommandList_OMSetRenderTargets(vio_d3d12.cmd_list, 1, &frame->rtv_handle, FALSE, &dsv_handle);
    vio_d3d12.current_rtv = frame->rtv_handle;
    vio_d3d12.current_rtvs[0] = frame->rtv_handle;
    vio_d3d12.current_rtv_count = 1;
    vio_d3d12.current_dsv = dsv_handle;
    vio_d3d12.current_rt_width = vio_d3d12.width;
    vio_d3d12.current_rt_height = vio_d3d12.height;
    vio_d3d12.current_has_rtv = 1;
    vio_d3d12.current_rt_samples = 1;
    vio_d3d12.current_rt_format = vio_d3d12.swapchain_format;
    d3d12_rt_set_viewport_scissor(vio_d3d12.width, vio_d3d12.height);
    d3d12_rearm_pso_for_target();
}

/* Initial clear of a freshly created target, recorded on the upload list. */
typedef struct _d3d12_rt_clear_job {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv0;
    int                         rtv_count;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;          /* first DSV; layered targets have dsv_count */
    int                         dsv_count;
} d3d12_rt_clear_job;

static void d3d12_record_rt_clear(ID3D12GraphicsCommandList *list, void *user)
{
    d3d12_rt_clear_job *job = (d3d12_rt_clear_job *)user;
    float zero[4] = {0, 0, 0, 0};
    for (int i = 0; i < job->rtv_count; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = { job->rtv0.ptr + (SIZE_T)i * vio_d3d12.rtv_descriptor_size };
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, h, zero, 0, NULL);
    }
    int n = job->dsv_count > 0 ? job->dsv_count : 1;
    for (int i = 0; i < n; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE d = { job->dsv.ptr + (SIZE_T)i * vio_d3d12.dsv_descriptor_size };
        ID3D12GraphicsCommandList_ClearDepthStencilView(list, d,
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
    }
}

/* Allocate a static SRV (staging heap) for a render-target view. */
static int d3d12_rt_alloc_srv(uint64_t *out_cpu, uint64_t *out_gpu)
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu; D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    if (d3d12_alloc_srv_descriptor(&cpu, &gpu) == UINT_MAX) return -1;
    *out_cpu = cpu.ptr; *out_gpu = gpu.ptr;
    return 0;
}

static int d3d12_create_render_target(void *rt_ptr, int width, int height, int hdr, int depth_only)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    (void)hdr;   /* rt->formats[0] already encodes it */
    if (!rt || !vio_d3d12.initialized || width <= 0 || height <= 0) return -1;
    HRESULT hr;
    int attachment_count = rt->attachment_count > 0 ? rt->attachment_count : 1;
    if (attachment_count > VIO_MAX_COLOR_ATTACHMENTS) attachment_count = VIO_MAX_COLOR_ATTACHMENTS;
    int mips = d3d12_rt_mips(rt);
    int layers = vio_rt_layer_count(rt);
    int layered = layers > 1;
    int want_samples = rt->samples;   /* cube / array MSAA at the end (A24) */
    /* MSAA (GAP-PHASE5 Block 1): clamp the request to a power of two the device
     * supports for attachment 0's format. Cube / depth-only targets stay
     * single-sample (no resolve path), like D3D11. The PSO side is handled by
     * the per-sample-count variants (d3d12_pipeline_pso_for_samples). */
    UINT samples = 1;
    if (!layered && !depth_only && rt->samples > 1) {
        UINT want = rt->samples > 8 ? 8 : (UINT)rt->samples;
        while (want & (want - 1)) want &= want - 1;   /* round down to a power of two */
        for (UINT s = want; s > 1; s >>= 1) {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS mq = {0};
            mq.Format = vio_pixel_format_to_dxgi(rt->formats[0]);
            mq.SampleCount = s;
            if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(vio_d3d12.device, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &mq, sizeof(mq)))
                && mq.NumQualityLevels > 0) {
                samples = s;
                break;
            }
        }
    }
    rt->samples = (int)samples;
    rt->backend_type = VIO_RT_BACKEND_D3D12;   /* the destructor releases whatever exists from here on */

    if (!depth_only) {
        D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {0};
        /* MSAA: attachments' RTVs first, then one RTV per single-sample resolve
         * target (used only by the initial clear). */
        rtv_heap_desc.NumDescriptors = layered ? (UINT)(layers * mips + 1) : (UINT)(attachment_count * (samples > 1 ? 2 : 1));
        rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ID3D12DescriptorHeap *rtv_heap = NULL;
        hr = ID3D12Device_CreateDescriptorHeap(vio_d3d12.device, &rtv_heap_desc, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap);
        if (FAILED(hr)) {
            php_error_docref(NULL, E_WARNING, "D3D12: Failed to create RTV heap (0x%08lx)", hr);
            return -1;
        }
        rt->d3d12_rtv_heap = rtv_heap;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_base;
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap, &rtv_base);

        D3D12_HEAP_PROPERTIES heap_props = {0};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

        if (layered) {
            /* Cube (6 slices, mip chain) or array ('layers' => N): one RTV per
             * (layer, level). */
            DXGI_FORMAT dxfmt = vio_pixel_format_to_dxgi(rt->formats[0]);
            D3D12_RESOURCE_DESC rd = {0};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = width; rd.Height = height;
            rd.DepthOrArraySize = (UINT16)layers;
            rd.MipLevels = (UINT16)mips;
            rd.Format = dxfmt;
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (mips > 1 && d3d12_format_supports_uav(dxfmt)) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;   /* GPU mip generation */
            D3D12_CLEAR_VALUE cv = {0};
            cv.Format = dxfmt;
            ID3D12Resource *cube = NULL;
            hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &heap_props, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, &IID_ID3D12Resource, (void **)&cube);
            if (FAILED(hr)) {
                php_error_docref(NULL, E_WARNING, "D3D12: Failed to create %s colour resource (0x%08lx)", rt->is_cube ? "cube" : "array", hr);
                return -1;
            }
            rt->d3d12_color_resource = cube;
            rt->d3d12_color_resources[0] = cube;
            for (int f = 0; f < layers; f++) {
                for (int l = 0; l < mips; l++) {
                    D3D12_RENDER_TARGET_VIEW_DESC vd = {0};
                    vd.Format = dxfmt;
                    vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    vd.Texture2DArray.MipSlice = (UINT)l;
                    vd.Texture2DArray.FirstArraySlice = (UINT)f;
                    vd.Texture2DArray.ArraySize = 1;
                    D3D12_CPU_DESCRIPTOR_HANDLE h = { rtv_base.ptr + (SIZE_T)(f * mips + l) * vio_d3d12.rtv_descriptor_size };
                    ID3D12Device_CreateRenderTargetView(vio_d3d12.device, cube, &vd, h);
                }
            }
            {
                /* Entry layers * mips: every slice of level 0 (VIO_RT_ALL_LAYERS). */
                D3D12_RENDER_TARGET_VIEW_DESC vd = {0};
                vd.Format = dxfmt;
                vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                vd.Texture2DArray.ArraySize = (UINT)layers;
                D3D12_CPU_DESCRIPTOR_HANDLE h = { rtv_base.ptr + (SIZE_T)(layers * mips) * vio_d3d12.rtv_descriptor_size };
                ID3D12Device_CreateRenderTargetView(vio_d3d12.device, cube, &vd, h);
            }
        } else {
            for (int ai = 0; ai < attachment_count; ai++) {
                DXGI_FORMAT dxfmt = vio_pixel_format_to_dxgi(rt->formats[ai]);
                D3D12_RESOURCE_DESC rd = {0};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                rd.Width = width; rd.Height = height;
                rd.DepthOrArraySize = 1;
                rd.MipLevels = 1;
                rd.Format = dxfmt;
                rd.SampleDesc.Count = 1;
                rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                D3D12_CLEAR_VALUE cv = {0};
                cv.Format = dxfmt;
                ID3D12Resource *color_res = NULL;
                hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &heap_props, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, &IID_ID3D12Resource, (void **)&color_res);
                if (FAILED(hr)) {
                    php_error_docref(NULL, E_WARNING, "D3D12: Failed to create color resource %d (0x%08lx)", ai, hr);
                    return -1;
                }
                rt->d3d12_color_resources[ai] = color_res;
                D3D12_CPU_DESCRIPTOR_HANDLE h = { rtv_base.ptr + (SIZE_T)ai * vio_d3d12.rtv_descriptor_size };
                if (samples > 1) {
                    /* The RTV targets the multisampled resource; the single-sample
                     * resource above becomes the resolve target (SRV / readback) and
                     * gets its own RTV behind the attachments for the initial clear. */
                    D3D12_RESOURCE_DESC md = rd;
                    md.SampleDesc.Count = samples;
                    md.SampleDesc.Quality = 0;
                    ID3D12Resource *ms_res = NULL;
                    hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &heap_props, D3D12_HEAP_FLAG_NONE, &md,
                        D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, &IID_ID3D12Resource, (void **)&ms_res);
                    if (FAILED(hr)) {
                        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create MSAA color resource %d (0x%08lx)", ai, hr);
                        return -1;
                    }
                    rt->d3d12_msaa_color_resources[ai] = ms_res;
                    ID3D12Device_CreateRenderTargetView(vio_d3d12.device, ms_res, NULL, h);
                    D3D12_CPU_DESCRIPTOR_HANDLE hr_resolve = { rtv_base.ptr + (SIZE_T)(attachment_count + ai) * vio_d3d12.rtv_descriptor_size };
                    ID3D12Device_CreateRenderTargetView(vio_d3d12.device, color_res, NULL, hr_resolve);
                } else {
                    ID3D12Device_CreateRenderTargetView(vio_d3d12.device, color_res, NULL, h);
                }
            }
            rt->d3d12_color_resource = rt->d3d12_color_resources[0];
        }
    }

    /* DSV heap + depth resource (level-0 sized; cube / array targets carry a
     * depth slice and a DSV per layer) */
    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {0};
    /* depth_only + 'mipmaps' (A26): a DSV per level for vio_generate_mipmaps. */
    int depth_mips = (depth_only && !layered && rt->mip_levels > 1) ? rt->mip_levels : 1;
    dsv_heap_desc.NumDescriptors = layered ? (UINT)layers + 1 : (UINT)depth_mips;   /* + the all-slice DSV */
    dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ID3D12DescriptorHeap *dsv_heap = NULL;
    hr = ID3D12Device_CreateDescriptorHeap(vio_d3d12.device, &dsv_heap_desc, &IID_ID3D12DescriptorHeap, (void **)&dsv_heap);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create DSV heap (0x%08lx)", hr);
        return -1;
    }
    rt->d3d12_dsv_heap = dsv_heap;

    D3D12_HEAP_PROPERTIES depth_heap_props = {0};
    depth_heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC depth_res_desc = {0};
    depth_res_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth_res_desc.Width = width;
    depth_res_desc.Height = height;
    depth_res_desc.DepthOrArraySize = (UINT16)layers;
    depth_res_desc.MipLevels = (UINT16)depth_mips;
    depth_res_desc.Format = depth_only ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_res_desc.SampleDesc.Count = samples;   /* multisampled with the colour (DSV infers 2DMS) */
    depth_res_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE depth_clear = {0};
    depth_clear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_clear.DepthStencil.Depth = 1.0f;
    ID3D12Resource *depth_res = NULL;
    hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &depth_heap_props, D3D12_HEAP_FLAG_NONE, &depth_res_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear, &IID_ID3D12Resource, (void **)&depth_res);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create depth resource (0x%08lx)", hr);
        return -1;
    }
    rt->d3d12_depth_resource = depth_res;

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_view_desc = {0};
    dsv_view_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv_view_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap, &dsv_handle);
    if (layered) {
        for (int l = 0; l < layers; l++) {
            D3D12_DEPTH_STENCIL_VIEW_DESC dd = {0};
            dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dd.Texture2DArray.FirstArraySlice = (UINT)l;
            dd.Texture2DArray.ArraySize = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = { dsv_handle.ptr + (SIZE_T)l * vio_d3d12.dsv_descriptor_size };
            ID3D12Device_CreateDepthStencilView(vio_d3d12.device, depth_res, &dd, h);
        }
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC dd = {0};
            dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dd.Texture2DArray.ArraySize = (UINT)layers;
            D3D12_CPU_DESCRIPTOR_HANDLE h = { dsv_handle.ptr + (SIZE_T)layers * vio_d3d12.dsv_descriptor_size };
            ID3D12Device_CreateDepthStencilView(vio_d3d12.device, depth_res, &dd, h);
        }
    } else {
        ID3D12Device_CreateDepthStencilView(vio_d3d12.device, depth_res, depth_only ? &dsv_view_desc : NULL, dsv_handle);
        for (int m = 1; m < depth_mips; m++) {
            D3D12_DEPTH_STENCIL_VIEW_DESC dd = dsv_view_desc;
            dd.Texture2D.MipSlice = (UINT)m;
            D3D12_CPU_DESCRIPTOR_HANDLE h = { dsv_handle.ptr + (SIZE_T)m * vio_d3d12.dsv_descriptor_size };
            ID3D12Device_CreateDepthStencilView(vio_d3d12.device, depth_res, &dd, h);
        }
    }

    /* Static SRVs (staging heap) for sampling the target later. */
    if (depth_only) {
        uint64_t cpu, gpu;
        if (d3d12_rt_alloc_srv(&cpu, &gpu) == 0) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
            sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (rt->is_cube) {
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                sd.TextureCube.MipLevels = 1;
            } else if (layered) {
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                sd.Texture2DArray.MipLevels = 1;
                sd.Texture2DArray.ArraySize = (UINT)layers;
            } else {
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = (UINT)depth_mips;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE h = { (SIZE_T)cpu };
            ID3D12Device_CreateShaderResourceView(vio_d3d12.device, depth_res, &sd, h);
            rt->d3d12_depth_srv_gpu = gpu;
            rt->d3d12_depth_srv_cpu = cpu;
        }
    } else if (layered) {
        uint64_t cpu, gpu;
        if (d3d12_rt_alloc_srv(&cpu, &gpu) == 0) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
            sd.Format = vio_pixel_format_to_dxgi(rt->formats[0]);
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (rt->is_cube) {
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                sd.TextureCube.MipLevels = (UINT)mips;
            } else {
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                sd.Texture2DArray.MipLevels = (UINT)mips;
                sd.Texture2DArray.ArraySize = (UINT)layers;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE h = { (SIZE_T)cpu };
            ID3D12Device_CreateShaderResourceView(vio_d3d12.device, (ID3D12Resource *)rt->d3d12_color_resource, &sd, h);
            rt->d3d12_color_srv_gpus[0] = rt->d3d12_color_srv_gpu = gpu;
            rt->d3d12_color_srv_cpus[0] = rt->d3d12_color_srv_cpu = cpu;
        }
    } else {
        for (int ai = 0; ai < attachment_count; ai++) {
            if (!rt->d3d12_color_resources[ai]) break;
            uint64_t cpu, gpu;
            if (d3d12_rt_alloc_srv(&cpu, &gpu) != 0) break;
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
            sd.Format = vio_pixel_format_to_dxgi(rt->formats[ai]);
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = { (SIZE_T)cpu };
            ID3D12Device_CreateShaderResourceView(vio_d3d12.device, (ID3D12Resource *)rt->d3d12_color_resources[ai], &sd, h);
            rt->d3d12_color_srv_gpus[ai] = gpu;
            rt->d3d12_color_srv_cpus[ai] = cpu;
            if (ai == 0) { rt->d3d12_color_srv_gpu = gpu; rt->d3d12_color_srv_cpu = cpu; }
        }
    }

    /* Defined initial contents (colour 0, depth 1.0) like GL / Metal. The
     * resources sit in RENDER_TARGET / DEPTH_WRITE right after creation, so the
     * clears need no barriers; they go through the upload queue (no stall —
     * the first frame that binds the target is queued behind them). */
    {
        d3d12_rt_clear_job job = {0};
        job.dsv = dsv_handle;
        job.dsv_count = layers;
        if (!depth_only && rt->d3d12_rtv_heap) {
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart((ID3D12DescriptorHeap *)rt->d3d12_rtv_heap, &job.rtv0);
            job.rtv_count = layered ? layers * mips : attachment_count * (samples > 1 ? 2 : 1);
        }
        d3d12_submit_upload(d3d12_record_rt_clear, &job);
    }
    /* Cube / array MSAA (A24): a multisampled colour + depth array beside the
     * single-sample cube / array; level-0 binds render into its layer, leaving
     * the layer resolves it into mip 0 of the face. */
    if (depth_only && !layered && rt->mip_levels <= 1 && want_samples > 1) {
        /* depth_only MSAA (A24): a multisampled R24G8 depth with its own DSV; the
         * resolve pass reduces its samples into the plain (sampled) depth. */
        UINT ms = 1;
        for (UINT s = want_samples > 8 ? 8 : (UINT)want_samples; s > 1; s >>= 1) {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS mq = {0};
            mq.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            mq.SampleCount = s;
            if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(vio_d3d12.device, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &mq, sizeof(mq)))
                && mq.NumQualityLevels > 0) { ms = s; break; }
        }
        if (ms > 1) {
            D3D12_HEAP_PROPERTIES hp = {0};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = {0};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = (UINT64)width;
            rd.Height = (UINT)height;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_R24G8_TYPELESS;
            rd.SampleDesc.Count = ms;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            D3D12_CLEAR_VALUE dc = {0};
            dc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            dc.DepthStencil.Depth = 1.0f;
            ID3D12Resource *mres = NULL;
            ID3D12DescriptorHeap *mh = NULL;
            D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
            hd.NumDescriptors = 1;
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            if (SUCCEEDED(ID3D12Device_CreateCommittedResource(vio_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE, &dc, &IID_ID3D12Resource, (void **)&mres)) &&
                SUCCEEDED(ID3D12Device_CreateDescriptorHeap(vio_d3d12.device, &hd, &IID_ID3D12DescriptorHeap, (void **)&mh))) {
                D3D12_DEPTH_STENCIL_VIEW_DESC dv = {0};
                dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
                dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
                D3D12_CPU_DESCRIPTOR_HANDLE h;
                ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(mh, &h);
                ID3D12Device_CreateDepthStencilView(vio_d3d12.device, mres, &dv, h);
                rt->d3d12_msaa_depth_resource = mres;
                rt->d3d12_msaa_dsv_heap = mh;
                rt->d3d12_msaa_depth_only = 1;
                rt->samples = (int)ms;
                d3d12_rt_clear_job mjob = {0};
                mjob.dsv = h;
                mjob.dsv_count = 1;
                d3d12_submit_upload(d3d12_record_rt_clear, &mjob);
            } else {
                if (mres) ID3D12Resource_Release(mres);
                if (mh) ID3D12DescriptorHeap_Release(mh);
            }
        }
    }
    if (layered && !depth_only && want_samples > 1) {
        DXGI_FORMAT cf = vio_pixel_format_to_dxgi(rt->formats[0]);
        UINT ms = 1;
        for (UINT s = want_samples > 8 ? 8 : (UINT)want_samples; s > 1; s >>= 1) {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS mq = {0};
            mq.Format = cf;
            mq.SampleCount = s;
            if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(vio_d3d12.device, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &mq, sizeof(mq)))
                && mq.NumQualityLevels > 0) { ms = s; break; }
        }
        if (ms > 1) {
            D3D12_HEAP_PROPERTIES hp = {0};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = {0};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = (UINT64)width;
            rd.Height = (UINT)height;
            rd.DepthOrArraySize = (UINT16)layers;
            rd.MipLevels = 1;
            rd.Format = cf;
            rd.SampleDesc.Count = ms;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cc = {0};
            cc.Format = cf;
            D3D12_RESOURCE_DESC dd = rd;
            dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            D3D12_CLEAR_VALUE dc = {0};
            dc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            dc.DepthStencil.Depth = 1.0f;
            ID3D12Resource *cres = NULL, *dres = NULL;
            ID3D12DescriptorHeap *rh = NULL, *dh = NULL;
            D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
            hd.NumDescriptors = (UINT)layers + 1;
            int ok = SUCCEEDED(ID3D12Device_CreateCommittedResource(vio_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                         D3D12_RESOURCE_STATE_RENDER_TARGET, &cc, &IID_ID3D12Resource, (void **)&cres))
                  && SUCCEEDED(ID3D12Device_CreateCommittedResource(vio_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &dd,
                         D3D12_RESOURCE_STATE_DEPTH_WRITE, &dc, &IID_ID3D12Resource, (void **)&dres));
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            if (ok) ok = SUCCEEDED(ID3D12Device_CreateDescriptorHeap(vio_d3d12.device, &hd, &IID_ID3D12DescriptorHeap, (void **)&rh));
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            if (ok) ok = SUCCEEDED(ID3D12Device_CreateDescriptorHeap(vio_d3d12.device, &hd, &IID_ID3D12DescriptorHeap, (void **)&dh));
            if (ok) {
                D3D12_CPU_DESCRIPTOR_HANDLE r0, d0;
                ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rh, &r0);
                ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dh, &d0);
                for (int l = 0; l <= layers; l++) {
                    D3D12_RENDER_TARGET_VIEW_DESC rv = {0};
                    rv.Format = cf;
                    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
                    rv.Texture2DMSArray.FirstArraySlice = l < layers ? (UINT)l : 0;
                    rv.Texture2DMSArray.ArraySize = l < layers ? 1 : (UINT)layers;
                    D3D12_DEPTH_STENCIL_VIEW_DESC dv = {0};
                    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
                    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
                    dv.Texture2DMSArray.FirstArraySlice = rv.Texture2DMSArray.FirstArraySlice;
                    dv.Texture2DMSArray.ArraySize = rv.Texture2DMSArray.ArraySize;
                    D3D12_CPU_DESCRIPTOR_HANDLE rhh = { r0.ptr + (SIZE_T)l * vio_d3d12.rtv_descriptor_size };
                    D3D12_CPU_DESCRIPTOR_HANDLE dhh = { d0.ptr + (SIZE_T)l * vio_d3d12.dsv_descriptor_size };
                    ID3D12Device_CreateRenderTargetView(vio_d3d12.device, cres, &rv, rhh);
                    ID3D12Device_CreateDepthStencilView(vio_d3d12.device, dres, &dv, dhh);
                }
                rt->d3d12_msaa_color_resources[0] = cres;
                rt->d3d12_msaa_depth_resource = dres;
                rt->d3d12_msaa_rtv_heap = rh;
                rt->d3d12_msaa_dsv_heap = dh;
                rt->d3d12_msaa_layered = 1;
                rt->d3d12_msaa_layer = 0;
                rt->samples = (int)ms;
                d3d12_rt_clear_job mjob = {0};
                mjob.rtv0 = r0;
                mjob.rtv_count = layers;
                mjob.dsv = d0;
                mjob.dsv_count = layers;
                d3d12_submit_upload(d3d12_record_rt_clear, &mjob);
            } else {
                if (cres) ID3D12Resource_Release(cres);
                if (dres) ID3D12Resource_Release(dres);
                if (rh) ID3D12DescriptorHeap_Release(rh);
                if (dh) ID3D12DescriptorHeap_Release(dh);
            }
        }
    }
    return 0;
}

static int d3d12_render_target_cubemap(void *rt_ptr, void *cm_obj)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    vio_cubemap_object *cm = (vio_cubemap_object *)cm_obj;
    if (!rt || !cm || !rt->is_cube) return -1;
    /* depth_only: the depth cube (TEXTURECUBE SRV over the R24 plane). */
    void *res = rt->depth_only ? rt->d3d12_depth_resource : rt->d3d12_color_resource;
    uint64_t srv_cpu = rt->depth_only ? rt->d3d12_depth_srv_cpu : rt->d3d12_color_srv_cpu;
    uint64_t srv_gpu = rt->depth_only ? rt->d3d12_depth_srv_gpu : rt->d3d12_color_srv_gpu;
    if (!res || !srv_cpu) return -1;
    /* Borrowed: the RT owns the resource + descriptor (d3d12_destroy_cubemap
     * honours cm->borrowed). Binding reads d3d12_srv_cpu, which stays valid for
     * the RT's life. */
    cm->d3d12_resource = res;
    cm->d3d12_srv_cpu  = srv_cpu;
    cm->d3d12_srv_gpu  = srv_gpu;
    cm->mipmaps        = rt->mip_levels > 1;
    cm->borrowed       = 1;
    cm->resolution     = rt->width;
    cm->backend_type   = 3;
    return 0;
}

/* Mip generation, CPU path (GAP-PLAN 2.3): D3D12 has no GenerateMips. Level 0
 * of every slice is read back, box-filtered on the CPU and the smaller levels
 * uploaded. Correct and portable (runs on WARP); a compute downsample can
 * replace it later. `state` is the resource's steady state, restored after. */
static int d3d12_generate_mips_cpu(ID3D12Resource *res, int slices, int levels, int w, int h, int channels,
                                   D3D12_RESOURCE_STATES state)
{
    if (levels <= 1) return 0;
    D3D12_RESOURCE_DESC rd;
    ID3D12Resource_GetDesc(res, &rd);
    for (int s = 0; s < slices; s++) {
        UINT pitch = 0;
        unsigned char *raw = d3d12_readback_subresource(res, (UINT)(s * levels), state, &pitch);
        if (!raw) return -1;
        /* Tightly pack level 0 (readback rows are 256-byte aligned). */
        uint8_t *level0 = (uint8_t *)malloc((size_t)w * h * channels);
        if (!level0) { free(raw); return -1; }
        for (int y = 0; y < h; y++) memcpy(level0 + (size_t)y * w * channels, raw + (size_t)y * pitch, (size_t)w * channels);
        free(raw);

        const void *srcs[16]; UINT pitches[16], rows[16], slcs[16];
        uint8_t *gen[16] = {0};
        int lw = w, lh = h, count = 0;
        const uint8_t *prev = level0;
        for (int l = 1; l < levels && l < 16; l++) {
            int nw = lw > 1 ? lw / 2 : 1, nh = lh > 1 ? lh / 2 : 1;
            gen[l] = (uint8_t *)malloc((size_t)nw * nh * channels);
            if (!gen[l]) break;
            d3d12_box_downsample(prev, lw, lh, channels, gen[l]);
            prev = gen[l]; lw = nw; lh = nh;
            srcs[count] = gen[l]; pitches[count] = (UINT)lw * channels; rows[count] = (UINT)lh; slcs[count] = 1;
            count++;
        }
        int rc = count > 0
            ? d3d12_upload_subresources(res, &rd, (UINT)(s * levels + 1), (UINT)count, srcs, pitches, rows, slcs,
                                        state, state, 0, 0, 0)
            : 0;
        for (int l = 1; l < 16; l++) free(gen[l]);
        free(level0);
        if (rc != 0) return -1;
    }
    return 0;
}

/* ── GPU mip generation (GAP-PHASE5 Block 11) ─────────────────────────────
 * One compute dispatch per level: the destination mip is a typed UAV, the
 * source mip an SRV sampled bilinearly at the destination texel centre, which
 * is exactly the 2x2 box filter of the CPU path. Texture2DArray views cover all
 * slices at once (1 for textures, 6 for cubes). Resources created with the UAV
 * flag (mipmapped textures / cubemaps / cube RTs whose format takes typed UAV
 * stores) take this path; everything else keeps the CPU box filter. */
static const char *d3d12_mipgen_hlsl =
    "Texture2DArray<float4> src : register(t0);\n"
    "RWTexture2DArray<float4> dst : register(u0);\n"
    "SamplerState smp : register(s0);\n"
    "cbuffer P : register(b0) { uint2 dstSize; uint slices; uint pad; };\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= dstSize.x || id.y >= dstSize.y || id.z >= slices) return;\n"
    "    float2 uv = (float2(id.xy) + 0.5) / float2(dstSize);\n"
    "    dst[id] = src.SampleLevel(smp, float3(uv, (float)id.z), 0.0);\n"
    "}\n";

static HRESULT d3d12_compile_cached(const char *src, const char *entry_tag, const char *profile, UINT flags, ID3DBlob **out);

#define VIO_D3D12_MIPGEN_MAX_LEVELS 16
/* mipgen_heap is a ring of blocks (one per vio_generate_mipmaps call, each
 * 2 * VIO_D3D12_MIPGEN_MAX_LEVELS descriptors): a call recorded on the frame
 * list must not overwrite descriptors of a frame still in flight. 64 blocks
 * cover 21 calls per frame at three frames in flight. */
#define VIO_D3D12_MIPGEN_BLOCKS 64

static void d3d12_restore_graphics_state_after_compute(void);

static int d3d12_ensure_mipgen(void)
{
    if (vio_d3d12.mipgen_pso && vio_d3d12.mipgen_heap) return 0;
    if (vio_d3d12.mipgen_failed || !vio_d3d12.device) return -1;
    vio_d3d12.mipgen_failed = 1;   /* cleared on success: a failing setup is not retried per call */

    D3D12_DESCRIPTOR_RANGE ranges[2] = {0};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER params[2] = {0};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;   /* b0: dstSize, slices */
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;  /* t0 + u0 */
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC smp = {0};
    smp.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.MaxLOD = D3D12_FLOAT32_MAX;
    smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs = {0};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &smp;

    ID3DBlob *sig = NULL, *err = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) || !sig) {
        if (err) ID3D10Blob_Release(err);
        return -1;
    }
    HRESULT hr = ID3D12Device_CreateRootSignature(vio_d3d12.device, 0, ID3D10Blob_GetBufferPointer(sig),
                                                  ID3D10Blob_GetBufferSize(sig), &IID_ID3D12RootSignature,
                                                  (void **)&vio_d3d12.mipgen_rs);
    ID3D10Blob_Release(sig);
    if (FAILED(hr)) return -1;

    ID3DBlob *cs = NULL;
    if (FAILED(d3d12_compile_cached(d3d12_mipgen_hlsl, "mipgen", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, &cs)) || !cs) return -1;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = vio_d3d12.mipgen_rs;
    pd.CS.pShaderBytecode = ID3D10Blob_GetBufferPointer(cs);
    pd.CS.BytecodeLength = ID3D10Blob_GetBufferSize(cs);
    hr = ID3D12Device_CreateComputePipelineState(vio_d3d12.device, &pd, &IID_ID3D12PipelineState, (void **)&vio_d3d12.mipgen_pso);
    ID3D10Blob_Release(cs);
    if (FAILED(hr)) { vio_d3d12.mipgen_pso = NULL; return -1; }

    if (d3d12_create_descriptor_heap(&vio_d3d12.mipgen_heap, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                     2 * VIO_D3D12_MIPGEN_MAX_LEVELS * VIO_D3D12_MIPGEN_BLOCKS,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
        return -1;
    }
    vio_d3d12.mipgen_failed = 0;
    return 0;
}

/* 0 = done, 1 = not applicable (caller falls back to the CPU filter), -1 = error.
 * `state` is the resource's steady state (uniform across subresources, like the
 * CPU path assumes) and is restored at the end. */
static int d3d12_generate_mips_gpu(ID3D12Resource *res, int slices, int levels, int w, int h,
                                   D3D12_RESOURCE_STATES state)
{
    D3D12_RESOURCE_DESC rd;
    ID3D12Resource_GetDesc(res, &rd);
    if (!(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || levels < 2 ||
        levels > VIO_D3D12_MIPGEN_MAX_LEVELS || slices < 1) {
        return 1;
    }
    if (d3d12_ensure_mipgen() != 0) return 1;

    /* Mid-frame the dispatches go onto the frame's own command list, after the
     * draws into the resource recorded so far: the queue keeps the order, so
     * neither a flush nor a CPU wait is needed (before 2.30 this drained the GPU
     * twice and cost ~10 ms CPU per call - every environment-cube update
     * stuttered, D3D12-MIPGEN-STALL-PLAN). Outside a frame (loading, tests) a
     * transient list executes and waits as before. */
    int in_frame = vio_d3d12.in_frame && vio_d3d12.cmd_list;
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    if (in_frame) {
        list = vio_d3d12.cmd_list;
    } else {
        HRESULT hr = ID3D12Device_CreateCommandAllocator(vio_d3d12.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         &IID_ID3D12CommandAllocator, (void **)&alloc);
        if (SUCCEEDED(hr)) {
            hr = ID3D12Device_CreateCommandList(vio_d3d12.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc,
                                                vio_d3d12.mipgen_pso, &IID_ID3D12GraphicsCommandList, (void **)&list);
        }
        if (FAILED(hr)) {
            if (alloc) ID3D12CommandAllocator_Release(alloc);
            return -1;
        }
    }
    D3D12_RESOURCE_BARRIER *b = calloc((size_t)slices * (size_t)levels, sizeof(*b));
    if (!b) {
        if (!in_frame) {
            ID3D12GraphicsCommandList_Release(list);
            ID3D12CommandAllocator_Release(alloc);
        }
        return -1;
    }

    /* SRV of level l and UAV of level l + 1, all slices, at heap slots 2l / 2l+1. */
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(vio_d3d12.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.mipgen_heap, &cpu);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(vio_d3d12.mipgen_heap, &gpu);
    UINT block = vio_d3d12.mipgen_block;
    vio_d3d12.mipgen_block = (block + 1) % VIO_D3D12_MIPGEN_BLOCKS;
    cpu.ptr += (SIZE_T)block * 2 * VIO_D3D12_MIPGEN_MAX_LEVELS * inc;
    gpu.ptr += (UINT64)block * 2 * VIO_D3D12_MIPGEN_MAX_LEVELS * inc;
    for (int l = 0; l < levels - 1; l++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
        sd.Format = rd.Format;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2DArray.MostDetailedMip = (UINT)l;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.ArraySize = (UINT)slices;
        D3D12_CPU_DESCRIPTOR_HANDLE hs = { cpu.ptr + (SIZE_T)(2 * l) * inc };
        ID3D12Device_CreateShaderResourceView(vio_d3d12.device, res, &sd, hs);
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {0};
        ud.Format = rd.Format;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        ud.Texture2DArray.MipSlice = (UINT)(l + 1);
        ud.Texture2DArray.ArraySize = (UINT)slices;
        D3D12_CPU_DESCRIPTOR_HANDLE hu = { cpu.ptr + (SIZE_T)(2 * l + 1) * inc };
        ID3D12Device_CreateUnorderedAccessView(vio_d3d12.device, res, NULL, &ud, hu);
    }

    /* Level 0 is read, every other level written — per (slice, mip) subresource. */
    int n = 0;
    for (int s = 0; s < slices; s++) {
        for (int m = 0; m < levels; m++) {
            b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[n].Transition.pResource = res;
            b[n].Transition.Subresource = (UINT)(s * levels + m);
            b[n].Transition.StateBefore = state;
            b[n].Transition.StateAfter = m == 0 ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                                : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            n++;
        }
    }
    ID3D12GraphicsCommandList_ResourceBarrier(list, (UINT)n, b);

    ID3D12DescriptorHeap *heaps[] = { vio_d3d12.mipgen_heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, heaps);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, vio_d3d12.mipgen_rs);
    ID3D12GraphicsCommandList_SetPipelineState(list, vio_d3d12.mipgen_pso);
    int lw = w, lh = h;
    for (int l = 0; l < levels - 1; l++) {
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
        UINT consts[4] = { (UINT)lw, (UINT)lh, (UINT)slices, 0 };
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(list, 0, 4, consts, 0);
        D3D12_GPU_DESCRIPTOR_HANDLE hg = { gpu.ptr + (UINT64)(2 * l) * inc };
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 1, hg);
        ID3D12GraphicsCommandList_Dispatch(list, (UINT)((lw + 7) / 8), (UINT)((lh + 7) / 8), (UINT)slices);
        /* The level just written is the next level's source. */
        n = 0;
        for (int s = 0; s < slices; s++) {
            b[n].Transition.Subresource = (UINT)(s * levels + l + 1);
            b[n].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b[n].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            n++;
        }
        ID3D12GraphicsCommandList_ResourceBarrier(list, (UINT)n, b);
    }
    /* Everything sits in NON_PIXEL_SHADER_RESOURCE now; back to the steady state. */
    n = 0;
    for (int s = 0; s < slices; s++) {
        for (int m = 0; m < levels; m++) {
            b[n].Transition.Subresource = (UINT)(s * levels + m);
            b[n].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            b[n].Transition.StateAfter = state;
            n++;
        }
    }
    ID3D12GraphicsCommandList_ResourceBarrier(list, (UINT)n, b);
    free(b);

    if (in_frame) {
        /* Back to the frame's graphics heaps, root signature and pipeline. */
        d3d12_restore_graphics_state_after_compute();
        return 0;
    }

    ID3D12GraphicsCommandList_Close(list);
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(vio_d3d12.cmd_queue, 1, lists);
    vio_d3d12_wait_for_gpu();
    d3d12_drain_info_queue("generate_mipmaps");
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(alloc);
    return 0;
}

/* GPU downsample when the resource allows it, otherwise the CPU box filter. */
static int d3d12_generate_mips(ID3D12Resource *res, int slices, int levels, int w, int h, int channels,
                               D3D12_RESOURCE_STATES state)
{
    int rc = d3d12_generate_mips_gpu(res, slices, levels, w, h, state);
    if (getenv("VIO_TRACE_MIPGEN")) {
        fprintf(stderr, "[d3d12] generate_mipmaps: %s (%d slice%s, %d levels, %dx%d)\n",
                rc == 0 ? "compute" : (rc < 0 ? "compute FAILED" : "CPU box filter"), slices, slices == 1 ? "" : "s", levels, w, h);
    }
    if (rc <= 0) return rc;
    return d3d12_generate_mips_cpu(res, slices, levels, w, h, channels, state);
}

/* Depth mip chain (A26): each level is the max / min of the 2x2 texels below
 * (odd sizes fold the extra column / row into the last texel), written as
 * SV_Depth by a full-screen triangle. The source level sits in
 * PIXEL_SHADER_RESOURCE (both planes) while the target level is DEPTH_WRITE;
 * SRVs come from the mipgen descriptor ring, the DSVs from the target's heap
 * (one per level). */
static const char *d3d12_dmip_vs_src =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 p = float2((id << 1) & 2, id & 2);\n"
    "    return float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char *d3d12_dmip_ps_src =
    "Texture2D<float> src : register(t0);\n"
    "cbuffer P : register(b0) { int4 sizes; int4 mode; };\n"
    "float main(float4 pos : SV_Position) : SV_Depth {\n"
    "    int2 o = int2(pos.xy);\n"
    "    int2 n = int2((o.x == sizes.z - 1 && (sizes.x & 1) == 1 && sizes.x > 1) ? 3 : 2,\n"
    "                  (o.y == sizes.w - 1 && (sizes.y & 1) == 1 && sizes.y > 1) ? 3 : 2);\n"
    "    float d = mode.x == 0 ? 0.0 : 1.0;\n"
    "    [unroll] for (int y = 0; y < 3; y++) [unroll] for (int x = 0; x < 3; x++) {\n"
    "        if (x < n.x && y < n.y) {\n"
    "            float s = src.Load(int3(min(o * 2 + int2(x, y), sizes.xy - 1), 0));\n"
    "            d = mode.x == 0 ? max(d, s) : min(d, s);\n"
    "        }\n"
    "    }\n"
    "    return d;\n"
    "}\n";

static int d3d12_ensure_depth_mip(void)
{
    if (vio_d3d12.dmip_pso) return 0;
    if (d3d12_ensure_mipgen() != 0) return -1;   /* the descriptor ring */
    D3D12_DESCRIPTOR_RANGE range = {0};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[2] = {0};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 8;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs = {0};
    rs.NumParameters = 2;
    rs.pParameters = params;
    ID3DBlob *sig = NULL, *err = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) || !sig) {
        if (err) ID3D10Blob_Release(err);
        return -1;
    }
    HRESULT hr = ID3D12Device_CreateRootSignature(vio_d3d12.device, 0, ID3D10Blob_GetBufferPointer(sig), ID3D10Blob_GetBufferSize(sig),
                                                  &IID_ID3D12RootSignature, (void **)&vio_d3d12.dmip_rs);
    ID3D10Blob_Release(sig);
    if (FAILED(hr)) return -1;
    ID3DBlob *vs = NULL, *ps = NULL;
    if (FAILED(d3d12_compile_cached(d3d12_dmip_vs_src, "vio_depth_mip_vs", "vs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, &vs)) || !vs) return -1;
    if (FAILED(d3d12_compile_cached(d3d12_dmip_ps_src, "vio_depth_mip_ps", "ps_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, &ps)) || !ps) {
        ID3D10Blob_Release(vs);
        return -1;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = vio_d3d12.dmip_rs;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
    pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps);
    pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFFu;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 0;
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pd.SampleDesc.Count = 1;
    hr = ID3D12Device_CreateGraphicsPipelineState(vio_d3d12.device, &pd, &IID_ID3D12PipelineState, (void **)&vio_d3d12.dmip_pso);
    ID3D10Blob_Release(vs);
    ID3D10Blob_Release(ps);
    if (FAILED(hr)) { vio_d3d12.dmip_pso = NULL; return -1; }
    return 0;
}

/* Both planes (depth, stencil) of one mip of a single-slice depth resource. */
static void d3d12_dmip_barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *res, int levels, int mip,
                               D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b[2] = {0};
    for (int p = 0; p < 2; p++) {
        b[p].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[p].Transition.pResource = res;
        b[p].Transition.Subresource = (UINT)(mip + p * levels);
        b[p].Transition.StateBefore = from;
        b[p].Transition.StateAfter = to;
    }
    ID3D12GraphicsCommandList_ResourceBarrier(list, 2, b);
}

/* depth_only MSAA (A24): each texel's samples reduced (max / min, the target's
 * depth_reduction) into the single-sample depth. The pass leaves both depth
 * resources in the states it found them in (one flag tracks the plain one). */
static const char *d3d12_dmip_resolve_src =
    "Texture2DMS<float> src : register(t0);\n"
    "cbuffer P : register(b0) { int4 sizes; int4 mode; };\n"
    "float main(float4 pos : SV_Position) : SV_Depth {\n"
    "    int2 p = int2(pos.xy);\n"
    "    float d = mode.x == 0 ? 0.0 : 1.0;\n"
    "    for (int s = 0; s < mode.y; s++) { float v = src.Load(p, s); d = mode.x == 0 ? max(d, v) : min(d, v); }\n"
    "    return d;\n"
    "}\n";

static int d3d12_resolve_depth_msaa(vio_render_target_object *rt)
{
    ID3D12GraphicsCommandList *list = vio_d3d12.cmd_list;
    ID3D12Resource *ms = (ID3D12Resource *)rt->d3d12_msaa_depth_resource;
    ID3D12Resource *dst = (ID3D12Resource *)rt->d3d12_depth_resource;
    if (!list || !ms || !dst || d3d12_ensure_depth_mip() != 0) return -1;
    if (!vio_d3d12.dmip_resolve_pso) {
        ID3DBlob *ps = NULL;
        if (FAILED(d3d12_compile_cached(d3d12_dmip_resolve_src, "vio_depth_resolve_ps", "ps_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, &ps)) || !ps) return -1;
        ID3DBlob *vs = NULL;
        if (FAILED(d3d12_compile_cached(d3d12_dmip_vs_src, "vio_depth_mip_vs", "vs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, &vs)) || !vs) { ID3D10Blob_Release(ps); return -1; }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
        pd.pRootSignature = vio_d3d12.dmip_rs;
        pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
        pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps);
        pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = 0xFFFFFFFFu;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        pd.SampleDesc.Count = 1;
        HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(vio_d3d12.device, &pd, &IID_ID3D12PipelineState, (void **)&vio_d3d12.dmip_resolve_pso);
        ID3D10Blob_Release(vs);
        ID3D10Blob_Release(ps);
        if (FAILED(hr)) { vio_d3d12.dmip_resolve_pso = NULL; return -1; }
    }
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(vio_d3d12.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu, dsv;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.mipgen_heap, &cpu);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(vio_d3d12.mipgen_heap, &gpu);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart((ID3D12DescriptorHeap *)rt->d3d12_dsv_heap, &dsv);
    UINT block = vio_d3d12.mipgen_block;
    vio_d3d12.mipgen_block = (block + 1) % VIO_D3D12_MIPGEN_BLOCKS;
    cpu.ptr += (SIZE_T)block * 2 * VIO_D3D12_MIPGEN_MAX_LEVELS * inc;
    gpu.ptr += (UINT64)block * 2 * VIO_D3D12_MIPGEN_MAX_LEVELS * inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ID3D12Device_CreateShaderResourceView(vio_d3d12.device, ms, &sd, cpu);

    int dst_srv = rt->d3d12_depth_is_srv;
    d3d12_rt_barrier(list, ms, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (dst_srv) d3d12_rt_barrier(list, dst, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ID3D12DescriptorHeap *heaps[] = { vio_d3d12.mipgen_heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, vio_d3d12.dmip_rs);
    ID3D12GraphicsCommandList_SetPipelineState(list, vio_d3d12.dmip_resolve_pso);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 0, NULL, FALSE, &dsv);
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)rt->width, (float)rt->height, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, rt->width, rt->height };
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
    UINT consts[8] = { (UINT)rt->width, (UINT)rt->height, (UINT)rt->width, (UINT)rt->height,
                       rt->depth_reduction == VIO_DEPTH_REDUCE_MIN ? 1u : 0u, (UINT)rt->samples, 0, 0 };
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 8, consts, 0);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 1, gpu);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    if (dst_srv) d3d12_rt_barrier(list, dst, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    d3d12_rt_barrier(list, ms, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    d3d12_restore_graphics_state_after_compute();
    return 0;
}

static int d3d12_generate_depth_mips(vio_render_target_object *rt)
{
    ID3D12Resource *res = (ID3D12Resource *)rt->d3d12_depth_resource;
    int levels = rt->mip_levels;
    if (!res || levels < 2 || levels > VIO_D3D12_MIPGEN_MAX_LEVELS || !rt->d3d12_dsv_heap) return -1;
    if (d3d12_ensure_depth_mip() != 0) return -1;
    if (vio_d3d12.current_bound_rt == rt && vio_d3d12.in_frame) d3d12_unbind_render_target(0, 0, 0);
    D3D12_RESOURCE_STATES steady = rt->d3d12_depth_is_srv ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                                          : D3D12_RESOURCE_STATE_DEPTH_WRITE;
    int in_frame = vio_d3d12.in_frame && vio_d3d12.cmd_list;
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    if (in_frame) {
        list = vio_d3d12.cmd_list;
    } else {
        HRESULT hr = ID3D12Device_CreateCommandAllocator(vio_d3d12.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         &IID_ID3D12CommandAllocator, (void **)&alloc);
        if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandList(vio_d3d12.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc,
                                                               vio_d3d12.dmip_pso, &IID_ID3D12GraphicsCommandList, (void **)&list);
        if (FAILED(hr)) {
            if (alloc) ID3D12CommandAllocator_Release(alloc);
            return -1;
        }
    }

    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(vio_d3d12.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu, dsv0;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.mipgen_heap, &cpu);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(vio_d3d12.mipgen_heap, &gpu);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart((ID3D12DescriptorHeap *)rt->d3d12_dsv_heap, &dsv0);
    UINT block = vio_d3d12.mipgen_block;
    vio_d3d12.mipgen_block = (block + 1) % VIO_D3D12_MIPGEN_BLOCKS;
    cpu.ptr += (SIZE_T)block * 2 * VIO_D3D12_MIPGEN_MAX_LEVELS * inc;
    gpu.ptr += (UINT64)block * 2 * VIO_D3D12_MIPGEN_MAX_LEVELS * inc;

    /* Every level readable first; each target level goes to DEPTH_WRITE for its pass. */
    if (steady != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        for (int m = 0; m < levels; m++) d3d12_dmip_barrier(list, res, levels, m, steady, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap *heaps[] = { vio_d3d12.mipgen_heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, vio_d3d12.dmip_rs);
    ID3D12GraphicsCommandList_SetPipelineState(list, vio_d3d12.dmip_pso);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (int l = 1; l < levels; l++) {
        int sw = rt->width >> (l - 1), sh = rt->height >> (l - 1), dw = rt->width >> l, dh = rt->height >> l;
        if (sw < 1) sw = 1;
        if (sh < 1) sh = 1;
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
        sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MostDetailedMip = (UINT)(l - 1);
        sd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE hs = { cpu.ptr + (SIZE_T)(l - 1) * inc };
        ID3D12Device_CreateShaderResourceView(vio_d3d12.device, res, &sd, hs);
        d3d12_dmip_barrier(list, res, levels, l, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = { dsv0.ptr + (SIZE_T)l * vio_d3d12.dsv_descriptor_size };
        ID3D12GraphicsCommandList_OMSetRenderTargets(list, 0, NULL, FALSE, &dsv);
        D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)dw, (float)dh, 0.0f, 1.0f };
        D3D12_RECT sc = { 0, 0, dw, dh };
        ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
        UINT consts[8] = { (UINT)sw, (UINT)sh, (UINT)dw, (UINT)dh, rt->depth_reduction == VIO_DEPTH_REDUCE_MIN ? 1u : 0u, 0, 0, 0 };
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 8, consts, 0);
        D3D12_GPU_DESCRIPTOR_HANDLE hg = { gpu.ptr + (UINT64)(l - 1) * inc };
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 1, hg);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        d3d12_dmip_barrier(list, res, levels, l, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    if (steady != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        for (int m = 0; m < levels; m++) d3d12_dmip_barrier(list, res, levels, m, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, steady);

    if (in_frame) {
        /* Re-arm the frame's target, viewport, scissor and graphics state. */
        if (vio_d3d12.current_has_rtv) {
            int n = vio_d3d12.current_rtv_count > 0 ? vio_d3d12.current_rtv_count : 1;
            ID3D12GraphicsCommandList_OMSetRenderTargets(list, (UINT)n, n > 1 ? vio_d3d12.current_rtvs : &vio_d3d12.current_rtv,
                                                         FALSE, &vio_d3d12.current_dsv);
        } else {
            ID3D12GraphicsCommandList_OMSetRenderTargets(list, 0, NULL, FALSE, &vio_d3d12.current_dsv);
        }
        D3D12_VIEWPORT vp = { 0, 0, (float)vio_d3d12.current_rt_width, (float)vio_d3d12.current_rt_height, 0.0f, 1.0f };
        D3D12_RECT sc = { 0, 0, vio_d3d12.current_rt_width, vio_d3d12.current_rt_height };
        ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
        d3d12_restore_graphics_state_after_compute();
        return 0;
    }
    ID3D12GraphicsCommandList_Close(list);
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(vio_d3d12.cmd_queue, 1, lists);
    vio_d3d12_wait_for_gpu();
    d3d12_drain_info_queue("generate_mipmaps (depth)");
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(alloc);
    return 0;
}

static int d3d12_generate_mipmaps(void *obj, int kind)
{
    if (!obj || !vio_d3d12.initialized) return -1;
    switch (kind) {
        case 0: {
            vio_render_target_object *rt = (vio_render_target_object *)obj;
            if (rt->backend_type == VIO_RT_BACKEND_D3D12 && rt->depth_only) return d3d12_generate_depth_mips(rt);
            if (rt->backend_type != VIO_RT_BACKEND_D3D12 || !rt->d3d12_color_resource) return -1;
            if (!rt->is_cube || rt->mip_levels <= 1) return 0;
            if (vio_d3d12.current_bound_rt == rt && vio_d3d12.in_frame) d3d12_unbind_render_target(0, 0, 0);
            D3D12_RESOURCE_STATES st = rt->d3d12_color_is_srv ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                                              : D3D12_RESOURCE_STATE_RENDER_TARGET;
            return d3d12_generate_mips((ID3D12Resource *)rt->d3d12_color_resource, 6, rt->mip_levels,
                                           rt->width, rt->height, vio_rt_format_bpp(rt->formats[0]), st);
        }
        case 1: {
            vio_texture_object *t = (vio_texture_object *)obj;
            vio_d3d12_texture *dt = (vio_d3d12_texture *)t->backend_texture;
            if (!dt || !dt->resource) return -1;
            if (dt->mip_levels <= 1 || dt->depth > 0 || dt->compressed) return 0;
            return d3d12_generate_mips(dt->resource, dt->layers > 1 ? dt->layers : 1, dt->mip_levels, dt->width, dt->height,
                                           dt->channels > 0 ? dt->channels : 4, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        case 2: {
            vio_cubemap_object *cm = (vio_cubemap_object *)obj;
            if (!cm->d3d12_resource) return -1;
            if (!cm->mipmaps) return 0;
            D3D12_RESOURCE_DESC rd; ID3D12Resource_GetDesc((ID3D12Resource *)cm->d3d12_resource, &rd);
            D3D12_RESOURCE_STATES st = cm->borrowed ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            return d3d12_generate_mips((ID3D12Resource *)cm->d3d12_resource, 6, (int)rd.MipLevels,
                                           (int)rd.Width, (int)rd.Height, 4, st);
        }
        default: return -1;
    }
}

/* vio_texture_update: sub-rectangle upload into a 2D texture (mip 0). */
static int d3d12_update_texture(void *tex_obj, const void *pixels, int x, int y, int w, int h)
{
    vio_texture_object *t = (vio_texture_object *)tex_obj;
    vio_d3d12_texture *dt = t ? (vio_d3d12_texture *)t->backend_texture : NULL;
    if (!dt || !dt->resource || !pixels || w <= 0 || h <= 0 || dt->depth > 0) return -1;
    int channels = dt->channels > 0 ? dt->channels : 4;
    /* Describe just the region so GetCopyableFootprints sizes the staging
     * buffer for w x h; the copy lands at (x, y) of mip 0. */
    D3D12_RESOURCE_DESC region;
    ID3D12Resource_GetDesc(dt->resource, &region);
    region.Width = (UINT64)w;
    region.Height = (UINT)h;
    region.MipLevels = 1;
    region.DepthOrArraySize = 1;
    const void *src = pixels;
    UINT pitch = (UINT)w * channels, rows = (UINT)h, slices = 1;
    return d3d12_upload_subresources(dt->resource, &region, 0, 1, &src, &pitch, &rows, &slices,
                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                     (UINT)x, (UINT)y, 0);
}

/* Glyph atlas filled on demand (A33): the atlas is an R8 texture from
 * create_texture (single_channel); the sub-region upload is update_texture's. */
static int d3d12_update_font_atlas(void *font_obj, const unsigned char *r8, int x, int y, int w, int h)
{
    vio_font_object *font = (vio_font_object *)font_obj;
    vio_texture_object shim;
    if (!font || !font->atlas_backend_texture || !r8) return -1;
    memset(&shim, 0, sizeof(shim));
    shim.backend_texture = font->atlas_backend_texture;
    shim.channels = 1;
    shim.width = font->atlas_w;
    shim.height = font->atlas_h;
    return d3d12_update_texture(&shim, r8, x, y, w, h);
}

/* ── Shaders ──────────────────────────────────────────────────────── */

/* HLSL target of the SPIRV-Cross transpile: the compile profile, so wave
 * intrinsics (subgroups, SM 6.0+) are emitted when DXC is in use. */
static int d3d12_hlsl_target(void)
{
    return vio_d3d12.shader_model == 6 ? vio_d3d12.shader_model_version : 51;
}

/* "vs_5_1" -> "vs_6_<minor>" under Shader Model 6, unchanged otherwise. */
static const char *d3d12_profile(const char *profile, char *buf, size_t n)
{
    if (vio_d3d12.shader_model != 6 || strlen(profile) < 6) return profile;
    snprintf(buf, n, "%.2s_%d_%d", profile, vio_d3d12.shader_model_version / 10, vio_d3d12.shader_model_version % 10);
    return buf;
}

/* D3DCompile with the on-disk DXBC cache (GAP-PHASE5 Block 4): the key hashes
 * the HLSL, the profile and the compile flags, so a debug build never reuses a
 * release blob and vice versa. Cached blobs are wrapped in an ID3DBlob so the
 * PSO / shader-object code stays unchanged. */
static HRESULT d3d12_compile_cached(const char *src, const char *entry_tag, const char *profile,
                                    UINT flags, ID3DBlob **out)
{
    /* The bindless table (root parameter VIO_D3D12_RP_BINDLESS) is an unbounded
     * SRV range; FXC refuses those without this flag (X3596). DXC accepts them
     * as is and ignores the D3DCOMPILE flags. Part of the cache key. */
    flags |= D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;
    /* Shader Model 6 (GAP-PHASE5 Block 7): the profile string becomes *_6_<minor>
     * and DXC produces DXIL; cached under "dxil" so FXC and DXC blobs never mix
     * (the profile, part of the key, keeps different minors apart). */
    char profile6[16];
    const char *ext = "dxbc";
    if (vio_d3d12.shader_model == 6) {
        profile = d3d12_profile(profile, profile6, sizeof(profile6));
        ext = "dxil";
    }
    uint64_t key = 0;
    int use_cache = vio_shader_cache_dir() != NULL;
    if (use_cache) {
        key = vio_shader_cache_hash(profile, src, strlen(src));
        key = vio_shader_cache_hash_more(key, &flags, sizeof(flags));
        size_t len = 0;
        void *data = vio_shader_cache_load(key, ext, &len);
        if (data) {
            if (SUCCEEDED(D3DCreateBlob(len, out)) && *out) {
                memcpy(ID3D10Blob_GetBufferPointer(*out), data, len);
                free(data);
                return S_OK;
            }
            free(data);
        }
    }
    HRESULT hr;
    if (vio_d3d12.shader_model == 6) {
        void *bytes = NULL; size_t len = 0; char *err = NULL;
        int rc = vio_dxc_compile(src, "main", profile, (flags & D3DCOMPILE_DEBUG) ? 1 : 0, vio_d3d12.native16,
                                 &bytes, &len, &err);
        if (rc != 0 || !bytes) {
            php_error_docref(NULL, E_WARNING, "D3D12: %s (DXC) compile failed: %s", profile, err ? err : "unknown");
            if (err) free(err);
            if (bytes) free(bytes);
            return E_FAIL;
        }
        if (err) free(err);   /* warnings */
        hr = D3DCreateBlob(len, out);
        if (FAILED(hr) || !*out) { free(bytes); return FAILED(hr) ? hr : E_FAIL; }
        memcpy(ID3D10Blob_GetBufferPointer(*out), bytes, len);
        free(bytes);
    } else {
        ID3DBlob *error_blob = NULL;
        hr = D3DCompile(src, strlen(src), entry_tag, NULL, NULL, "main", profile, flags, 0, out, &error_blob);
        if (FAILED(hr)) {
            php_error_docref(NULL, E_WARNING, "D3D12: %s compile failed: %s", profile,
                              error_blob ? (char *)ID3D10Blob_GetBufferPointer(error_blob) : "unknown");
            if (error_blob) ID3D10Blob_Release(error_blob);
            return hr;
        }
        if (error_blob) ID3D10Blob_Release(error_blob);
    }
    if (use_cache && *out) {
        vio_shader_cache_store(key, ext, ID3D10Blob_GetBufferPointer(*out), ID3D10Blob_GetBufferSize(*out));
    }
    return S_OK;
}

/* One optional stage (geometry / hull / domain): SPIR-V or GLSL -> HLSL ->
 * DXBC blob, same transpile path selection as the VS / PS code below. */
/* Hull / domain shader from the GLSL tessellation pair (vio_tess_hlsl.c). */
static ID3DBlob *d3d12_compile_tess_blob(vio_d3d12_shader *shader, int stage, int fixup_depth,
                                         const char *profile, const char *label, UINT compile_flags)
{
    vio_tess_hlsl_desc td = { shader->tess_tcs, shader->tess_tcs_size, shader->tess_tes, shader->tess_tes_size,
                              0, d3d12_hlsl_target(), fixup_depth };
    char *err = NULL;
    char *hlsl = vio_tess_to_hlsl(stage, &td, &err);
    if (!hlsl) {
        php_error_docref(NULL, E_WARNING, "D3D12: %s from GLSL: %s", label, err ? err : "unknown");
        free(err);
        return NULL;
    }
    ID3DBlob *blob = NULL;
    if (FAILED(d3d12_compile_cached(hlsl, label, profile, compile_flags, &blob))) blob = NULL;
    free(hlsl);
    return blob;
}

static ID3DBlob *d3d12_compile_stage_blob(const void *data, size_t size, int stage, int fixup_depth,
                                          const char *profile, const char *label,
                                          UINT compile_flags, vio_shader_format format,
                                          const char *hlsl_override)
{
    const char *hlsl = NULL;
    char *allocated = NULL;
    if (hlsl_override) {
        /* 'hlsl' => [stage => source] (VIO_FEATURE_HLSL_STAGE_OVERRIDE): compiled
         * as given; `data` is the GLSL stage's SPIR-V, used only to check the
         * cbuffer layout below. */
        hlsl = hlsl_override;
    } else if (format == VIO_SHADER_GLSL || format == VIO_SHADER_GLSL_RAW || format == VIO_SHADER_AUTO) {
        char *err = NULL;
        uint32_t *spirv = NULL;
        size_t spirv_size = 0;
        int free_spirv = 0;
        int is_spirv = (size >= 4 && *(const uint32_t *)data == 0x07230203);
        if (is_spirv) {
            spirv = (uint32_t *)data;
            spirv_size = size;
        } else {
            spirv = vio_compile_glsl_stage_to_spirv((const char *)data, stage, &spirv_size, &err);
            if (!spirv) {
                php_error_docref(NULL, E_WARNING, "D3D12: %s GLSL->SPIR-V failed: %s", label, err ? err : "unknown");
                if (err) free(err);
                return NULL;
            }
            free_spirv = 1;
        }
        allocated = vio_spirv_to_hlsl_ex(spirv, spirv_size, d3d12_hlsl_target(), fixup_depth, &err);
        if (free_spirv) free(spirv);
        if (!allocated) {
            php_error_docref(NULL, E_WARNING, "D3D12: %s SPIR-V->HLSL failed: %s", label, err ? err : "unknown");
            if (err) free(err);
            return NULL;
        }
        hlsl = allocated;
    } else {
        hlsl = (const char *)data;
    }

    /* Same compile path as VS / PS: the on-disk cache, and DXIL under Shader
     * Model 6 - a PSO cannot mix a DXBC geometry stage with DXIL VS / PS. */
    ID3DBlob *blob = NULL;
    HRESULT hr = d3d12_compile_cached(hlsl, label, profile, compile_flags, &blob);
    if (allocated) free(allocated);
    if (SUCCEEDED(hr) && hlsl_override && size >= 4 && *(const uint32_t *)data == 0x07230203) {
        vio_d3d_check_override_cbuffer(blob, data, size, "D3D12", label);
    }
    return SUCCEEDED(hr) ? blob : NULL;
}

/* Mesh pipeline (VIO_FEATURE_MESH_SHADER): SPIR-V mesh / task / fragment stages
 * -> HLSL ms / as / ps (SPIRV-Cross, SM 6.5+) -> DXIL. SPIRV-Cross applies no
 * clip-space fixup to mesh outputs, so vio maps every stored position's z from
 * GL's [-w, w] to [0, w] itself (y stays: the D3D convention vio uses). */
static void *d3d12_compile_mesh_shader(vio_shader_desc *desc, vio_d3d12_shader *shader)
{
    char *ms = NULL, *ps = NULL, *as = NULL, *err = NULL;
    const char *what = "mesh";
    shader->is_mesh = 1;
    shader->compile_flags = vio_d3d12.debug_enabled ? (D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION)
                                                    : D3DCOMPILE_OPTIMIZATION_LEVEL3;
    if (vio_d3d12.shader_model != 6 || vio_d3d12.shader_model_version < 65) {
        php_error_docref(NULL, E_WARNING, "D3D12: mesh shaders need Shader Model 6.5 (vio_create(['shader_model' => 6]) + DXC)");
        goto fail;
    }
    if (desc->fragment_size < 20 || *(const uint32_t *)desc->fragment_data != 0x07230203 ||
        (desc->task_data && (desc->task_size < 20 || *(const uint32_t *)desc->task_data != 0x07230203))) {
        php_error_docref(NULL, E_WARNING, "D3D12: mesh pipeline stages must reach the backend as SPIR-V");
        goto fail;
    }
    ms = vio_spirv_to_hlsl_ex((const uint32_t *)desc->vertex_data, desc->vertex_size, d3d12_hlsl_target(), 0, &err);
    if (!ms) goto translate_fail;
    ms = vio_mesh_fix_positions(ms, 0, 1, "float4");
    what = "fragment";
    ps = vio_spirv_to_hlsl((const uint32_t *)desc->fragment_data, desc->fragment_size, d3d12_hlsl_target(), &err);
    if (!ps) goto translate_fail;
    if (desc->task_data) {
        what = "task";
        as = vio_spirv_to_hlsl_ex((const uint32_t *)desc->task_data, desc->task_size, d3d12_hlsl_target(), 0, &err);
        if (!as) goto translate_fail;
    }
    if (getenv("VIO_DUMP_HLSL")) {
        if (as) fprintf(stderr, "==== D3D12 AS HLSL ====\n%s\n", as);
        fprintf(stderr, "==== D3D12 MS HLSL ====\n%s\n==== D3D12 PS HLSL ====\n%s\n==== end ====\n", ms, ps);
        fflush(stderr);
    }
    /* "xs_5_1" profiles become xs_6_<minor> under Shader Model 6. */
    if (FAILED(d3d12_compile_cached(ms, "MS", "ms_5_1", shader->compile_flags, &shader->vs_blob))) goto fail;
    if (FAILED(d3d12_compile_cached(ps, "PS", "ps_5_1", shader->compile_flags, &shader->ps_blob))) goto fail;
    if (as && FAILED(d3d12_compile_cached(as, "AS", "as_5_1", shader->compile_flags, &shader->as_blob))) goto fail;
    free(ms); free(ps); free(as);
    return shader;

translate_fail:
    php_error_docref(NULL, E_WARNING, "D3D12: %s SPIR-V->HLSL failed: %s", what, err ? err : "unknown");
    free(err);
fail:
    free(ms); free(ps); free(as);
    if (shader->vs_blob) ID3D10Blob_Release(shader->vs_blob);
    if (shader->ps_blob) ID3D10Blob_Release(shader->ps_blob);
    if (shader->as_blob) ID3D10Blob_Release(shader->as_blob);
    free(shader);
    return NULL;
}

static void *d3d12_compile_shader(vio_shader_desc *desc)
{
    vio_d3d12_shader *shader = calloc(1, sizeof(vio_d3d12_shader));
    if (!shader) return NULL;
    if (desc->vertex_data && vio_spirv_execution_model(desc->vertex_data, desc->vertex_size) == VIO_SPIRV_MODEL_MESH_EXT)
        return d3d12_compile_mesh_shader(desc, shader);
    /* gl_PrimitiveShadingRateEXT (BuiltIn PrimitiveShadingRateKHR = 4432). */
    shader->writes_shading_rate = desc->vertex_data && vio_spirv_has_builtin(desc->vertex_data, desc->vertex_size, 4432);

    const char *hlsl_vs = NULL;
    const char *hlsl_ps = NULL;
    char *allocated_vs = NULL;
    char *allocated_ps = NULL;

    if (desc->format == VIO_SHADER_GLSL || desc->format == VIO_SHADER_GLSL_RAW || desc->format == VIO_SHADER_AUTO) {
        char *err = NULL;
        uint32_t *vs_spirv = NULL;
        uint32_t *ps_spirv = NULL;
        size_t vs_spirv_size = 0, ps_spirv_size = 0;
        int free_vs_spirv = 0, free_ps_spirv = 0;

        /* Check if data is already SPIR-V */
        int vs_is_spirv = (desc->vertex_size >= 4 &&
            *(const uint32_t *)desc->vertex_data == 0x07230203);
        int ps_is_spirv = (desc->fragment_size >= 4 &&
            *(const uint32_t *)desc->fragment_data == 0x07230203);

        if (vs_is_spirv) {
            vs_spirv = (uint32_t *)desc->vertex_data;
            vs_spirv_size = desc->vertex_size;
        } else {
            vs_spirv = vio_compile_glsl_to_spirv(
                (const char *)desc->vertex_data, 0, &vs_spirv_size, &err);
            if (!vs_spirv) {
                php_error_docref(NULL, E_WARNING, "D3D12: VS GLSL->SPIR-V failed: %s", err ? err : "unknown");
                if (err) free(err);
                free(shader);
                return NULL;
            }
            free_vs_spirv = 1;
        }

        if (ps_is_spirv) {
            ps_spirv = (uint32_t *)desc->fragment_data;
            ps_spirv_size = desc->fragment_size;
        } else {
            ps_spirv = vio_compile_glsl_to_spirv(
                (const char *)desc->fragment_data, 1, &ps_spirv_size, &err);
            if (!ps_spirv) {
                php_error_docref(NULL, E_WARNING, "D3D12: PS GLSL->SPIR-V failed: %s", err ? err : "unknown");
                if (err) free(err);
                if (free_vs_spirv) free(vs_spirv);
                free(shader);
                return NULL;
            }
            free_ps_spirv = 1;
        }

        /* SPIR-V -> HLSL SM 5.1. The GL->D3D depth fixup goes on the LAST
         * stage that writes gl_Position (GS, else DS, else VS). */
        int vs_is_last = !desc->geometry_data && !desc->tess_eval_data;
        allocated_vs = vio_spirv_to_hlsl_ex(vs_spirv, vs_spirv_size, d3d12_hlsl_target(), vs_is_last, &err);
        if (free_vs_spirv) free(vs_spirv);
        if (!allocated_vs) {
            php_error_docref(NULL, E_WARNING, "D3D12: VS SPIR-V->HLSL failed: %s", err ? err : "unknown");
            if (err) free(err);
            if (free_ps_spirv) free(ps_spirv);
            free(shader);
            return NULL;
        }

        allocated_ps = vio_spirv_to_hlsl(ps_spirv, ps_spirv_size, d3d12_hlsl_target(), &err);
        if (free_ps_spirv) free(ps_spirv);
        if (!allocated_ps) {
            php_error_docref(NULL, E_WARNING, "D3D12: PS SPIR-V->HLSL failed: %s", err ? err : "unknown");
            if (err) free(err);
            free(allocated_vs);
            free(shader);
            return NULL;
        }

        hlsl_vs = allocated_vs;
        hlsl_ps = allocated_ps;
    } else {
        hlsl_vs = (const char *)desc->vertex_data;
        hlsl_ps = (const char *)desc->fragment_data;
    }

    /* FXC defaults to OPTIMIZATION_LEVEL1; LEVEL3 is the release codegen the
     * driver-side JIT benefits from (GAP-PLAN 2.8). Debug builds keep the
     * un-optimised, symbol-carrying blob for PIX / RenderDoc. */
    UINT compile_flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    if (vio_d3d12.debug_enabled) {
        compile_flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    }

    HRESULT hr;

    /* 'hlsl' => ['fragment' => src]: the pixel shader as given (sampler
     * feedback has no GLSL form); the GLSL fragment stage only defines the
     * cbuffer layout, checked below like the other overrides. */
    if (desc->fragment_hlsl) hlsl_ps = desc->fragment_hlsl;

    /* Bindless table: SPIRV-Cross keeps Set 1 in register space 1. */
    shader->uses_draw_params = hlsl_vs && strstr(hlsl_vs, "SPIRV_Cross_VertexInfo") != NULL;
    shader->uses_bindless = (hlsl_vs && (strstr(hlsl_vs, "space1)") || strstr(hlsl_vs, "space3)") || strstr(hlsl_vs, "space4)")))
                         || (hlsl_ps && (strstr(hlsl_ps, "space1)") || strstr(hlsl_ps, "space3)") || strstr(hlsl_ps, "space4)")));
    shader->uses_feedback = hlsl_ps && strstr(hlsl_ps, "FeedbackTexture2D") != NULL;
    if (shader->uses_feedback && !vio_d3d12.sampler_feedback) {
        php_error_docref(NULL, E_WARNING, "D3D12: the pixel shader writes sampler feedback but the device has none "
                         "(VIO_FEATURE_SAMPLER_FEEDBACK = 0: needs SM 6.5 via shader_model 6, SamplerFeedbackTier 0.9)");
        goto fail;
    }
    hr = d3d12_compile_cached(hlsl_vs, "vs_main", "vs_5_1", compile_flags, &shader->vs_blob);
    if (FAILED(hr)) goto fail;

    hr = d3d12_compile_cached(hlsl_ps, "ps_main", "ps_5_1", compile_flags, &shader->ps_blob);
    if (FAILED(hr)) goto fail;
    if (desc->fragment_hlsl && desc->fragment_size >= 4 && *(const uint32_t *)desc->fragment_data == 0x07230203)
        vio_d3d_check_override_cbuffer(shader->ps_blob, desc->fragment_data, desc->fragment_size, "D3D12", "PS");

    /* GLSL tessellation without an HLSL override: vio translates both stages
     * together (vio_tess_hlsl.c) and keeps their SPIR-V for hull shader
     * variants per patch size (d3d12_create_pipeline). */
    shader->compile_flags = compile_flags;
    int tess_generated = desc->tess_control_data && desc->tess_eval_data &&
        (!desc->tess_control_hlsl || !desc->tess_eval_hlsl) &&
        (desc->format == VIO_SHADER_GLSL || desc->format == VIO_SHADER_GLSL_RAW || desc->format == VIO_SHADER_AUTO);
    if (tess_generated) {
        char *err = NULL;
        shader->tess_tcs = vio_tess_stage_spirv(desc->tess_control_data, desc->tess_control_size, VIO_STAGE_TESS_CONTROL,
                                                &shader->tess_tcs_size, &err);
        if (shader->tess_tcs)
            shader->tess_tes = vio_tess_stage_spirv(desc->tess_eval_data, desc->tess_eval_size, VIO_STAGE_TESS_EVAL,
                                                    &shader->tess_tes_size, &err);
        if (!shader->tess_tes) {
            php_error_docref(NULL, E_WARNING, "D3D12: tessellation GLSL->SPIR-V failed: %s", err ? err : "unknown");
            free(err);
            goto fail;
        }
        shader->hs_input_points = vio_tess_output_vertices(shader->tess_tcs, shader->tess_tcs_size);
    }

    /* Optional stages: geometry (gs_5_1), hull (hs_5_1), domain (ds_5_1). */
    if (desc->geometry_data) {
        shader->gs_blob = d3d12_compile_stage_blob(desc->geometry_data, desc->geometry_size,
                                                   VIO_STAGE_GEOMETRY, 1, "gs_5_1", "GS",
                                                   compile_flags, desc->format, desc->geometry_hlsl);
        if (!shader->gs_blob) goto fail;
    }
    if (desc->tess_control_data) {
        if (tess_generated && !desc->tess_control_hlsl)
            shader->hs_blob = d3d12_compile_tess_blob(shader, VIO_STAGE_TESS_CONTROL, 0, "hs_5_1", "HS", compile_flags);
        else
            shader->hs_blob = d3d12_compile_stage_blob(desc->tess_control_data, desc->tess_control_size,
                                                       VIO_STAGE_TESS_CONTROL, 0, "hs_5_1", "HS",
                                                       compile_flags, desc->format, desc->tess_control_hlsl);
        if (!shader->hs_blob) goto fail;
    }
    if (desc->tess_eval_data) {
        if (tess_generated && !desc->tess_eval_hlsl)
            shader->ds_blob = d3d12_compile_tess_blob(shader, VIO_STAGE_TESS_EVAL, desc->geometry_data ? 0 : 1,
                                                      "ds_5_1", "DS", compile_flags);
        else
            shader->ds_blob = d3d12_compile_stage_blob(desc->tess_eval_data, desc->tess_eval_size,
                                                       VIO_STAGE_TESS_EVAL, desc->geometry_data ? 0 : 1,
                                                       "ds_5_1", "DS", compile_flags, desc->format, desc->tess_eval_hlsl);
        if (!shader->ds_blob) goto fail;
    }

    if (allocated_vs) free(allocated_vs);
    if (allocated_ps) free(allocated_ps);
    return shader;

fail:
    if (allocated_vs) free(allocated_vs);
    if (allocated_ps) free(allocated_ps);
    if (shader->vs_blob) ID3D10Blob_Release(shader->vs_blob);
    if (shader->ps_blob) ID3D10Blob_Release(shader->ps_blob);
    if (shader->gs_blob) ID3D10Blob_Release(shader->gs_blob);
    if (shader->hs_blob) ID3D10Blob_Release(shader->hs_blob);
    if (shader->ds_blob) ID3D10Blob_Release(shader->ds_blob);
    free(shader->tess_tcs);
    free(shader->tess_tes);
    free(shader);
    return NULL;
}

static void d3d12_gpu_info(const char **name, uint64_t *vram_bytes)
{
    if (!vio_d3d12.initialized) return;
    *name = vio_d3d12.gpu_name;
    *vram_bytes = vio_d3d12.vram_bytes;
}

static int d3d12_supports_feature(vio_feature feature);

/* vio_adapters (A6): a throwaway device on the adapter answers the hardware
 * tiers (OS runtime; the Agility SDK may report more on the context). */
static int d3d12_probe_adapter(IDXGIAdapter1 *adapter, vio_adapter_info *a)
{
    ID3D12Device *dev = NULL;
    if (FAILED(D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev)) || !dev)
        return -1;
    static const int base[] = { VIO_FEATURE_COMPUTE, VIO_FEATURE_3D_PIPELINE, VIO_FEATURE_GEOMETRY, VIO_FEATURE_TESSELLATION,
                                VIO_FEATURE_MULTI_VIEWPORT, VIO_FEATURE_INDIRECT_DRAW, VIO_FEATURE_TEXTURE_COMPRESSION_BC };
    memset(&a->features, 0, sizeof(a->features));
    for (size_t i = 0; i < sizeof(base) / sizeof(base[0]); i++) vio_featset_add(&a->features, base[i]);
    D3D12_FEATURE_DATA_D3D12_OPTIONS o = {0};
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o)))
        && o.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3)
        vio_featset_add(&a->features, VIO_FEATURE_BINDLESS);
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1 = {0};
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1))) && o1.WaveOps)
        vio_featset_add(&a->features, VIO_FEATURE_SUBGROUP);
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3 = {0};
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3)))) {
        if (o3.ViewInstancingTier > D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED) vio_featset_add(&a->features, VIO_FEATURE_MULTIVIEW);
        if (o3.BarycentricsSupported) vio_featset_add(&a->features, VIO_FEATURE_BARYCENTRICS);
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {0};
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5)))) {
        if (o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0) vio_featset_add(&a->features, VIO_FEATURE_RAYTRACING);
        if (o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1) vio_featset_add(&a->features, VIO_FEATURE_RAY_QUERY);
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6 = {0};
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6)))) {
        if (o6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_1) vio_featset_add(&a->features, VIO_FEATURE_SHADING_RATE);
        if (o6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_2)
            { vio_featset_add(&a->features, VIO_FEATURE_SHADING_RATE_IMAGE); vio_featset_add(&a->features, VIO_FEATURE_SHADING_RATE_PRIMITIVE); }
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {0};
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7)))) {
        if (o7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1) vio_featset_add(&a->features, VIO_FEATURE_MESH_SHADER);
        if (o7.SamplerFeedbackTier >= D3D12_SAMPLER_FEEDBACK_TIER_0_9) vio_featset_add(&a->features, VIO_FEATURE_SAMPLER_FEEDBACK);
    }
    if (!a->device_type) {
        D3D12_FEATURE_DATA_ARCHITECTURE1 arch = {0};
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_ARCHITECTURE1, &arch, sizeof(arch))))
            a->device_type = arch.UMA ? "integrated" : "discrete";
    }
    ID3D12Device_Release(dev);
    return 0;
}

static int d3d12_enumerate_adapters(vio_adapter_info *out, int max)
{
    return vio_dxgi_enumerate_adapters(out, max, d3d12_probe_adapter);
}

/* vio_backend_info (A4): feature level, shader model in use / of the device,
 * adapter identity and the optional features. */
static int d3d12_describe(vio_backend_description *out)
{
    static char fl_name[16];
    if (!vio_d3d12.initialized || !vio_d3d12.device || !out) return -1;
    /* 12_2 (0xc200) first; runtimes that do not know it reject the query. */
    static const D3D_FEATURE_LEVEL levels[] = { (D3D_FEATURE_LEVEL)0xc200, D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    int flv = D3D_FEATURE_LEVEL_11_0;
    for (int first = 0; first < 2; first++) {
        D3D12_FEATURE_DATA_FEATURE_LEVELS fl = { (UINT)(5 - first), levels + first, D3D_FEATURE_LEVEL_11_0 };
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(vio_d3d12.device, D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl)))) {
            flv = (int)fl.MaxSupportedFeatureLevel;
            break;
        }
    }
    snprintf(fl_name, sizeof(fl_name), "fl_%d_%d", (flv >> 12) & 0xF, (flv >> 8) & 0xF);
    int sm_max = 51;
    for (int v = 0x69; v >= 0x60; v--) {
        D3D12_FEATURE_DATA_SHADER_MODEL sm = { (D3D_SHADER_MODEL)v };
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(vio_d3d12.device, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
            if ((int)sm.HighestShaderModel >= 0x60)
                sm_max = ((int)sm.HighestShaderModel >> 4) * 10 + ((int)sm.HighestShaderModel & 0xF);
            break;
        }
    }
    out->api = "Direct3D 12";
    out->device = vio_d3d12.gpu_name;
    out->shading_language = "HLSL";
    out->shading_language_version = vio_d3d12.shader_model == 6 ? vio_d3d12.shader_model_version : 51;
    out->shading_language_max = sm_max > out->shading_language_version ? sm_max : out->shading_language_version;
    out->family_count = 0;
    out->families[out->family_count++] = fl_name;
    out->families[out->family_count++] = vio_d3d12.shader_model == 6 ? "dxil" : "dxbc";
    if (vio_d3d12.agility_sdk) out->families[out->family_count++] = "agility_sdk";
    out->cap_count = 0;
    vio_describe_feature_caps(out, d3d12_supports_feature);
    out->vendor_id = vio_d3d12.vendor_id;
    out->driver = vio_d3d12.driver;
    out->vram_bytes = vio_d3d12.vram_bytes;
    if (vio_d3d12.software_adapter) out->device_type = "software";
    else {
        D3D12_FEATURE_DATA_ARCHITECTURE1 arch = {0};
        out->device_type = SUCCEEDED(ID3D12Device_CheckFeatureSupport(vio_d3d12.device, D3D12_FEATURE_ARCHITECTURE1, &arch, sizeof(arch)))
            ? (arch.UMA ? "integrated" : "discrete") : NULL;
    }
    return 0;
}

static void d3d12_destroy_shader(void *shader_ptr)
{
    vio_d3d12_shader *s = (vio_d3d12_shader *)shader_ptr;
    if (!s) return;
    if (s->vs_blob) ID3D10Blob_Release(s->vs_blob);
    if (s->ps_blob) ID3D10Blob_Release(s->ps_blob);
    if (s->gs_blob) ID3D10Blob_Release(s->gs_blob);
    if (s->hs_blob) ID3D10Blob_Release(s->hs_blob);
    if (s->ds_blob) ID3D10Blob_Release(s->ds_blob);
    if (s->as_blob) ID3D10Blob_Release(s->as_blob);
    free(s->tess_tcs);
    free(s->tess_tes);
    free(s);
}

/* Bind the constant block of a geometry / hull / domain stage: copy `data`
 * into a fresh 256-byte-aligned slice of this frame's cbuffer ring and point
 * the stage's root CBV at it (same scheme as the VS / PS slices pushed by
 * php_vio.c - D3D12 has no buffer renaming, so every draw gets its own). */
static void d3d12_bind_stage_constants(int stage, void *backend_buffer,
                                       const void *data, size_t size)
{
    (void)backend_buffer;
    if (!vio_d3d12.cmd_list || !vio_d3d12.cbuffer_heap_mapped || !data || size == 0) return;
    UINT param;
    switch (stage) {
        case VIO_STAGE_GEOMETRY:     param = VIO_D3D12_RP_GS_CBV; break;
        case VIO_STAGE_TESS_CONTROL: param = VIO_D3D12_RP_HS_CBV; break;
        case VIO_STAGE_TESS_EVAL:    param = VIO_D3D12_RP_DS_CBV; break;
        default: return;
    }
    UINT aligned = (UINT)((size + 255) & ~(size_t)255);
    UINT offset = vio_d3d12.cbuffer_heap_offset;
    if (offset + aligned > vio_d3d12.cbuffer_frame_end) return;   /* slice exhausted; grows next frame */
    memcpy(vio_d3d12.cbuffer_heap_mapped + offset, data, size);
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(
        vio_d3d12.cmd_list, param, vio_d3d12.cbuffer_heap_gpu + offset);
    vio_d3d12.cbuffer_heap_offset = offset + aligned;
}

/* ── Drawing ──────────────────────────────────────────────────────── */

static void d3d12_begin_frame(void)
{
    vio_d3d12_frame *frame = &vio_d3d12.frames[vio_d3d12.frame_index];

    /* If the device was already lost on a prior frame, do not touch the command
     * list / allocator (Reset on a removed device fails and would re-crash).
     * The loss was already logged once with full DRED context. */
    if (vio_d3d12.device_lost) return;

    /* A device can be lost asynchronously (TDR/hang) without a Present failing
     * yet — catch it here too so the DRED dump fires from the actual frame that
     * was in flight when the GPU hung, not one frame later. */
    if (vio_d3d12.device) {
        HRESULT dr = ID3D12Device_GetDeviceRemovedReason(vio_d3d12.device);
        if (FAILED(dr)) {
            d3d12_handle_device_removed("begin_frame", dr);
            return;
        }
    }

    /* Drain any validation messages from the last frame so the warning that
     * actually killed the GPU (badly-formed command, resource-state mismatch
     * etc.) shows up next to the symptomatic crash rather than the Windows
     * event log. Drain on every frame; the InfoQueue normally only fills up
     * on real errors, so the spam stays bounded in practice. */
    d3d12_drain_info_queue("begin_frame");
    d3d12_bundle_graves_collect(0);

    /* Waitable swapchain: block until DXGI has a backbuffer for us (caps the
     * CPU at frame_latency frames ahead — the input-latency control). A bounded
     * wait so a stalled compositor cannot hang the process. */
    if (vio_d3d12.frame_latency_waitable) {
        WaitForSingleObjectEx(vio_d3d12.frame_latency_waitable, 1000, TRUE);
    }

    /* Wait for this frame's previous work to complete */
    d3d12_wait_for_frame(vio_d3d12.frame_index);
    /* This slot's previous command list has retired: PSOs parked while it was
     * recording can go now (see d3d12_destroy_pipeline). */
    d3d12_release_pending_psos(vio_d3d12.frame_index);
    d3d12_release_parked(vio_d3d12.frame_index);
    vio_d3d12.dp_used[vio_d3d12.frame_index] = 0;
    /* Staging buffers of uploads whose fence has passed (GAP-PLAN 4.1). */
    d3d12_retire_uploads(0);

    /* This slot's previous frame has retired (wait_for_frame above): read its
     * GPU timestamps before the slot is reused. */
    if (vio_d3d12.ts_readback && vio_d3d12.ts_pending[vio_d3d12.frame_index]) {
        UINT64 *ts = NULL;
        const vio_gpu_mark_names *marks = &vio_d3d12.ts_marks[vio_d3d12.frame_index];
        SIZE_T base = (SIZE_T)vio_d3d12.frame_index * VIO_GPU_TS_PER_FRAME;
        D3D12_RANGE rr = { base * 8, (base + 2 + (SIZE_T)marks->count) * 8 };
        if (SUCCEEDED(ID3D12Resource_Map(vio_d3d12.ts_readback, 0, &rr, (void **)&ts)) && ts) {
            UINT64 b = ts[base], e = ts[base + 1];
            if (e > b && vio_d3d12.ts_frequency) {
                vio_d3d12.last_gpu_ms = (double)(e - b) * 1000.0 / (double)vio_d3d12.ts_frequency;
                vio_gpu_mark_resolve(&vio_d3d12.ts_result, marks, (const uint64_t *)&ts[base],
                                     1000.0 / (double)vio_d3d12.ts_frequency);
                vio_d3d12.ts_result_valid = 1;
            }
            D3D12_RANGE wr = {0, 0};
            ID3D12Resource_Unmap(vio_d3d12.ts_readback, 0, &wr);
        }
        vio_d3d12.ts_pending[vio_d3d12.frame_index] = 0;
    }

    vio_d3d12.in_frame = 1;
    vio_d3d12.frame_serial++;   /* indirect-draw UAV tracking (Block 8) */

    /* Reset command allocator and command list */
    ID3D12CommandAllocator_Reset(frame->cmd_allocator);
    ID3D12GraphicsCommandList_Reset(vio_d3d12.cmd_list, frame->cmd_allocator, NULL);
    if (vio_d3d12.shading_rate != VIO_SHADING_RATE_1X1 || vio_d3d12.vrs_image_active) d3d12_apply_shading_rate();   /* VRS is list state */
    vio_d3d12.ts_marks[vio_d3d12.frame_index].count = 0;
    if (vio_d3d12.ts_heap) {
        ID3D12GraphicsCommandList_EndQuery(vio_d3d12.cmd_list, vio_d3d12.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP,
                                           (UINT)vio_d3d12.frame_index * VIO_GPU_TS_PER_FRAME);
    }

    /* Transition render target: PRESENT -> RENDER_TARGET */
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame->render_target;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(vio_d3d12.cmd_list, 1, &barrier);

    /* Set render target */
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.dsv_heap, &dsv_handle);
    ID3D12GraphicsCommandList_OMSetRenderTargets(vio_d3d12.cmd_list, 1,
                                                  &frame->rtv_handle, FALSE, &dsv_handle);

    /* Track current render target */
    vio_d3d12.current_rt_samples = 1;
    vio_d3d12.current_rt_format = vio_d3d12.swapchain_format;
    vio_d3d12.current_rtv = frame->rtv_handle;
    vio_d3d12.current_rtvs[0] = frame->rtv_handle;
    vio_d3d12.current_rtv_count = 1;
    vio_d3d12.current_dsv = dsv_handle;
    vio_d3d12.current_rt_width = vio_d3d12.width;
    vio_d3d12.current_rt_height = vio_d3d12.height;
    vio_d3d12.current_has_rtv = 1;

    /* Apply a vio_clear() latched before vio_begin() (colour + depth). */
    if (vio_d3d12.clear_pending) {
        vio_d3d12.clear_pending = 0;
        float color[4] = {vio_d3d12.clear_r, vio_d3d12.clear_g, vio_d3d12.clear_b, vio_d3d12.clear_a};
        ID3D12GraphicsCommandList_ClearRenderTargetView(vio_d3d12.cmd_list, frame->rtv_handle, color, 0, NULL);
        ID3D12GraphicsCommandList_ClearDepthStencilView(vio_d3d12.cmd_list, dsv_handle,
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
    }

    /* Grow cbuffer heap if last frame used >75% of its per-frame slice */
    UINT cb_slice = vio_d3d12.cbuffer_heap_capacity / vio_d3d12.frame_count;
    UINT cb_last_used = vio_d3d12.cbuffer_heap_offset - vio_d3d12.cbuffer_frame_base;
    if (cb_last_used > cb_slice * 3 / 4) {
        UINT new_size = vio_d3d12.cbuffer_heap_capacity * 2;
        if (new_size > 256 * 1024 * 1024) new_size = 256 * 1024 * 1024; /* cap at 256MB */

        /* Full GPU sync before releasing the old heap.
         *
         * d3d12_wait_for_frame() above only waited for THIS frame slot's
         * previous use. With FRAME_COUNT=2 the OTHER frame's command list
         * is still in flight and references the old heap via root CBV
         * (SetGraphicsRootConstantBufferView with raw GPU virtual address).
         * The runtime does NOT track resources used via root descriptors —
         * Releasing the resource while the GPU still reads its VA is
         * undefined behaviour and produces a complete-frame flicker. */
        vio_d3d12_wait_for_gpu();

        /* Release old heap (now safe — all GPU work has completed) */
        if (vio_d3d12.cbuffer_heap) {
            ID3D12Resource_Unmap(vio_d3d12.cbuffer_heap, 0, NULL);
            ID3D12Resource_Release(vio_d3d12.cbuffer_heap);
            vio_d3d12.cbuffer_heap = NULL;
            vio_d3d12.cbuffer_heap_mapped = NULL;
        }

        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = new_size;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (SUCCEEDED(ID3D12Device_CreateCommittedResource(vio_d3d12.device,
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                NULL, &IID_ID3D12Resource, (void **)&vio_d3d12.cbuffer_heap))) {
            vio_d3d12.cbuffer_heap_gpu = ID3D12Resource_GetGPUVirtualAddress(vio_d3d12.cbuffer_heap);
            vio_d3d12.cbuffer_heap_capacity = new_size;
            D3D12_RANGE rr = {0, 0};
            ID3D12Resource_Map(vio_d3d12.cbuffer_heap, 0, &rr, (void **)&vio_d3d12.cbuffer_heap_mapped);
            php_error_docref(NULL, E_NOTICE, "D3D12: cbuffer heap grown to %u MB", new_size / (1024*1024));
        }
    }

    /* Grow instance heap if last frame used >75% of its per-frame slice.
     * Same sync discipline as the cbuffer grow above: the slot-1 VBV references
     * this heap by raw GPU VA (untracked by the runtime), so the OTHER in-flight
     * frame may still be reading the old heap. Full GPU sync BEFORE Release. */
    {
        UINT inst_slice = vio_d3d12.instance_heap_capacity / vio_d3d12.frame_count;
        UINT inst_last_used = vio_d3d12.instance_heap_offset - vio_d3d12.instance_frame_base;
        if (inst_slice > 0 && inst_last_used > inst_slice * 3 / 4) {
            UINT new_size = vio_d3d12.instance_heap_capacity * 2;
            if (new_size > 256 * 1024 * 1024) new_size = 256 * 1024 * 1024; /* cap at 256MB */

            vio_d3d12_wait_for_gpu();

            if (vio_d3d12.instance_heap) {
                ID3D12Resource_Unmap(vio_d3d12.instance_heap, 0, NULL);
                ID3D12Resource_Release(vio_d3d12.instance_heap);
                vio_d3d12.instance_heap = NULL;
                vio_d3d12.instance_heap_mapped = NULL;
            }

            D3D12_HEAP_PROPERTIES hp = {0};
            hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC rd = {0};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = new_size;
            rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            if (SUCCEEDED(ID3D12Device_CreateCommittedResource(vio_d3d12.device,
                    &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                    NULL, &IID_ID3D12Resource, (void **)&vio_d3d12.instance_heap))) {
                vio_d3d12.instance_heap_gpu = ID3D12Resource_GetGPUVirtualAddress(vio_d3d12.instance_heap);
                vio_d3d12.instance_heap_capacity = new_size;
                D3D12_RANGE rr = {0, 0};
                ID3D12Resource_Map(vio_d3d12.instance_heap, 0, &rr, (void **)&vio_d3d12.instance_heap_mapped);
                php_error_docref(NULL, E_NOTICE, "D3D12: instance heap grown to %u MB", new_size / (1024*1024));
            }
        }
    }

    /* Reset per-frame allocators.
     *
     * Static SRVs occupy [capacity - srv_heap.count, capacity), growing
     * downward as more textures load. The per-frame regions live in
     * [0, capacity - srv_heap.count), split into vio_d3d12.frame_count
     * equal slices indexed by frame_index. The two regions never overlap
     * (until the heap is genuinely full), so a texture created mid-frame
     * gets a high-index SRV that's outside every frame's per-frame slice
     * and is therefore safe from the null-init sweep in flush_srv_table. */
    /* Rebase the cbuffer allocator into THIS frame's slice. The other frame
     * in flight keeps reading its own slice — never overwritten from here. */
    cb_slice = vio_d3d12.cbuffer_heap_capacity / vio_d3d12.frame_count;
    vio_d3d12.cbuffer_frame_base = vio_d3d12.frame_index * cb_slice;
    vio_d3d12.cbuffer_frame_end  = vio_d3d12.cbuffer_frame_base + cb_slice;
    vio_d3d12.cbuffer_heap_offset = vio_d3d12.cbuffer_frame_base;
    /* Rebase the instance allocator into THIS frame's slice (same as cbuffer). */
    UINT inst_slice = vio_d3d12.instance_heap_capacity / vio_d3d12.frame_count;
    vio_d3d12.instance_frame_base = vio_d3d12.frame_index * inst_slice;
    vio_d3d12.instance_frame_end  = vio_d3d12.instance_frame_base + inst_slice;
    vio_d3d12.instance_heap_offset = vio_d3d12.instance_frame_base;
    UINT perframe_total = (vio_d3d12.srv_heap.capacity > vio_d3d12.srv_heap.count)
                           ? (vio_d3d12.srv_heap.capacity - vio_d3d12.srv_heap.count) : 0;
    vio_d3d12.srv_frame_capacity = perframe_total / vio_d3d12.frame_count;
    vio_d3d12.srv_frame_base     = vio_d3d12.frame_index * vio_d3d12.srv_frame_capacity;
    vio_d3d12.srv_frame_offset   = vio_d3d12.srv_frame_base;
    memset(vio_d3d12.pending_srv_valid, 0, sizeof(vio_d3d12.pending_srv_valid));
    memset(vio_d3d12.pending_samplers, 0, sizeof(vio_d3d12.pending_samplers));
    /* The per-frame SRV ring just rebased: last frame's cached descriptor block
     * lives in (possibly) the other in-flight frame's region, so its GPU handle
     * is stale. Force the first flush of THIS frame to rebuild. */
    vio_d3d12.srv_table_bound = 0;
    /* Same for the sampler ring + its per-frame set cache. */
    vio_d3d12.sampler_frame_capacity = (VIO_D3D12_SAMPLER_HEAP_CAPACITY - VIO_D3D12_BUNDLE_SAMPLER_RESERVE) / vio_d3d12.frame_count;
    vio_d3d12.sampler_frame_base     = vio_d3d12.frame_index * vio_d3d12.sampler_frame_capacity;
    vio_d3d12.sampler_frame_offset   = vio_d3d12.sampler_frame_base;
    vio_d3d12.sampler_set_count      = 0;
    vio_d3d12.sampler_table_bound    = 0;

    /* Set viewport and scissor */
    D3D12_VIEWPORT vp = {0};
    vp.Width = (float)vio_d3d12.width;
    vp.Height = (float)vio_d3d12.height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D12GraphicsCommandList_RSSetViewports(vio_d3d12.cmd_list, 1, &vp);

    D3D12_RECT scissor = {0, 0, vio_d3d12.width, vio_d3d12.height};
    ID3D12GraphicsCommandList_RSSetScissorRects(vio_d3d12.cmd_list, 1, &scissor);

    /* The list was reset: re-arm the pipeline bound in an earlier frame, which
     * GL, D3D11 and Vulkan keep across vio_end / vio_begin as well. A draw
     * without it ran with no PSO / root signature and removed the device. */
    if (d3d12_current_pipeline) d3d12_bind_pipeline(d3d12_current_pipeline);
}

static void d3d12_end_frame(void)
{
    /* begin_frame bailed (device lost, or it was never opened) — the command
     * list is closed/stale, so there is nothing to transition, close or
     * execute. Guarding here keeps a post-loss frame from recording onto a dead
     * list. */
    if (vio_d3d12.device_lost || !vio_d3d12.in_frame) return;

    vio_d3d12_frame *frame = &vio_d3d12.frames[vio_d3d12.frame_index];

    /* Transition render target: RENDER_TARGET -> PRESENT */
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame->render_target;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(vio_d3d12.cmd_list, 1, &barrier);

    /* GPU timestamp: end of the frame's command stream, resolved into this
     * slot's readback range; begin_frame reads it once the fence has passed. */
    if (vio_d3d12.ts_heap && vio_d3d12.ts_readback) {
        UINT q = (UINT)vio_d3d12.frame_index * VIO_GPU_TS_PER_FRAME;
        ID3D12GraphicsCommandList_EndQuery(vio_d3d12.cmd_list, vio_d3d12.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, q + 1);
        ID3D12GraphicsCommandList_ResolveQueryData(vio_d3d12.cmd_list, vio_d3d12.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP,
                                                   q, 2 + (UINT)vio_d3d12.ts_marks[vio_d3d12.frame_index].count,
                                                   vio_d3d12.ts_readback, (UINT64)q * 8);
        vio_d3d12.ts_pending[vio_d3d12.frame_index] = 1;
    }

    /* Close and execute command list */
    ID3D12GraphicsCommandList_Close(vio_d3d12.cmd_list);
    vio_d3d12.in_frame = 0;

    ID3D12CommandList *cmd_lists[] = { (ID3D12CommandList *)vio_d3d12.cmd_list };
    ID3D12CommandQueue_ExecuteCommandLists(vio_d3d12.cmd_queue, 1, cmd_lists);
}

static void d3d12_draw(vio_draw_cmd *cmd)
{
    if (!cmd) return;
    /* Nothing bound yet: no PSO / root signature on the list, and a draw there
     * removes the device. GL, D3D11 and Vulkan draw nothing as well. */
    if (!d3d12_current_pipeline) return;

    vio_d3d12_buffer *vb = (vio_d3d12_buffer *)cmd->vertex_buffer;
    if (vb) {
        UINT stride = cmd->vertex_stride > 0 ? (UINT)cmd->vertex_stride
                    : (d3d12_current_pipeline ? d3d12_current_pipeline->vertex_stride : 0);
        D3D12_VERTEX_BUFFER_VIEW vbvs[2];
        /* Slot 0: mesh vertex data */
        vbvs[0].BufferLocation = vb->gpu_address;
        vbvs[0].SizeInBytes = (UINT)vb->size;
        vbvs[0].StrideInBytes = stride;
        /* Slot 1: identity instance buffer (non-instanced draws) */
        vbvs[1].BufferLocation = vio_d3d12.identity_instance_gpu;
        vbvs[1].SizeInBytes = 64;
        vbvs[1].StrideInBytes = 64;
        ID3D12GraphicsCommandList_IASetVertexBuffers(vio_d3d12.cmd_list, 0, 2, vbvs);
    }

    vio_d3d12_flush_srv_table();

    UINT instance_count = cmd->instance_count > 0 ? cmd->instance_count : 1;
    d3d12_apply_view_mask();
    d3d12_apply_bindless();
    ID3D12GraphicsCommandList_DrawInstanced(vio_d3d12.cmd_list,
                                             cmd->vertex_count,
                                             instance_count,
                                             cmd->first_vertex, 0);
}

static void d3d12_draw_indexed(vio_draw_indexed_cmd *cmd)
{
    if (!cmd) return;
    if (!d3d12_current_pipeline) return;   /* see d3d12_draw */

    vio_d3d12_buffer *vb = (vio_d3d12_buffer *)cmd->vertex_buffer;
    vio_d3d12_buffer *ib = (vio_d3d12_buffer *)cmd->index_buffer;

    if (vb) {
        UINT stride = cmd->vertex_stride > 0 ? (UINT)cmd->vertex_stride
                    : (d3d12_current_pipeline ? d3d12_current_pipeline->vertex_stride : 0);
        D3D12_VERTEX_BUFFER_VIEW vbvs[2];
        vbvs[0].BufferLocation = vb->gpu_address;
        vbvs[0].SizeInBytes = (UINT)vb->size;
        vbvs[0].StrideInBytes = stride;
        vbvs[1].BufferLocation = vio_d3d12.identity_instance_gpu;
        vbvs[1].SizeInBytes = 64;
        vbvs[1].StrideInBytes = 64;
        ID3D12GraphicsCommandList_IASetVertexBuffers(vio_d3d12.cmd_list, 0, 2, vbvs);
    }

    if (ib) {
        D3D12_INDEX_BUFFER_VIEW ibv = {0};
        ibv.BufferLocation = ib->gpu_address;
        ibv.SizeInBytes = (UINT)ib->size;
        ibv.Format = cmd->index_bytes == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        ID3D12GraphicsCommandList_IASetIndexBuffer(vio_d3d12.cmd_list, &ibv);
    }

    vio_d3d12_flush_srv_table();

    UINT instance_count = cmd->instance_count > 0 ? cmd->instance_count : 1;
    d3d12_apply_view_mask();
    d3d12_apply_bindless();
    ID3D12GraphicsCommandList_DrawIndexedInstanced(vio_d3d12.cmd_list,
                                                    cmd->index_count,
                                                    instance_count,
                                                    cmd->first_index,
                                                    cmd->vertex_offset, 0);
}

static void d3d12_present(void)
{
    if (!vio_d3d12.swapchain) return;

    /* Once the device is lost, stop presenting entirely. The first loss already
     * logged the reason + DRED breadcrumbs; continuing to Present would only
     * re-fail every frame and bury that one useful dump under 64KB of spam. */
    if (vio_d3d12.device_lost) return;

    /* Offscreen render target still bound at end-of-frame (warm-render /
     * render-to-texture): the frame's draws went to the offscreen target, not
     * the swapchain backbuffer. Presenting here would flip an undrawn
     * FLIP_DISCARD backbuffer (undefined contents) to the screen — the visible
     * "pre-warm" flash. Skip the Present and the buffer rotation, but STILL
     * signal the fence: end_frame already did ExecuteCommandLists, and the next
     * d3d12_begin_frame()'s wait_for_frame() (which reuses this same frame_index
     * since we didn't rotate) must see that work complete before it resets the
     * allocator. Mirrors metal_present's offscreen (no-drawable) path. */
    if (vio_d3d12.current_bound_rt) {
        vio_d3d12.fence_value++;
        vio_d3d12.frames[vio_d3d12.frame_index].fence_value = vio_d3d12.fence_value;
        ID3D12CommandQueue_Signal(vio_d3d12.cmd_queue, vio_d3d12.fence, vio_d3d12.fence_value);
        return;
    }

    UINT sync_interval = vio_d3d12.vsync ? 1 : 0;

    /* DXGI_PRESENT_ALLOW_TEARING is legal ONLY when all of these hold:
     *   - the swapchain was created with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING,
     *   - SyncInterval == 0 (with a non-zero interval DXGI fails the Present with
     *     DXGI_ERROR_INVALID_CALL — an API violation, not a hint),
     *   - the swapchain is windowed (guaranteed here: NO_ALT_ENTER is set above and
     *     this backend never calls SetFullscreenState).
     * The flag is what lets VRR engage and hands the frame to scan-out without
     * waiting for a vblank boundary. It does NOT raise throughput — SyncInterval=0
     * already presents uncapped without it (measured). */
    UINT present_flags = 0;
    if (sync_interval == 0 &&
        (vio_d3d12.swapchain_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) {
        present_flags = DXGI_PRESENT_ALLOW_TEARING;
    }

    HRESULT hr = IDXGISwapChain3_Present(vio_d3d12.swapchain, sync_interval, present_flags);
    if (FAILED(hr)) {
        /* Log reason + DRED breadcrumbs/page-fault ONCE, then latch device_lost
         * so we stop presenting (no per-frame spam). */
        d3d12_handle_device_removed("present_fail", hr);
        return;
    }

    /* Signal fence for current frame */
    vio_d3d12.fence_value++;
    vio_d3d12.frames[vio_d3d12.frame_index].fence_value = vio_d3d12.fence_value;
    ID3D12CommandQueue_Signal(vio_d3d12.cmd_queue, vio_d3d12.fence, vio_d3d12.fence_value);

    /* Record which buffer we just presented BEFORE rotating to the next one.
     * vio_read_pixels uses this to read the last-rendered frame instead of
     * the freshly-rotated (discarded) upcoming backbuffer. */
    vio_d3d12.last_presented_frame_idx = vio_d3d12.frame_index;

    /* Move to next frame */
    vio_d3d12.frame_index = IDXGISwapChain3_GetCurrentBackBufferIndex(vio_d3d12.swapchain);
}

/* ── Frame capture (readback) ─────────────────────────────────────────
 *
 * vio_read_pixels' previous D3D12 path read frames[last_presented_frame_idx]
 * and assumed it was in PRESENT state. That is only valid AFTER vio_end (the
 * frame's command list is closed, executed and presented). The engine's
 * shadow-debug screenshot, however, fires MID-FRAME (after vio_draw_3d, before
 * vio_end): at that point the whole frame — 3D + shadow + 2D HUD — is still in
 * ONE open, un-executed command list, the buffer being drawn is frames[
 * frame_index] in RENDER_TARGET state, and last_presented_frame_idx points at
 * the PREVIOUS frame's buffer (often still at the pre-resize creation size).
 * That mismatch produced the "1280x720, white, no 3D scene" capture.
 *
 * This helper captures the correct buffer in BOTH states:
 *  - in_frame: flush the live command list so this frame's draws land on the
 *    GPU, copy frames[frame_index] (RENDER_TARGET) into a readback buffer, then
 *    re-Reset and re-arm the frame command list so vio_end/d3d12_end_frame can
 *    close→PRESENT→Present it normally.
 *  - !in_frame: copy frames[last_presented_frame_idx] (PRESENT) as before.
 * Size is taken from the source resource desc, so it always tracks the live
 * (resized) swapchain dimensions.
 */
unsigned char *vio_d3d12_capture_frame(int *out_w, int *out_h, size_t *out_size)
{
    if (!vio_d3d12.initialized || !vio_d3d12.device) return NULL;

    int mid_frame = vio_d3d12.in_frame;
    UINT read_idx = mid_frame ? vio_d3d12.frame_index
                              : vio_d3d12.last_presented_frame_idx;
    vio_d3d12_frame *frame = &vio_d3d12.frames[read_idx];
    ID3D12Resource *src = frame->render_target;
    if (!src) return NULL;

    /* The source's current resource state: a buffer being drawn this frame is
     * RENDER_TARGET; a presented buffer is PRESENT. We transition to/from this
     * known state so the runtime/GPU-based-validation stays happy. */
    D3D12_RESOURCE_STATES src_state = mid_frame
        ? D3D12_RESOURCE_STATE_RENDER_TARGET
        : D3D12_RESOURCE_STATE_PRESENT;

    D3D12_RESOURCE_DESC src_desc;
    ID3D12Resource_GetDesc(src, &src_desc);
    int w = (int)src_desc.Width;
    int h = (int)src_desc.Height;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {0};
    UINT num_rows = 0;
    UINT64 row_size = 0, total_bytes = 0;
    ID3D12Device_GetCopyableFootprints(vio_d3d12.device, &src_desc, 0, 1, 0,
                                        &footprint, &num_rows, &row_size, &total_bytes);

    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb_desc = {0};
    rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb_desc.Width = total_bytes;
    rb_desc.Height = 1;
    rb_desc.DepthOrArraySize = 1;
    rb_desc.MipLevels = 1;
    rb_desc.SampleDesc.Count = 1;
    rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource *readback = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &hp,
        D3D12_HEAP_FLAG_NONE, &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
        &IID_ID3D12Resource, (void **)&readback);
    if (FAILED(hr) || !readback) {
        php_error_docref(NULL, E_WARNING,
            "vio_d3d12_capture_frame: readback buffer create failed (0x%08lx)", hr);
        return NULL;
    }

    D3D12_TEXTURE_COPY_LOCATION src_loc = {0};
    src_loc.pResource = src;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst_loc = {0};
    dst_loc.pResource = readback;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint = footprint;

    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = src;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    if (mid_frame) {
        /* Record the copy into the LIVE, still-open frame command list, then
         * close+execute it so the frame's draws-so-far are actually on the GPU
         * before we read back. */
        barrier.Transition.StateBefore = src_state;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ID3D12GraphicsCommandList_ResourceBarrier(vio_d3d12.cmd_list, 1, &barrier);

        ID3D12GraphicsCommandList_CopyTextureRegion(vio_d3d12.cmd_list,
                                                     &dst_loc, 0, 0, 0, &src_loc, NULL);

        /* Restore the source to RENDER_TARGET so the re-opened frame continues
         * drawing into it and d3d12_end_frame's RENDER_TARGET->PRESENT is valid. */
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter  = src_state;
        ID3D12GraphicsCommandList_ResourceBarrier(vio_d3d12.cmd_list, 1, &barrier);

        ID3D12GraphicsCommandList_Close(vio_d3d12.cmd_list);
        ID3D12CommandList *lists[] = { (ID3D12CommandList *)vio_d3d12.cmd_list };
        ID3D12CommandQueue_ExecuteCommandLists(vio_d3d12.cmd_queue, 1, lists);

        /* Wait for the copy (and all preceding frame work) to finish. */
        vio_d3d12_wait_for_gpu();

        /* Re-open the frame command list so subsequent draws + vio_end work.
         * Safe to Reset the allocator: we just waited for the GPU to drain it.
         * Re-arm render target binding + viewport/scissor exactly as
         * d3d12_begin_frame did; the RT is already in RENDER_TARGET state (we
         * transitioned it back above) so no entry barrier is needed here. */
        ID3D12CommandAllocator_Reset(frame->cmd_allocator);
        ID3D12GraphicsCommandList_Reset(vio_d3d12.cmd_list, frame->cmd_allocator, NULL);
        if (vio_d3d12.shading_rate != VIO_SHADING_RATE_1X1 || vio_d3d12.vrs_image_active) d3d12_apply_shading_rate();   /* VRS is list state */
        ID3D12GraphicsCommandList_OMSetRenderTargets(vio_d3d12.cmd_list, 1,
            &vio_d3d12.current_rtv, FALSE, &vio_d3d12.current_dsv);
        D3D12_VIEWPORT vp = {0, 0, (float)vio_d3d12.width, (float)vio_d3d12.height, 0.0f, 1.0f};
        ID3D12GraphicsCommandList_RSSetViewports(vio_d3d12.cmd_list, 1, &vp);
        D3D12_RECT sc = {0, 0, vio_d3d12.width, vio_d3d12.height};
        ID3D12GraphicsCommandList_RSSetScissorRects(vio_d3d12.cmd_list, 1, &sc);
    } else {
        /* Post-present: use a transient command list so we don't disturb the
         * frame's own list (which is closed/idle at this point). */
        ID3D12CommandAllocator *alloc = NULL;
        hr = ID3D12Device_CreateCommandAllocator(vio_d3d12.device,
            D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc);
        if (FAILED(hr)) { ID3D12Resource_Release(readback); return NULL; }

        ID3D12GraphicsCommandList *list = NULL;
        hr = ID3D12Device_CreateCommandList(vio_d3d12.device, 0,
            D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&list);
        if (FAILED(hr)) {
            ID3D12CommandAllocator_Release(alloc);
            ID3D12Resource_Release(readback);
            return NULL;
        }

        barrier.Transition.StateBefore = src_state;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);

        ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst_loc, 0, 0, 0, &src_loc, NULL);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter  = src_state;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);

        ID3D12GraphicsCommandList_Close(list);
        ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
        ID3D12CommandQueue_ExecuteCommandLists(vio_d3d12.cmd_queue, 1, lists);
        vio_d3d12_wait_for_gpu();

        ID3D12GraphicsCommandList_Release(list);
        ID3D12CommandAllocator_Release(alloc);
    }

    /* Map readback and copy rows out (RowPitch may include alignment padding). */
    D3D12_RANGE read_range = {0, (SIZE_T)total_bytes};
    void *mapped_ptr = NULL;
    hr = ID3D12Resource_Map(readback, 0, &read_range, &mapped_ptr);
    if (FAILED(hr) || !mapped_ptr) {
        ID3D12Resource_Release(readback);
        php_error_docref(NULL, E_WARNING, "vio_d3d12_capture_frame: map failed");
        return NULL;
    }

    size_t sz = (size_t)w * h * 4;
    unsigned char *out = (unsigned char *)malloc(sz);
    if (!out) {
        ID3D12Resource_Unmap(readback, 0, NULL);
        ID3D12Resource_Release(readback);
        return NULL;
    }
    const unsigned char *srcp = (const unsigned char *)mapped_ptr;
    for (int y = 0; y < h; y++) {
        memcpy(out + (size_t)y * w * 4,
               srcp + (size_t)y * footprint.Footprint.RowPitch,
               (size_t)w * 4);
    }

    ID3D12Resource_Unmap(readback, 0, NULL);
    ID3D12Resource_Release(readback);
    if (src_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        vio_rt_rgb10a2_to_rgba8_inplace(out, (size_t)w * (size_t)h);
    }

    if (out_w)    *out_w = w;
    if (out_h)    *out_h = h;
    if (out_size) *out_size = sz;
    return out;
}

static void d3d12_clear(float r, float g, float b, float a)
{
    float color[4] = {r, g, b, a};

    if (!vio_d3d12.in_frame) {
        /* Outside a frame the command list is closed: latch the colour and let
         * begin_frame clear colour + depth — the portable "clear before begin"
         * pattern (OpenGL / Metal / D3D11 behave the same way). */
        vio_d3d12.clear_r = r; vio_d3d12.clear_g = g; vio_d3d12.clear_b = b; vio_d3d12.clear_a = a;
        vio_d3d12.clear_pending = 1;
        return;
    }

    /* Clear whichever render target is currently bound (every MRT attachment) */
    if (vio_d3d12.current_has_rtv) {
        ID3D12GraphicsCommandList_ClearRenderTargetView(vio_d3d12.cmd_list,
                                                         vio_d3d12.current_rtv,
                                                         color, 0, NULL);
        for (int i = 1; i < vio_d3d12.current_rtv_count && i < VIO_MAX_COLOR_ATTACHMENTS; i++) {
            ID3D12GraphicsCommandList_ClearRenderTargetView(vio_d3d12.cmd_list,
                                                             vio_d3d12.current_rtvs[i],
                                                             color, 0, NULL);
        }
    }

    ID3D12GraphicsCommandList_ClearDepthStencilView(vio_d3d12.cmd_list,
                                                     vio_d3d12.current_dsv,
                                                     D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                                     1.0f, 0, 0, NULL);
}

/* ── Compute ──────────────────────────────────────────────────────── */

/* Build a PER-PIPELINE compute root signature whose registers are DATA-DRIVEN
 * from the reflected shader (no hardcoded t0/u0/b0):
 *   [0] root CBV  b{cbv_register}     (Params constant block; omitted if < 0)
 *   [1] SRV table t{srv_base_reg}..   (read-only storage buffers)
 *   [2] UAV table u{uav_base_reg}..   (writeonly storage buffers)
 * Always emits all three root parameters in fixed slots (0=CBV, 1=SRV, 2=UAV) so
 * dispatch_compute can bind by index; if there is no UBO the CBV uses a benign
 * unreferenced register (b0, space1) — a root CBV that the shader never reads is
 * legal and triggers no validation error. ALL shader visibility, Flags = NONE
 * (compute has no input-assembler). On success stores the root signature on the
 * pipeline and returns 0. */
static int d3d12_build_compute_root_signature(vio_d3d12_compute_pipeline *cp)
{
    /* SRV range: base register = the lowest readonly storage-buffer binding.
     * NumDescriptors spans MAX so any t{base..base+MAX-1} is covered. */
    D3D12_DESCRIPTOR_RANGE srv_range = {0};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = VIO_D3D12_COMPUTE_MAX_BINDINGS;
    srv_range.BaseShaderRegister = (UINT)(cp->srv_base_reg >= 0 ? cp->srv_base_reg : 0);
    srv_range.RegisterSpace = 0;
    srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE uav_range = {0};
    uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav_range.NumDescriptors = VIO_D3D12_COMPUTE_MAX_BINDINGS;
    uav_range.BaseShaderRegister = (UINT)(cp->uav_base_reg >= 0 ? cp->uav_base_reg : 0);
    uav_range.RegisterSpace = 0;
    uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[5] = {0};

    /* [4] the bindless table (BINDLESS-PLAN 4b): the compute heap's bindless block. */
    D3D12_DESCRIPTOR_RANGE bindless_ranges[3];
    d3d12_bindless_ranges(bindless_ranges);
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 3;
    params[4].DescriptorTable.pDescriptorRanges = bindless_ranges;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC bindless_samplers[4];
    d3d12_bindless_static_samplers(bindless_samplers);

    /* [3] root SRV t0, space9 — the ray-query acceleration structure, when used. */
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].Descriptor.ShaderRegister = 0;
    params[3].Descriptor.RegisterSpace = 9;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    /* [0] root CBV — the Params block, at its reflected register b{cbv_register}.
     * With no UBO, park it at b0/space1 (never referenced -> no bind error). */
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = (UINT)(cp->cbv_register >= 0 ? cp->cbv_register : 0);
    params[0].Descriptor.RegisterSpace = (cp->cbv_register >= 0 ? 0u : 1u);
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    /* [1] SRV table t{srv_base_reg}.. */
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srv_range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    /* [2] UAV table u{uav_base_reg}.. */
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &uav_range;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc = {0};
    rs_desc.NumParameters = cp->uses_bindless ? 5 : cp->uses_accel ? 4 : 3;
    rs_desc.pParameters = params;
    rs_desc.NumStaticSamplers = cp->uses_bindless ? 4 : 0;
    rs_desc.pStaticSamplers = cp->uses_bindless ? bindless_samplers : NULL;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE; /* NO input-assembler flag */

    ID3DBlob *sig = NULL, *err = NULL;
    HRESULT hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: compute root signature serialize failed: %s",
                         err ? (char *)ID3D10Blob_GetBufferPointer(err) : "unknown");
        if (err) ID3D10Blob_Release(err);
        return -1;
    }
    hr = ID3D12Device_CreateRootSignature(vio_d3d12.device, 0,
                                          ID3D10Blob_GetBufferPointer(sig),
                                          ID3D10Blob_GetBufferSize(sig),
                                          &IID_ID3D12RootSignature,
                                          (void **)&cp->root_signature);
    ID3D10Blob_Release(sig);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: CreateRootSignature(compute) failed (0x%08lx)", hr);
        return -1;
    }
    return 0;
}

/* Dedicated shader-visible CBV/SRV/UAV heap for compute dispatches. Layout per
 * dispatch: [0..VIO_D3D12_COMPUTE_MAX_BINDINGS) SRVs, then the same many UAVs.
 * Recreated lazily (small, fixed). Separate from the graphics srv_heap so the
 * complex per-frame partitioning there is untouched. */

static int d3d12_ensure_compute_srv_heap(void)
{
    if (vio_d3d12.compute_srv_heap) return 0;
    /* Behind the dispatch blocks: the bindless table, copied from its CPU mirror
     * (d3d12_bindless_publish keeps it current). */
    UINT extra = (vio_d3d12.bindless && vio_d3d12.bindless_cpu_heap) ? VIO_BINDLESS_MAX : 0;
    if (d3d12_create_descriptor_heap(&vio_d3d12.compute_srv_heap,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                     VIO_D3D12_COMPUTE_MAX_BINDINGS * 2 * VIO_D3D12_COMPUTE_HEAP_BLOCKS + extra,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
        return -1;
    }
    vio_d3d12.compute_srv_descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(
        vio_d3d12.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (extra) {
        D3D12_CPU_DESCRIPTOR_HANDLE c, m;
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.compute_srv_heap, &c);
        c.ptr += (SIZE_T)VIO_D3D12_COMPUTE_BINDLESS_BASE * vio_d3d12.compute_srv_descriptor_size;
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.bindless_cpu_heap, &m);
        ID3D12Device_CopyDescriptorsSimple(vio_d3d12.device, VIO_BINDLESS_MAX, c, m, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    return 0;
}

static void *d3d12_create_compute_pipeline(vio_shader_desc *desc)
{
    if (!vio_d3d12.device) return NULL;

    /* GLSL compute source arrives in fragment_data (see vio_compute_pipeline).
     * It may also already be SPIR-V. Compile to SPIR-V, then transpile to HLSL
     * (SM 5.1) just like the vs/ps path, then D3DCompile as cs_5_1. */
    const char *src = (const char *)desc->fragment_data;
    if (!src && desc->vertex_data) src = (const char *)desc->vertex_data;
    if (!src) {
        php_error_docref(NULL, E_WARNING, "D3D12: compute pipeline missing source");
        return NULL;
    }
    size_t src_size = desc->fragment_size ? desc->fragment_size : desc->vertex_size;

    char *err = NULL;
    uint32_t *spirv = NULL;
    size_t spirv_size = 0;
    int free_spirv = 0;

    int is_spirv = (src_size >= 4 && *(const uint32_t *)src == 0x07230203);
    if (is_spirv) {
        spirv = (uint32_t *)src;
        spirv_size = src_size;
    } else {
        spirv = vio_compile_glsl_compute_to_spirv(src, &spirv_size, &err);
        if (!spirv) {
            php_error_docref(NULL, E_WARNING, "D3D12: CS GLSL->SPIR-V failed: %s", err ? err : "unknown");
            if (err) free(err);
            return NULL;
        }
        free_spirv = 1;
    }

    /* ── DATA-DRIVEN register mapping ──────────────────────────────────────
     * Reflect the SAME SPIR-V we are about to transpile. spirv-cross maps a
     * GLSL `binding = N` straight to the HLSL register NUMBER N within each
     * register file (UBO -> bN, readonly buffer -> tN, writeonly buffer -> uN),
     * so the reflected `binding` IS the HLSL register. We resolve:
     *   cbv_register = the Params UBO's binding (its b#); -1 if there is no UBO.
     *   srv/uav table BASE register = the minimum storage-buffer binding. The
     *     descriptor table maps heap-offset k -> register base+k, and at bind
     *     time each buffer is placed at heap-offset (binding - base), so any
     *     binding in [base, base+MAX) lands on its exact register. SRV vs UAV
     *     classification comes from the vio_compute_bind_buffer access flag
     *     (the PHP contract; reflection's NonWritable/NonReadable is not exposed
     *     reliably through the SPIRV-Cross C API), so a single shared base over
     *     all storage buffers is correct for both tables. */
    int cbv_register = -1;
    int storage_min_binding = -1;
    {
        vio_reflect_result refl;
        char *rerr = NULL;
        if (vio_spirv_reflect(spirv, spirv_size, &refl, &rerr) == 0) {
            if (refl.ubo_count > 0) {
                cbv_register = (int)refl.ubos[0].binding;
                for (int i = 1; i < refl.ubo_count; i++) {
                    if ((int)refl.ubos[i].binding < cbv_register) cbv_register = (int)refl.ubos[i].binding;
                }
            }
            for (int i = 0; i < refl.storage_buffer_count; i++) {
                int bnd = (int)refl.storage_buffers[i].binding;
                if (storage_min_binding < 0 || bnd < storage_min_binding) storage_min_binding = bnd;
            }
            /* Storage images share the UAV table (RWTexture2D/3D at u{binding}). */
            for (int i = 0; i < refl.storage_image_count; i++) {
                int bnd = (int)refl.storage_images[i].binding;
                if (storage_min_binding < 0 || bnd < storage_min_binding) storage_min_binding = bnd;
            }
            vio_reflect_free(&refl);
        } else {
            /* Reflection failure is non-fatal: fall back to register 0 bases and
             * b0 CBV (the legacy pinned-binding layout still works for that). */
            php_error_docref(NULL, E_WARNING, "D3D12: CS reflection failed (%s); using default registers",
                             rerr ? rerr : "unknown");
            if (rerr) free(rerr);
        }
    }
    int srv_base_reg = storage_min_binding >= 0 ? storage_min_binding : 0;
    int uav_base_reg = storage_min_binding >= 0 ? storage_min_binding : 0;

    char *hlsl = vio_spirv_to_hlsl(spirv, spirv_size, d3d12_hlsl_target(), &err);
    if (free_spirv) free(spirv);
    if (!hlsl) {
        php_error_docref(NULL, E_WARNING, "D3D12: CS SPIR-V->HLSL failed: %s", err ? err : "unknown");
        if (err) free(err);
        return NULL;
    }

    if (getenv("VIO_DUMP_CS_HLSL")) {
        fprintf(stderr, "==== compute HLSL (CBV b%d, SRV table base t%d, UAV table base u%d) ====\n%s\n==== end ====\n",
                cbv_register, srv_base_reg, uav_base_reg, hlsl);
        fflush(stderr);
    }

    /* FXC defaults to OPTIMIZATION_LEVEL1; LEVEL3 is the release codegen the
     * driver-side JIT benefits from (GAP-PLAN 2.8). Debug builds keep the
     * un-optimised, symbol-carrying blob for PIX / RenderDoc. */
    UINT compile_flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    if (vio_d3d12.debug_enabled) {
        compile_flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    }

    vio_d3d12_compute_pipeline *cp = calloc(1, sizeof(vio_d3d12_compute_pipeline));
    if (!cp) { free(hlsl); return NULL; }
    cp->cbv_register = cbv_register;
    cp->srv_base_reg = srv_base_reg;
    cp->uav_base_reg = uav_base_reg;
    cp->uses_accel   = strstr(hlsl, "RaytracingAccelerationStructure") != NULL;
    cp->uses_bindless = vio_d3d12.bindless && vio_d3d12.bindless_cpu_heap
                     && (strstr(hlsl, "space1)") || strstr(hlsl, "space3)") || strstr(hlsl, "space4)"));

    /* Build the per-pipeline root signature from the reflected registers. */
    if (d3d12_build_compute_root_signature(cp) != 0) {
        free(cp);
        free(hlsl);
        return NULL;
    }

    HRESULT hr = d3d12_compile_cached(hlsl, "cs_main", "cs_5_1", compile_flags, &cp->cs_blob);
    free(hlsl);
    if (FAILED(hr)) {
        if (cp->root_signature) ID3D12RootSignature_Release(cp->root_signature);
        free(cp);
        return NULL;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {0};
    pso_desc.pRootSignature = cp->root_signature;
    pso_desc.CS.pShaderBytecode = ID3D10Blob_GetBufferPointer(cp->cs_blob);
    pso_desc.CS.BytecodeLength = ID3D10Blob_GetBufferSize(cp->cs_blob);

    hr = ID3D12Device_CreateComputePipelineState(vio_d3d12.device, &pso_desc,
                                                 &IID_ID3D12PipelineState, (void **)&cp->pso);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: CreateComputePipelineState failed (0x%08lx)", hr);
        d3d12_drain_info_queue("create_compute_pipeline");
        if (cp->root_signature) ID3D12RootSignature_Release(cp->root_signature);
        ID3D10Blob_Release(cp->cs_blob);
        free(cp);
        return NULL;
    }

    return cp;
}

static void d3d12_destroy_compute_pipeline(void *pipeline_ptr)
{
    vio_d3d12_compute_pipeline *cp = (vio_d3d12_compute_pipeline *)pipeline_ptr;
    if (!cp) return;
    /* The GPU may still reference this PSO if a dispatch is in flight. All vio
     * compute dispatches are fully fenced (wait_for_gpu before returning), so by
     * the time PHP drops the pipeline the GPU is idle — no extra wait needed. */
    if (cp->params_buf) ID3D12Resource_Release(cp->params_buf);
    free(cp->params_cpu);
    if (cp->pso) ID3D12PipelineState_Release(cp->pso);
    if (cp->root_signature) ID3D12RootSignature_Release(cp->root_signature);
    if (cp->cs_blob) ID3D10Blob_Release(cp->cs_blob);
    free(cp);
}

static void d3d12_compute_bind_buffer(void *pipeline_ptr, void *backend_buffer,
                                      int slot, int access, int element_count, int stride)
{
    vio_d3d12_compute_pipeline *cp = (vio_d3d12_compute_pipeline *)pipeline_ptr;
    vio_d3d12_buffer *buf = (vio_d3d12_buffer *)backend_buffer;
    if (!cp || !buf) return;

    vio_d3d12_compute_binding b = {0};
    b.buffer = buf;
    b.slot = slot;
    b.access = access;
    b.element_count = element_count;
    b.stride = stride > 0 ? stride : (buf->stride > 0 ? buf->stride : 4);

    /* One buffer per slot: rebinding a slot replaces its binding (also when the
     * access changes). The list used to only grow, so a pipeline reused with
     * fresh buffers kept feeding the kernel the first ones and dropped every
     * bind past VIO_D3D12_COMPUTE_MAX_BINDINGS. */
    for (int i = 0; i < cp->srv_count; i++) {
        if (cp->srvs[i].slot == slot) { cp->srvs[i] = cp->srvs[--cp->srv_count]; break; }
    }
    for (int i = 0; i < cp->uav_count; i++) {
        if (cp->uavs[i].slot == slot) { cp->uavs[i] = cp->uavs[--cp->uav_count]; break; }
    }

    if (access == 1 /* VIO_COMPUTE_WRITE */) {
        if (cp->uav_count < VIO_D3D12_COMPUTE_MAX_BINDINGS) cp->uavs[cp->uav_count++] = b;
    } else {
        if (cp->srv_count < VIO_D3D12_COMPUTE_MAX_BINDINGS) cp->srvs[cp->srv_count++] = b;
    }
}

static void d3d12_compute_set_uniforms(void *pipeline_ptr, const void *data, int size)
{
    vio_d3d12_compute_pipeline *cp = (vio_d3d12_compute_pipeline *)pipeline_ptr;
    if (!cp || !data || size <= 0) return;

    size_t aligned = ((size_t)size + 255) & ~(size_t)255; /* CB 256-byte alignment */

    if (!cp->params_buf || cp->params_capacity < aligned) {
        if (cp->params_buf) { ID3D12Resource_Release(cp->params_buf); cp->params_buf = NULL; }
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = aligned;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &hp,
            D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void **)&cp->params_buf);
        if (FAILED(hr)) {
            php_error_docref(NULL, E_WARNING, "D3D12: compute params buffer create failed (0x%08lx)", hr);
            cp->params_buf = NULL;
            return;
        }
        cp->params_capacity = aligned;
        unsigned char *shadow = (unsigned char *)realloc(cp->params_cpu, aligned);
        if (!shadow) free(cp->params_cpu);
        else memset(shadow, 0, aligned);
        cp->params_cpu = shadow;
    }

    void *mapped = NULL;
    D3D12_RANGE no_read = {0, 0};
    if (SUCCEEDED(ID3D12Resource_Map(cp->params_buf, 0, &no_read, &mapped))) {
        memcpy(mapped, data, (size_t)size);
        ID3D12Resource_Unmap(cp->params_buf, 0, NULL);
    }
    cp->params_size = (size_t)size;
    if (cp->params_cpu) memcpy(cp->params_cpu, data, (size_t)size);
}

static void d3d12_compute_bind_image(void *pipeline_ptr, void *tex_obj, int slot, int access)
{
    vio_d3d12_compute_pipeline *cp = (vio_d3d12_compute_pipeline *)pipeline_ptr;
    vio_texture_object *t = (vio_texture_object *)tex_obj;
    vio_d3d12_texture *dt = t ? (vio_d3d12_texture *)t->backend_texture : NULL;
    if (!cp || !dt || !dt->resource) return;
    for (int i = 0; i < cp->image_count; i++) {
        if (cp->images[i].slot == slot) { cp->images[i].tex = dt; cp->images[i].access = access; return; }
    }
    if (cp->image_count >= VIO_D3D12_COMPUTE_MAX_BINDINGS) return;
    cp->images[cp->image_count].tex    = dt;
    cp->images[cp->image_count].slot   = slot;
    cp->images[cp->image_count].access = access;
    cp->image_count++;
}

/* Re-arm the graphics state a compute record on the frame list disturbed: the
 * graphics descriptor heap + root signature, and the PSO/topology of the
 * pipeline the caller has bound (d3d12_bind_pipeline_state caches it). */
static void d3d12_restore_graphics_state_after_compute(void)
{
    vio_d3d12_bind_graphics_heaps(vio_d3d12.cmd_list);   /* also drops the cached root tables */
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(vio_d3d12.cmd_list, d3d12_graphics_root_signature());
    if (d3d12_current_pipeline && d3d12_current_pipeline->pso) {
        ID3D12GraphicsCommandList_SetPipelineState(vio_d3d12.cmd_list,
            d3d12_pipeline_pso_for_target(d3d12_current_pipeline, vio_d3d12.current_rt_samples, vio_d3d12.current_rt_format));
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(vio_d3d12.cmd_list, d3d12_current_pipeline->topology);
        ID3D12GraphicsCommandList_OMSetStencilRef(vio_d3d12.cmd_list, d3d12_current_pipeline->stencil_ref);
    }
}

/* Wait for async dispatches recorded into the frame list. Mid-frame this
 * closes + executes + waits on the live list and reopens it (the same pattern
 * vio_d3d12_capture_frame uses for mid-frame readback); after the frame a plain
 * GPU wait suffices. */
static void d3d12_compute_wait(void)
{
    if (!vio_d3d12.compute_async_pending) return;
    vio_d3d12.compute_async_pending = 0;
    if (!vio_d3d12.in_frame || !vio_d3d12.cmd_list) {
        vio_d3d12_wait_for_gpu();
        return;
    }
    vio_d3d12_frame *frame = &vio_d3d12.frames[vio_d3d12.frame_index];
    ID3D12GraphicsCommandList_Close(vio_d3d12.cmd_list);
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)vio_d3d12.cmd_list };
    ID3D12CommandQueue_ExecuteCommandLists(vio_d3d12.cmd_queue, 1, lists);
    vio_d3d12_wait_for_gpu();

    ID3D12CommandAllocator_Reset(frame->cmd_allocator);
    ID3D12GraphicsCommandList_Reset(vio_d3d12.cmd_list, frame->cmd_allocator, NULL);
    if (vio_d3d12.shading_rate != VIO_SHADING_RATE_1X1 || vio_d3d12.vrs_image_active) d3d12_apply_shading_rate();   /* VRS is list state */
    /* Re-arm the bound target (swapchain or RT), viewport, scissor and the
     * graphics pipeline state exactly as the frame had them. */
    if (vio_d3d12.current_has_rtv) {
        int n = vio_d3d12.current_rtv_count > 0 ? vio_d3d12.current_rtv_count : 1;
        ID3D12GraphicsCommandList_OMSetRenderTargets(vio_d3d12.cmd_list, (UINT)n,
            n > 1 ? vio_d3d12.current_rtvs : &vio_d3d12.current_rtv, FALSE, &vio_d3d12.current_dsv);
    } else {
        ID3D12GraphicsCommandList_OMSetRenderTargets(vio_d3d12.cmd_list, 0, NULL, FALSE, &vio_d3d12.current_dsv);
    }
    D3D12_VIEWPORT vp = {0, 0, (float)vio_d3d12.current_rt_width, (float)vio_d3d12.current_rt_height, 0.0f, 1.0f};
    ID3D12GraphicsCommandList_RSSetViewports(vio_d3d12.cmd_list, 1, &vp);
    D3D12_RECT sc = {0, 0, vio_d3d12.current_rt_width, vio_d3d12.current_rt_height};
    ID3D12GraphicsCommandList_RSSetScissorRects(vio_d3d12.cmd_list, 1, &sc);
    d3d12_restore_graphics_state_after_compute();
}

/* After a kernel / ray tracing launch on `list`: UAV barrier on a STORAGE
 * buffer (UNORDERED_ACCESS by promotion), then copy it into its lazily created
 * READBACK staging buffer, so a later vio_storage_buffer_read just maps it. */
static void d3d12_buffer_to_readback(ID3D12GraphicsCommandList *list, vio_d3d12_buffer *buf)
{
    if (!buf || !buf->resource) return;

    D3D12_RESOURCE_BARRIER uavb = {0};
    uavb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavb.UAV.pResource = buf->resource;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &uavb);

    /* Lazily (re)create a READBACK staging buffer sized to the output. */
    if (!buf->readback_resource || buf->readback_size < buf->size) {
        if (buf->readback_resource) { ID3D12Resource_Release(buf->readback_resource); buf->readback_resource = NULL; }
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = buf->size;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT rhr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &hp,
            D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void **)&buf->readback_resource);
        if (FAILED(rhr)) {
            php_error_docref(NULL, E_WARNING, "D3D12: compute readback buffer create failed (0x%08lx)", rhr);
            buf->readback_resource = NULL;
            return;
        }
    }
    buf->readback_size = buf->size;

    /* STORAGE buffers live in UNORDERED_ACCESS; transition -> COPY_SOURCE,
     * copy the whole buffer, then transition back so a subsequent dispatch
     * (or another read) finds it in its declared UAV state again. */
    D3D12_RESOURCE_BARRIER tb = {0};
    tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    tb.Transition.pResource = buf->resource;
    tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    tb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &tb);

    ID3D12GraphicsCommandList_CopyBufferRegion(list, buf->readback_resource, 0,
                                               buf->resource, 0, buf->size);

    tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &tb);
}

static void d3d12_dispatch_compute(vio_compute_cmd *cmd)
{
    if (!cmd) return;
    vio_d3d12_compute_pipeline *cp = (vio_d3d12_compute_pipeline *)cmd->pipeline;
    if (!cp || !cp->pso) {
        php_error_docref(NULL, E_WARNING, "D3D12: dispatch_compute with invalid pipeline");
        return;
    }
    if (d3d12_ensure_compute_srv_heap() != 0) return;

    /* Async inside an open frame: record onto the frame's own command list so
     * the dispatch executes in order with the surrounding draws (a later draw
     * this frame sees the kernel's writes). Otherwise: transient list + fence. */
    int in_frame_async = cmd->async && vio_d3d12.in_frame && vio_d3d12.cmd_list != NULL;

    /* Build the descriptor table contents in the compute heap. SRVs occupy heap
     * indices [0, MAX); UAVs occupy [MAX, 2*MAX). Within each region a buffer
     * bound at GLSL binding=slot is placed at heap-offset (slot - table_base_reg),
     * because the root descriptor table maps heap-offset k -> register
     * {table_base_reg + k}. So a readonly buffer at binding 0 (table base t0)
     * lands at offset 0 -> t0, and a writeonly buffer at binding 1 (table base
     * u1) lands at offset 0 -> u1 — fully data-driven from reflection, no
     * hardcoded t0/u0. */
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.compute_srv_heap, &cpu_start);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(vio_d3d12.compute_srv_heap, &gpu_start);
    UINT dsz = vio_d3d12.compute_srv_descriptor_size;
    /* Ring block for this dispatch's descriptors (see compute_heap_block). */
    UINT block = vio_d3d12.compute_heap_block;
    vio_d3d12.compute_heap_block = (block + 1) % VIO_D3D12_COMPUTE_HEAP_BLOCKS;
    cpu_start.ptr += (SIZE_T)block * (2 * VIO_D3D12_COMPUTE_MAX_BINDINGS) * dsz;
    gpu_start.ptr += (UINT64)block * (2 * VIO_D3D12_COMPUTE_MAX_BINDINGS) * dsz;

    const UINT SRV_BASE = 0;
    const UINT UAV_BASE = VIO_D3D12_COMPUTE_MAX_BINDINGS;
    const int  srv_reg_base = cp->srv_base_reg >= 0 ? cp->srv_base_reg : 0;
    const int  uav_reg_base = cp->uav_base_reg >= 0 ? cp->uav_base_reg : 0;

    /* SRVs. spirv-cross transpiles GLSL std430 `buffer { float x[]; }` to an HLSL
     * ByteAddressBuffer (raw), NOT a StructuredBuffer — confirmed via the debug
     * layer. So a stride of 4 (raw/default) builds a RAW view (R32_TYPELESS +
     * FLAG_RAW, NumElements counted in 4-byte words). A stride > 4 builds a true
     * structured view (StructuredBuffer<T> in HLSL). */
    for (int i = 0; i < cp->srv_count; i++) {
        vio_d3d12_compute_binding *b = &cp->srvs[i];
        if (!b->buffer || !b->buffer->resource) continue;
        int rel = b->slot - srv_reg_base;        /* heap-offset within the SRV region */
        if (rel < 0 || rel >= VIO_D3D12_COMPUTE_MAX_BINDINGS) continue;
        UINT idx = SRV_BASE + (UINT)rel;
        int raw = (b->stride <= 4);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.FirstElement = 0;
        if (raw) {
            sd.Format = DXGI_FORMAT_R32_TYPELESS;
            sd.Buffer.NumElements = (UINT)(b->buffer->size / 4);
            sd.Buffer.StructureByteStride = 0;
            sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        } else {
            sd.Format = DXGI_FORMAT_UNKNOWN;
            sd.Buffer.NumElements = (UINT)b->element_count;
            sd.Buffer.StructureByteStride = (UINT)b->stride;
            sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE h = { cpu_start.ptr + (SIZE_T)idx * dsz };
        ID3D12Device_CreateShaderResourceView(vio_d3d12.device, b->buffer->resource, &sd, h);
    }

    /* UAVs (same raw-vs-structured logic as SRVs). */
    for (int i = 0; i < cp->uav_count; i++) {
        vio_d3d12_compute_binding *b = &cp->uavs[i];
        if (in_frame_async && b->buffer) b->buffer->uav_live_serial = vio_d3d12.frame_serial;   /* UAV on this frame's list (Block 8) */
        if (!b->buffer || !b->buffer->resource) continue;
        int rel = b->slot - uav_reg_base;        /* heap-offset within the UAV region */
        if (rel < 0 || rel >= VIO_D3D12_COMPUTE_MAX_BINDINGS) continue;
        UINT idx = UAV_BASE + (UINT)rel;
        int raw = (b->stride <= 4);
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {0};
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.FirstElement = 0;
        ud.Buffer.CounterOffsetInBytes = 0;
        if (raw) {
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.Buffer.NumElements = (UINT)(b->buffer->size / 4);
            ud.Buffer.StructureByteStride = 0;
            ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        } else {
            ud.Format = DXGI_FORMAT_UNKNOWN;
            ud.Buffer.NumElements = (UINT)b->element_count;
            ud.Buffer.StructureByteStride = (UINT)b->stride;
            ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE h = { cpu_start.ptr + (SIZE_T)idx * dsz };
        ID3D12Device_CreateUnorderedAccessView(vio_d3d12.device, b->buffer->resource, NULL, &ud, h);
    }

    /* Storage images: texture UAV descriptors in the same UAV region (a
     * RWTexture2D/3D at GLSL binding=slot lands on u{slot} exactly like a
     * buffer). The texture resource is in PIXEL_SHADER_RESOURCE after its
     * upload; it is transitioned to UNORDERED_ACCESS for the dispatch and back
     * afterwards (see below) so sampling it in a later pass needs no caller
     * barrier. */
    for (int i = 0; i < cp->image_count; i++) {
        vio_d3d12_texture *dt = cp->images[i].tex;
        if (!dt || !dt->resource) continue;
        int rel = cp->images[i].slot - uav_reg_base;
        if (rel < 0 || rel >= VIO_D3D12_COMPUTE_MAX_BINDINGS) continue;
        UINT idx = UAV_BASE + (UINT)rel;
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {0};
        ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (dt->depth > 0) {
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            ud.Texture3D.MipSlice = 0;
            ud.Texture3D.FirstWSlice = 0;
            ud.Texture3D.WSize = (UINT)dt->depth;
        } else {
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            ud.Texture2D.MipSlice = 0;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE h = { cpu_start.ptr + (SIZE_T)idx * dsz };
        ID3D12Device_CreateUnorderedAccessView(vio_d3d12.device, dt->resource, NULL, &ud, h);
    }

    /* Record onto a transient DIRECT command list. We never run inside an open
     * graphics frame for the SDF bake (it happens behind the loading screen,
     * outside vio_begin/vio_end), so a dedicated allocator/list keeps us fully
     * decoupled from the frame command list and its state. */
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    if (in_frame_async) {
        list = vio_d3d12.cmd_list;
    } else {
        HRESULT hr = ID3D12Device_CreateCommandAllocator(vio_d3d12.device,
            D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc);
        if (FAILED(hr)) { php_error_docref(NULL, E_WARNING, "D3D12: compute allocator failed"); return; }

        hr = ID3D12Device_CreateCommandList(vio_d3d12.device, 0,
            D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, cp->pso,
            &IID_ID3D12GraphicsCommandList, (void **)&list);
        if (FAILED(hr)) {
            ID3D12CommandAllocator_Release(alloc);
            php_error_docref(NULL, E_WARNING, "D3D12: compute command list failed");
            return;
        }
    }

    ID3D12GraphicsCommandList_SetComputeRootSignature(list, cp->root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(list, cp->pso);
    if (cp->uses_accel && d3d12_bound_as && d3d12_bound_as->tlas)
        ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(list, 3, ID3D12Resource_GetGPUVirtualAddress(d3d12_bound_as->tlas));
    ID3D12DescriptorHeap *heaps[] = { vio_d3d12.compute_srv_heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, heaps);
    if (cp->uses_bindless) {
        D3D12_GPU_DESCRIPTOR_HANDLE bt;
        ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(vio_d3d12.compute_srv_heap, &bt);
        bt.ptr += (UINT64)VIO_D3D12_COMPUTE_BINDLESS_BASE * vio_d3d12.compute_srv_descriptor_size;
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 4, bt);
    }

    if (cp->params_buf) {
        D3D12_GPU_VIRTUAL_ADDRESS params_gpu = ID3D12Resource_GetGPUVirtualAddress(cp->params_buf);
        /* A dispatch on the frame list runs when the frame is submitted. By then
         * the CPU may have staged other params into params_buf (a second dispatch
         * of this pipeline, or the next frame while this one is in flight), so it
         * reads its own copy from this frame's cbuffer slice. A full slice keeps
         * the shared buffer (the heap grows at the next begin_frame). */
        if (in_frame_async && cp->params_cpu && vio_d3d12.cbuffer_heap_mapped) {
            UINT aligned = (UINT)cp->params_capacity;
            if (vio_d3d12.cbuffer_heap_offset + aligned <= vio_d3d12.cbuffer_frame_end) {
                UINT offset = vio_d3d12.cbuffer_heap_offset;
                vio_d3d12.cbuffer_heap_offset += aligned;
                memcpy(vio_d3d12.cbuffer_heap_mapped + offset, cp->params_cpu, aligned);
                params_gpu = vio_d3d12.cbuffer_heap_gpu + offset;
            }
        }
        ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(list, 0, params_gpu);
    }
    /* [1] SRV table base, [2] UAV table base */
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = { gpu_start.ptr + (UINT64)SRV_BASE * dsz };
    D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu = { gpu_start.ptr + (UINT64)UAV_BASE * dsz };
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 1, srv_gpu);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 2, uav_gpu);

    /* Storage images: PIXEL_SHADER_RESOURCE -> UNORDERED_ACCESS for the dispatch. */
    for (int i = 0; i < cp->image_count; i++) {
        vio_d3d12_texture *dt = cp->images[i].tex;
        if (!dt || !dt->resource) continue;
        D3D12_RESOURCE_BARRIER ib = {0};
        ib.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        ib.Transition.pResource = dt->resource;
        ib.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ib.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ib.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &ib);
    }

    int gx = cmd->group_count_x > 0 ? cmd->group_count_x : 1;
    int gy = cmd->group_count_y > 0 ? cmd->group_count_y : 1;
    int gz = cmd->group_count_z > 0 ? cmd->group_count_z : 1;
    ID3D12GraphicsCommandList_Dispatch(list, (UINT)gx, (UINT)gy, (UINT)gz);

    /* ...and back to PIXEL_SHADER_RESOURCE so the texture samples as before. */
    for (int i = 0; i < cp->image_count; i++) {
        vio_d3d12_texture *dt = cp->images[i].tex;
        if (!dt || !dt->resource) continue;
        D3D12_RESOURCE_BARRIER ib = {0};
        ib.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        ib.Transition.pResource = dt->resource;
        ib.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ib.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ib.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &ib);
    }

    /* UAV barrier (ensure all writes complete) + transition each UAV output to
     * COPY_SOURCE and copy into its READBACK staging buffer, so a later
     * vio_storage_buffer_read just Maps the staging without re-running the GPU. */
    for (int i = 0; i < cp->uav_count; i++) d3d12_buffer_to_readback(list, cp->uavs[i].buffer);

    if (in_frame_async) {
        /* Stay on the frame list: put the graphics state back for the draws
         * that follow and remember that a wait is due before any readback. */
        d3d12_restore_graphics_state_after_compute();
        vio_d3d12.compute_async_pending++;
        return;
    }

    ID3D12GraphicsCommandList_Close(list);
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(vio_d3d12.cmd_queue, 1, lists);
    vio_d3d12_wait_for_gpu();

    d3d12_drain_info_queue("dispatch_compute");

    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(alloc);
}

/* GPU->CPU readback. The output buffer's bytes were already copied into its
 * READBACK staging buffer by dispatch_compute (fully fenced), so this just Maps
 * and memcpys — no GPU re-run. Returns bytes written. */
static size_t d3d12_read_buffer(void *backend_buffer, void *out, size_t size)
{
    vio_d3d12_buffer *buf = (vio_d3d12_buffer *)backend_buffer;
    if (!buf || !out || size == 0) return 0;
    if (buf->fs_dirty && buf->resource) {
        /* Written by draws (A15): copy it out now - on the frame list mid-frame
         * (the wait below submits it), else on its own list after the frames. */
        buf->fs_dirty = 0;
        if (vio_d3d12.in_frame && vio_d3d12.cmd_list) {
            d3d12_buffer_to_readback(vio_d3d12.cmd_list, buf);
            vio_d3d12.compute_async_pending++;
        } else {
            vio_d3d12_wait_for_gpu();
            ID3D12CommandAllocator *alloc = NULL;
            ID3D12GraphicsCommandList *list = NULL;
            if (SUCCEEDED(ID3D12Device_CreateCommandAllocator(vio_d3d12.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                               &IID_ID3D12CommandAllocator, (void **)&alloc))
                && SUCCEEDED(ID3D12Device_CreateCommandList(vio_d3d12.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                                                            &IID_ID3D12GraphicsCommandList, (void **)&list))) {
                /* The buffer decayed to COMMON: promote it to UNORDERED_ACCESS
                 * with a no-op UAV barrier path - d3d12_buffer_to_readback expects that. */
                D3D12_RESOURCE_BARRIER tb = {0};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource = buf->resource;
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                tb.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &tb);
                d3d12_buffer_to_readback(list, buf);
                ID3D12GraphicsCommandList_Close(list);
                ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
                ID3D12CommandQueue_ExecuteCommandLists(vio_d3d12.cmd_queue, 1, lists);
                vio_d3d12_wait_for_gpu();
            }
            if (list) ID3D12GraphicsCommandList_Release(list);
            if (alloc) ID3D12CommandAllocator_Release(alloc);
        }
    }
    d3d12_compute_wait();   /* async dispatches (and their staging copies) must have executed */
    if (!buf->readback_resource) {
        php_error_docref(NULL, E_WARNING,
            "D3D12: read_buffer before any compute dispatch produced a readback");
        return 0;
    }

    size_t n = size < buf->readback_size ? size : buf->readback_size;
    D3D12_RANGE rr = { 0, (SIZE_T)n };
    void *mapped = NULL;
    HRESULT hr = ID3D12Resource_Map(buf->readback_resource, 0, &rr, &mapped);
    if (FAILED(hr) || !mapped) {
        php_error_docref(NULL, E_WARNING, "D3D12: read_buffer map failed (0x%08lx)", hr);
        return 0;
    }
    memcpy(out, mapped, n);
    D3D12_RANGE no_write = {0, 0};
    ID3D12Resource_Unmap(buf->readback_resource, 0, &no_write);
    return n;
}

/* ── Ray tracing pipeline (VIO_FEATURE_RAYTRACING) ──────────────────
 * DXR 1.0 state object from one DXC library (lib_6_x) with the exports
 * vio_raygen, vio_miss, vio_closest_hit and optional vio_any_hit (one triangle
 * hit group "vio_hit_group"). Global root signature: [0] root SRV t0 (the bound
 * top level), [1 + n] root UAV u<n> for n = 0..VIO_D3D12_RT_UAVS-1 (the buffers of
 * vio_rt_bind_buffer). The shader table holds one record per group in an
 * upload buffer, each table 64-byte aligned. vio_trace_rays records into its
 * own list and waits, like the acceleration structure build. */
#define VIO_D3D12_RT_UAVS 16
#define VIO_D3D12_RT_TEXTURES 15   /* t1..t15 (one compute-heap block holds 16) */

typedef struct _vio_d3d12_rtp {
    ID3D12StateObject   *state;
    ID3D12RootSignature *root_sig;
    ID3D12RootSignature *local_sig;     /* shader records (A13): root constants b0, space1; NULL without */
    ID3D12Resource      *table;         /* raygen | miss | hit groups | callables, each table 64-byte aligned */
    UINT64               stride;        /* one record: identifier + record data, 32-byte aligned */
    UINT64               off_miss, off_hit, off_call;
    int                  miss_count, hit_count, callable_count;
    int                  used_in_frame;   /* traced on a frame list: drain before freeing */
} vio_d3d12_rtp;

static void d3d12_rtp_free(vio_d3d12_rtp *p)
{
    if (!p) return;
    if (p->table) ID3D12Resource_Release(p->table);
    if (p->state) ID3D12StateObject_Release(p->state);
    if (p->root_sig) ID3D12RootSignature_Release(p->root_sig);
    if (p->local_sig) ID3D12RootSignature_Release(p->local_sig);
    free(p);
}

/* Export names of the library: base, base<k> for k > 0 (vio_callable<k> always numbered). */
static void d3d12_rt_name(WCHAR *out, size_t cap, const char *base, int k, int always)
{
    char tmp[48];
    if (k == 0 && !always) snprintf(tmp, sizeof(tmp), "%s", base);
    else snprintf(tmp, sizeof(tmp), "%s%d", base, k);
    size_t i = 0;
    for (; tmp[i] && i + 1 < cap; i++) out[i] = (WCHAR)tmp[i];
    out[i] = 0;
}

static ID3D12RootSignature *d3d12_rt_root_sig(const D3D12_ROOT_SIGNATURE_DESC *rs, const char *what)
{
    ID3DBlob *sig = NULL, *err = NULL;
    ID3D12RootSignature *out = NULL;
    if (FAILED(D3D12SerializeRootSignature(rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) || !sig) {
        php_error_docref(NULL, E_WARNING, "D3D12: ray tracing %s root signature: %s", what,
                         err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "serialize failed");
    } else if (FAILED(ID3D12Device_CreateRootSignature(vio_d3d12.device, 0, ID3D10Blob_GetBufferPointer(sig),
                                                       ID3D10Blob_GetBufferSize(sig), &IID_ID3D12RootSignature, (void **)&out))) {
        out = NULL;
    }
    if (sig) ID3D10Blob_Release(sig);
    if (err) ID3D10Blob_Release(err);
    return out;
}

static UINT64 d3d12_align(UINT64 v, UINT64 a) { return (v + a - 1) / a * a; }

static void *d3d12_create_rt_pipeline(const vio_rt_pipeline_desc *desc)
{
    if (!desc || !vio_d3d12.device) return NULL;
    if (!desc->hlsl || !desc->hlsl[0]) {
        php_error_docref(NULL, E_WARNING, "D3D12: vio_rt_pipeline needs 'hlsl' (a DXR library with vio_raygen, vio_miss, "
                         "vio_closest_hit%s)", desc->hit[0].spirv[1] ? ", vio_any_hit" : "");
        return NULL;
    }
    ID3D12Device5 *dev5 = NULL;
    if (FAILED(ID3D12Device_QueryInterface(vio_d3d12.device, &IID_ID3D12Device5, (void **)&dev5)) || !dev5) return NULL;
    vio_d3d12_rtp *p = calloc(1, sizeof(vio_d3d12_rtp));
    void *dxil = NULL;
    size_t dxil_len = 0;
    char *err = NULL;
    ID3D12StateObjectProperties *props = NULL;
    if (!p) goto fail;
    p->miss_count = desc->miss_count;
    p->hit_count = desc->hit_count;
    p->callable_count = desc->callable_count;

    char profile[16];
    int minor = vio_d3d12.shader_model_version % 10;
    snprintf(profile, sizeof(profile), "lib_6_%d", minor < 3 ? 3 : minor);
    if (vio_dxc_compile(desc->hlsl, NULL, profile, 0, vio_d3d12.native16, &dxil, &dxil_len, &err) != 0 || !dxil) {
        php_error_docref(NULL, E_WARNING, "D3D12: ray tracing library (%s): %s", profile, err ? err : "compile failed");
        goto fail;
    }

    {
        D3D12_ROOT_PARAMETER rp[2 + VIO_D3D12_RT_UAVS];
        memset(rp, 0, sizeof(rp));
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rp[0].Descriptor.ShaderRegister = 0;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        for (int i = 0; i < VIO_D3D12_RT_UAVS; i++) {
            rp[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            rp[1 + i].Descriptor.ShaderRegister = (UINT)i;
            rp[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        /* [17] textures t1..t16 (vio_rt_bind_texture, A13) from a compute-heap block,
         * with a linear / repeat static sampler at the same s register. */
        D3D12_DESCRIPTOR_RANGE tr = {0};
        tr.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        tr.NumDescriptors = VIO_D3D12_RT_TEXTURES;
        tr.BaseShaderRegister = 1;
        tr.OffsetInDescriptorsFromTableStart = 0;
        rp[1 + VIO_D3D12_RT_UAVS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1 + VIO_D3D12_RT_UAVS].DescriptorTable.NumDescriptorRanges = 1;
        rp[1 + VIO_D3D12_RT_UAVS].DescriptorTable.pDescriptorRanges = &tr;
        rp[1 + VIO_D3D12_RT_UAVS].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC ss[VIO_D3D12_RT_TEXTURES];
        memset(ss, 0, sizeof(ss));
        for (int i = 0; i < VIO_D3D12_RT_TEXTURES; i++) {
            ss[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            ss[i].AddressU = ss[i].AddressV = ss[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            ss[i].MaxLOD = D3D12_FLOAT32_MAX;
            ss[i].ShaderRegister = (UINT)(1 + i);
            ss[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        D3D12_ROOT_SIGNATURE_DESC rs = {0};
        rs.NumParameters = 2 + VIO_D3D12_RT_UAVS;
        rs.pParameters = rp;
        rs.NumStaticSamplers = VIO_D3D12_RT_TEXTURES;
        rs.pStaticSamplers = ss;
        if (!(p->root_sig = d3d12_rt_root_sig(&rs, "global"))) goto fail;
    }
    if (desc->record_size > 0) {
        /* Shader records: the bytes behind each identifier, as root constants. */
        D3D12_ROOT_PARAMETER lp;
        memset(&lp, 0, sizeof(lp));
        lp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        lp.Constants.ShaderRegister = 0;
        lp.Constants.RegisterSpace = 1;
        lp.Constants.Num32BitValues = (UINT)(desc->record_size / 4);
        lp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs = {0};
        rs.NumParameters = 1;
        rs.pParameters = &lp;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
        if (!(p->local_sig = d3d12_rt_root_sig(&rs, "local (shader record)"))) goto fail;
    }

    /* Export names. */
    WCHAR n_raygen[32], n_miss[VIO_RT_MAX_GROUPS][32], n_chit[VIO_RT_MAX_GROUPS][32], n_ahit[VIO_RT_MAX_GROUPS][32];
    WCHAR n_group[VIO_RT_MAX_GROUPS][32], n_call[VIO_RT_MAX_GROUPS][32];
    d3d12_rt_name(n_raygen, 32, "vio_raygen", 0, 0);
    for (int k = 0; k < desc->miss_count; k++) d3d12_rt_name(n_miss[k], 32, "vio_miss", k, 0);
    for (int k = 0; k < desc->hit_count; k++) {
        d3d12_rt_name(n_chit[k], 32, "vio_closest_hit", k, 0);
        d3d12_rt_name(n_ahit[k], 32, "vio_any_hit", k, 0);
        d3d12_rt_name(n_group[k], 32, "vio_hit_group", k, 0);
    }
    for (int k = 0; k < desc->callable_count; k++) d3d12_rt_name(n_call[k], 32, "vio_callable", k, 1);

    D3D12_DXIL_LIBRARY_DESC lib = {0};
    lib.DXILLibrary.pShaderBytecode = dxil;
    lib.DXILLibrary.BytecodeLength = dxil_len;   /* no export list: every export */
    D3D12_HIT_GROUP_DESC hg[VIO_RT_MAX_GROUPS];
    memset(hg, 0, sizeof(hg));
    D3D12_RAYTRACING_SHADER_CONFIG sc = {0};
    sc.MaxPayloadSizeInBytes = (UINT)desc->payload_size;
    sc.MaxAttributeSizeInBytes = D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES;
    D3D12_GLOBAL_ROOT_SIGNATURE grs = {0};
    grs.pGlobalRootSignature = p->root_sig;
    D3D12_LOCAL_ROOT_SIGNATURE lrs = {0};
    lrs.pLocalRootSignature = p->local_sig;
    D3D12_RAYTRACING_PIPELINE_CONFIG pc = {0};
    pc.MaxTraceRecursionDepth = (UINT)desc->max_recursion;
    LPCWSTR assoc_exports[1 + 3 * VIO_RT_MAX_GROUPS];
    UINT assoc_n = 0;
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION assoc = {0};
    D3D12_STATE_SUBOBJECT sub[8 + VIO_RT_MAX_GROUPS];
    UINT ns = 0;
    sub[ns].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY; sub[ns].pDesc = &lib; ns++;
    for (int k = 0; k < desc->hit_count; k++) {
        hg[k].HitGroupExport = n_group[k];
        hg[k].Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hg[k].ClosestHitShaderImport = n_chit[k];
        hg[k].AnyHitShaderImport = desc->hit[k].spirv[1] ? n_ahit[k] : NULL;
        sub[ns].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP; sub[ns].pDesc = &hg[k]; ns++;
    }
    sub[ns].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG; sub[ns].pDesc = &sc; ns++;
    sub[ns].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;  sub[ns].pDesc = &grs; ns++;
    sub[ns].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG; sub[ns].pDesc = &pc; ns++;
    if (p->local_sig) {
        UINT li = ns;
        sub[ns].Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE; sub[ns].pDesc = &lrs; ns++;
        assoc_exports[assoc_n++] = n_raygen;
        for (int k = 0; k < desc->miss_count; k++) assoc_exports[assoc_n++] = n_miss[k];
        for (int k = 0; k < desc->hit_count; k++) assoc_exports[assoc_n++] = n_group[k];
        for (int k = 0; k < desc->callable_count; k++) assoc_exports[assoc_n++] = n_call[k];
        assoc.pSubobjectToAssociate = &sub[li];
        assoc.NumExports = assoc_n;
        assoc.pExports = assoc_exports;
        sub[ns].Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION; sub[ns].pDesc = &assoc; ns++;
    }
    D3D12_STATE_OBJECT_DESC so = {0};
    so.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    so.NumSubobjects = ns;
    so.pSubobjects = sub;
    HRESULT hr = ID3D12Device5_CreateStateObject(dev5, &so, &IID_ID3D12StateObject, (void **)&p->state);
    if (FAILED(hr) || !p->state) {
        php_error_docref(NULL, E_WARNING, "D3D12: CreateStateObject failed (0x%08lx): the library needs the exports "
                         "vio_raygen, vio_miss[<k>] (%d), vio_closest_hit[<k>] / vio_any_hit[<k>] (%d hit groups), "
                         "vio_callable<k> (%d); resources only at t0 / u0..u%d%s",
                         hr, desc->miss_count, desc->hit_count, desc->callable_count, VIO_D3D12_RT_UAVS - 1,
                         p->local_sig ? ", records at b0, space1" : "");
        d3d12_drain_info_queue("create_rt_pipeline");
        goto fail;
    }
    if (FAILED(ID3D12StateObject_QueryInterface(p->state, &IID_ID3D12StateObjectProperties, (void **)&props))) goto fail;

    /* Shader table: raygen | miss | hit groups | callables, each table 64-byte aligned. */
    p->stride = d3d12_align(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + (UINT64)desc->record_size, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    const UINT64 ta = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    p->off_miss = d3d12_align(p->stride, ta);
    p->off_hit = p->off_miss + d3d12_align(p->stride * (UINT64)desc->miss_count, ta);
    p->off_call = p->off_hit + d3d12_align(p->stride * (UINT64)desc->hit_count, ta);
    UINT64 total = p->off_call + d3d12_align(p->stride * (UINT64)(desc->callable_count > 0 ? desc->callable_count : 1), ta);
    p->table = d3d12_as_buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    unsigned char *map = NULL;
    D3D12_RANGE none = { 0, 0 };
    if (!p->table || FAILED(ID3D12Resource_Map(p->table, 0, &none, (void **)&map)) || !map) goto fail;
    memset(map, 0, (size_t)total);
    {
        struct { const WCHAR *name; const vio_rt_group_src *src; UINT64 at; } recs[1 + 3 * VIO_RT_MAX_GROUPS];
        int nr = 0;
        recs[nr].name = n_raygen; recs[nr].src = &desc->raygen; recs[nr].at = 0; nr++;
        for (int k = 0; k < desc->miss_count; k++) { recs[nr].name = n_miss[k]; recs[nr].src = &desc->miss[k]; recs[nr].at = p->off_miss + (UINT64)k * p->stride; nr++; }
        for (int k = 0; k < desc->hit_count; k++) { recs[nr].name = n_group[k]; recs[nr].src = &desc->hit[k]; recs[nr].at = p->off_hit + (UINT64)k * p->stride; nr++; }
        for (int k = 0; k < desc->callable_count; k++) { recs[nr].name = n_call[k]; recs[nr].src = &desc->callable[k]; recs[nr].at = p->off_call + (UINT64)k * p->stride; nr++; }
        for (int r = 0; r < nr; r++) {
            void *id = ID3D12StateObjectProperties_GetShaderIdentifier(props, recs[r].name);
            if (!id) {
                ID3D12Resource_Unmap(p->table, 0, NULL);
                php_error_docref(NULL, E_WARNING, "D3D12: the ray tracing library has no export for group %d", r);
                goto fail;
            }
            memcpy(map + recs[r].at, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
            if (desc->record_size > 0) memcpy(map + recs[r].at + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, recs[r].src->record, (size_t)desc->record_size);
        }
    }
    ID3D12Resource_Unmap(p->table, 0, NULL);

    ID3D12StateObjectProperties_Release(props);
    free(dxil);
    free(err);
    ID3D12Device5_Release(dev5);
    return p;

fail:
    if (props) ID3D12StateObjectProperties_Release(props);
    free(dxil);
    free(err);
    d3d12_rtp_free(p);
    ID3D12Device5_Release(dev5);
    return NULL;
}

static void d3d12_destroy_rt_pipeline(void *ptr)
{
    vio_d3d12_rtp *p = (vio_d3d12_rtp *)ptr;
    if (p && p->used_in_frame) {
        /* A trace on a frame list may still be recorded or in flight. */
        if (vio_d3d12.in_frame && vio_d3d12.cmd_list) {
            vio_d3d12.compute_async_pending++;
            d3d12_compute_wait();   /* submits the open frame so far and reopens it */
        } else {
            vio_d3d12_wait_for_gpu();
        }
    }
    d3d12_rtp_free(p);
}
/* Bound textures PIXEL_SHADER_RESOURCE <-> PIXEL | NON_PIXEL for a trace. */
static void d3d12_rt_texture_states(ID3D12GraphicsCommandList *list, const vio_rt_buffer_binding *b, int count, int before)
{
    for (int i = 0; i < count; i++) {
        if (b[i].kind != VIO_RT_BIND_TEXTURE) continue;
        vio_d3d12_texture *dt = (vio_d3d12_texture *)b[i].backend_buffer;
        if (!dt || !dt->resource) continue;
        D3D12_RESOURCE_BARRIER tb = {0};
        tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        tb.Transition.pResource = dt->resource;
        tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        D3D12_RESOURCE_STATES both = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        tb.Transition.StateBefore = before ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : both;
        tb.Transition.StateAfter = before ? both : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &tb);
    }
}

static int d3d12_trace_rays(void *ptr, const vio_rt_buffer_binding *buffers, int count, int w, int h, int d)
{
    vio_d3d12_rtp *p = (vio_d3d12_rtp *)ptr;
    if (!p || !p->state || !vio_d3d12.device) return -1;
    int in_frame = vio_d3d12.in_frame && vio_d3d12.cmd_list != NULL;
    if (!d3d12_bound_as || !d3d12_bound_as->tlas) {
        php_error_docref(NULL, E_WARNING, "vio_trace_rays: no acceleration structure bound (vio_bind_acceleration_structure)");
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if (buffers[i].binding >= VIO_D3D12_RT_UAVS) {
            php_error_docref(NULL, E_WARNING, "vio_trace_rays: D3D12 takes buffers at u0..u%d (binding %d)",
                             VIO_D3D12_RT_UAVS - 1, buffers[i].binding);
            return -1;
        }
    }
    if (d3d12_ensure_compute_srv_heap() != 0) return -1;
    if (!in_frame) d3d12_compute_wait();   /* in a frame the list keeps the order */
    /* Textures (A13): SRVs t1..t15 in a block of the compute heap, null views
     * where nothing is bound. */
    D3D12_GPU_DESCRIPTOR_HANDLE tex_gpu;
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.compute_srv_heap, &cpu);
        ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(vio_d3d12.compute_srv_heap, &tex_gpu);
        UINT dsz = vio_d3d12.compute_srv_descriptor_size;
        UINT block = vio_d3d12.compute_heap_block;
        vio_d3d12.compute_heap_block = (block + 1) % VIO_D3D12_COMPUTE_HEAP_BLOCKS;
        cpu.ptr += (SIZE_T)block * (2 * VIO_D3D12_COMPUTE_MAX_BINDINGS) * dsz;
        tex_gpu.ptr += (UINT64)block * (2 * VIO_D3D12_COMPUTE_MAX_BINDINGS) * dsz;
        D3D12_SHADER_RESOURCE_VIEW_DESC nd = {0};
        nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nd.Texture2D.MipLevels = 1;
        for (int t = 0; t < VIO_D3D12_RT_TEXTURES; t++) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = { cpu.ptr + (SIZE_T)t * dsz };
            vio_d3d12_texture *dt = NULL;
            for (int i = 0; i < count; i++)
                if (buffers[i].kind == VIO_RT_BIND_TEXTURE && buffers[i].binding == t + 1) dt = (vio_d3d12_texture *)buffers[i].backend_buffer;
            if (dt && dt->resource) ID3D12Device_CreateShaderResourceView(vio_d3d12.device, dt->resource, NULL, h);
            else ID3D12Device_CreateShaderResourceView(vio_d3d12.device, NULL, &nd, h);
        }
    }
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    ID3D12GraphicsCommandList4 *list4 = NULL;
    int rc = -1;
    if (in_frame) {
        /* Inside a frame (A13): record onto the frame list, in order with the
         * draws and async dispatches around it. */
        list = vio_d3d12.cmd_list;
    } else {
        if (FAILED(ID3D12Device_CreateCommandAllocator(vio_d3d12.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       &IID_ID3D12CommandAllocator, (void **)&alloc))) goto done;
        if (FAILED(ID3D12Device_CreateCommandList(vio_d3d12.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                                                  &IID_ID3D12GraphicsCommandList, (void **)&list))) goto done;
    }
    if (FAILED(ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList4, (void **)&list4))) goto done;
    if (in_frame) {
        /* Earlier UAV writes of the frame (async dispatches, traces) first. */
        D3D12_RESOURCE_BARRIER uav;
        memset(&uav, 0, sizeof(uav));
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &uav);
    }

    ID3D12GraphicsCommandList_SetComputeRootSignature(list, p->root_sig);
    ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(list, 0, ID3D12Resource_GetGPUVirtualAddress(d3d12_bound_as->tlas));
    {
        ID3D12DescriptorHeap *heaps[] = { vio_d3d12.compute_srv_heap };
        ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, heaps);
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 1 + VIO_D3D12_RT_UAVS, tex_gpu);
    }
    /* Textures rest in PIXEL_SHADER_RESOURCE; ray tracing stages read them as non-pixel. */
    d3d12_rt_texture_states(list, buffers, count, 1);
    for (int i = 0; i < count; i++) {
        if (buffers[i].kind != VIO_RT_BIND_BUFFER) continue;
        vio_d3d12_buffer *buf = (vio_d3d12_buffer *)buffers[i].backend_buffer;
        if (!buf || !buf->resource) continue;
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, (UINT)(1 + buffers[i].binding),
                                                                   ID3D12Resource_GetGPUVirtualAddress(buf->resource));
    }
    ID3D12GraphicsCommandList4_SetPipelineState1(list4, p->state);
    D3D12_GPU_VIRTUAL_ADDRESS t = ID3D12Resource_GetGPUVirtualAddress(p->table);
    D3D12_DISPATCH_RAYS_DESC dr;
    memset(&dr, 0, sizeof(dr));
    dr.RayGenerationShaderRecord.StartAddress = t;
    dr.RayGenerationShaderRecord.SizeInBytes = p->stride;
    dr.MissShaderTable.StartAddress = t + p->off_miss;
    dr.MissShaderTable.SizeInBytes = p->stride * (UINT64)p->miss_count;
    dr.MissShaderTable.StrideInBytes = p->stride;
    dr.HitGroupTable.StartAddress = t + p->off_hit;
    dr.HitGroupTable.SizeInBytes = p->stride * (UINT64)p->hit_count;
    dr.HitGroupTable.StrideInBytes = p->stride;
    if (p->callable_count > 0) {
        dr.CallableShaderTable.StartAddress = t + p->off_call;
        dr.CallableShaderTable.SizeInBytes = p->stride * (UINT64)p->callable_count;
        dr.CallableShaderTable.StrideInBytes = p->stride;
    }
    dr.Width = (UINT)w;
    dr.Height = (UINT)h;
    dr.Depth = (UINT)d;
    ID3D12GraphicsCommandList4_DispatchRays(list4, &dr);
    d3d12_rt_texture_states(list, buffers, count, 0);
    for (int i = 0; i < count; i++)
        if (buffers[i].kind == VIO_RT_BIND_BUFFER) d3d12_buffer_to_readback(list, (vio_d3d12_buffer *)buffers[i].backend_buffer);
    if (in_frame) {
        for (int i = 0; i < count; i++) {
            vio_d3d12_buffer *buf = (vio_d3d12_buffer *)buffers[i].backend_buffer;
            if (buffers[i].kind == VIO_RT_BIND_BUFFER && buf) buf->uav_live_serial = vio_d3d12.frame_serial;
        }
        p->used_in_frame = 1;
        /* Back to the frame's graphics state; readbacks wait for the frame so far. */
        d3d12_restore_graphics_state_after_compute();
        vio_d3d12.compute_async_pending++;
        rc = 0;
        goto done;
    }
    if (FAILED(ID3D12GraphicsCommandList_Close(list))) goto done;
    {
        ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
        ID3D12CommandQueue_ExecuteCommandLists(vio_d3d12.cmd_queue, 1, lists);
        vio_d3d12_wait_for_gpu();
    }
    d3d12_drain_info_queue("trace_rays");
    rc = 0;
done:
    if (list4) ID3D12GraphicsCommandList4_Release(list4);
    if (list && !in_frame) ID3D12GraphicsCommandList_Release(list);
    if (alloc) ID3D12CommandAllocator_Release(alloc);
    return rc;
}

/* ── Work graphs (VIO_FEATURE_WORK_GRAPHS) ────────────────────────── */

#ifdef VIO_D3D12_HAS_WORK_GRAPHS

#define VIO_D3D12_WG_BUFFERS 8          /* root UAVs u0..u7, space 0 */
#define VIO_D3D12_WG_MAX_BACKING (64ull << 20)

typedef struct _vio_d3d12_work_graph {
    ID3D12StateObject       *state_object;
    ID3D12RootSignature     *root_signature;   /* global: 8 root UAVs */
    ID3D12Resource          *backing;          /* GetWorkGraphMemoryRequirements */
    UINT64                   backing_size;
    D3D12_PROGRAM_IDENTIFIER program;
    UINT                     entry_index;
    int                      record_size;
    int                      initialized;      /* backing memory initialized by a SetProgram */
    UINT64                   used_serial;      /* frame_serial of the last in-frame dispatch */
    vio_d3d12_buffer        *buffers[VIO_D3D12_WG_BUFFERS];
} vio_d3d12_work_graph;

static void d3d12_work_graph_free(vio_d3d12_work_graph *g)
{
    if (!g) return;
    if (g->backing)        ID3D12Resource_Release(g->backing);
    if (g->state_object)   ID3D12StateObject_Release(g->state_object);
    if (g->root_signature) ID3D12RootSignature_Release(g->root_signature);
    free(g);
}

static char *d3d12_wg_error(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return estrdup(buf);
}

/* One graph over every node of the lib_6_8 library (INCLUDE_ALL_AVAILABLE_NODES),
 * a global root signature of eight root UAVs (u0..u7) and backing memory of
 * the size the runtime asks for. */
static void *d3d12_create_work_graph(const char *hlsl, const char *entry, int record_size, char **error)
{
    if (error) *error = NULL;
    void *dxil = NULL;
    size_t dxil_len = 0;
    char *dxc_err = NULL;
    if (vio_dxc_compile(hlsl, "", "lib_6_8", vio_d3d12.debug_enabled, vio_d3d12.native16, &dxil, &dxil_len, &dxc_err) != 0 || !dxil) {
        if (error) *error = d3d12_wg_error("lib_6_8 compile failed: %s", dxc_err ? dxc_err : "DXC unavailable");
        free(dxc_err);
        return NULL;
    }
    free(dxc_err);

    vio_d3d12_work_graph *g = (vio_d3d12_work_graph *)calloc(1, sizeof(*g));
    if (!g) { free(dxil); return NULL; }
    g->record_size = record_size;

    /* Global root signature: root UAV descriptors u0..u7 (raw / structured). */
    D3D12_ROOT_PARAMETER params[VIO_D3D12_WG_BUFFERS];
    memset(params, 0, sizeof(params));
    for (int i = 0; i < VIO_D3D12_WG_BUFFERS; i++) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[i].Descriptor.ShaderRegister = (UINT)i;
        params[i].Descriptor.RegisterSpace = 0;
        params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rs = {0};
    rs.NumParameters = VIO_D3D12_WG_BUFFERS;
    rs.pParameters = params;
    ID3DBlob *sig = NULL, *sig_err = NULL;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sig_err);
    if (SUCCEEDED(hr) && sig)
        hr = ID3D12Device_CreateRootSignature(vio_d3d12.device, 0, ID3D10Blob_GetBufferPointer(sig),
                                              ID3D10Blob_GetBufferSize(sig), &IID_ID3D12RootSignature,
                                              (void **)&g->root_signature);
    if (sig) ID3D10Blob_Release(sig);
    if (sig_err) ID3D10Blob_Release(sig_err);
    if (FAILED(hr) || !g->root_signature) {
        if (error) *error = d3d12_wg_error("root signature failed (0x%08lx)", (unsigned long)hr);
        free(dxil); d3d12_work_graph_free(g);
        return NULL;
    }

    /* State object: DXIL library (all exports) + global root signature + work graph. */
    D3D12_DXIL_LIBRARY_DESC lib = {0};
    lib.DXILLibrary.pShaderBytecode = dxil;
    lib.DXILLibrary.BytecodeLength = dxil_len;
    D3D12_GLOBAL_ROOT_SIGNATURE grs = { g->root_signature };
    D3D12_WORK_GRAPH_DESC wg;
    memset(&wg, 0, sizeof(wg));
    wg.ProgramName = L"vio_work_graph";
    wg.Flags = D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES;
    D3D12_STATE_SUBOBJECT subs[3];
    subs[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;     subs[0].pDesc = &lib;
    subs[1].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE; subs[1].pDesc = &grs;
    subs[2].Type = D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH;       subs[2].pDesc = &wg;
    D3D12_STATE_OBJECT_DESC so = {0};
    so.Type = D3D12_STATE_OBJECT_TYPE_EXECUTABLE;
    so.NumSubobjects = 3;
    so.pSubobjects = subs;

    ID3D12Device5 *dev5 = NULL;
    hr = ID3D12Device_QueryInterface(vio_d3d12.device, &IID_ID3D12Device5, (void **)&dev5);
    if (SUCCEEDED(hr) && dev5) {
        hr = ID3D12Device5_CreateStateObject(dev5, &so, &IID_ID3D12StateObject, (void **)&g->state_object);
        ID3D12Device5_Release(dev5);
    }
    free(dxil);
    if (FAILED(hr) || !g->state_object) {
        d3d12_drain_info_queue("create_work_graph");
        if (error) *error = d3d12_wg_error("CreateStateObject failed (0x%08lx) - check the node attributes and that "
                                           "every NodeOutput has a node", (unsigned long)hr);
        d3d12_work_graph_free(g);
        return NULL;
    }

    ID3D12StateObjectProperties1 *props = NULL;
    ID3D12WorkGraphProperties *wgp = NULL;
    ID3D12StateObject_QueryInterface(g->state_object, &IID_ID3D12StateObjectProperties1, (void **)&props);
    ID3D12StateObject_QueryInterface(g->state_object, &IID_ID3D12WorkGraphProperties, (void **)&wgp);
    int ok = props && wgp;
    UINT wg_index = 0;
    if (ok) {
        ID3D12StateObjectProperties1_GetProgramIdentifier(props, &g->program, L"vio_work_graph");
        wg_index = ID3D12WorkGraphProperties_GetWorkGraphIndex(wgp, L"vio_work_graph");
        ok = wg_index != 0xFFFFFFFFu;
    }
    if (ok) {
        wchar_t wentry[256];
        int n = MultiByteToWideChar(CP_UTF8, 0, entry, -1, wentry, 256);
        /* Look the name up among the entry points instead of asking
         * GetEntrypointIndex: an unknown name is a caller error vio reports
         * itself, and the query logs a debug-layer error for it. */
        g->entry_index = 0xFFFFFFFFu;
        UINT entries = n > 0 ? ID3D12WorkGraphProperties_GetNumEntrypoints(wgp, wg_index) : 0;
        for (UINT e = 0; e < entries; e++) {
            D3D12_NODE_ID id;
            ID3D12WorkGraphProperties_GetEntrypointID(wgp, &id, wg_index, e);   /* C ABI: struct return as out parameter */
            if (id.ArrayIndex == 0 && id.Name && wcscmp(id.Name, wentry) == 0) { g->entry_index = e; break; }
        }
        if (g->entry_index == 0xFFFFFFFFu) {
            if (error) *error = d3d12_wg_error("'%s' is not an entry node of the graph (a node no other node targets)", entry);
            ok = 0;
        } else {
            UINT rs_bytes = ID3D12WorkGraphProperties_GetEntrypointRecordSizeInBytes(wgp, wg_index, g->entry_index);
            if ((int)rs_bytes != record_size) {
                if (error) *error = d3d12_wg_error("entry node '%s' takes %u-byte records, record_size is %d",
                                                   entry, rs_bytes, record_size);
                ok = 0;
            }
        }
    } else if (error && !*error) {
        *error = d3d12_wg_error("the state object has no work graph properties");
    }
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS req;
    memset(&req, 0, sizeof(req));
    if (ok) ID3D12WorkGraphProperties_GetWorkGraphMemoryRequirements(wgp, wg_index, &req);
    if (props) ID3D12StateObjectProperties1_Release(props);
    if (wgp) ID3D12WorkGraphProperties_Release(wgp);
    if (!ok) { d3d12_work_graph_free(g); return NULL; }

    /* Backing memory: the maximum the runtime may use (faster), capped at
     * 64 MB but never below the minimum, in whole granules. */
    UINT64 size = req.MaxSizeInBytes;
    if (size > VIO_D3D12_WG_MAX_BACKING) size = VIO_D3D12_WG_MAX_BACKING;
    if (size < req.MinSizeInBytes) size = req.MinSizeInBytes;
    if (req.SizeGranularityInBytes > 1)
        size = (size + req.SizeGranularityInBytes - 1) / req.SizeGranularityInBytes * req.SizeGranularityInBytes;
    if (size > 0) {
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = size;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                  D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource,
                                                  (void **)&g->backing);
        if (FAILED(hr) || !g->backing) {
            if (error) *error = d3d12_wg_error("backing memory allocation failed (0x%08lx)", (unsigned long)hr);
            d3d12_work_graph_free(g);
            return NULL;
        }
        g->backing_size = size;
    }
    return g;
}

static void d3d12_destroy_work_graph(void *ptr)
{
    vio_d3d12_work_graph *g = (vio_d3d12_work_graph *)ptr;
    if (!g) return;
    /* Recorded into the open frame: submit the frame so far, then drain. */
    if (vio_d3d12.in_frame && g->used_serial == vio_d3d12.frame_serial) vio_d3d12.compute_async_pending++;
    d3d12_compute_wait();
    vio_d3d12_wait_for_gpu();
    d3d12_work_graph_free(g);
}

static void d3d12_work_graph_bind_buffer(void *ptr, void *buffer, int slot)
{
    vio_d3d12_work_graph *g = (vio_d3d12_work_graph *)ptr;
    if (!g || slot < 0 || slot >= VIO_D3D12_WG_BUFFERS) return;
    g->buffers[slot] = (vio_d3d12_buffer *)buffer;
}

static void d3d12_dispatch_graph(void *ptr, const void *records, int count)
{
    vio_d3d12_work_graph *g = (vio_d3d12_work_graph *)ptr;
    if (!g || !g->state_object || !records || count < 1) return;

    int in_frame = vio_d3d12.in_frame && vio_d3d12.cmd_list != NULL;
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    if (in_frame) {
        list = vio_d3d12.cmd_list;
    } else {
        HRESULT hr = ID3D12Device_CreateCommandAllocator(vio_d3d12.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         &IID_ID3D12CommandAllocator, (void **)&alloc);
        if (SUCCEEDED(hr))
            hr = ID3D12Device_CreateCommandList(vio_d3d12.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                                                &IID_ID3D12GraphicsCommandList, (void **)&list);
        if (FAILED(hr) || !list) {
            if (alloc) ID3D12CommandAllocator_Release(alloc);
            php_error_docref(NULL, E_WARNING, "D3D12: dispatch_graph command list failed (0x%08lx)", hr);
            return;
        }
    }
    ID3D12GraphicsCommandList10 *list10 = NULL;
    if (FAILED(ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList10, (void **)&list10)) || !list10) {
        php_error_docref(NULL, E_WARNING, "D3D12: dispatch_graph needs ID3D12GraphicsCommandList10");
        if (!in_frame) {
            ID3D12GraphicsCommandList_Close(list);
            ID3D12GraphicsCommandList_Release(list);
            ID3D12CommandAllocator_Release(alloc);
        }
        return;
    }

    ID3D12GraphicsCommandList_SetComputeRootSignature(list, g->root_signature);
    for (int i = 0; i < VIO_D3D12_WG_BUFFERS; i++) {
        vio_d3d12_buffer *b = g->buffers[i];
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, (UINT)i,
            b && b->resource ? ID3D12Resource_GetGPUVirtualAddress(b->resource) : 0);
        if (in_frame && b) b->uav_live_serial = vio_d3d12.frame_serial;   /* UAV on this frame's list (Block 8) */
    }
    /* Earlier writes (a dispatch, a previous graph on the same backing memory) first. */
    D3D12_RESOURCE_BARRIER all_uav = {0};
    all_uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    all_uav.UAV.pResource = NULL;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &all_uav);

    D3D12_SET_PROGRAM_DESC sp;
    memset(&sp, 0, sizeof(sp));
    sp.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
    sp.WorkGraph.ProgramIdentifier = g->program;
    sp.WorkGraph.Flags = g->initialized ? D3D12_SET_WORK_GRAPH_FLAG_NONE : D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
    if (g->backing) {
        sp.WorkGraph.BackingMemory.StartAddress = ID3D12Resource_GetGPUVirtualAddress(g->backing);
        sp.WorkGraph.BackingMemory.SizeInBytes = g->backing_size;
    }
    ID3D12GraphicsCommandList10_SetProgram(list10, &sp);
    g->initialized = 1;

    /* The records are copied into the command list at record time. */
    D3D12_DISPATCH_GRAPH_DESC dg;
    memset(&dg, 0, sizeof(dg));
    dg.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
    dg.NodeCPUInput.EntrypointIndex = g->entry_index;
    dg.NodeCPUInput.NumRecords = (UINT)count;
    dg.NodeCPUInput.pRecords = records;
    dg.NodeCPUInput.RecordStrideInBytes = (UINT64)g->record_size;
    ID3D12GraphicsCommandList10_DispatchGraph(list10, &dg);
    ID3D12GraphicsCommandList10_Release(list10);

    for (int i = 0; i < VIO_D3D12_WG_BUFFERS; i++)
        if (g->buffers[i] && g->buffers[i]->resource) d3d12_buffer_to_readback(list, g->buffers[i]);

    if (in_frame) {
        g->used_serial = vio_d3d12.frame_serial;
        d3d12_restore_graphics_state_after_compute();
        vio_d3d12.compute_async_pending++;   /* read_buffer waits for the frame */
        return;
    }
    ID3D12GraphicsCommandList_Close(list);
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(vio_d3d12.cmd_queue, 1, lists);
    vio_d3d12_wait_for_gpu();
    d3d12_drain_info_queue("dispatch_graph");
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(alloc);
}

#endif /* VIO_D3D12_HAS_WORK_GRAPHS */

/* ── Feature Query ────────────────────────────────────────────────── */

static void d3d12_swapchain_info(vio_swapchain_info *out)
{
    if (!out) return;
    out->buffer_count  = vio_d3d12.swapchain ? (int)vio_d3d12.frame_count : 0;
    out->frame_latency = vio_d3d12.frame_latency;
    out->waitable      = vio_d3d12.frame_latency_waitable != NULL;
    out->hdr_output    = vio_d3d12.hdr_output;
    out->format        = vio_d3d12.swapchain_format == DXGI_FORMAT_R10G10B10A2_UNORM ? VIO_FORMAT_RGB10A2 : VIO_FORMAT_RGBA8;
    out->shader_model  = vio_d3d12.shader_model == 6 ? 6 : 5;
    out->shader_model_version = vio_d3d12.shader_model_version;
    out->agility_sdk   = vio_d3d12.agility_sdk;
}

static double d3d12_gpu_frame_time(void)
{
    return vio_d3d12.initialized && vio_d3d12.ts_heap ? vio_d3d12.last_gpu_ms : -1.0;
}

static int d3d12_gpu_mark(const char *name)
{
    if (!vio_d3d12.initialized || !vio_d3d12.ts_heap || !vio_d3d12.in_frame) return 0;
    int i = vio_gpu_mark_push(&vio_d3d12.ts_marks[vio_d3d12.frame_index], name);
    if (i < 0) return 0;
    ID3D12GraphicsCommandList_EndQuery(vio_d3d12.cmd_list, vio_d3d12.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP,
                                       (UINT)(vio_d3d12.frame_index * VIO_GPU_TS_PER_FRAME + 2 + i));
    return 1;
}

static const vio_gpu_mark_result *d3d12_gpu_marks(void)
{
    return vio_d3d12.initialized && vio_d3d12.ts_heap && vio_d3d12.ts_result_valid ? &vio_d3d12.ts_result : NULL;
}

/* Second half of the optional-stage probe (see the D3D11 twin): the canonical
 * stage's SPIRV-Cross HLSL must also pass the compiler the shaders will use -
 * FXC for SM 5.1, DXC for the Shader Model 6 profile. The result is cached
 * per compile target (51, 60..69). */
static int d3d12_stage_supported(int stage, const char *profile)
{
    static int cache[20][VIO_PROBE_COUNT];
    static int cache_init = 0;
    if (!cache_init) { memset(cache, 0xFF, sizeof(cache)); cache_init = 1; }   /* -1 = untried */
    if (stage < 0 || stage >= VIO_PROBE_COUNT) return 0;
    int target = d3d12_hlsl_target();
    int row = target >= 60 && target <= 69 ? target - 50 : 0;   /* 0 = FXC 5.1, 10..19 = SM 6.0..6.9 */
    if (cache[row][stage] >= 0) return cache[row][stage];
    int ok = 0;
    if (vio_hlsl_stage_supported(stage)) {
        char *hlsl = vio_hlsl_probe_hlsl(stage, target);
        if (hlsl) {
            if (vio_d3d12.shader_model == 6) {
                char profile6[16];
                const char *p6 = d3d12_profile(profile, profile6, sizeof(profile6));
                void *bytes = NULL; size_t len = 0; char *err = NULL;
                ok = vio_dxc_compile(hlsl, "main", p6, 0, vio_d3d12.native16, &bytes, &len, &err) == 0 && bytes ? 1 : 0;
                if (!ok && getenv("VIO_DEBUG_STAGE_PROBE")) {
                    fprintf(stderr, "[vio] D3D12 stage probe %d (%s): DXC rejected the SPIRV-Cross HLSL: %s\n",
                            stage, p6, err ? err : "unknown");
                }
                free(bytes);
                free(err);
            } else {
                ID3DBlob *blob = NULL, *errs = NULL;
                HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "probe", NULL, NULL, "main", profile,
                                        D3DCOMPILE_OPTIMIZATION_LEVEL0, 0, &blob, &errs);
                ok = SUCCEEDED(hr) ? 1 : 0;
                if (!ok && getenv("VIO_DEBUG_STAGE_PROBE")) {
                    fprintf(stderr, "[vio] D3D12 stage probe %d (%s): FXC rejected the SPIRV-Cross HLSL: %s\n",
                            stage, profile, errs ? (const char *)ID3D10Blob_GetBufferPointer(errs) : "unknown");
                }
                if (blob) ID3D10Blob_Release(blob);
                if (errs) ID3D10Blob_Release(errs);
            }
            free(hlsl);
        }
    }
    cache[row][stage] = ok;
    return ok;
}

static int d3d12_supports_feature(vio_feature feature)
{
    switch (feature) {
        case VIO_FEATURE_COMPUTE:      return 1; /* compute pipeline + dispatch + readback wired */
        /* vio_shader 'geometry' / 'tess_control' + 'tess_eval': SPIR-V -> HLSL
         * gs/hs/ds_5_1 via SPIRV-Cross, baked into the PSO; per-stage root
         * CBV + SRV / sampler table mirrors (VIO_D3D12_RP_GS_* ..). Every
         * D3D12 device is feature level 11_0+, so the GPU side always has the
         * stages; the flag additionally requires a SPIRV-Cross that can emit
         * them (older Vulkan-SDK builds cannot - vio_hlsl_stage_supported). */
        case VIO_FEATURE_TESSELLATION:
            return d3d12_stage_supported(VIO_STAGE_TESS_CONTROL, "hs_5_1")
                && d3d12_stage_supported(VIO_STAGE_TESS_EVAL, "ds_5_1");
        case VIO_FEATURE_GEOMETRY:     return d3d12_stage_supported(VIO_STAGE_GEOMETRY, "gs_5_1");
        case VIO_FEATURE_HLSL_STAGE_OVERRIDE: return 1;   /* 'hlsl' => [stage => source] for GS / HS / DS */
        case VIO_FEATURE_GEOMETRY_INSTANCING: return d3d12_stage_supported(VIO_PROBE_GS_INSTANCED, "gs_5_1");   /* [instance(N)] */
        /* GL_KHR_shader_subgroup_* -> SPIRV-Cross wave intrinsics: needs the
         * DXC path (SM 6.0+) and a device with WaveOps. */
        case VIO_FEATURE_SUBGROUP:     return vio_d3d12.shader_model == 6 && vio_d3d12.wave_ops;
        case VIO_FEATURE_SUBGROUP_QUAD: return vio_d3d12.shader_model == 6 && vio_d3d12.wave_ops;   /* QuadReadAcross* (SM 6.0) */
        /* SV_Barycentrics: SPIRV-Cross needs an HLSL target of 6.1. */
        /* InterlockedX64 on raw buffers (SM 6.6, mandatory there); vio_shader_reflect.c
         * renames SPIRV-Cross's 32-bit method names for 64-bit operands. */
        /* Inline ray tracing: RayQuery (SM 6.5) needs DXR Tier 1.1. */
        case VIO_FEATURE_RAY_QUERY:    return vio_d3d12.shader_model == 6 && vio_d3d12.shader_model_version >= 65
                                              && vio_d3d12.raytracing_tier >= D3D12_RAYTRACING_TIER_1_1;
        /* View instancing (SV_ViewID, SM 6.1) through a pipeline-state stream. */
        case VIO_FEATURE_MULTIVIEW:    return vio_d3d12.shader_model == 6 && vio_d3d12.shader_model_version >= 61 && vio_d3d12.view_instancing > 0;
        /* SV_ViewID is valid in every stage of a view-instanced PSO (A27). */
        case VIO_FEATURE_MULTIVIEW_GEOMETRY:     return d3d12_supports_feature(VIO_FEATURE_MULTIVIEW) && d3d12_supports_feature(VIO_FEATURE_GEOMETRY);
        case VIO_FEATURE_MULTIVIEW_TESSELLATION: return d3d12_supports_feature(VIO_FEATURE_MULTIVIEW) && d3d12_supports_feature(VIO_FEATURE_TESSELLATION);
        case VIO_FEATURE_SHADER_FLOAT16: return vio_d3d12.shader_model == 6 && vio_d3d12.native16;   /* SM 6.2 half */
        /* SV_Start*Location from SM 6.8, below that the b13 root constants (A11). */
        case VIO_FEATURE_BASE_VERTEX:  return 1;
        case VIO_FEATURE_COMPUTE_DERIVATIVES: return vio_d3d12.shader_model == 6 && vio_d3d12.shader_model_version >= 66;
        /* Unbounded SRV table in register space 1 (Resource Binding Tier 2+);
         * works with FXC 5.1 and DXC. */
        case VIO_FEATURE_BINDLESS:     return vio_d3d12.bindless;
        /* MinMip feedback maps (SM 6.5 WriteSamplerFeedback, Tier 0.9). */
        case VIO_FEATURE_SAMPLER_FEEDBACK: return vio_d3d12.sampler_feedback;
        case VIO_FEATURE_ATOMIC64:     return vio_d3d12.shader_model == 6 && vio_d3d12.shader_model_version >= 66 && vio_d3d12.int64_ops;
        case VIO_FEATURE_BARYCENTRICS: return vio_d3d12.shader_model == 6 && vio_d3d12.shader_model_version >= 61 && vio_d3d12.barycentrics;
        case VIO_FEATURE_RAYTRACING:   return vio_d3d12.shader_model == 6 && vio_d3d12.shader_model_version >= 63
                                              && vio_d3d12.raytracing_tier >= D3D12_RAYTRACING_TIER_1_0; /* DXR 1.0 state objects, lib_6_3 */
        case VIO_FEATURE_3D_PIPELINE:  return 1;
        case VIO_FEATURE_READ_PIXELS:  return 1;
        case VIO_FEATURE_INSTANCED_DRAW: return 1;
        case VIO_FEATURE_RENDER_TARGET:       return 1;
        case VIO_FEATURE_RENDER_TARGET_HDR:   return 1;
        case VIO_FEATURE_RENDER_TARGET_DEPTH: return 1;
        /* Multisampled colour + depth resources per target, PSO SampleDesc
         * variants picked at bind time, ResolveSubresource on unbind (GAP-PHASE5
         * Block 1). */
        case VIO_FEATURE_RENDER_TARGET_MSAA:  return 1;
        case VIO_FEATURE_STENCIL:             return 1; /* D24S8 everywhere + PSO depth-stencil state */
        case VIO_FEATURE_GPU_TIMESTAMP:       return vio_d3d12.ts_heap != NULL; /* timestamp query heap + readback ring */
        case VIO_FEATURE_FRAME_LATENCY:       return 1; /* FRAME_LATENCY_WAITABLE_OBJECT swapchain */
        case VIO_FEATURE_HDR_OUTPUT:          return 1; /* RGB10A2 + SetColorSpace1(ST 2084), PSO format variants */
        case VIO_FEATURE_INDIRECT_DRAW:       return 1; /* ExecuteIndirect with DrawIndexed / Draw signatures */
        case VIO_FEATURE_TEXTURE_ARRAY:       return 1; /* DepthOrArraySize > 1 + TEXTURE2DARRAY SRV */
        case VIO_FEATURE_TEXTURE_COMPRESSION_BC: return 1; /* BC1-BC7 mandatory on every D3D12 device */
        case VIO_FEATURE_DEPTH_MIPMAPS: return 1;   /* d3d12_generate_depth_mips (A26) */
        case VIO_FEATURE_SHADING_RATE:        return vio_d3d12.vrs_tier > 0; /* RSSetShadingRate, VRS Tier 1+ (GAP-PHASE5 12) */
        /* SV_ShadingRate (SM 6.4) + the OVERRIDE combiner (Tier 2). */
        case VIO_FEATURE_MESH_SHADER:  return vio_d3d12.shader_model == 6 && vio_d3d12.shader_model_version >= 65 && vio_d3d12.mesh_tier > 0;
#ifdef VIO_D3D12_HAS_WORK_GRAPHS
        case VIO_FEATURE_WORK_GRAPHS:  return vio_d3d12.shader_model == 6 && vio_d3d12.shader_model_version >= 68 && vio_d3d12.work_graphs_tier >= 10;
#endif
        /* RSSetShadingRateImage + the MAX combiner (Tier 2). */
        case VIO_FEATURE_SHADING_RATE_IMAGE:  return vio_d3d12.vrs_tier >= 2 && vio_d3d12.vrs_tile_size > 0;
        case VIO_FEATURE_SHADING_RATE_PRIMITIVE: return vio_d3d12.vrs_tier >= 2 && vio_d3d12.shader_model == 6 && vio_d3d12.shader_model_version >= 64;
        case VIO_FEATURE_RENDER_TARGET_CUBE:  return 1; /* 6-slice array + per-(face,mip) RTVs (GAP-PLAN Phase 2) */
        case VIO_FEATURE_RENDER_TARGET_LAYERED: return 1; /* array resources: RTV / DSV per layer, array / cube SRVs */
        /* All-slice RTV / DSV + SV_RenderTargetArrayIndex from the GS. */
        /* SV_RenderTargetArrayIndex from the VS is core D3D12 (drivers without
         * VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation
         * emulate it), so layered binds always have a stage that picks the slice. */
        case VIO_FEATURE_LAYERED_RENDER: return 1;
        case VIO_FEATURE_VERTEX_LAYER:   return 1;
        case VIO_FEATURE_MULTI_VIEWPORT: return 1;   /* RSSetViewports(n) + scissor per viewport */
        case VIO_FEATURE_MIPMAP_GEN:          return 1; /* compute downsample (GAP-PHASE5 11), CPU box filter fallback */
        case VIO_FEATURE_CUBEMAP:      return 1;
        case VIO_FEATURE_DEPTH_BIAS:   return 1; /* PSO rasterizer state */
        case VIO_FEATURE_SCISSOR:      return 1;
        case VIO_FEATURE_TEXTURE_SWIZZLE: return 1; /* SRV Shader4ComponentMapping (R8 atlas -> (1,1,1,R)) */
        case VIO_FEATURE_NATIVE_2D_BATCH: return 1; /* vio_2d_d3d12_* */
        case VIO_FEATURE_TEXTURE_3D:   return 1; /* TEXTURE3D resource + SRV */
        case VIO_FEATURE_VERTEX_STORAGE: return 1; /* VS-visible root SRV in the shared root signature */
        case VIO_FEATURE_FRAGMENT_STORAGE: return 1; /* pixel root UAVs u4..u7 */
        case VIO_FEATURE_SAMPLER_FEEDBACK_GLSL: return 1;
        case VIO_FEATURE_STORAGE_IMAGE:  return 1; /* texture UAV in the compute UAV table */
        case VIO_FEATURE_MRT:            return 1; /* per-RT RTV heap with up to 4 descriptors, PSO 'attachments' */
        default:                       return 0;
    }
}

/* ── Graphics-stage storage buffers (Path B: readback-free instancing) ──── */

/* Bind a storage buffer to the vertex stage as root SRV param [3] (register t0,
 * VERTEX visibility). No explicit resource barrier: the compute UAV output lives
 * in / decays to COMMON after its (fenced, transient-list) dispatch, and a D3D12
 * buffer in COMMON is implicitly promoted to a shader-resource state on the
 * vertex-stage read — so the root SRV set here is sufficient. */
static void d3d12_bind_storage_buffer(void *backend_buffer, int binding, int access,
                                      int element_count, int stride)
{
    (void)binding; (void)access; (void)element_count; (void)stride;
    if (!vio_d3d12.initialized || !vio_d3d12.cmd_list || !backend_buffer) return;
    vio_d3d12_buffer *buf = (vio_d3d12_buffer *)backend_buffer;
    if (!buf->resource || !buf->gpu_address) return;
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(vio_d3d12.cmd_list, 3, buf->gpu_address);
}

/* Instanced draw pulling per-instance data from the bound storage buffer
 * (SV_InstanceID -> gl_InstanceIndex). Binds only the mesh vertex buffer (slot
 * 0) plus the shared identity buffer (slot 1, harmless — the from-storage VS
 * declares no location 3..6 attributes). Cbuffer flushing is the caller's job. */
static void d3d12_draw_instanced_from_storage(void *mesh_obj, int instance_count)
{
    vio_mesh_object *mesh = (vio_mesh_object *)mesh_obj;
    if (!vio_d3d12.initialized || !vio_d3d12.cmd_list || !mesh || instance_count <= 0) return;
    if (!d3d12_current_pipeline) return;   /* see d3d12_draw */

    vio_d3d12_buffer *vb = (vio_d3d12_buffer *)mesh->backend_vb;
    if (!vb) return;

    D3D12_VERTEX_BUFFER_VIEW vbvs[2];
    vbvs[0].BufferLocation = vb->gpu_address;
    vbvs[0].SizeInBytes = (UINT)vb->size;
    vbvs[0].StrideInBytes = (UINT)mesh->stride;
    vbvs[1].BufferLocation = vio_d3d12.identity_instance_gpu;
    vbvs[1].SizeInBytes = 64;
    vbvs[1].StrideInBytes = 64;
    ID3D12GraphicsCommandList_IASetVertexBuffers(vio_d3d12.cmd_list, 0, 2, vbvs);

    if (mesh->index_count > 0 && mesh->backend_ib) {
        vio_d3d12_buffer *ib = (vio_d3d12_buffer *)mesh->backend_ib;
        D3D12_INDEX_BUFFER_VIEW ibv = {0};
        ibv.BufferLocation = ib->gpu_address;
        ibv.SizeInBytes = (UINT)ib->size;
        ibv.Format = mesh->index_bytes == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        ID3D12GraphicsCommandList_IASetIndexBuffer(vio_d3d12.cmd_list, &ibv);
        d3d12_apply_view_mask();
        d3d12_apply_bindless();
        ID3D12GraphicsCommandList_DrawIndexedInstanced(vio_d3d12.cmd_list,
            (UINT)mesh->index_count, (UINT)instance_count, 0, 0, 0);
    } else {
        d3d12_apply_view_mask();
        d3d12_apply_bindless();
        ID3D12GraphicsCommandList_DrawInstanced(vio_d3d12.cmd_list,
            (UINT)mesh->vertex_count, (UINT)instance_count, 0, 0);
    }
}

/* ── State binding ────────────────────────────────────────────────── */

/* Indirect draw (GAP-PHASE5 Block 8): ExecuteIndirect with a DrawIndexed / Draw
 * command signature over max_draws records of the argument buffer. */
/* SPIRV-Cross' HLSL backend reads SV_ViewID in VS and PS only: vio_shader hands the
 * view to geometry / tessellation stages from the vertex stage (A27). */
static int d3d12_multiview_view_from_vertex(void)
{
    return 1;
}

static ID3D12CommandSignature *d3d12_indirect_signature(int indexed)
{
    ID3D12CommandSignature **slot = indexed ? &vio_d3d12.cmdsig_indexed : &vio_d3d12.cmdsig_plain;
    if (*slot) return *slot;
    D3D12_INDIRECT_ARGUMENT_DESC arg = {0};
    arg.Type = indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC sd = {0};
    sd.ByteStride = indexed ? 20 : 16;
    sd.NumArgumentDescs = 1;
    sd.pArgumentDescs = &arg;
    if (FAILED(ID3D12Device_CreateCommandSignature(vio_d3d12.device, &sd, NULL, &IID_ID3D12CommandSignature, (void **)slot))) {
        *slot = NULL;
    }
    return *slot;
}

/* Objects replaced mid-frame (a grown draw-parameter ring): released once this
 * frame slot's fence has passed (begin_frame), like the parked PSOs. An upload
 * signal in between would come too early for the frame list. */
#define VIO_D3D12_PARKED_MAX 16
static IUnknown *d3d12_parked[VIO_D3D12_MAX_FRAME_COUNT][VIO_D3D12_PARKED_MAX];
static int       d3d12_parked_count[VIO_D3D12_MAX_FRAME_COUNT];

static void d3d12_release_parked(UINT slot)
{
    if (slot >= VIO_D3D12_MAX_FRAME_COUNT) return;
    for (int i = 0; i < d3d12_parked_count[slot]; i++) {
        if (d3d12_parked[slot][i]) IUnknown_Release(d3d12_parked[slot][i]);
        d3d12_parked[slot][i] = NULL;
    }
    d3d12_parked_count[slot] = 0;
}

static void d3d12_release_parked_all(void)
{
    for (UINT s = 0; s < VIO_D3D12_MAX_FRAME_COUNT; s++) d3d12_release_parked(s);
}

static void d3d12_park_object(IUnknown *obj)
{
    UINT slot = vio_d3d12.frame_index < VIO_D3D12_MAX_FRAME_COUNT ? vio_d3d12.frame_index : 0;
    if (d3d12_parked_count[slot] < VIO_D3D12_PARKED_MAX) {
        d3d12_parked[slot][d3d12_parked_count[slot]++] = obj;
    } else {
        vio_d3d12_wait_for_gpu();   /* bookkeeping full: drain instead */
        IUnknown_Release(obj);
    }
}

/* Draw parameters below SM 6.8 (OPEN-ITEMS-PLAN A11): command signatures that set
 * the b13 root constants before each Draw(Indexed). */
static ID3D12CommandSignature *d3d12_indirect_signature_dp(int indexed)
{
    ID3D12CommandSignature **slot = indexed ? &vio_d3d12.cmdsig_indexed_dp : &vio_d3d12.cmdsig_plain_dp;
    if (*slot) return *slot;
    D3D12_INDIRECT_ARGUMENT_DESC arg[2];
    memset(arg, 0, sizeof(arg));
    arg[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    arg[0].Constant.RootParameterIndex = VIO_D3D12_RP_DRAW_PARAMS;
    arg[0].Constant.DestOffsetIn32BitValues = 0;
    arg[0].Constant.Num32BitValuesToSet = 2;
    arg[1].Type = indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC sd = {0};
    sd.ByteStride = indexed ? 28 : 24;
    sd.NumArgumentDescs = 2;
    sd.pArgumentDescs = arg;
    if (FAILED(ID3D12Device_CreateCommandSignature(vio_d3d12.device, &sd, vio_d3d12.root_signature,
                                                   &IID_ID3D12CommandSignature, (void **)slot))) {
        *slot = NULL;
    }
    return *slot;
}

/* Room for `bytes` in this frame's draw-parameter ring: the buffer and the offset.
 * A ring too small for the frame is replaced (the old one parked until the
 * frame's fence); begin_frame rewinds it. */
static ID3D12Resource *d3d12_dp_alloc(UINT64 bytes, UINT64 *offset)
{
    UINT slot = vio_d3d12.frame_index;
    if (!vio_d3d12.dp_buf[slot] || vio_d3d12.dp_used[slot] + bytes > vio_d3d12.dp_cap[slot]) {
        UINT64 cap = vio_d3d12.dp_cap[slot] ? vio_d3d12.dp_cap[slot] * 2 : 64 * 1024;
        while (cap < vio_d3d12.dp_used[slot] + bytes) cap *= 2;
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = cap;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource *buf = NULL;
        if (FAILED(ID3D12Device_CreateCommittedResource(vio_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&buf)) || !buf) return NULL;
        if (vio_d3d12.dp_buf[slot]) d3d12_park_object((IUnknown *)vio_d3d12.dp_buf[slot]);
        vio_d3d12.dp_buf[slot] = buf;
        vio_d3d12.dp_cap[slot] = cap;
        vio_d3d12.dp_used[slot] = 0;
    }
    *offset = vio_d3d12.dp_used[slot];
    vio_d3d12.dp_used[slot] += (bytes + 255) & ~(UINT64)255;
    return vio_d3d12.dp_buf[slot];
}

static void d3d12_buffer_barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(vio_d3d12.cmd_list, 1, &b);
}

/* vio records (indexed: indexCount, instanceCount, firstIndex, baseVertex,
 * firstInstance; plain: vertexCount, instanceCount, firstVertex, firstInstance)
 * become D3D12 records with gl_BaseVertex / gl_BaseInstance in front: two
 * CopyBufferRegion per record - the pair (baseVertex / firstVertex and
 * firstInstance sit next to each other), then the arguments. GPU-written
 * arguments stay GPU-side. Returns 0 when the draw was issued. */
static int d3d12_draw_indirect_dp(vio_d3d12_buffer *args, int indexed, int max_draws, size_t offset, int live_uav)
{
    ID3D12CommandSignature *sig = d3d12_indirect_signature_dp(indexed);
    if (!sig) return -1;
    UINT src_stride = indexed ? 20 : 16, dst_stride = src_stride + 8;
    UINT64 base = 0;
    ID3D12Resource *dp = d3d12_dp_alloc((UINT64)max_draws * dst_stride, &base);
    if (!dp) return -1;
    if (live_uav) {
        D3D12_RESOURCE_BARRIER u = {0};
        u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        u.UAV.pResource = args->resource;
        ID3D12GraphicsCommandList_ResourceBarrier(vio_d3d12.cmd_list, 1, &u);
        d3d12_buffer_barrier(args->resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    d3d12_buffer_barrier(dp, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT pair = indexed ? 12 : 8;   /* byte offset of baseVertex / firstVertex */
    for (int i = 0; i < max_draws; i++) {
        UINT64 s = (UINT64)offset + (UINT64)i * src_stride, d = base + (UINT64)i * dst_stride;
        ID3D12GraphicsCommandList_CopyBufferRegion(vio_d3d12.cmd_list, dp, d, args->resource, s + pair, 8);
        ID3D12GraphicsCommandList_CopyBufferRegion(vio_d3d12.cmd_list, dp, d + 8, args->resource, s, src_stride);
    }
    d3d12_buffer_barrier(dp, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    if (live_uav) d3d12_buffer_barrier(args->resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    d3d12_apply_view_mask();
    d3d12_apply_bindless();
    ID3D12GraphicsCommandList_ExecuteIndirect(vio_d3d12.cmd_list, sig, (UINT)max_draws, dp, base, NULL, 0);
    d3d12_buffer_barrier(dp, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COMMON);
    d3d12_apply_draw_params_zero();   /* the signature left the constants undefined */
    return 0;
}

static void d3d12_draw_indirect(void *mesh_obj, void *args_buffer, int max_draws, size_t offset)
{
    if (!d3d12_current_pipeline) return;   /* see d3d12_draw */
    vio_mesh_object *mesh = (vio_mesh_object *)mesh_obj;
    vio_d3d12_buffer *args = (vio_d3d12_buffer *)args_buffer;
    if (!vio_d3d12.initialized || !vio_d3d12.cmd_list || !mesh || !args || !args->resource || max_draws <= 0) return;
    vio_d3d12_buffer *vb = (vio_d3d12_buffer *)mesh->backend_vb;
    if (!vb) return;
    int indexed = mesh->index_count > 0 && mesh->backend_ib;
    ID3D12CommandSignature *sig = d3d12_indirect_signature(indexed);
    if (!sig) return;

    D3D12_VERTEX_BUFFER_VIEW vbvs[2];
    vbvs[0].BufferLocation = vb->gpu_address;
    vbvs[0].SizeInBytes = (UINT)vb->size;
    vbvs[0].StrideInBytes = (UINT)mesh->stride;
    vbvs[1].BufferLocation = vio_d3d12.identity_instance_gpu;
    vbvs[1].SizeInBytes = 64;
    vbvs[1].StrideInBytes = 64;
    ID3D12GraphicsCommandList_IASetVertexBuffers(vio_d3d12.cmd_list, 0, 2, vbvs);
    if (indexed) {
        vio_d3d12_buffer *ib = (vio_d3d12_buffer *)mesh->backend_ib;
        D3D12_INDEX_BUFFER_VIEW ibv = {0};
        ibv.BufferLocation = ib->gpu_address;
        ibv.SizeInBytes = (UINT)ib->size;
        ibv.Format = mesh->index_bytes == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        ID3D12GraphicsCommandList_IASetIndexBuffer(vio_d3d12.cmd_list, &ibv);
    }

    /* A buffer an async dispatch wrote on this frame's list sits in UNORDERED_ACCESS;
     * ExecuteIndirect needs INDIRECT_ARGUMENT (COMMON / GENERIC_READ promote by
     * themselves). Flush the UAV writes, transition, draw, transition back. */
    int live_uav = args->uav_live_serial != 0 && args->uav_live_serial == vio_d3d12.frame_serial;
    if (d3d12_current_pipeline->uses_draw_params && d3d12_draw_indirect_dp(args, indexed, max_draws, offset, live_uav) == 0) return;
    if (live_uav) {
        D3D12_RESOURCE_BARRIER b[2] = {0};
        b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b[0].UAV.pResource = args->resource;
        b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[1].Transition.pResource = args->resource;
        b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        ID3D12GraphicsCommandList_ResourceBarrier(vio_d3d12.cmd_list, 2, b);
    }
    d3d12_apply_view_mask();
    d3d12_apply_bindless();
    ID3D12GraphicsCommandList_ExecuteIndirect(vio_d3d12.cmd_list, sig, (UINT)max_draws, args->resource, (UINT64)offset, NULL, 0);
    if (live_uav) {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = args->resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ID3D12GraphicsCommandList_ResourceBarrier(vio_d3d12.cmd_list, 1, &b);
    }
}

/* ── Mesh pipelines (VIO_FEATURE_MESH_SHADER) ───────────────────── */

static int d3d12_mesh_bound(void)
{
    return vio_d3d12.initialized && vio_d3d12.cmd_list && d3d12_current_pipeline && d3d12_current_pipeline->is_mesh;
}

/* Runs before php_vio.c points root params 0 / 1 at this draw's vertex /
 * fragment blocks. A mesh pipeline re-asserts its root signature (a 2D flush or
 * compute pass in between may have switched it, which would also drop the root
 * arguments set after it) and hands the vertex-stage block to the
 * amplification stage through the GS CBV slot of the mesh root signature. */
static void d3d12_push_cbuffers(const void *vs_data, int vs_size, const void *fs_data, int fs_size)
{
    (void)fs_data; (void)fs_size;
    if (!d3d12_mesh_bound() || !vio_d3d12.mesh_root_signature) return;
    vio_d3d12_pipeline *p = d3d12_current_pipeline;
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(vio_d3d12.cmd_list, vio_d3d12.mesh_root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(vio_d3d12.cmd_list,
        d3d12_pipeline_pso_for_target(p, vio_d3d12.current_rt_samples, vio_d3d12.current_rt_format));
    ID3D12GraphicsCommandList_OMSetStencilRef(vio_d3d12.cmd_list, p->stencil_ref);
    vio_d3d12_bind_graphics_heaps(vio_d3d12.cmd_list);
    if (p->as_blob && vs_data && vs_size > 0 && vio_d3d12.cbuffer_heap_mapped) {
        UINT aligned = (UINT)((vs_size + 255) & ~255);
        UINT offset = vio_d3d12.cbuffer_heap_offset;
        if (offset + aligned <= vio_d3d12.cbuffer_frame_end) {
            memcpy(vio_d3d12.cbuffer_heap_mapped + offset, vs_data, (size_t)vs_size);
            ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(
                vio_d3d12.cmd_list, VIO_D3D12_RP_GS_CBV, vio_d3d12.cbuffer_heap_gpu + offset);
            vio_d3d12.cbuffer_heap_offset = offset + aligned;
        }
    }
}

static ID3D12GraphicsCommandList6 *d3d12_cmd_list6(void)
{
    if (!vio_d3d12.cmd_list6 &&
        FAILED(ID3D12GraphicsCommandList_QueryInterface(vio_d3d12.cmd_list, &IID_ID3D12GraphicsCommandList6, (void **)&vio_d3d12.cmd_list6)))
        vio_d3d12.cmd_list6 = NULL;
    return vio_d3d12.cmd_list6;
}

static void d3d12_draw_mesh_tasks(uint32_t x, uint32_t y, uint32_t z)
{
    if (!d3d12_mesh_bound() || !x || !y || !z) return;
    ID3D12GraphicsCommandList6 *cl6 = d3d12_cmd_list6();
    if (!cl6) return;
    vio_d3d12_flush_srv_table();
    d3d12_apply_view_mask();
    ID3D12GraphicsCommandList6_DispatchMesh(cl6, x, y, z);
}

static void d3d12_draw_mesh_tasks_indirect(void *args_buffer, int max_draws, size_t offset)
{
    vio_d3d12_buffer *args = (vio_d3d12_buffer *)args_buffer;
    if (!d3d12_mesh_bound() || !args || !args->resource || max_draws <= 0) return;
    if (!vio_d3d12.cmdsig_mesh) {
        D3D12_INDIRECT_ARGUMENT_DESC arg = {0};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
        D3D12_COMMAND_SIGNATURE_DESC sd = {0};
        sd.ByteStride = 12;   /* D3D12_DISPATCH_MESH_ARGUMENTS */
        sd.NumArgumentDescs = 1;
        sd.pArgumentDescs = &arg;
        if (FAILED(ID3D12Device_CreateCommandSignature(vio_d3d12.device, &sd, NULL, &IID_ID3D12CommandSignature,
                                                       (void **)&vio_d3d12.cmdsig_mesh)))
            vio_d3d12.cmdsig_mesh = NULL;
        if (!vio_d3d12.cmdsig_mesh) return;
    }
    vio_d3d12_flush_srv_table();
    /* Same UAV -> INDIRECT_ARGUMENT dance as d3d12_draw_indirect for a buffer an
     * async dispatch of this frame wrote. */
    int live_uav = args->uav_live_serial != 0 && args->uav_live_serial == vio_d3d12.frame_serial;
    if (live_uav) {
        D3D12_RESOURCE_BARRIER b[2] = {0};
        b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b[0].UAV.pResource = args->resource;
        b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[1].Transition.pResource = args->resource;
        b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        ID3D12GraphicsCommandList_ResourceBarrier(vio_d3d12.cmd_list, 2, b);
    }
    d3d12_apply_view_mask();
    ID3D12GraphicsCommandList_ExecuteIndirect(vio_d3d12.cmd_list, vio_d3d12.cmdsig_mesh, (UINT)max_draws,
                                              args->resource, (UINT64)offset, NULL, 0);
    if (live_uav) {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = args->resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ID3D12GraphicsCommandList_ResourceBarrier(vio_d3d12.cmd_list, 1, &b);
    }
}

static void d3d12_set_uniform(const char *name, const void *data, int count, int type)
{
    /* Pushes the uniform value into the per-frame cbuffer heap and binds its GPU-virtual
     * address as a root CBV (b0) for both VS (root param 0) and PS (root param 1).
     *
     * The root signature declares b0 per stage, so SPIRV-Cross places the combined UBO
     * at b0 for both. We mirror the same slice into VS and PS. This is a convenience
     * fallback for simple `vio_set_uniform()` usage; larger uniform data should go
     * through `vio_uniform_buffer()` + `vio_bind_buffer()`. */
    if (!vio_d3d12.cmd_list || !vio_d3d12.cbuffer_heap || !vio_d3d12.cbuffer_heap_mapped) return;

    size_t data_size;
    switch (type) {
        case VIO_UNIFORM_INT:   data_size = sizeof(int)   *  1 * count; break;
        case VIO_UNIFORM_FLOAT: data_size = sizeof(float) *  1 * count; break;
        case VIO_UNIFORM_VEC2:  data_size = sizeof(float) *  2 * count; break;
        case VIO_UNIFORM_VEC3:  data_size = sizeof(float) *  3 * count; break;
        case VIO_UNIFORM_VEC4:  data_size = sizeof(float) *  4 * count; break;
        case VIO_UNIFORM_MAT3:  data_size = sizeof(float) *  9 * count; break;
        case VIO_UNIFORM_MAT4:  data_size = sizeof(float) * 16 * count; break;
        default: return;
    }

    /* CBV must be 256-byte aligned */
    UINT aligned_size = (UINT)((data_size + 255) & ~255u);
    if (vio_d3d12.cbuffer_heap_offset + aligned_size > vio_d3d12.cbuffer_frame_end) {
        /* This frame's slice ran out — heap will grow at next begin_frame.
         * (Never spill past the slice: the bytes beyond it belong to the
         * other in-flight frame.) */
        return;
    }

    UINT offset = vio_d3d12.cbuffer_heap_offset;
    vio_d3d12.cbuffer_heap_offset += aligned_size;

    memcpy(vio_d3d12.cbuffer_heap_mapped + offset, data, data_size);

    D3D12_GPU_VIRTUAL_ADDRESS gpu_addr = vio_d3d12.cbuffer_heap_gpu + offset;
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(vio_d3d12.cmd_list, 0, gpu_addr);
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(vio_d3d12.cmd_list, 1, gpu_addr);

    (void)name;
}

static void d3d12_bind_texture(void *texture, int slot)
{
    if (!texture || slot < 0 || slot >= VIO_D3D12_SRV_TABLE_SIZE) return;
    vio_d3d12_texture *tex = (vio_d3d12_texture *)texture;

    /* Store pending binding (+ the texture's sampler combo for s<slot>) —
     * flushed before draw via vio_d3d12_flush_srv_table(). */
    vio_d3d12_bind_srv_slot(tex->srv_cpu, slot, tex->sampler_index);
}

/* Build (or reuse) the 8-sampler block matching pending_samplers[] for the
 * bound SRV slots and point root param 4 at it. Slots without a texture get
 * combo 0 so the block content is fully determined by the bound set. */
/* Point the PS SRV / sampler table root parameters at `gpu` and mirror the
 * same block into the GEOMETRY / HULL / DOMAIN table parameters when the
 * bound pipeline carries those stages (a domain shader sampling a
 * displacement map sees the texture at the same register as the PS). */
static void d3d12_set_srv_tables(D3D12_GPU_DESCRIPTOR_HANDLE gpu)
{
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(vio_d3d12.cmd_list, VIO_D3D12_RP_PS_SRV, gpu);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(vio_d3d12.cmd_list, VIO_D3D12_RP_VS_SRV, gpu);
    vio_d3d12_pipeline *p = d3d12_current_pipeline;
    if (!p) return;
    if (p->has_gs || p->is_mesh) ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(vio_d3d12.cmd_list, VIO_D3D12_RP_GS_SRV, gpu);
    if (p->has_hs || p->is_mesh) ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(vio_d3d12.cmd_list, VIO_D3D12_RP_HS_SRV, gpu);
    if (p->has_ds) ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(vio_d3d12.cmd_list, VIO_D3D12_RP_DS_SRV, gpu);
}

static void d3d12_set_sampler_tables(D3D12_GPU_DESCRIPTOR_HANDLE gpu)
{
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(vio_d3d12.cmd_list, VIO_D3D12_RP_PS_SAMPLER, gpu);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(vio_d3d12.cmd_list, VIO_D3D12_RP_VS_SAMPLER, gpu);
    vio_d3d12_pipeline *p = d3d12_current_pipeline;
    if (!p) return;
    if (p->has_gs || p->is_mesh) ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(vio_d3d12.cmd_list, VIO_D3D12_RP_GS_SAMPLER, gpu);
    if (p->has_hs || p->is_mesh) ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(vio_d3d12.cmd_list, VIO_D3D12_RP_HS_SAMPLER, gpu);
    if (p->has_ds) ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(vio_d3d12.cmd_list, VIO_D3D12_RP_DS_SAMPLER, gpu);
}

static void d3d12_flush_sampler_table(void)
{
    if (!vio_d3d12.sampler_heap || !vio_d3d12.sampler_combo_heap) return;

    int combos[VIO_D3D12_SAMPLER_TABLE_SIZE];
    for (int i = 0; i < VIO_D3D12_SAMPLER_TABLE_SIZE; i++) {
        combos[i] = vio_d3d12.pending_srv_valid[i] ? vio_d3d12.pending_samplers[i] : 0;
    }

    /* Recording a bundle: a table outside the frame rings. */
    if (d3d12_brec) {
        D3D12_GPU_DESCRIPTOR_HANDLE g;
        if (d3d12_brec_sampler_table(combos, &g) == 0) d3d12_set_sampler_tables(g);
        else d3d12_brec_fail();
        return;
    }

    /* Already bound with this exact set? */
    if (vio_d3d12.sampler_table_bound &&
        memcmp(combos, vio_d3d12.sampler_set_cache[0].combos, sizeof(combos)) == 0) {
        return;
    }

    /* Built earlier THIS frame? Re-point without consuming ring space. Entry 0
     * mirrors the block currently bound so the fast check above stays O(1). */
    for (int i = 0; i < vio_d3d12.sampler_set_count; i++) {
        if (memcmp(combos, vio_d3d12.sampler_set_cache[i].combos, sizeof(combos)) == 0) {
            vio_d3d12.sampler_table_gpu = vio_d3d12.sampler_set_cache[i].gpu;
            d3d12_set_sampler_tables(vio_d3d12.sampler_table_gpu);
            vio_d3d12.sampler_table_bound = 1;
            if (i != 0) {
                /* swap to front */
                unsigned char tmp[sizeof(vio_d3d12.sampler_set_cache[0])];
                memcpy(tmp, &vio_d3d12.sampler_set_cache[0], sizeof(tmp));
                memcpy(&vio_d3d12.sampler_set_cache[0], &vio_d3d12.sampler_set_cache[i], sizeof(tmp));
                memcpy(&vio_d3d12.sampler_set_cache[i], tmp, sizeof(tmp));
            }
            return;
        }
    }

    UINT base = vio_d3d12.sampler_frame_offset;
    UINT region_end = vio_d3d12.sampler_frame_base + vio_d3d12.sampler_frame_capacity;
    if (base + VIO_D3D12_SAMPLER_TABLE_SIZE > region_end) {
        /* Ring exhausted (hundreds of distinct sampler sets in one frame):
         * keep whatever block is bound rather than drop the draw. */
        if (vio_d3d12.sampler_table_bound) {
            d3d12_set_sampler_tables(vio_d3d12.sampler_table_gpu);
        }
        return;
    }
    vio_d3d12.sampler_frame_offset += VIO_D3D12_SAMPLER_TABLE_SIZE;

    D3D12_CPU_DESCRIPTOR_HANDLE dst_cpu, combo_base;
    D3D12_GPU_DESCRIPTOR_HANDLE dst_gpu;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.sampler_heap, &dst_cpu);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(vio_d3d12.sampler_heap, &dst_gpu);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.sampler_combo_heap, &combo_base);
    dst_cpu.ptr += (SIZE_T)base * vio_d3d12.sampler_descriptor_size;
    dst_gpu.ptr += (UINT64)base * vio_d3d12.sampler_descriptor_size;

    D3D12_CPU_DESCRIPTOR_HANDLE srcs[VIO_D3D12_SAMPLER_TABLE_SIZE];
    for (int i = 0; i < VIO_D3D12_SAMPLER_TABLE_SIZE; i++) {
        srcs[i].ptr = combo_base.ptr + (SIZE_T)combos[i] * vio_d3d12.sampler_descriptor_size;
    }
    UINT dst_size = VIO_D3D12_SAMPLER_TABLE_SIZE;
    ID3D12Device_CopyDescriptors(vio_d3d12.device, 1, &dst_cpu, &dst_size,
                                 VIO_D3D12_SAMPLER_TABLE_SIZE, srcs, NULL,
                                 D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    d3d12_set_sampler_tables(dst_gpu);

    vio_d3d12.sampler_table_gpu = dst_gpu;
    vio_d3d12.sampler_table_bound = 1;
    /* Remember it (front of the cache); evict the oldest when full. */
    int n = vio_d3d12.sampler_set_count;
    if (n >= VIO_D3D12_SAMPLER_SET_CACHE) n = VIO_D3D12_SAMPLER_SET_CACHE - 1;
    memmove(&vio_d3d12.sampler_set_cache[1], &vio_d3d12.sampler_set_cache[0], (size_t)n * sizeof(vio_d3d12.sampler_set_cache[0]));
    memcpy(vio_d3d12.sampler_set_cache[0].combos, combos, sizeof(combos));
    vio_d3d12.sampler_set_cache[0].gpu = dst_gpu;
    vio_d3d12.sampler_set_count = n + 1;
}

/* Flush pending texture bindings into a contiguous SRV descriptor block */
void vio_d3d12_flush_srv_table(void)
{
    int any_bound = 0;
    for (int i = 0; i < VIO_D3D12_SRV_TABLE_SIZE; i++) {
        if (vio_d3d12.pending_srv_valid[i]) { any_bound = 1; break; }
    }
    if (!any_bound) return; /* no textures — skip descriptor table binding entirely */

    /* Samplers first: independent ring + cache, cheap when the set repeats. */
    d3d12_flush_sampler_table();

    /* VIO_D3D12_NO_SRV_DEDUP=1 forces the per-draw rebuild (A/B diagnostic toggle,
     * probed once). Both paths are pixel-identical; this isolates the dedup's win. */
    static int s_dedup_disabled = -1;
    if (s_dedup_disabled < 0) {
        const char *e = getenv("VIO_D3D12_NO_SRV_DEDUP");
        s_dedup_disabled = (e && e[0] == '1') ? 1 : 0;
    }

    /* DEDUP FAST-PATH: if a block was already built THIS frame for the exact same
     * set of bound SRVs (and no begin_frame / pipeline bind invalidated it since),
     * the cached block is still valid for the rest of the frame — the per-frame
     * SRV ring only advances, so an earlier block is never overwritten, and a
     * CopyDescriptorsSimple'd view stays valid as long as it views the same
     * resource (same source CPU handle ptr). Re-point root param 2 at the cached
     * GPU handle and skip the 16-descriptor rebuild. Content-based, so it is
     * correct regardless of which caller populated pending_srvs (3D draw paths,
     * instanced path, or the 2D batch renderer writing pending_srvs directly). */
    if (!s_dedup_disabled && vio_d3d12.srv_table_bound) {
        int same = 1;
        for (int i = 0; i < VIO_D3D12_SRV_TABLE_SIZE; i++) {
            if (vio_d3d12.pending_srv_valid[i] != vio_d3d12.srv_flushed_valid[i] ||
                vio_d3d12.pending_srvs[i].ptr != vio_d3d12.srv_flushed[i].ptr) {
                same = 0;
                break;
            }
        }
        if (same) {
            d3d12_set_srv_tables(vio_d3d12.srv_table_gpu);
            return;
        }
    }

    /* Allocate VIO_D3D12_SRV_TABLE_SIZE contiguous descriptors from THIS frame's
     * region of the SRV heap. Bound the check by the region end (not the full
     * heap) so we can't bleed into another frame's slice. */
    UINT base_idx = vio_d3d12.srv_frame_offset;
    UINT region_end = vio_d3d12.srv_frame_base + vio_d3d12.srv_frame_capacity;
    if (base_idx + VIO_D3D12_SRV_TABLE_SIZE > region_end) {   /* out of space in this frame's region */
        if (d3d12_brec) d3d12_brec_fail();                      /* the bundle's range: record it again larger */
        return;
    }
    vio_d3d12.srv_frame_offset += VIO_D3D12_SRV_TABLE_SIZE;

    D3D12_CPU_DESCRIPTOR_HANDLE dst_cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE dst_gpu;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.srv_heap.heap, &dst_cpu);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(vio_d3d12.srv_heap.heap, &dst_gpu);
    dst_cpu.ptr += base_idx * vio_d3d12.srv_heap.descriptor_size;
    dst_gpu.ptr += base_idx * vio_d3d12.srv_heap.descriptor_size;

    /* Null-initialise the block (one copy of the pre-built null block — GAP-PLAN
     * 4.3), then overwrite the bound slots. */
    if (vio_d3d12.null_srv_block_valid) {
        ID3D12Device_CopyDescriptorsSimple(vio_d3d12.device, VIO_D3D12_SRV_TABLE_SIZE,
            dst_cpu, vio_d3d12.null_srv_block, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    } else {
        for (int i = 0; i < VIO_D3D12_SRV_TABLE_SIZE; i++) {
            D3D12_CPU_DESCRIPTOR_HANDLE slot_cpu = dst_cpu;
            slot_cpu.ptr += i * vio_d3d12.srv_heap.descriptor_size;
            D3D12_SHADER_RESOURCE_VIEW_DESC null_srv = {0};
            null_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            null_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            null_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            null_srv.Texture2D.MipLevels = 1;
            ID3D12Device_CreateShaderResourceView(vio_d3d12.device, NULL, &null_srv, slot_cpu);
        }
    }

    /* Overwrite bound slots with actual texture SRVs */
    for (int i = 0; i < VIO_D3D12_SRV_TABLE_SIZE; i++) {
        if (vio_d3d12.pending_srv_valid[i] && vio_d3d12.pending_srvs[i].ptr) {
            D3D12_CPU_DESCRIPTOR_HANDLE slot_cpu = dst_cpu;
            slot_cpu.ptr += i * vio_d3d12.srv_heap.descriptor_size;
            ID3D12Device_CopyDescriptorsSimple(vio_d3d12.device, 1,
                slot_cpu, vio_d3d12.pending_srvs[i],
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }

    d3d12_set_srv_tables(dst_gpu);

    /* Cache this block + the texture set it was built from for the dedup
     * fast-path above (valid until begin_frame rebases the ring or a pipeline
     * bind resets root param 2). */
    vio_d3d12.srv_table_gpu = dst_gpu;
    memcpy(vio_d3d12.srv_flushed, vio_d3d12.pending_srvs, sizeof(vio_d3d12.srv_flushed));
    memcpy(vio_d3d12.srv_flushed_valid, vio_d3d12.pending_srv_valid, sizeof(vio_d3d12.srv_flushed_valid));
    vio_d3d12.srv_table_bound = 1;
}

static void d3d12_set_viewport(int x, int y, int width, int height)
{
    /* D3D11 callers issue vio_viewport() before vio_begin() to set the render
     * target binding state — D3D12 has no equivalent need (RSSetViewports is
     * a command-list op, not a device state). When called outside a frame we
     * simply skip; vio_begin → d3d12_begin_frame is followed by a second
     * viewport call which records onto the now-open command list. */
    if (!vio_d3d12.in_frame || !vio_d3d12.cmd_list) {
        return;
    }

    D3D12_VIEWPORT vp = {0};
    vp.TopLeftX = (float)x;
    vp.TopLeftY = (float)y;
    vp.Width = (float)width;
    vp.Height = (float)height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D12GraphicsCommandList_RSSetViewports(vio_d3d12.cmd_list, 1, &vp);

    D3D12_RECT scissor = {x, y, x + width, y + height};
    ID3D12GraphicsCommandList_RSSetScissorRects(vio_d3d12.cmd_list, 1, &scissor);
}

/* Several viewports, each with its own scissor (D3D12 always scissors, and a
 * viewport without a scissor rect draws nothing). */
static int d3d12_set_viewports(const int *rects, int count)
{
    if (!vio_d3d12.in_frame || !vio_d3d12.cmd_list || count < 1 || count > VIO_MAX_VIEWPORTS) return -1;
    D3D12_VIEWPORT vps[VIO_MAX_VIEWPORTS];
    D3D12_RECT scs[VIO_MAX_VIEWPORTS];
    for (int i = 0; i < count; i++) {
        int x = rects[i * 4], y = rects[i * 4 + 1], w = rects[i * 4 + 2], h = rects[i * 4 + 3];
        vps[i].TopLeftX = (float)x; vps[i].TopLeftY = (float)y;
        vps[i].Width = (float)w;    vps[i].Height = (float)h;
        vps[i].MinDepth = 0.0f;     vps[i].MaxDepth = 1.0f;
        scs[i].left = x; scs[i].top = y; scs[i].right = x + w; scs[i].bottom = y + h;
    }
    ID3D12GraphicsCommandList_RSSetViewports(vio_d3d12.cmd_list, (UINT)count, vps);
    ID3D12GraphicsCommandList_RSSetScissorRects(vio_d3d12.cmd_list, (UINT)count, scs);
    return 0;
}

/* ── Setup context (called from vio_create after window creation) ── */

int vio_d3d12_setup_context(void *platform_window, vio_config *cfg)
{
    vio_d3d12.platform_window = platform_window;

    /* Create surface (swapchain + render targets + depth buffer) */
    void *surface = d3d12_create_surface(cfg);
    if (!surface) {
        return -1;
    }

    return 0;
}

/* ── Recorded draw sequences (BUNDLE-PLAN phase 3) ─────────────────── */

/* A bundle is a D3D12 bundle command list. While one is recorded the draw
 * path records into it unchanged: vio_d3d12.cmd_list is the bundle list, the
 * cbuffer ring is a bundle-owned upload buffer (the root CBVs point into it in
 * every later frame), the SRV ring a bundle-owned range of the shader-visible
 * heap and sampler tables come from a reserve at the top of the sampler heap.
 * A bundle inherits the target, viewports and heaps of the list that runs it,
 * so its signature is the PSO variant (samples, format); after it the bound
 * pipeline is armed again (a bundle leaks its PSO and root arguments). */
struct _vio_d3d12_bundle {
    ID3D12CommandAllocator    *alloc;
    ID3D12GraphicsCommandList *list;
    ID3D12Resource            *cb;
    unsigned char             *cb_mapped;
    D3D12_GPU_VIRTUAL_ADDRESS  cb_gpu;
    UINT                       cb_size;
    UINT                       srv_base, srv_count;
    UINT                       samples;
    DXGI_FORMAT                format;
    int                        failed;
    int                        released;    /* GPU objects gone with the device */
    UINT64                     grave_fence; /* destroyed: free once this passed (0 = in the open frame) */
    struct _vio_d3d12_bundle  *prev, *next;
};

static vio_d3d12_bundle *d3d12_bundles;   /* live bundles */
static vio_d3d12_bundle *d3d12_graves;    /* destroyed, waiting for their fence */
static UINT d3d12_bundle_cb_hint  = 256 * 1024;
static UINT d3d12_bundle_srv_hint = 64;   /* tables */
static struct { int combos[VIO_D3D12_SAMPLER_TABLE_SIZE]; } d3d12_bundle_samplers[VIO_D3D12_BUNDLE_SAMPLER_SETS];
static int d3d12_bundle_sampler_count;
/* freed SRV ranges, reused for a range of the same size */
static struct { UINT base, count; } d3d12_srv_free[32];
static int d3d12_srv_free_count;
static struct {
    ID3D12GraphicsCommandList *cmd_list;
    ID3D12Resource            *cb_heap;
    unsigned char             *cb_mapped;
    D3D12_GPU_VIRTUAL_ADDRESS  cb_gpu;
    UINT                       cb_offset, cb_capacity, cb_base, cb_end;
    UINT                       srv_offset, srv_base, srv_capacity;
    vio_d3d12_pipeline        *pipe;
} d3d12_brec_saved;

static void d3d12_brec_fail(void) { if (d3d12_brec) d3d12_brec->failed = 1; }

static int d3d12_brec_sampler_table(const int *combos, D3D12_GPU_DESCRIPTOR_HANDLE *out)
{
    if (!vio_d3d12.sampler_heap || !vio_d3d12.sampler_combo_heap) return -1;
    int i;
    for (i = 0; i < d3d12_bundle_sampler_count; i++)
        if (memcmp(d3d12_bundle_samplers[i].combos, combos, sizeof(d3d12_bundle_samplers[i].combos)) == 0) break;
    UINT base = VIO_D3D12_SAMPLER_HEAP_CAPACITY - VIO_D3D12_BUNDLE_SAMPLER_RESERVE + (UINT)i * VIO_D3D12_SAMPLER_TABLE_SIZE;
    if (i == d3d12_bundle_sampler_count) {
        if (i >= VIO_D3D12_BUNDLE_SAMPLER_SETS) return -1;
        D3D12_CPU_DESCRIPTOR_HANDLE dst, combo_base, srcs[VIO_D3D12_SAMPLER_TABLE_SIZE];
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.sampler_heap, &dst);
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.sampler_combo_heap, &combo_base);
        dst.ptr += (SIZE_T)base * vio_d3d12.sampler_descriptor_size;
        for (int s = 0; s < VIO_D3D12_SAMPLER_TABLE_SIZE; s++)
            srcs[s].ptr = combo_base.ptr + (SIZE_T)combos[s] * vio_d3d12.sampler_descriptor_size;
        UINT n = VIO_D3D12_SAMPLER_TABLE_SIZE;
        ID3D12Device_CopyDescriptors(vio_d3d12.device, 1, &dst, &n, VIO_D3D12_SAMPLER_TABLE_SIZE, srcs, NULL,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        memcpy(d3d12_bundle_samplers[i].combos, combos, sizeof(d3d12_bundle_samplers[i].combos));
        d3d12_bundle_sampler_count++;
    }
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(vio_d3d12.sampler_heap, out);
    out->ptr += (UINT64)base * vio_d3d12.sampler_descriptor_size;
    return 0;
}

/* A contiguous range of the shader-visible SRV heap that outlives frames: a
 * freed one of the same size, else carved from the static (downward) region. */
static int d3d12_bundle_srv_alloc(UINT count, UINT *base)
{
    for (int i = 0; i < d3d12_srv_free_count; i++) {
        if (d3d12_srv_free[i].count != count) continue;
        *base = d3d12_srv_free[i].base;
        d3d12_srv_free[i] = d3d12_srv_free[--d3d12_srv_free_count];
        return 0;
    }
    if (vio_d3d12.srv_heap.count + count > vio_d3d12.srv_heap.capacity) return -1;
    vio_d3d12.srv_heap.count += count;
    *base = vio_d3d12.srv_heap.capacity - vio_d3d12.srv_heap.count;
    return 0;
}

static void d3d12_bundle_unlink(vio_d3d12_bundle *b)
{
    if (b->prev) b->prev->next = b->next; else if (d3d12_bundles == b) d3d12_bundles = b->next;
    if (b->next) b->next->prev = b->prev;
    b->prev = b->next = NULL;
}

/* Release the GPU objects (the GPU is done with them); the range goes back to
 * the free list unless the device is going away. */
static void d3d12_bundle_release(vio_d3d12_bundle *b, int keep_range)
{
    if (b->list)  { ID3D12GraphicsCommandList_Release(b->list); b->list = NULL; }
    if (b->alloc) { ID3D12CommandAllocator_Release(b->alloc); b->alloc = NULL; }
    if (b->cb)    { ID3D12Resource_Release(b->cb); b->cb = NULL; b->cb_mapped = NULL; }
    if (keep_range && b->srv_count && d3d12_srv_free_count < (int)(sizeof(d3d12_srv_free) / sizeof(d3d12_srv_free[0]))) {
        d3d12_srv_free[d3d12_srv_free_count].base = b->srv_base;
        d3d12_srv_free[d3d12_srv_free_count].count = b->srv_count;
        d3d12_srv_free_count++;
    }
    b->srv_count = 0;
    b->released = 1;
}

static void d3d12_bundle_graves_collect(int force)
{
    UINT64 done = vio_d3d12.fence ? ID3D12Fence_GetCompletedValue(vio_d3d12.fence) : UINT64_MAX;
    vio_d3d12_bundle **pp = &d3d12_graves;
    while (*pp) {
        vio_d3d12_bundle *b = *pp;
        /* the frame it died in is submitted now: its signal is at most fence_value */
        if (!force && b->grave_fence == 0) b->grave_fence = vio_d3d12.fence_value;
        if (force || b->grave_fence <= done) {
            *pp = b->next;
            d3d12_bundle_release(b, !force);
            free(b);
        } else {
            pp = &b->next;
        }
    }
}

/* Leave a recording: the frame's list, rings and pipeline come back. */
static void d3d12_bundle_stop(void)
{
    vio_d3d12.cmd_list              = d3d12_brec_saved.cmd_list;
    vio_d3d12.cbuffer_heap          = d3d12_brec_saved.cb_heap;
    vio_d3d12.cbuffer_heap_mapped   = d3d12_brec_saved.cb_mapped;
    vio_d3d12.cbuffer_heap_gpu      = d3d12_brec_saved.cb_gpu;
    vio_d3d12.cbuffer_heap_offset   = d3d12_brec_saved.cb_offset;
    vio_d3d12.cbuffer_heap_capacity = d3d12_brec_saved.cb_capacity;
    vio_d3d12.cbuffer_frame_base    = d3d12_brec_saved.cb_base;
    vio_d3d12.cbuffer_frame_end     = d3d12_brec_saved.cb_end;
    vio_d3d12.srv_frame_offset      = d3d12_brec_saved.srv_offset;
    vio_d3d12.srv_frame_base        = d3d12_brec_saved.srv_base;
    vio_d3d12.srv_frame_capacity    = d3d12_brec_saved.srv_capacity;
    d3d12_current_pipeline          = d3d12_brec_saved.pipe;
    vio_d3d12.srv_table_bound       = 0;
    vio_d3d12.sampler_table_bound   = 0;
    d3d12_brec = NULL;
}

static void *d3d12_begin_bundle(void)
{
    if (!vio_d3d12.initialized || !vio_d3d12.in_frame || !vio_d3d12.cmd_list || vio_d3d12.device_lost
        || d3d12_brec || !vio_d3d12.cbuffer_heap || !vio_d3d12.srv_heap.heap) return NULL;
    if (vio_d3d12.vrs_image_active) return NULL;   /* bundles cannot carry the shading-rate image combiner */
    vio_d3d12_bundle *b = (vio_d3d12_bundle *)calloc(1, sizeof(vio_d3d12_bundle));
    if (!b) return NULL;
    b->samples = vio_d3d12.current_rt_samples;
    b->format  = vio_d3d12.current_rt_format;
    b->cb_size = d3d12_bundle_cb_hint + 65536;   /* + one maximal CBV: an allocation never runs out unseen */
    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = b->cb_size;
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_RANGE none = {0, 0};
    if (FAILED(ID3D12Device_CreateCommandAllocator(vio_d3d12.device, D3D12_COMMAND_LIST_TYPE_BUNDLE,
                                                   &IID_ID3D12CommandAllocator, (void **)&b->alloc))
        || FAILED(ID3D12Device_CreateCommandList(vio_d3d12.device, 0, D3D12_COMMAND_LIST_TYPE_BUNDLE, b->alloc, NULL,
                                                 &IID_ID3D12GraphicsCommandList, (void **)&b->list))
        || FAILED(ID3D12Device_CreateCommittedResource(vio_d3d12.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                       &IID_ID3D12Resource, (void **)&b->cb))
        || FAILED(ID3D12Resource_Map(b->cb, 0, &none, (void **)&b->cb_mapped))
        || d3d12_bundle_srv_alloc(d3d12_bundle_srv_hint * VIO_D3D12_SRV_TABLE_SIZE, &b->srv_base) != 0) {
        d3d12_bundle_release(b, 0);
        free(b);
        return NULL;
    }
    b->srv_count = d3d12_bundle_srv_hint * VIO_D3D12_SRV_TABLE_SIZE;
    b->cb_gpu = ID3D12Resource_GetGPUVirtualAddress(b->cb);
    b->next = d3d12_bundles;
    if (d3d12_bundles) d3d12_bundles->prev = b;
    d3d12_bundles = b;

    d3d12_brec_saved.cmd_list     = vio_d3d12.cmd_list;
    d3d12_brec_saved.cb_heap      = vio_d3d12.cbuffer_heap;
    d3d12_brec_saved.cb_mapped    = vio_d3d12.cbuffer_heap_mapped;
    d3d12_brec_saved.cb_gpu       = vio_d3d12.cbuffer_heap_gpu;
    d3d12_brec_saved.cb_offset    = vio_d3d12.cbuffer_heap_offset;
    d3d12_brec_saved.cb_capacity  = vio_d3d12.cbuffer_heap_capacity;
    d3d12_brec_saved.cb_base      = vio_d3d12.cbuffer_frame_base;
    d3d12_brec_saved.cb_end       = vio_d3d12.cbuffer_frame_end;
    d3d12_brec_saved.srv_offset   = vio_d3d12.srv_frame_offset;
    d3d12_brec_saved.srv_base     = vio_d3d12.srv_frame_base;
    d3d12_brec_saved.srv_capacity = vio_d3d12.srv_frame_capacity;
    d3d12_brec_saved.pipe         = d3d12_current_pipeline;
    vio_d3d12.cmd_list              = b->list;
    vio_d3d12.cbuffer_heap          = b->cb;
    vio_d3d12.cbuffer_heap_mapped   = b->cb_mapped;
    vio_d3d12.cbuffer_heap_gpu      = b->cb_gpu;
    vio_d3d12.cbuffer_heap_offset   = 0;
    vio_d3d12.cbuffer_heap_capacity = b->cb_size;
    vio_d3d12.cbuffer_frame_base    = 0;
    vio_d3d12.cbuffer_frame_end     = b->cb_size;
    vio_d3d12.srv_frame_offset      = b->srv_base;
    vio_d3d12.srv_frame_base        = b->srv_base;
    vio_d3d12.srv_frame_capacity    = b->srv_count;
    vio_d3d12.srv_table_bound       = 0;
    vio_d3d12.sampler_table_bound   = 0;
    d3d12_brec = b;
    /* the pipeline bound now, for records without their own (also sets the
     * root signature and the heaps, which must equal the caller's) */
    if (d3d12_current_pipeline) d3d12_bind_pipeline(d3d12_current_pipeline);
    else vio_d3d12_bind_graphics_heaps(b->list);
    return b;
}

static int d3d12_end_bundle(void *bundle)
{
    vio_d3d12_bundle *b = (vio_d3d12_bundle *)bundle;
    if (!b || d3d12_brec != b) return -1;
    UINT cb_used  = vio_d3d12.cbuffer_heap_offset;
    UINT srv_used = vio_d3d12.srv_frame_offset - b->srv_base;
    d3d12_bundle_stop();
    HRESULT hr = ID3D12GraphicsCommandList_Close(b->list);
    /* Out of room: the next recording of a bundle gets more. */
    if (cb_used + 65536 > b->cb_size) {
        b->failed = 1;
        if (d3d12_bundle_cb_hint < cb_used * 2) d3d12_bundle_cb_hint = cb_used * 2;
    }
    if (b->failed && srv_used + VIO_D3D12_SRV_TABLE_SIZE > b->srv_count && d3d12_bundle_srv_hint < 4096)
        d3d12_bundle_srv_hint *= 2;
    return (FAILED(hr) || b->failed) ? -1 : 0;
}

static int d3d12_draw_bundle(void *bundle)
{
    vio_d3d12_bundle *b = (vio_d3d12_bundle *)bundle;
    if (!b || !b->list || b->failed || b->released || d3d12_brec || !vio_d3d12.in_frame || !vio_d3d12.cmd_list
        || vio_d3d12.device_lost) return -1;
    if (b->samples != vio_d3d12.current_rt_samples || b->format != vio_d3d12.current_rt_format
        || vio_d3d12.vrs_image_active) return -1;   /* recorded again */
    /* the bundle sets the graphics root signature and heaps: the caller's must match */
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(vio_d3d12.cmd_list, vio_d3d12.root_signature);
    vio_d3d12_bind_graphics_heaps(vio_d3d12.cmd_list);
    ID3D12GraphicsCommandList_ExecuteBundle(vio_d3d12.cmd_list, b->list);
    /* the bundle's PSO, topology and root arguments stay on the list */
    if (d3d12_current_pipeline) d3d12_bind_pipeline(d3d12_current_pipeline);
    vio_d3d12.srv_table_bound = 0;
    vio_d3d12.sampler_table_bound = 0;
    return 0;
}

static void d3d12_destroy_bundle(void *bundle)
{
    vio_d3d12_bundle *b = (vio_d3d12_bundle *)bundle;
    if (!b) return;
    if (d3d12_brec == b) { d3d12_bundle_stop(); ID3D12GraphicsCommandList_Close(b->list); }
    if (b->released || !vio_d3d12.device) { d3d12_bundle_release(b, 0); free(b); return; }
    d3d12_bundle_unlink(b);
    /* In the open frame its list may run in this frame: the fence of this frame
     * is known once it is submitted (begin_frame). */
    b->grave_fence = vio_d3d12.in_frame ? 0 : vio_d3d12.fence_value;
    b->next = d3d12_graves;
    d3d12_graves = b;
    if (!vio_d3d12.in_frame) d3d12_bundle_graves_collect(0);
}

/* Device shutdown (GPU idle): no bundle object outlives the device; the PHP
 * objects free their structs later. */
static void d3d12_bundles_sweep(void)
{
    if (d3d12_brec) { vio_d3d12_bundle *b = d3d12_brec; d3d12_bundle_stop(); ID3D12GraphicsCommandList_Close(b->list); }
    d3d12_bundle_graves_collect(1);
    while (d3d12_bundles) {
        vio_d3d12_bundle *b = d3d12_bundles;
        d3d12_bundle_unlink(b);
        d3d12_bundle_release(b, 0);
    }
    d3d12_bundle_sampler_count = 0;
    d3d12_srv_free_count = 0;
}

static const char *d3d12_bundle_method(void) { return "bundle"; }

/* ── Backend registration ─────────────────────────────────────────── */

static const vio_backend d3d12_backend = {
    .name              = "d3d12",
    .api_version       = VIO_BACKEND_API_VERSION,
    .init              = d3d12_init,
    .shutdown          = d3d12_shutdown,
    .create_surface    = d3d12_create_surface,
    .destroy_surface   = d3d12_destroy_surface,
    .resize            = d3d12_resize,
    .create_pipeline   = d3d12_create_pipeline,
    .destroy_pipeline  = d3d12_destroy_pipeline,
    .bind_pipeline     = d3d12_bind_pipeline,
    .create_buffer     = d3d12_create_buffer,
    .update_buffer     = d3d12_update_buffer,
    .destroy_buffer    = d3d12_destroy_buffer,
    .create_texture    = d3d12_create_texture,
    .create_texture_3d = d3d12_create_texture_3d,
    .destroy_texture   = d3d12_destroy_texture,
    .compile_shader    = d3d12_compile_shader,
    .destroy_shader    = d3d12_destroy_shader,
    .begin_frame       = d3d12_begin_frame,
    .end_frame         = d3d12_end_frame,
    .draw              = d3d12_draw,
    .draw_indexed      = d3d12_draw_indexed,
    .present           = d3d12_present,
    .clear             = d3d12_clear,
    .set_uniform       = d3d12_set_uniform,
    .bind_texture      = d3d12_bind_texture,
    .set_viewport      = d3d12_set_viewport,
    .set_viewports     = d3d12_set_viewports,
    .gpu_flush         = vio_d3d12_wait_for_gpu,
    .dispatch_compute  = d3d12_dispatch_compute,
    .create_compute_pipeline  = d3d12_create_compute_pipeline,
    .destroy_compute_pipeline = d3d12_destroy_compute_pipeline,
    .compute_bind_buffer      = d3d12_compute_bind_buffer,
    .compute_bind_image       = d3d12_compute_bind_image,
    .compute_wait             = d3d12_compute_wait,
    .compute_set_uniforms     = d3d12_compute_set_uniforms,
    .read_buffer              = d3d12_read_buffer,
    .bind_storage_buffer          = d3d12_bind_storage_buffer,
    .draw_instanced_from_storage  = d3d12_draw_instanced_from_storage,
    .supports_feature  = d3d12_supports_feature,
    .multiview_view_from_vertex = d3d12_multiview_view_from_vertex,
    .apply_mesh_layout = d3d12_apply_mesh_layout,
    .gpu_frame_time    = d3d12_gpu_frame_time,
    .gpu_mark          = d3d12_gpu_mark,
    .gpu_marks         = d3d12_gpu_marks,
    .swapchain_info    = d3d12_swapchain_info,
    .bindless_set      = d3d12_bindless_set,
    .draw_indirect     = d3d12_draw_indirect,
    .push_cbuffers     = d3d12_push_cbuffers,
    .draw_mesh_tasks          = d3d12_draw_mesh_tasks,
    .draw_mesh_tasks_indirect = d3d12_draw_mesh_tasks_indirect,
#ifdef VIO_D3D12_HAS_WORK_GRAPHS
    .create_work_graph        = d3d12_create_work_graph,
    .destroy_work_graph       = d3d12_destroy_work_graph,
    .work_graph_bind_buffer   = d3d12_work_graph_bind_buffer,
    .dispatch_graph           = d3d12_dispatch_graph,
#endif
    .set_shading_rate  = d3d12_set_shading_rate,
    .set_shading_rate_image = d3d12_set_shading_rate_image,
    .shading_rate_tile_size = d3d12_shading_rate_tile_size,
    .destroy_cubemap   = d3d12_destroy_cubemap,
    .upload_cubemap    = d3d12_upload_cubemap,
    .read_render_target = d3d12_read_render_target,
    .destroy_font_atlas = d3d12_destroy_font_atlas,
    .update_font_atlas  = d3d12_update_font_atlas,
    .destroy_render_target = d3d12_destroy_render_target,
    /* Render-target lifecycle (GAP-PLAN Phase 2 — previously inline in php_vio.c). */
    .create_render_target    = d3d12_create_render_target,
    .bind_render_target      = d3d12_bind_render_target,
    .unbind_render_target    = d3d12_unbind_render_target,
    .bind_render_target_face = d3d12_bind_render_target_face,
    .render_target_cubemap   = d3d12_render_target_cubemap,
    .generate_mipmaps        = d3d12_generate_mipmaps,
    .update_texture          = d3d12_update_texture,
    .bind_stage_constants    = d3d12_bind_stage_constants,
    .gpu_info                = d3d12_gpu_info,
    .describe                = d3d12_describe,
    .enumerate_adapters      = d3d12_enumerate_adapters,
    .create_acceleration_structure  = d3d12_create_acceleration_structure,
    .update_acceleration_structure  = d3d12_update_acceleration_structure,
    .bind_fragment_storage          = d3d12_bind_fragment_storage,
    .destroy_acceleration_structure = d3d12_destroy_acceleration_structure,
    .bind_acceleration_structure    = d3d12_bind_acceleration_structure,
    .sampler_feedback_bind          = d3d12_sampler_feedback_bind,
    .sampler_feedback_read          = d3d12_sampler_feedback_read,
    .sampler_feedback_clear         = d3d12_sampler_feedback_clear,
    .create_rt_pipeline             = d3d12_create_rt_pipeline,
    .destroy_rt_pipeline            = d3d12_destroy_rt_pipeline,
    .trace_rays                     = d3d12_trace_rays,
    .begin_bundle                   = d3d12_begin_bundle,
    .end_bundle                     = d3d12_end_bundle,
    .draw_bundle                    = d3d12_draw_bundle,
    .destroy_bundle                 = d3d12_destroy_bundle,
    .bundle_method                  = d3d12_bundle_method,
    .rt_origin_top     = 1,
};

void vio_backend_d3d12_register(void)
{
    vio_register_backend(&d3d12_backend);
}

#endif /* HAVE_D3D12 */
