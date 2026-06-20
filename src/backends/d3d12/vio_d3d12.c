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
#include <string.h>
#include <stdlib.h>

vio_d3d12_state vio_d3d12 = {0};

/* Currently bound pipeline (for vertex stride in draw calls) */
static vio_d3d12_pipeline *d3d12_current_pipeline = NULL;

/* Pull pending validation messages out of the D3D12 InfoQueue and forward them
 * to PHP's error log. No-op when the debug layer is not active. Called after
 * any operation that might have triggered validation errors (resource creation
 * failures, present(), etc.) so the underlying cause shows up next to the
 * symptomatic failure rather than scrolling past in the Windows event log. */
static void d3d12_drain_info_queue(const char *context)
{
    if (!vio_d3d12.device) return;

    ID3D12InfoQueue *iq = NULL;
    HRESULT hr = ID3D12Device_QueryInterface(vio_d3d12.device, &IID_ID3D12InfoQueue, (void **)&iq);
    if (FAILED(hr) || !iq) {
        /* One-shot note via the channel that actually surfaces (direct stderr),
         * so "no messages" is never ambiguous: it means the debug layer / its
         * InfoQueue is genuinely unavailable, not that the drain was skipped. */
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[d3d12] drain(%s): InfoQueue UNAVAILABLE (debug layer not active)\n",
                    context ? context : "?");
            fflush(stderr);
        }
        return; /* debug layer not enabled */
    }

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
    ID3D12InfoQueue_Release(iq);
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
     */
    D3D12_ROOT_PARAMETER params[3] = {0};

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

    /* Static samplers: s0-s7 = regular, s8-s11 = comparison.
     * SPIRV-Cross assigns shadow samplers to s8+ and regular to s0+ (see
     * vio_shader_reflect.c — the base is 8 so up to 8 regular samplers fit). */
    D3D12_STATIC_SAMPLER_DESC static_samplers[12] = {0};

    /* s0-s7: Regular linear wrap (general textures) */
    for (int s = 0; s < 8; s++) {
        static_samplers[s].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        static_samplers[s].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        static_samplers[s].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        static_samplers[s].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        static_samplers[s].MaxAnisotropy = 1;
        static_samplers[s].MaxLOD = D3D12_FLOAT32_MAX;
        static_samplers[s].ShaderRegister = s;
        static_samplers[s].RegisterSpace = 0;
        static_samplers[s].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    /* s8-s11: Comparison samplers (shadow maps with sampler2DShadow) */
    for (int s = 8; s < 12; s++) {
        static_samplers[s].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        static_samplers[s].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        static_samplers[s].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        static_samplers[s].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        static_samplers[s].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        static_samplers[s].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        static_samplers[s].MaxLOD = D3D12_FLOAT32_MAX;
        static_samplers[s].ShaderRegister = s;
        static_samplers[s].RegisterSpace = 0;
        static_samplers[s].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_ROOT_SIGNATURE_DESC rs_desc = {0};
    rs_desc.NumParameters = 3; /* VS CBV, PS CBV, SRV table */
    rs_desc.pParameters = params;
    rs_desc.NumStaticSamplers = 12;
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

    for (UINT i = 0; i < VIO_D3D12_FRAME_COUNT; i++) {
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
    for (UINT i = 0; i < VIO_D3D12_FRAME_COUNT; i++) {
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

static int d3d12_init(vio_config *cfg)
{
    HRESULT hr;

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
            ID3D12InfoQueue_Release(iq);
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
    for (UINT i = 0; i < VIO_D3D12_FRAME_COUNT; i++) {
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

    /* Create descriptor heaps */
    if (d3d12_create_descriptor_heap(&vio_d3d12.rtv_heap,
                                      D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                                      VIO_D3D12_FRAME_COUNT,
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

    /* Root signature */
    if (d3d12_create_root_signature() != 0) {
        goto init_fail;
    }

    /* Per-frame linear cbuffer allocator: persistently mapped UPLOAD heap.
     * Each draw call allocates a 256-byte-aligned slice for its cbuffer data.
     *
     * The heap is split into VIO_D3D12_FRAME_COUNT equal per-frame slices
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
            vio_d3d12.cbuffer_frame_end = heap_size / VIO_D3D12_FRAME_COUNT;
            vio_d3d12.cbuffer_heap_offset = 0;
            /* Persistently map (never unmap — valid for UPLOAD heaps in D3D12) */
            D3D12_RANGE rr = {0, 0};
            ID3D12Resource_Map(vio_d3d12.cbuffer_heap, 0, &rr, (void **)&vio_d3d12.cbuffer_heap_mapped);
        }
    }

    /* Per-frame linear instance-data allocator: persistently mapped UPLOAD heap.
     * Mirrors the cbuffer heap exactly. Each vio_draw_instanced gets a 256-byte-
     * aligned slice holding its mat4 instance array; vbvs[1] points at that
     * slice's GPU VA. The heap is split into VIO_D3D12_FRAME_COUNT equal slices
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
            vio_d3d12.instance_frame_end = heap_size / VIO_D3D12_FRAME_COUNT;
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

    for (UINT i = 0; i < VIO_D3D12_FRAME_COUNT; i++) {
        if (vio_d3d12.frames[i].cmd_allocator) {
            ID3D12CommandAllocator_Release(vio_d3d12.frames[i].cmd_allocator);
        }
    }

    if (vio_d3d12.cmd_list)       ID3D12GraphicsCommandList_Release(vio_d3d12.cmd_list);
    if (vio_d3d12.root_signature) ID3D12RootSignature_Release(vio_d3d12.root_signature);
    if (vio_d3d12.fence)          ID3D12Fence_Release(vio_d3d12.fence);
    if (vio_d3d12.fence_event)    CloseHandle(vio_d3d12.fence_event);
    if (vio_d3d12.rtv_heap)       ID3D12DescriptorHeap_Release(vio_d3d12.rtv_heap);
    if (vio_d3d12.dsv_heap)       ID3D12DescriptorHeap_Release(vio_d3d12.dsv_heap);
    if (vio_d3d12.srv_heap.heap)  ID3D12DescriptorHeap_Release(vio_d3d12.srv_heap.heap);
    if (vio_d3d12.srv_staging_heap) ID3D12DescriptorHeap_Release(vio_d3d12.srv_staging_heap);
    if (vio_d3d12.swapchain)      IDXGISwapChain3_Release(vio_d3d12.swapchain);
    if (vio_d3d12.cmd_queue)      ID3D12CommandQueue_Release(vio_d3d12.cmd_queue);
    if (vio_d3d12.factory)        IDXGIFactory4_Release(vio_d3d12.factory);
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
    sc_desc.BufferCount = VIO_D3D12_FRAME_COUNT;
    sc_desc.Scaling = DXGI_SCALING_STRETCH;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

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

    HRESULT hr = IDXGISwapChain3_ResizeBuffers(vio_d3d12.swapchain,
                                                VIO_D3D12_FRAME_COUNT,
                                                width, height,
                                                DXGI_FORMAT_R8G8B8A8_UNORM, 0);
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

static void *d3d12_create_pipeline(vio_pipeline_desc *desc)
{
    vio_d3d12_pipeline *pipeline = calloc(1, sizeof(vio_d3d12_pipeline));
    if (!pipeline) return NULL;

    vio_d3d12_shader *shader = (vio_d3d12_shader *)desc->shader;
    if (!shader) { free(pipeline); return NULL; }

    pipeline->topology = vio_topology_to_d3d12(desc->topology);

    /* Build input layout */
    D3D12_INPUT_ELEMENT_DESC *elements = NULL;
    UINT vertex_stride = 0;
    if (desc->vertex_attrib_count > 0 && desc->vertex_layout) {
        elements = calloc(desc->vertex_attrib_count, sizeof(D3D12_INPUT_ELEMENT_DESC));
        UINT vertex_offset = 0;
        for (int i = 0; i < desc->vertex_attrib_count; i++) {
            int loc = desc->vertex_layout[i].location;
            elements[i].SemanticName = vio_usage_to_semantic(desc->vertex_layout[i].usage);
            elements[i].SemanticIndex = loc;
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
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso_desc.DepthStencilState.DepthFunc = (desc->depth_func == VIO_DEPTH_LEQUAL)
        ? D3D12_COMPARISON_FUNC_LESS_EQUAL
        : D3D12_COMPARISON_FUNC_LESS;
    pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    /* Blend */
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (desc->blend == VIO_BLEND_ALPHA) {
        pso_desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        pso_desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        pso_desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        pso_desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pso_desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        pso_desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    } else if (desc->blend == VIO_BLEND_ADDITIVE) {
        pso_desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        pso_desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        pso_desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        pso_desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pso_desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        pso_desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }

    /* Topology type */
    pso_desc.PrimitiveTopologyType = vio_topology_to_d3d12_type(desc->topology);

    /* Render target format. Must match the format of the render target bound at
     * draw time (D3D12 hard rule), else DrawIndexedInstanced is dropped with
     * "render target format ... does not match that specified by the current
     * pipeline state". Default is R8G8B8A8_UNORM (swapchain + every LDR offscreen
     * target); desc->hdr_output selects R16G16B16A16_FLOAT to match a render
     * target created with hdr=true (e.g. the SSAO G-buffer). */
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = desc->hdr_output
        ? DXGI_FORMAT_R16G16B16A16_FLOAT
        : DXGI_FORMAT_R8G8B8A8_UNORM;

    /* MSAA */
    pso_desc.SampleDesc.Count = 1;
    pso_desc.SampleMask = UINT_MAX;

    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(vio_d3d12.device, &pso_desc,
                                                           &IID_ID3D12PipelineState,
                                                           (void **)&pipeline->pso);
    if (elements) free(elements);
    if (FAILED(hr)) {
        d3d12_drain_info_queue("create_pso_fail");
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create PSO (0x%08lx)", hr);
        free(pipeline);
        return NULL;
    }

    return pipeline;
}

static void d3d12_destroy_pipeline(void *pipeline_ptr)
{
    vio_d3d12_pipeline *p = (vio_d3d12_pipeline *)pipeline_ptr;
    if (!p) return;
    if (d3d12_current_pipeline == p) d3d12_current_pipeline = NULL;
    if (p->pso) ID3D12PipelineState_Release(p->pso);
    free(p);
}

static void d3d12_bind_pipeline(void *pipeline_ptr)
{
    vio_d3d12_pipeline *p = (vio_d3d12_pipeline *)pipeline_ptr;
    if (!p) return;

    d3d12_current_pipeline = p;
    ID3D12GraphicsCommandList_SetPipelineState(vio_d3d12.cmd_list, p->pso);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(vio_d3d12.cmd_list,
                                                        vio_d3d12.root_signature);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(vio_d3d12.cmd_list, p->topology);

    /* Bind SRV heap */
    ID3D12DescriptorHeap *heaps[] = { vio_d3d12.srv_heap.heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(vio_d3d12.cmd_list, 1, heaps);
}

/* ── Resources: Buffers ───────────────────────────────────────────── */

static void *d3d12_create_buffer(vio_buffer_desc *desc)
{
    vio_d3d12_buffer *buf = calloc(1, sizeof(vio_d3d12_buffer));
    if (!buf) return NULL;

    buf->type = desc->type;
    buf->size = desc->size;
    buf->binding = desc->binding;

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
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
        initial_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        res_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    } else {
        /* Vertex/index: upload heap for simplicity (can optimize to default+staging later) */
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
    if (desc->data && heap_props.Type == D3D12_HEAP_TYPE_UPLOAD) {
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
    if (buf->upload_resource) ID3D12Resource_Release(buf->upload_resource);
    if (buf->resource) ID3D12Resource_Release(buf->resource);
    free(buf);
}

/* ── Resources: Textures ──────────────────────────────────────────── */

static void *d3d12_create_texture(vio_texture_desc *desc)
{
    vio_d3d12_texture *tex = calloc(1, sizeof(vio_d3d12_texture));
    if (!tex) return NULL;

    tex->width = desc->width;
    tex->height = desc->height;

    /* Create texture resource on default heap */
    D3D12_HEAP_PROPERTIES heap_props = {0};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC res_desc = {0};
    res_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    res_desc.Width = desc->width;
    res_desc.Height = desc->height;
    res_desc.DepthOrArraySize = 1;
    res_desc.MipLevels = 1;
    res_desc.Format = desc->single_channel ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    res_desc.SampleDesc.Count = 1;
    res_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    HRESULT hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device,
                                                       &heap_props,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &res_desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       NULL,
                                                       &IID_ID3D12Resource,
                                                       (void **)&tex->resource);
    if (FAILED(hr)) {
        free(tex);
        return NULL;
    }

    /* Upload data via staging buffer */
    if (desc->data) {
        UINT64 upload_size = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {0};
        ID3D12Device_GetCopyableFootprints(vio_d3d12.device, &res_desc,
                                            0, 1, 0, &footprint, NULL, NULL, &upload_size);

        D3D12_HEAP_PROPERTIES upload_heap = {0};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC upload_desc = {0};
        upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload_desc.Width = upload_size;
        upload_desc.Height = 1;
        upload_desc.DepthOrArraySize = 1;
        upload_desc.MipLevels = 1;
        upload_desc.SampleDesc.Count = 1;
        upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device,
                                                   &upload_heap,
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &upload_desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   NULL,
                                                   &IID_ID3D12Resource,
                                                   (void **)&tex->upload_resource);
        if (SUCCEEDED(hr)) {
            /* Copy pixel data to upload buffer */
            void *mapped = NULL;
            D3D12_RANGE read_range = {0, 0};
            hr = ID3D12Resource_Map(tex->upload_resource, 0, &read_range, &mapped);
            if (SUCCEEDED(hr)) {
                const uint8_t *src = (const uint8_t *)desc->data;
                uint8_t *dst = (uint8_t *)mapped + footprint.Offset;
                UINT src_pitch = desc->width * (desc->single_channel ? 1 : 4);
                for (int row = 0; row < desc->height; row++) {
                    memcpy(dst + row * footprint.Footprint.RowPitch,
                           src + row * src_pitch,
                           src_pitch);
                }
                ID3D12Resource_Unmap(tex->upload_resource, 0, NULL);
            }

            /* Execute copy on a temporary command list */
            ID3D12CommandAllocator *upload_alloc = NULL;
            ID3D12GraphicsCommandList *upload_cmd = NULL;

            hr = ID3D12Device_CreateCommandAllocator(vio_d3d12.device,
                D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                (void **)&upload_alloc);
            if (SUCCEEDED(hr)) {
                hr = ID3D12Device_CreateCommandList(vio_d3d12.device, 0,
                    D3D12_COMMAND_LIST_TYPE_DIRECT, upload_alloc, NULL,
                    &IID_ID3D12GraphicsCommandList, (void **)&upload_cmd);
            }
            if (SUCCEEDED(hr)) {
                D3D12_TEXTURE_COPY_LOCATION dst_loc = {0};
                dst_loc.pResource = tex->resource;
                dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst_loc.SubresourceIndex = 0;

                D3D12_TEXTURE_COPY_LOCATION src_loc = {0};
                src_loc.pResource = tex->upload_resource;
                src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src_loc.PlacedFootprint = footprint;

                ID3D12GraphicsCommandList_CopyTextureRegion(upload_cmd,
                    &dst_loc, 0, 0, 0, &src_loc, NULL);

                /* Transition: COPY_DEST -> PIXEL_SHADER_RESOURCE */
                D3D12_RESOURCE_BARRIER barrier = {0};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = tex->resource;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                ID3D12GraphicsCommandList_ResourceBarrier(upload_cmd, 1, &barrier);

                ID3D12GraphicsCommandList_Close(upload_cmd);

                ID3D12CommandList *lists[] = { (ID3D12CommandList *)upload_cmd };
                ID3D12CommandQueue_ExecuteCommandLists(vio_d3d12.cmd_queue, 1, lists);

                /* Wait for upload to finish */
                vio_d3d12_wait_for_gpu();
            }
            if (upload_cmd) ID3D12GraphicsCommandList_Release(upload_cmd);
            if (upload_alloc) ID3D12CommandAllocator_Release(upload_alloc);

            /* Release staging buffer — data is on the GPU now */
            ID3D12Resource_Release(tex->upload_resource);
            tex->upload_resource = NULL;
        }
    }

    /* Create SRV. For an R8 glyph atlas, swizzle the single channel so the
     * texture reads as (1,1,1,R) — white RGB, coverage in alpha — exactly what
     * the shared sprite shader expects, so no separate text shader is needed
     * (unlike D3D11, whose SRVs have no component mapping). */
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
    srv_desc.Format = desc->single_channel ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = desc->single_channel
        ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
              D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1,
              D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1,
              D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1,
              D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0)
        : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;

    d3d12_alloc_srv_descriptor(&tex->srv_cpu, &tex->srv_gpu);
    ID3D12Device_CreateShaderResourceView(vio_d3d12.device, tex->resource,
                                           &srv_desc, tex->srv_cpu);

    return tex;
}

/* Create a 3D / volume texture (Fieldtracing SDF). Mirrors d3d12_create_texture
 * but with a TEXTURE3D resource, a per-slice upload copy (GetCopyableFootprints
 * lays the subresource out as Depth slices of RowPitch*Height each), and a
 * TEXTURE3D SRV. The bind path (d3d12_bind_texture) is unchanged — it binds the
 * SRV descriptor, which is dimension-agnostic. */
static void *d3d12_create_texture_3d(vio_texture_desc *desc)
{
    if (desc->depth <= 0) return NULL;

    vio_d3d12_texture *tex = calloc(1, sizeof(vio_d3d12_texture));
    if (!tex) return NULL;

    tex->width = desc->width;
    tex->height = desc->height;
    tex->depth = desc->depth;

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

    HRESULT hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device,
                                                       &heap_props,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &res_desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       NULL,
                                                       &IID_ID3D12Resource,
                                                       (void **)&tex->resource);
    if (FAILED(hr)) {
        free(tex);
        return NULL;
    }

    if (desc->data) {
        UINT64 upload_size = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {0};
        ID3D12Device_GetCopyableFootprints(vio_d3d12.device, &res_desc,
                                            0, 1, 0, &footprint, NULL, NULL, &upload_size);

        D3D12_HEAP_PROPERTIES upload_heap = {0};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC upload_desc = {0};
        upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload_desc.Width = upload_size;
        upload_desc.Height = 1;
        upload_desc.DepthOrArraySize = 1;
        upload_desc.MipLevels = 1;
        upload_desc.SampleDesc.Count = 1;
        upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device,
                                                   &upload_heap,
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &upload_desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   NULL,
                                                   &IID_ID3D12Resource,
                                                   (void **)&tex->upload_resource);
        if (SUCCEEDED(hr)) {
            void *mapped = NULL;
            D3D12_RANGE read_range = {0, 0};
            hr = ID3D12Resource_Map(tex->upload_resource, 0, &read_range, &mapped);
            if (SUCCEEDED(hr)) {
                const uint8_t *src = (const uint8_t *)desc->data;
                uint8_t *dst = (uint8_t *)mapped + footprint.Offset;
                UINT src_row_pitch   = (UINT)desc->width * 4;
                UINT src_slice_pitch = src_row_pitch * (UINT)desc->height;
                UINT dst_row_pitch   = footprint.Footprint.RowPitch;
                UINT dst_slice_pitch = dst_row_pitch * (UINT)desc->height;
                for (int z = 0; z < desc->depth; z++) {
                    for (int row = 0; row < desc->height; row++) {
                        memcpy(dst + (size_t)z * dst_slice_pitch + (size_t)row * dst_row_pitch,
                               src + (size_t)z * src_slice_pitch + (size_t)row * src_row_pitch,
                               src_row_pitch);
                    }
                }
                ID3D12Resource_Unmap(tex->upload_resource, 0, NULL);
            }

            ID3D12CommandAllocator *upload_alloc = NULL;
            ID3D12GraphicsCommandList *upload_cmd = NULL;

            hr = ID3D12Device_CreateCommandAllocator(vio_d3d12.device,
                D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                (void **)&upload_alloc);
            if (SUCCEEDED(hr)) {
                hr = ID3D12Device_CreateCommandList(vio_d3d12.device, 0,
                    D3D12_COMMAND_LIST_TYPE_DIRECT, upload_alloc, NULL,
                    &IID_ID3D12GraphicsCommandList, (void **)&upload_cmd);
            }
            if (SUCCEEDED(hr)) {
                D3D12_TEXTURE_COPY_LOCATION dst_loc = {0};
                dst_loc.pResource = tex->resource;
                dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst_loc.SubresourceIndex = 0;

                D3D12_TEXTURE_COPY_LOCATION src_loc = {0};
                src_loc.pResource = tex->upload_resource;
                src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src_loc.PlacedFootprint = footprint;   /* footprint covers all Depth slices */

                ID3D12GraphicsCommandList_CopyTextureRegion(upload_cmd,
                    &dst_loc, 0, 0, 0, &src_loc, NULL);

                D3D12_RESOURCE_BARRIER barrier = {0};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = tex->resource;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                ID3D12GraphicsCommandList_ResourceBarrier(upload_cmd, 1, &barrier);

                ID3D12GraphicsCommandList_Close(upload_cmd);

                ID3D12CommandList *lists[] = { (ID3D12CommandList *)upload_cmd };
                ID3D12CommandQueue_ExecuteCommandLists(vio_d3d12.cmd_queue, 1, lists);
                vio_d3d12_wait_for_gpu();
            }
            if (upload_cmd) ID3D12GraphicsCommandList_Release(upload_cmd);
            if (upload_alloc) ID3D12CommandAllocator_Release(upload_alloc);

            ID3D12Resource_Release(tex->upload_resource);
            tex->upload_resource = NULL;
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

static void d3d12_destroy_cubemap(void *cm_ptr)
{
    vio_cubemap_object *cm = (vio_cubemap_object *)cm_ptr;
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

    UINT compile_flags = 0;
    if (vio_d3d12.debug_enabled) {
        compile_flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
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

    vio_d3d12.in_frame = 1;

    /* Reset command allocator and command list */
    ID3D12CommandAllocator_Reset(frame->cmd_allocator);
    ID3D12GraphicsCommandList_Reset(vio_d3d12.cmd_list, frame->cmd_allocator, NULL);

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
    vio_d3d12.current_rtv = frame->rtv_handle;
    vio_d3d12.current_dsv = dsv_handle;
    vio_d3d12.current_rt_width = vio_d3d12.width;
    vio_d3d12.current_rt_height = vio_d3d12.height;
    vio_d3d12.current_has_rtv = 1;

    /* Grow cbuffer heap if last frame used >75% of its per-frame slice */
    UINT cb_slice = vio_d3d12.cbuffer_heap_capacity / VIO_D3D12_FRAME_COUNT;
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
        UINT inst_slice = vio_d3d12.instance_heap_capacity / VIO_D3D12_FRAME_COUNT;
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
     * [0, capacity - srv_heap.count), split into VIO_D3D12_FRAME_COUNT
     * equal slices indexed by frame_index. The two regions never overlap
     * (until the heap is genuinely full), so a texture created mid-frame
     * gets a high-index SRV that's outside every frame's per-frame slice
     * and is therefore safe from the null-init sweep in flush_srv_table. */
    /* Rebase the cbuffer allocator into THIS frame's slice. The other frame
     * in flight keeps reading its own slice — never overwritten from here. */
    cb_slice = vio_d3d12.cbuffer_heap_capacity / VIO_D3D12_FRAME_COUNT;
    vio_d3d12.cbuffer_frame_base = vio_d3d12.frame_index * cb_slice;
    vio_d3d12.cbuffer_frame_end  = vio_d3d12.cbuffer_frame_base + cb_slice;
    vio_d3d12.cbuffer_heap_offset = vio_d3d12.cbuffer_frame_base;
    /* Rebase the instance allocator into THIS frame's slice (same as cbuffer). */
    UINT inst_slice = vio_d3d12.instance_heap_capacity / VIO_D3D12_FRAME_COUNT;
    vio_d3d12.instance_frame_base = vio_d3d12.frame_index * inst_slice;
    vio_d3d12.instance_frame_end  = vio_d3d12.instance_frame_base + inst_slice;
    vio_d3d12.instance_heap_offset = vio_d3d12.instance_frame_base;
    UINT perframe_total = (vio_d3d12.srv_heap.capacity > vio_d3d12.srv_heap.count)
                           ? (vio_d3d12.srv_heap.capacity - vio_d3d12.srv_heap.count) : 0;
    vio_d3d12.srv_frame_capacity = perframe_total / VIO_D3D12_FRAME_COUNT;
    vio_d3d12.srv_frame_base     = vio_d3d12.frame_index * vio_d3d12.srv_frame_capacity;
    vio_d3d12.srv_frame_offset   = vio_d3d12.srv_frame_base;
    memset(vio_d3d12.pending_srv_valid, 0, sizeof(vio_d3d12.pending_srv_valid));

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
        ibv.Format = DXGI_FORMAT_R32_UINT;
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

    HRESULT hr = IDXGISwapChain3_Present(vio_d3d12.swapchain, vio_d3d12.vsync ? 1 : 0, 0);
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

    /* Clear whichever render target is currently bound */
    if (vio_d3d12.current_has_rtv) {
        ID3D12GraphicsCommandList_ClearRenderTargetView(vio_d3d12.cmd_list,
                                                         vio_d3d12.current_rtv,
                                                         color, 0, NULL);
    }

    ID3D12GraphicsCommandList_ClearDepthStencilView(vio_d3d12.cmd_list,
                                                     vio_d3d12.current_dsv,
                                                     D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                                     1.0f, 0, 0, NULL);
}

/* ── Compute ──────────────────────────────────────────────────────── */

static void d3d12_dispatch_compute(vio_compute_cmd *cmd)
{
    /* TODO: Implement compute pipeline + dispatch
     * 1. Create compute PSO (separate from graphics PSO)
     * 2. Bind UAVs via root signature
     * 3. ID3D12GraphicsCommandList_Dispatch(cmd_list, x, y, z)
     */
    (void)cmd;
    php_error_docref(NULL, E_NOTICE, "D3D12: Compute dispatch not yet implemented");
}

/* ── Feature Query ────────────────────────────────────────────────── */

static int d3d12_supports_feature(vio_feature feature)
{
    switch (feature) {
        case VIO_FEATURE_COMPUTE:      return 0; /* TODO: not yet implemented */
        case VIO_FEATURE_TESSELLATION: return 1;
        case VIO_FEATURE_GEOMETRY:     return 1;
        case VIO_FEATURE_RAYTRACING:   return 0; /* DXR possible but not implemented */
        case VIO_FEATURE_MULTIVIEW:    return 0;
        case VIO_FEATURE_3D_PIPELINE:  return 1;
        case VIO_FEATURE_READ_PIXELS:  return 1;
        case VIO_FEATURE_INSTANCED_DRAW: return 1;
        case VIO_FEATURE_RENDER_TARGET:       return 1;
        case VIO_FEATURE_RENDER_TARGET_HDR:   return 1;
        case VIO_FEATURE_RENDER_TARGET_DEPTH: return 1;
        case VIO_FEATURE_RENDER_TARGET_MSAA:  return 1;
        case VIO_FEATURE_CUBEMAP:      return 1;
        case VIO_FEATURE_DEPTH_BIAS:   return 1; /* PSO rasterizer state */
        case VIO_FEATURE_SCISSOR:      return 1;
        case VIO_FEATURE_TEXTURE_SWIZZLE: return 0; /* needs CPU expansion */
        case VIO_FEATURE_NATIVE_2D_BATCH: return 1; /* vio_2d_d3d12_* */
        case VIO_FEATURE_TEXTURE_3D:   return 1; /* TEXTURE3D resource + SRV */
        default:                       return 0;
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

    /* Store pending binding — flushed before draw via d3d12_flush_srv_table() */
    vio_d3d12.pending_srvs[slot] = tex->srv_cpu;
    vio_d3d12.pending_srv_valid[slot] = 1;
}

/* Flush pending texture bindings into a contiguous SRV descriptor block */
void vio_d3d12_flush_srv_table(void)
{
    int any_bound = 0;
    for (int i = 0; i < VIO_D3D12_SRV_TABLE_SIZE; i++) {
        if (vio_d3d12.pending_srv_valid[i]) { any_bound = 1; break; }
    }
    if (!any_bound) return; /* no textures — skip descriptor table binding entirely */

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

    /* Create null SRV for all slots first, then overwrite bound ones */
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
    .supports_feature  = d3d12_supports_feature,
    .destroy_cubemap   = d3d12_destroy_cubemap,
    .destroy_font_atlas = d3d12_destroy_font_atlas,
    .destroy_render_target = d3d12_destroy_render_target,
};

void vio_backend_d3d12_register(void)
{
    vio_register_backend(&d3d12_backend);
}

#endif /* HAVE_D3D12 */
