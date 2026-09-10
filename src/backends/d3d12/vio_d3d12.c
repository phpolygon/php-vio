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

#ifdef HAVE_GLFW
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

#include "vio_d3d12.h"
#include "../vio_d3d_common.h"
#include "../../vio_shader_reflect.h"   /* vio_spirv_reflect — data-driven compute register mapping */
#include "../../vio_texture.h"          /* vio_texture_object — storage-image binds */
#include <string.h>
#include <stdlib.h>

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

static D3D12_PRIMITIVE_TOPOLOGY vio_topology_to_d3d12(vio_topology t)
{
    switch (t) {
        case VIO_TRIANGLES:      return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case VIO_TRIANGLE_STRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case VIO_LINES:          return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case VIO_LINE_STRIP:     return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case VIO_POINTS:         return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        case VIO_TRIANGLE_FAN:   return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
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

static int d3d12_create_root_signature(void)
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
     */
    D3D12_ROOT_PARAMETER params[5] = {0};

    /* [0] CBV b0 — vertex shader only */
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

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
    params[3].Descriptor.RegisterSpace = 0;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

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
        static_samplers[s].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_ROOT_SIGNATURE_DESC rs_desc = {0};
    rs_desc.NumParameters = 5; /* VS CBV, PS CBV, PS SRV table, VS storage SRV, PS sampler table */
    rs_desc.pParameters = params;
    rs_desc.NumStaticSamplers = 4;
    rs_desc.pStaticSamplers = static_samplers;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

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
                                           (void **)&vio_d3d12.root_signature);
    ID3D10Blob_Release(signature_blob);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create root signature (0x%08lx)", hr);
        return -1;
    }

    return 0;
}

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

    /* Select adapter (WARP for headless, hardware otherwise) */
    IDXGIAdapter1 *adapter = NULL;
    if (cfg->headless) {
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
            int n = WideCharToMultiByte(CP_UTF8, 0, sel_desc.Description, -1,
                                        vio_d3d12.gpu_name, (int)sizeof(vio_d3d12.gpu_name),
                                        NULL, NULL);
            if (n <= 0) {
                /* Conversion failed — leave name empty rather than garbage. */
                vio_d3d12.gpu_name[0] = '\0';
            }
        }
    }

    /* Create device */
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

    /* GPU timestamps (GAP-PHASE5 Block 3) — optional; a failure just leaves the
     * feature off. */
    vio_d3d12.last_gpu_ms = -1.0;
    {
        D3D12_QUERY_HEAP_DESC qh = {0};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = 2 * VIO_D3D12_MAX_FRAME_COUNT;
        ID3D12QueryHeap *heap = NULL;
        if (SUCCEEDED(ID3D12Device_CreateQueryHeap(vio_d3d12.device, &qh, &IID_ID3D12QueryHeap, (void **)&heap)) && heap) {
            D3D12_HEAP_PROPERTIES hp = {0};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rb = {0};
            rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rb.Width = sizeof(UINT64) * 2 * VIO_D3D12_MAX_FRAME_COUNT;
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

    if (d3d12_create_descriptor_heap(&vio_d3d12.dsv_heap,
                                      D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_NONE) != 0) {
        goto init_fail;
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
    if (vio_d3d12.compute_srv_heap) ID3D12DescriptorHeap_Release(vio_d3d12.compute_srv_heap);
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
    if (vio_d3d12.swapchain)      IDXGISwapChain3_Release(vio_d3d12.swapchain);
    if (vio_d3d12.cmd_queue)      ID3D12CommandQueue_Release(vio_d3d12.cmd_queue);
    if (vio_d3d12.factory)        IDXGIFactory4_Release(vio_d3d12.factory);
    /* Cached debug-layer InfoQueue — must go before the device it was QI'd from. */
    if (vio_d3d12.info_queue)     ID3D12InfoQueue_Release(vio_d3d12.info_queue);
    if (vio_d3d12.device)         ID3D12Device_Release(vio_d3d12.device);

    d3d12_current_pipeline = NULL;
    memset(&vio_d3d12, 0, sizeof(vio_d3d12));
}

/* ── Surface & Window ─────────────────────────────────────────────── */

static void *d3d12_create_surface(vio_config *cfg)
{
#ifdef HAVE_GLFW
    if (!vio_d3d12.glfw_window) {
        php_error_docref(NULL, E_WARNING, "D3D12: No GLFW window set");
        return NULL;
    }

    HWND hwnd = glfwGetWin32Window((GLFWwindow *)vio_d3d12.glfw_window);

    DXGI_SWAP_CHAIN_DESC1 sc_desc = {0};
    sc_desc.Width = cfg->width;
    sc_desc.Height = cfg->height;
    sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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
#else
    (void)cfg;
    php_error_docref(NULL, E_WARNING, "D3D12: Built without GLFW, cannot create surface");
    return NULL;
#endif
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
                                                DXGI_FORMAT_R8G8B8A8_UNORM,
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

static void *d3d12_create_pipeline(vio_pipeline_desc *desc)
{
    vio_d3d12_pipeline *pipeline = calloc(1, sizeof(vio_d3d12_pipeline));
    if (!pipeline) return NULL;

    vio_d3d12_shader *shader = (vio_d3d12_shader *)desc->shader;
    if (!shader) { free(pipeline); return NULL; }

    pipeline->topology = vio_topology_to_d3d12(desc->topology);

    /* Build input layout */
    D3D12_INPUT_ELEMENT_DESC *elements = NULL;
    char (*sem_names)[24] = NULL;
    UINT vertex_stride = 0;
    if (desc->vertex_attrib_count > 0 && desc->vertex_layout) {
        elements = calloc(desc->vertex_attrib_count, sizeof(D3D12_INPUT_ELEMENT_DESC));
        /* Matrix columns: SPIRV-Cross names them TEXCOORD{base}_{column}
         * (semantic "TEXCOORD3_" index 0..3), see the D3D11 twin. */
        sem_names = calloc(desc->vertex_attrib_count, sizeof(*sem_names));
        UINT vertex_offset = 0;
        for (int i = 0; i < desc->vertex_attrib_count; i++) {
            int loc = desc->vertex_layout[i].location;
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
    pso_desc.pRootSignature = vio_d3d12.root_signature;

    /* Shaders */
    pso_desc.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(shader->vs_blob);
    pso_desc.VS.BytecodeLength = ID3D10Blob_GetBufferSize(shader->vs_blob);
    pso_desc.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(shader->ps_blob);
    pso_desc.PS.BytecodeLength = ID3D10Blob_GetBufferSize(shader->ps_blob);

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
    pso_desc.PrimitiveTopologyType = vio_topology_to_d3d12_type(desc->topology);

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

    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(vio_d3d12.device, &pso_desc,
                                                           &IID_ID3D12PipelineState,
                                                           (void **)&pipeline->pso);
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

/* The PSO variant for a render target with `samples` samples (1 = the base
 * PSO). Variants are created on first use and live as long as the pipeline. */
static ID3D12PipelineState *d3d12_pipeline_pso_for_samples(vio_d3d12_pipeline *p, int samples)
{
    if (!p) return NULL;
    if (samples <= 1) return p->pso;
    int idx = samples >= 8 ? 3 : (samples >= 4 ? 2 : 1);
    if (p->pso_ms[idx]) return p->pso_ms[idx];
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = p->pso_desc;
    d.SampleDesc.Count = (UINT)(1u << idx);
    d.SampleDesc.Quality = 0;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(vio_d3d12.device, &d,
                                                           &IID_ID3D12PipelineState,
                                                           (void **)&p->pso_ms[idx]);
    if (FAILED(hr) || !p->pso_ms[idx]) {
        d3d12_drain_info_queue("create_pso_msaa_fail");
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create %d-sample PSO variant (0x%08lx)", 1 << idx, hr);
        p->pso_ms[idx] = NULL;
        return p->pso;
    }
    return p->pso_ms[idx];
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
    for (int v = -1; v < 4; v++) {
        ID3D12PipelineState *pso = v < 0 ? p->pso : p->pso_ms[v];
        if (!pso) continue;
        UINT slot = vio_d3d12.frame_index < 3 ? vio_d3d12.frame_index : 0;
        if (vio_d3d12.in_frame && d3d12_pending_pso_count[slot] < VIO_D3D12_PENDING_PSO_MAX) {
            d3d12_pending_pso[slot][d3d12_pending_pso_count[slot]++] = pso;
        } else {
            ID3D12PipelineState_Release(pso);
        }
    }
    if (p->input_elements) free(p->input_elements);
    if (p->sem_names) free(p->sem_names);
    free(p);
}

/* Re-issue the bound pipeline's PSO for the sample count of the (new) bound
 * target — called after render-target binds / unbinds so "bind pipeline, then
 * bind target" orders pick the right variant too. */
static void d3d12_rearm_pso_for_target(void)
{
    if (!d3d12_current_pipeline || !vio_d3d12.cmd_list || !vio_d3d12.in_frame) return;
    ID3D12PipelineState *pso = d3d12_pipeline_pso_for_samples(d3d12_current_pipeline, vio_d3d12.current_rt_samples);
    if (pso) ID3D12GraphicsCommandList_SetPipelineState(vio_d3d12.cmd_list, pso);
}

static void d3d12_bind_pipeline(void *pipeline_ptr)
{
    vio_d3d12_pipeline *p = (vio_d3d12_pipeline *)pipeline_ptr;
    if (!p) return;

    d3d12_current_pipeline = p;
    ID3D12GraphicsCommandList_SetPipelineState(vio_d3d12.cmd_list,
        d3d12_pipeline_pso_for_samples(p, vio_d3d12.current_rt_samples));
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(vio_d3d12.cmd_list,
                                                        vio_d3d12.root_signature);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(vio_d3d12.cmd_list, p->topology);
    ID3D12GraphicsCommandList_OMSetStencilRef(vio_d3d12.cmd_list, p->stencil_ref);

    /* Bind SRV + sampler heaps. Also invalidates the cached root arguments for
     * params 2 / 4 (SetGraphicsRootSignature / SetDescriptorHeaps may reset
     * them), so the next flush rebuilds + re-points rather than reusing. */
    vio_d3d12_bind_graphics_heaps(vio_d3d12.cmd_list);
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
        if (desc->data) {
            /* Compute SRV input: UPLOAD heap so the box bytes are CPU-writable
             * (the data block below maps + memcpys them). Bound as a raw/SRV in
             * dispatch_compute. UPLOAD buffers sit in GENERIC_READ, which already
             * permits shader-resource reads, so no transition is needed. */
            heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
            initial_state = D3D12_RESOURCE_STATE_GENERIC_READ;
            /* NO ALLOW_UNORDERED_ACCESS: forbidden on UPLOAD heaps, and the input
             * is read-only (raw SRV, which doesn't require the UAV flag). */
        } else {
            /* Compute UAV output: DEFAULT heap. Buffers have no layout, so the
             * runtime always creates them in COMMON regardless of the requested
             * state (it warns if you ask for UNORDERED_ACCESS); a buffer in
             * COMMON is implicitly promoted to UNORDERED_ACCESS on first UAV
             * access, so COMMON is the correct, warning-free initial state. */
            heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
            initial_state = D3D12_RESOURCE_STATE_COMMON;
            res_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
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

static void d3d12_update_buffer(void *buffer_ptr, const void *data, size_t size)
{
    vio_d3d12_buffer *buf = (vio_d3d12_buffer *)buffer_ptr;
    if (!buf || !buf->resource || !data) return;
    if (size > buf->size) size = buf->size;

    if (buf->default_heap) {
        /* GPU-local buffer: staging copy on the upload queue (ordered before
         * the next frame list on the same queue). */
        d3d12_upload_buffer_region(buf->resource, data, size);
        return;
    }

    void *mapped = NULL;
    D3D12_RANGE read_range = {0, 0};
    HRESULT hr = ID3D12Resource_Map(buf->resource, 0, &read_range, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped, data, size);
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
            ID3D12Resource_Release(vio_d3d12.upload_retire[i].res);
        } else {
            vio_d3d12.upload_retire[kept++] = vio_d3d12.upload_retire[i];
        }
    }
    vio_d3d12.upload_retire_count = kept;
}

static void d3d12_retire_later(ID3D12Resource *res, UINT64 fence)
{
    if (!res) return;
    if (vio_d3d12.upload_retire_count >= vio_d3d12.upload_retire_cap) {
        int cap = vio_d3d12.upload_retire_cap ? vio_d3d12.upload_retire_cap * 2 : 32;
        void *grown = realloc(vio_d3d12.upload_retire, (size_t)cap * sizeof(*vio_d3d12.upload_retire));
        if (!grown) {
            /* Out of memory for bookkeeping: fall back to the old stall. */
            vio_d3d12_wait_for_gpu();
            ID3D12Resource_Release(res);
            return;
        }
        vio_d3d12.upload_retire = grown;
        vio_d3d12.upload_retire_cap = cap;
    }
    vio_d3d12.upload_retire[vio_d3d12.upload_retire_count].res = res;
    vio_d3d12.upload_retire[vio_d3d12.upload_retire_count].fence = fence;
    vio_d3d12.upload_retire_count++;
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
} d3d12_buffer_upload_job;

static void d3d12_record_buffer_upload(ID3D12GraphicsCommandList *list, void *user)
{
    d3d12_buffer_upload_job *job = (d3d12_buffer_upload_job *)user;
    ID3D12GraphicsCommandList_CopyBufferRegion(list, job->dst, 0, job->staging, 0, job->size);
}

static int d3d12_upload_buffer_region(ID3D12Resource *dst, const void *data, size_t size)
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

    d3d12_buffer_upload_job job = { dst, staging, (UINT64)size };
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

static void *d3d12_create_texture(vio_texture_desc *desc)
{
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

/* vio_read_render_target on D3D12: colour attachment (any vio_pixel_format,
 * converted to RGBA8 by the shared converter) or the depth buffer
 * (R24G8_TYPELESS -> grey ramp). Cube targets are not available on D3D12. */
static int d3d12_read_render_target(void *rt_ptr, int face, int attachment, void *out_rgba)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!rt || rt->backend_type != VIO_RT_BACKEND_D3D12 || !vio_d3d12.device) return -1;
    int n = rt->attachment_count > 0 ? rt->attachment_count : 1;
    if (attachment < 0 || attachment >= n) return -1;
    /* Cube targets: subresource = face * mip_levels (mip 0 of that slice). */
    UINT subresource = 0;
    if (rt->is_cube) {
        int f = face >= 0 ? face : (rt->bound_face >= 0 ? rt->bound_face : 0);
        subresource = (UINT)(f * (rt->mip_levels > 0 ? rt->mip_levels : 1));
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
static void d3d12_rt_resolve_msaa(vio_render_target_object *rt)
{
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
    if (rt->d3d12_msaa_color_resources[0]) {
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

    if (rt->depth_only) {
        ID3D12GraphicsCommandList_OMSetRenderTargets(vio_d3d12.cmd_list, 0, NULL, FALSE, &dsv_handle);
        vio_d3d12.current_has_rtv = 0;
        vio_d3d12.current_rtv_count = 0;
        vio_d3d12.current_dsv = dsv_handle;
    } else if (rt->is_cube) {
        int mips = d3d12_rt_mips(rt);
        if (face < 0 || face > 5) face = 0;
        if (level < 0 || level >= mips) level = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_base;
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart((ID3D12DescriptorHeap *)rt->d3d12_rtv_heap, &rtv_base);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = { rtv_base.ptr + (SIZE_T)(face * mips + level) * vio_d3d12.rtv_descriptor_size };
        /* The shared depth texture matches level 0 only (GL / Metal contract). */
        ID3D12GraphicsCommandList_OMSetRenderTargets(vio_d3d12.cmd_list, 1, &rtv, FALSE, level == 0 ? &dsv_handle : NULL);
        vio_d3d12.current_rtv = rtv;
        vio_d3d12.current_rtvs[0] = rtv;
        vio_d3d12.current_rtv_count = 1;
        vio_d3d12.current_has_rtv = 1;
        vio_d3d12.current_dsv = dsv_handle;
        w = h = (rt->width >> level) > 0 ? (rt->width >> level) : 1;
        rt->bound_face = face;
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
    vio_d3d12.current_rt_samples = rt->samples > 1 ? rt->samples : 1;
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
    if (!rt->is_cube || face < 0 || face > 5 || level < 0 || level >= d3d12_rt_mips(rt)) return -1;
    if (!vio_d3d12.in_frame) {
        php_error_docref(NULL, E_WARNING, "D3D12: cube face binds are only valid between vio_begin and vio_end");
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
        if (bound_rt->d3d12_msaa_color_resources[0]) {
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
    d3d12_rt_set_viewport_scissor(vio_d3d12.width, vio_d3d12.height);
    d3d12_rearm_pso_for_target();
}

/* Initial clear of a freshly created target, recorded on the upload list. */
typedef struct _d3d12_rt_clear_job {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv0;
    int                         rtv_count;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
} d3d12_rt_clear_job;

static void d3d12_record_rt_clear(ID3D12GraphicsCommandList *list, void *user)
{
    d3d12_rt_clear_job *job = (d3d12_rt_clear_job *)user;
    float zero[4] = {0, 0, 0, 0};
    for (int i = 0; i < job->rtv_count; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = { job->rtv0.ptr + (SIZE_T)i * vio_d3d12.rtv_descriptor_size };
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, h, zero, 0, NULL);
    }
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, job->dsv,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
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
    /* MSAA (GAP-PHASE5 Block 1): clamp the request to a power of two the device
     * supports for attachment 0's format. Cube / depth-only targets stay
     * single-sample (no resolve path), like D3D11. The PSO side is handled by
     * the per-sample-count variants (d3d12_pipeline_pso_for_samples). */
    UINT samples = 1;
    if (!rt->is_cube && !depth_only && rt->samples > 1) {
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
        rtv_heap_desc.NumDescriptors = rt->is_cube ? (UINT)(6 * mips) : (UINT)(attachment_count * (samples > 1 ? 2 : 1));
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

        if (rt->is_cube) {
            DXGI_FORMAT dxfmt = vio_pixel_format_to_dxgi(rt->formats[0]);
            D3D12_RESOURCE_DESC rd = {0};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = width; rd.Height = height;
            rd.DepthOrArraySize = 6;
            rd.MipLevels = (UINT16)mips;
            rd.Format = dxfmt;
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv = {0};
            cv.Format = dxfmt;
            ID3D12Resource *cube = NULL;
            hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &heap_props, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, &IID_ID3D12Resource, (void **)&cube);
            if (FAILED(hr)) {
                php_error_docref(NULL, E_WARNING, "D3D12: Failed to create cube colour resource (0x%08lx)", hr);
                return -1;
            }
            rt->d3d12_color_resource = cube;
            rt->d3d12_color_resources[0] = cube;
            for (int f = 0; f < 6; f++) {
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

    /* DSV heap + depth resource (shared, level-0 sized) */
    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {0};
    dsv_heap_desc.NumDescriptors = 1;
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
    depth_res_desc.DepthOrArraySize = 1;
    depth_res_desc.MipLevels = 1;
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
    ID3D12Device_CreateDepthStencilView(vio_d3d12.device, depth_res, depth_only ? &dsv_view_desc : NULL, dsv_handle);

    /* Static SRVs (staging heap) for sampling the target later. */
    if (depth_only) {
        uint64_t cpu, gpu;
        if (d3d12_rt_alloc_srv(&cpu, &gpu) == 0) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
            sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = { (SIZE_T)cpu };
            ID3D12Device_CreateShaderResourceView(vio_d3d12.device, depth_res, &sd, h);
            rt->d3d12_depth_srv_gpu = gpu;
            rt->d3d12_depth_srv_cpu = cpu;
        }
    } else if (rt->is_cube) {
        uint64_t cpu, gpu;
        if (d3d12_rt_alloc_srv(&cpu, &gpu) == 0) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
            sd.Format = vio_pixel_format_to_dxgi(rt->formats[0]);
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.TextureCube.MipLevels = (UINT)mips;
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
        if (!depth_only && rt->d3d12_rtv_heap) {
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart((ID3D12DescriptorHeap *)rt->d3d12_rtv_heap, &job.rtv0);
            job.rtv_count = rt->is_cube ? 6 * mips : attachment_count * (samples > 1 ? 2 : 1);
        }
        d3d12_submit_upload(d3d12_record_rt_clear, &job);
    }
    return 0;
}

static int d3d12_render_target_cubemap(void *rt_ptr, void *cm_obj)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    vio_cubemap_object *cm = (vio_cubemap_object *)cm_obj;
    if (!rt || !cm || !rt->is_cube || !rt->d3d12_color_resource || !rt->d3d12_color_srv_cpu) return -1;
    /* Borrowed: the RT owns the resource + descriptor (d3d12_destroy_cubemap
     * honours cm->borrowed). Binding reads d3d12_srv_cpu, which stays valid for
     * the RT's life. */
    cm->d3d12_resource = rt->d3d12_color_resource;
    cm->d3d12_srv_cpu  = rt->d3d12_color_srv_cpu;
    cm->d3d12_srv_gpu  = rt->d3d12_color_srv_gpu;
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

static int d3d12_generate_mipmaps(void *obj, int kind)
{
    if (!obj || !vio_d3d12.initialized) return -1;
    switch (kind) {
        case 0: {
            vio_render_target_object *rt = (vio_render_target_object *)obj;
            if (rt->backend_type != VIO_RT_BACKEND_D3D12 || !rt->d3d12_color_resource) return -1;
            if (!rt->is_cube || rt->mip_levels <= 1) return 0;
            if (vio_d3d12.current_bound_rt == rt && vio_d3d12.in_frame) d3d12_unbind_render_target(0, 0, 0);
            D3D12_RESOURCE_STATES st = rt->d3d12_color_is_srv ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                                              : D3D12_RESOURCE_STATE_RENDER_TARGET;
            return d3d12_generate_mips_cpu((ID3D12Resource *)rt->d3d12_color_resource, 6, rt->mip_levels,
                                           rt->width, rt->height, vio_rt_format_bpp(rt->formats[0]), st);
        }
        case 1: {
            vio_texture_object *t = (vio_texture_object *)obj;
            vio_d3d12_texture *dt = (vio_d3d12_texture *)t->backend_texture;
            if (!dt || !dt->resource) return -1;
            if (dt->mip_levels <= 1 || dt->depth > 0) return 0;
            return d3d12_generate_mips_cpu(dt->resource, 1, dt->mip_levels, dt->width, dt->height,
                                           dt->channels > 0 ? dt->channels : 4, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        case 2: {
            vio_cubemap_object *cm = (vio_cubemap_object *)obj;
            if (!cm->d3d12_resource) return -1;
            if (!cm->mipmaps) return 0;
            D3D12_RESOURCE_DESC rd; ID3D12Resource_GetDesc((ID3D12Resource *)cm->d3d12_resource, &rd);
            D3D12_RESOURCE_STATES st = cm->borrowed ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            return d3d12_generate_mips_cpu((ID3D12Resource *)cm->d3d12_resource, 6, (int)rd.MipLevels,
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

/* ── Shaders ──────────────────────────────────────────────────────── */

static void *d3d12_compile_shader(vio_shader_desc *desc)
{
    vio_d3d12_shader *shader = calloc(1, sizeof(vio_d3d12_shader));
    if (!shader) return NULL;

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

        /* SPIR-V -> HLSL SM 5.1 */
        allocated_vs = vio_spirv_to_hlsl(vs_spirv, vs_spirv_size, 51, &err);
        if (free_vs_spirv) free(vs_spirv);
        if (!allocated_vs) {
            php_error_docref(NULL, E_WARNING, "D3D12: VS SPIR-V->HLSL failed: %s", err ? err : "unknown");
            if (err) free(err);
            if (free_ps_spirv) free(ps_spirv);
            free(shader);
            return NULL;
        }

        allocated_ps = vio_spirv_to_hlsl(ps_spirv, ps_spirv_size, 51, &err);
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

    ID3DBlob *error_blob = NULL;
    HRESULT hr;

    hr = D3DCompile(hlsl_vs, strlen(hlsl_vs), "vs_main", NULL, NULL,
                     "main", "vs_5_1", compile_flags, 0, &shader->vs_blob, &error_blob);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: VS compile failed: %s",
                          error_blob ? (char *)ID3D10Blob_GetBufferPointer(error_blob) : "unknown");
        if (error_blob) ID3D10Blob_Release(error_blob);
        goto fail;
    }

    hr = D3DCompile(hlsl_ps, strlen(hlsl_ps), "ps_main", NULL, NULL,
                     "main", "ps_5_1", compile_flags, 0, &shader->ps_blob, &error_blob);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: PS compile failed: %s",
                          error_blob ? (char *)ID3D10Blob_GetBufferPointer(error_blob) : "unknown");
        if (error_blob) ID3D10Blob_Release(error_blob);
        goto fail;
    }

    if (allocated_vs) free(allocated_vs);
    if (allocated_ps) free(allocated_ps);
    return shader;

fail:
    if (allocated_vs) free(allocated_vs);
    if (allocated_ps) free(allocated_ps);
    if (shader->vs_blob) ID3D10Blob_Release(shader->vs_blob);
    if (shader->ps_blob) ID3D10Blob_Release(shader->ps_blob);
    free(shader);
    return NULL;
}

static void d3d12_destroy_shader(void *shader_ptr)
{
    vio_d3d12_shader *s = (vio_d3d12_shader *)shader_ptr;
    if (!s) return;
    if (s->vs_blob) ID3D10Blob_Release(s->vs_blob);
    if (s->ps_blob) ID3D10Blob_Release(s->ps_blob);
    free(s);
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

    /* Wait for this frame's previous work to complete */
    d3d12_wait_for_frame(vio_d3d12.frame_index);
    /* This slot's previous command list has retired: PSOs parked while it was
     * recording can go now (see d3d12_destroy_pipeline). */
    d3d12_release_pending_psos(vio_d3d12.frame_index);
    /* Staging buffers of uploads whose fence has passed (GAP-PLAN 4.1). */
    d3d12_retire_uploads(0);

    /* This slot's previous frame has retired (wait_for_frame above): read its
     * GPU timestamps before the slot is reused. */
    if (vio_d3d12.ts_readback && vio_d3d12.ts_pending[vio_d3d12.frame_index]) {
        UINT64 *ts = NULL;
        D3D12_RANGE rr = { (SIZE_T)vio_d3d12.frame_index * 16, (SIZE_T)vio_d3d12.frame_index * 16 + 16 };
        if (SUCCEEDED(ID3D12Resource_Map(vio_d3d12.ts_readback, 0, &rr, (void **)&ts)) && ts) {
            UINT64 b = ts[vio_d3d12.frame_index * 2], e = ts[vio_d3d12.frame_index * 2 + 1];
            if (e > b && vio_d3d12.ts_frequency) {
                vio_d3d12.last_gpu_ms = (double)(e - b) * 1000.0 / (double)vio_d3d12.ts_frequency;
            }
            D3D12_RANGE wr = {0, 0};
            ID3D12Resource_Unmap(vio_d3d12.ts_readback, 0, &wr);
        }
        vio_d3d12.ts_pending[vio_d3d12.frame_index] = 0;
    }

    vio_d3d12.in_frame = 1;

    /* Reset command allocator and command list */
    ID3D12CommandAllocator_Reset(frame->cmd_allocator);
    ID3D12GraphicsCommandList_Reset(vio_d3d12.cmd_list, frame->cmd_allocator, NULL);
    if (vio_d3d12.ts_heap) {
        ID3D12GraphicsCommandList_EndQuery(vio_d3d12.cmd_list, vio_d3d12.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP,
                                           (UINT)vio_d3d12.frame_index * 2);
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
    vio_d3d12.sampler_frame_capacity = VIO_D3D12_SAMPLER_HEAP_CAPACITY / vio_d3d12.frame_count;
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
        UINT q = (UINT)vio_d3d12.frame_index * 2;
        ID3D12GraphicsCommandList_EndQuery(vio_d3d12.cmd_list, vio_d3d12.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, q + 1);
        ID3D12GraphicsCommandList_ResolveQueryData(vio_d3d12.cmd_list, vio_d3d12.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP,
                                                   q, 2, vio_d3d12.ts_readback, (UINT64)vio_d3d12.frame_index * 16);
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
    ID3D12GraphicsCommandList_DrawInstanced(vio_d3d12.cmd_list,
                                             cmd->vertex_count,
                                             instance_count,
                                             cmd->first_vertex, 0);
}

static void d3d12_draw_indexed(vio_draw_indexed_cmd *cmd)
{
    if (!cmd) return;

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

    D3D12_ROOT_PARAMETER params[3] = {0};

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
    rs_desc.NumParameters = 3;
    rs_desc.pParameters = params;
    rs_desc.NumStaticSamplers = 0;
    rs_desc.pStaticSamplers = NULL;
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
#define VIO_D3D12_COMPUTE_HEAP_BLOCKS 16

static int d3d12_ensure_compute_srv_heap(void)
{
    if (vio_d3d12.compute_srv_heap) return 0;
    if (d3d12_create_descriptor_heap(&vio_d3d12.compute_srv_heap,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                     VIO_D3D12_COMPUTE_MAX_BINDINGS * 2 * VIO_D3D12_COMPUTE_HEAP_BLOCKS,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
        return -1;
    }
    vio_d3d12.compute_srv_descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(
        vio_d3d12.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
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

    char *hlsl = vio_spirv_to_hlsl(spirv, spirv_size, 51, &err);
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

    /* Build the per-pipeline root signature from the reflected registers. */
    if (d3d12_build_compute_root_signature(cp) != 0) {
        free(cp);
        free(hlsl);
        return NULL;
    }

    ID3DBlob *error_blob = NULL;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "cs_main", NULL, NULL,
                            "main", "cs_5_1", compile_flags, 0, &cp->cs_blob, &error_blob);
    free(hlsl);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: CS compile failed: %s",
                         error_blob ? (char *)ID3D10Blob_GetBufferPointer(error_blob) : "unknown");
        if (error_blob) ID3D10Blob_Release(error_blob);
        if (cp->root_signature) ID3D12RootSignature_Release(cp->root_signature);
        free(cp);
        return NULL;
    }
    if (error_blob) ID3D10Blob_Release(error_blob);

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
    }

    void *mapped = NULL;
    D3D12_RANGE no_read = {0, 0};
    if (SUCCEEDED(ID3D12Resource_Map(cp->params_buf, 0, &no_read, &mapped))) {
        memcpy(mapped, data, (size_t)size);
        ID3D12Resource_Unmap(cp->params_buf, 0, NULL);
    }
    cp->params_size = (size_t)size;
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
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(vio_d3d12.cmd_list, vio_d3d12.root_signature);
    if (d3d12_current_pipeline && d3d12_current_pipeline->pso) {
        ID3D12GraphicsCommandList_SetPipelineState(vio_d3d12.cmd_list,
            d3d12_pipeline_pso_for_samples(d3d12_current_pipeline, vio_d3d12.current_rt_samples));
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
    ID3D12DescriptorHeap *heaps[] = { vio_d3d12.compute_srv_heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, heaps);

    if (cp->params_buf) {
        ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(list, 0,
            ID3D12Resource_GetGPUVirtualAddress(cp->params_buf));
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
    for (int i = 0; i < cp->uav_count; i++) {
        vio_d3d12_buffer *buf = cp->uavs[i].buffer;
        if (!buf || !buf->resource) continue;

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
                continue;
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

/* ── Feature Query ────────────────────────────────────────────────── */

static double d3d12_gpu_frame_time(void)
{
    return vio_d3d12.initialized && vio_d3d12.ts_heap ? vio_d3d12.last_gpu_ms : -1.0;
}

static int d3d12_supports_feature(vio_feature feature)
{
    switch (feature) {
        case VIO_FEATURE_COMPUTE:      return 1; /* compute pipeline + dispatch + readback wired */
        /* The hardware can, but vio_shader_desc only carries a vertex + fragment
         * stage — there is no way to hand a GS / HS / DS to the backend, so the
         * flags must not promise it. */
        case VIO_FEATURE_TESSELLATION: return 0;
        case VIO_FEATURE_GEOMETRY:     return 0;
        case VIO_FEATURE_RAYTRACING:   return 0; /* DXR possible but not implemented */
        case VIO_FEATURE_MULTIVIEW:    return 0;
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
        case VIO_FEATURE_RENDER_TARGET_CUBE:  return 1; /* 6-slice array + per-(face,mip) RTVs (GAP-PLAN Phase 2) */
        case VIO_FEATURE_MIPMAP_GEN:          return 1; /* CPU box filter + re-upload (GAP-PLAN 2.3) */
        case VIO_FEATURE_CUBEMAP:      return 1;
        case VIO_FEATURE_DEPTH_BIAS:   return 1; /* PSO rasterizer state */
        case VIO_FEATURE_SCISSOR:      return 1;
        case VIO_FEATURE_TEXTURE_SWIZZLE: return 1; /* SRV Shader4ComponentMapping (R8 atlas -> (1,1,1,R)) */
        case VIO_FEATURE_NATIVE_2D_BATCH: return 1; /* vio_2d_d3d12_* */
        case VIO_FEATURE_TEXTURE_3D:   return 1; /* TEXTURE3D resource + SRV */
        case VIO_FEATURE_VERTEX_STORAGE: return 1; /* VS-visible root SRV in the shared root signature */
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
        ID3D12GraphicsCommandList_DrawIndexedInstanced(vio_d3d12.cmd_list,
            (UINT)mesh->index_count, (UINT)instance_count, 0, 0, 0);
    } else {
        ID3D12GraphicsCommandList_DrawInstanced(vio_d3d12.cmd_list,
            (UINT)mesh->vertex_count, (UINT)instance_count, 0, 0);
    }
}

/* ── State binding ────────────────────────────────────────────────── */

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
static void d3d12_flush_sampler_table(void)
{
    if (!vio_d3d12.sampler_heap || !vio_d3d12.sampler_combo_heap) return;

    int combos[VIO_D3D12_SAMPLER_TABLE_SIZE];
    for (int i = 0; i < VIO_D3D12_SAMPLER_TABLE_SIZE; i++) {
        combos[i] = vio_d3d12.pending_srv_valid[i] ? vio_d3d12.pending_samplers[i] : 0;
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
            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(vio_d3d12.cmd_list, 4, vio_d3d12.sampler_table_gpu);
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
            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(vio_d3d12.cmd_list, 4, vio_d3d12.sampler_table_gpu);
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
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(vio_d3d12.cmd_list, 4, dst_gpu);

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
            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(
                vio_d3d12.cmd_list, 2, vio_d3d12.srv_table_gpu);
            return;
        }
    }

    /* Allocate VIO_D3D12_SRV_TABLE_SIZE contiguous descriptors from THIS frame's
     * region of the SRV heap. Bound the check by the region end (not the full
     * heap) so we can't bleed into another frame's slice. */
    UINT base_idx = vio_d3d12.srv_frame_offset;
    UINT region_end = vio_d3d12.srv_frame_base + vio_d3d12.srv_frame_capacity;
    if (base_idx + VIO_D3D12_SRV_TABLE_SIZE > region_end) return; /* out of space in this frame's region */
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

    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(vio_d3d12.cmd_list, 2, dst_gpu);

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

/* ── Setup context (called from vio_create after window creation) ── */

int vio_d3d12_setup_context(void *glfw_window, vio_config *cfg)
{
    vio_d3d12.glfw_window = glfw_window;

    /* Create surface (swapchain + render targets + depth buffer) */
    void *surface = d3d12_create_surface(cfg);
    if (!surface) {
        return -1;
    }

    return 0;
}

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
    .gpu_frame_time    = d3d12_gpu_frame_time,
    .destroy_cubemap   = d3d12_destroy_cubemap,
    .upload_cubemap    = d3d12_upload_cubemap,
    .read_render_target = d3d12_read_render_target,
    .destroy_font_atlas = d3d12_destroy_font_atlas,
    .destroy_render_target = d3d12_destroy_render_target,
    /* Render-target lifecycle (GAP-PLAN Phase 2 — previously inline in php_vio.c). */
    .create_render_target    = d3d12_create_render_target,
    .bind_render_target      = d3d12_bind_render_target,
    .unbind_render_target    = d3d12_unbind_render_target,
    .bind_render_target_face = d3d12_bind_render_target_face,
    .render_target_cubemap   = d3d12_render_target_cubemap,
    .generate_mipmaps        = d3d12_generate_mipmaps,
    .update_texture          = d3d12_update_texture,
};

void vio_backend_d3d12_register(void)
{
    vio_register_backend(&d3d12_backend);
}

#endif /* HAVE_D3D12 */
