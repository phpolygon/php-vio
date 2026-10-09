--TEST--
Backend capability matrix: VIO_FEATURE_* returns the documented values per backend
--SKIPIF--
<?php
require __DIR__ . '/../skipif_gl.inc';
if (!extension_loaded('vio')) die('skip vio not loaded');
?>
--FILE--
<?php
/* CLAUDE.md "OpenGL-Feature-Ladder" + each backend's supports_feature impl
 * documents what should be 1 / 0. This test pins that contract:
 *
 *   - opengl  : full set on 3.3+, plus ARB-fallback caps where applicable
 *   - null    : 0 for everything (it's a no-op backend)
 *
 * Other backends are skipped where unavailable. The intent isn't full
 * cross-backend parity — that's 067 — but a regression gate on what
 * supports_feature reports.
 *
 * vio_supports_feature($ctx, $flag) reads ctx->backend->supports_feature. */

function probe(string $backend_name, array $expected, int $width = 16): void {
    $ctx = vio_create($backend_name, ["width" => $width, "height" => $width, "headless" => true]);
    if (!$ctx) {
        echo "$backend_name: skip (unavailable)\n";
        return;
    }
    foreach ($expected as $flag => $want) {
        $got = vio_supports_feature($ctx, $flag);
        if ((bool)$got !== (bool)$want) {
            echo "$backend_name: $flag expected " . ($want ? "1" : "0") .
                 " got " . ($got ? "1" : "0") . "\n";
        }
    }
    vio_destroy($ctx);
    echo "$backend_name: OK\n";
}

/* OpenGL: guaranteed at the 3.3 floor (per CLAUDE.md table) */
probe("opengl", [
    VIO_FEATURE_3D_PIPELINE        => 1,
    VIO_FEATURE_GEOMETRY           => 1,
    VIO_FEATURE_READ_PIXELS        => 1,
    VIO_FEATURE_INSTANCED_DRAW     => 1,
    VIO_FEATURE_RENDER_TARGET      => 1,
    VIO_FEATURE_RENDER_TARGET_HDR  => 1,
    VIO_FEATURE_RENDER_TARGET_DEPTH=> 1,
    VIO_FEATURE_RENDER_TARGET_MSAA => 1,
    VIO_FEATURE_STENCIL            => 1,   /* DEPTH24_STENCIL8 attachments (GAP-PHASE5 1) */
    VIO_FEATURE_CUBEMAP            => 1,
    VIO_FEATURE_DEPTH_BIAS         => 1,
    VIO_FEATURE_SCISSOR            => 1,
    VIO_FEATURE_TEXTURE_SWIZZLE    => 1,
    VIO_FEATURE_NATIVE_2D_BATCH    => 1,
    VIO_FEATURE_RENDER_TARGET_CUBE => 1,
    VIO_FEATURE_MIPMAP_GEN         => 1,
    VIO_FEATURE_MRT                => 1,
    VIO_FEATURE_RENDER_TARGET_LAYERED => 1,   /* GL_TEXTURE_2D_ARRAY / depth cubemaps (GEOMETRY-STAGES-PLAN 1a) */
    VIO_FEATURE_RENDER_TARGET_DEPTH_SAMPLE => 1,   /* every target's depth is a texture (test 223) */
    VIO_FEATURE_RENDER_TARGET_STORAGE => 0,   /* 'storage' targets: D3D12 / Vulkan only (test 225) */
    VIO_FEATURE_UPSCALER_NATIVE    => 0,   /* native upscalers: D3D12 / Vulkan only (test 226) */
    VIO_FEATURE_RAYTRACING         => 0,
    /* MULTIVIEW is device dependent here (GL_OVR_multiview2 / VkPhysicalDeviceMultiviewFeatures): test 158. */
]);

/* Null: always 0 — it's the no-op test backend */
probe("null", [
    VIO_FEATURE_3D_PIPELINE        => 0,
    VIO_FEATURE_COMPUTE            => 0,
    VIO_FEATURE_TESSELLATION       => 0,
    VIO_FEATURE_GEOMETRY           => 0,
    VIO_FEATURE_RENDER_TARGET      => 0,
    VIO_FEATURE_READ_PIXELS        => 0,
    VIO_FEATURE_NATIVE_2D_BATCH    => 0,
    VIO_FEATURE_STENCIL            => 0,
    VIO_FEATURE_GPU_TIMESTAMP      => 0,
    VIO_FEATURE_FRAME_LATENCY      => 0,
    VIO_FEATURE_HDR_OUTPUT         => 0,
    VIO_FEATURE_INDIRECT_DRAW      => 0,
    VIO_FEATURE_TEXTURE_ARRAY      => 0,
    VIO_FEATURE_TEXTURE_COMPRESSION_BC => 0,
    VIO_FEATURE_SHADING_RATE       => 0,
    VIO_FEATURE_RENDER_TARGET_LAYERED => 0,
    VIO_FEATURE_LAYERED_RENDER     => 0,   /* device / extension dependent elsewhere: test 137 is the contract */
    VIO_FEATURE_VERTEX_LAYER       => 0,
    VIO_FEATURE_MULTI_VIEWPORT     => 0,
    VIO_FEATURE_GEOMETRY_INSTANCING => 0,
    VIO_FEATURE_HLSL_STAGE_OVERRIDE => 0,
    VIO_FEATURE_SUBGROUP           => 0,
    VIO_FEATURE_SUBGROUP_QUAD      => 0,
    VIO_FEATURE_BARYCENTRICS       => 0,
    VIO_FEATURE_ATOMIC64           => 0,
    VIO_FEATURE_SHADER_FLOAT16     => 0,
    VIO_FEATURE_BASE_VERTEX        => 0,
    VIO_FEATURE_COMPUTE_DERIVATIVES => 0,
    VIO_FEATURE_SHADING_RATE_PRIMITIVE => 0,
    VIO_FEATURE_SHADING_RATE_IMAGE => 0,
    VIO_FEATURE_BINDLESS           => 0,
    VIO_FEATURE_RAY_QUERY          => 0,
    VIO_FEATURE_MESH_SHADER        => 0,
    VIO_FEATURE_SAMPLER_FEEDBACK   => 0,
    VIO_FEATURE_COOPERATIVE_MATRIX => 0,
    VIO_FEATURE_WORK_GRAPHS        => 0,
    VIO_FEATURE_RENDER_TARGET_DEPTH_SAMPLE => 0,
    VIO_FEATURE_RENDER_TARGET_STORAGE => 0,
    VIO_FEATURE_UPSCALER_NATIVE    => 0,
]);

/* D3D11 / D3D12 (Windows) and Vulkan — pinned by D3D-VULKAN-GAP-PLAN.md Phase 0.
 * Each block folds its asserts into one "<name>: %s" line so the --EXPECTF--
 * shape is the same whether the backend is compiled in / can open a device or
 * not (Linux and macOS CI have no D3D; macOS CI has no Vulkan ICD). */
function probe_fold(string $backend_name, array $expected): void {
    $ctx = @vio_create($backend_name, ["width" => 16, "height" => 16, "headless" => true, "vsync" => false]);
    if (!$ctx) {
        echo "$backend_name: skip (unavailable)\n";
        return;
    }
    $bad = [];
    foreach ($expected as $flag => $want) {
        $got = vio_supports_feature($ctx, $flag);
        if ((bool)$got !== (bool)$want) $bad[] = "$flag=" . ($got ? 1 : 0) . " (want " . ($want ? 1 : 0) . ")";
    }
    vio_destroy($ctx);
    /* A failure prints a SECOND line so the single "<name>: %s" expectation
     * cannot swallow it. */
    echo $bad ? "$backend_name: FAIL\n  " . implode("\n  ", $bad) . "\n" : "$backend_name: OK\n";
}

/* Shared by both D3D backends: everything a wired 3D backend must have, plus
 * the honest zeros. Cube targets, mip generation and multisampled targets differ per backend
 * (see the per-backend entries below). */
$d3d_common = [
    VIO_FEATURE_3D_PIPELINE        => 1,
    VIO_FEATURE_COMPUTE            => 1,
    VIO_FEATURE_READ_PIXELS        => 1,
    VIO_FEATURE_INSTANCED_DRAW     => 1,
    VIO_FEATURE_RENDER_TARGET      => 1,
    VIO_FEATURE_RENDER_TARGET_HDR  => 1,
    VIO_FEATURE_RENDER_TARGET_DEPTH=> 1,
    VIO_FEATURE_CUBEMAP            => 1,
    VIO_FEATURE_DEPTH_BIAS         => 1,
    VIO_FEATURE_SCISSOR            => 1,
    VIO_FEATURE_NATIVE_2D_BATCH    => 1,
    VIO_FEATURE_TEXTURE_3D         => 1,
    VIO_FEATURE_VERTEX_STORAGE     => 1,
    VIO_FEATURE_STORAGE_IMAGE      => 1,
    VIO_FEATURE_MRT                => 1,
    VIO_FEATURE_STENCIL            => 1,   /* D24S8 + depth-stencil state (GAP-PHASE5 1) */
    VIO_FEATURE_RENDER_TARGET_MSAA => 1,   /* D3D11: ResolveSubresource (GAP-PLAN 3); D3D12: PSO sample variants (GAP-PHASE5 1) */
    VIO_FEATURE_GPU_TIMESTAMP      => 1,   /* timestamp query ring (GAP-PHASE5 3) */
    VIO_FEATURE_FRAME_LATENCY      => 1,   /* waitable swapchain (GAP-PHASE5 5) */
    VIO_FEATURE_HDR_OUTPUT         => 1,   /* HDR10 backbuffer (GAP-PHASE5 6) */
    VIO_FEATURE_INDIRECT_DRAW      => 1,   /* ExecuteIndirect / Draw*Indirect (GAP-PHASE5 8) */
    VIO_FEATURE_TEXTURE_ARRAY      => 1,   /* Texture2D arrays (GAP-PHASE5 9) */
    VIO_FEATURE_TEXTURE_COMPRESSION_BC => 1, /* BC1-BC7 (GAP-PHASE5 9) */
    VIO_FEATURE_HLSL_STAGE_OVERRIDE => 1, /* 'hlsl' => [stage => src] (GEOMETRY-STAGES-PLAN 3) */
    VIO_FEATURE_RENDER_TARGET_LAYERED => 1, /* RTV / DSV per array slice (GEOMETRY-STAGES-PLAN 1a) */
    VIO_FEATURE_RENDER_TARGET_DEPTH_SAMPLE => 1, /* R24G8 typeless depth + SRV (test 223) */
    /* TESSELLATION / GEOMETRY are not pinned for D3D: the GPU side always has
     * the stages, but the flag also requires a SPIRV-Cross that can emit HLSL
     * for them (vio_hlsl_stage_supported - older Vulkan-SDK builds cannot).
     * tests/render3d/109 + 110 are the contract: flag = 1 => the stage renders. */
    VIO_FEATURE_RAYTRACING         => 0,
    /* MULTIVIEW: D3D11 by instancing wherever the vertex stage can pick the
     * layer (D3D11_OPTIONS3, device dependent: tests 158 / 182); D3D12 below. */
];
probe_fold("d3d11", $d3d_common + [
    VIO_FEATURE_TEXTURE_SWIZZLE    => 0,   /* D3D11 SRVs have no component mapping */
    VIO_FEATURE_SUBGROUP           => 0,   /* wave intrinsics need SM 6 (D3D12 only); D3D11 is FXC 5.0 */
    VIO_FEATURE_RENDER_TARGET_CUBE => 1,   /* GAP-PLAN 2.2 */
    VIO_FEATURE_MIPMAP_GEN         => 1,   /* GenerateMips (GAP-PLAN 2.3) */
    VIO_FEATURE_RENDER_TARGET_STORAGE => 0, /* 'storage' targets: D3D12 / Vulkan only (test 225) */
    VIO_FEATURE_UPSCALER_NATIVE    => 0,   /* native upscalers: D3D12 / Vulkan only (test 226) */
]);
probe_fold("d3d12", $d3d_common + [
    VIO_FEATURE_TEXTURE_SWIZZLE    => 1,   /* Shader4ComponentMapping */
    VIO_FEATURE_RENDER_TARGET_CUBE => 1,   /* GAP-PLAN 2.2 */
    VIO_FEATURE_MIPMAP_GEN         => 1,   /* compute downsample, CPU box filter fallback (GAP-PHASE5 11) */
    VIO_FEATURE_SUBGROUP           => 0,   /* default context is FXC 5.1; SM 6 + WaveOps: test 149 */
    VIO_FEATURE_MULTIVIEW          => 0,   /* view instancing needs SM 6.1: test 158 */
    VIO_FEATURE_RENDER_TARGET_STORAGE => 1, /* ALLOW_UNORDERED_ACCESS attachments (test 225) */
    /* UPSCALER_NATIVE follows the FSR runtime library and the device: tests 226-228. */
]);

/* Vulkan: 3D pipeline since GAP-PHASE5 Block 10 (SPIR-V round trip, frame upload
 * ring, HDR / depth targets); MRT, MSAA and cube targets, cubemaps and mip
 * generation since Block 10b; texture arrays / BC since 10c. SHADING_RATE depends on the
 * device (VK_KHR_fragment_shading_rate) and is exercised by test 122 instead; the
 * geometry / tessellation stages likewise (109, 110, 135). */
probe_fold("vulkan", [
    VIO_FEATURE_3D_PIPELINE        => 1,   /* GAP-PHASE5 10 */
    VIO_FEATURE_STENCIL            => 1,   /* D32S8 / D24S8 attachments */
    VIO_FEATURE_GPU_TIMESTAMP      => 1,   /* vkCmdWriteTimestamp (GAP-PHASE5 3) */
    VIO_FEATURE_INDIRECT_DRAW      => 1,   /* vkCmdDraw(Indexed)Indirect */
    VIO_FEATURE_RENDER_TARGET_HDR  => 1,
    VIO_FEATURE_RENDER_TARGET_DEPTH=> 1,
    VIO_FEATURE_MRT                => 1,   /* GAP-PHASE5 10b */
    VIO_FEATURE_RENDER_TARGET_MSAA => 1,
    VIO_FEATURE_RENDER_TARGET_CUBE => 1,
    VIO_FEATURE_CUBEMAP            => 1,
    VIO_FEATURE_MIPMAP_GEN         => 1,
    VIO_FEATURE_TEXTURE_ARRAY      => 1,   /* GAP-PHASE5 10c */
    VIO_FEATURE_TEXTURE_COMPRESSION_BC => 1,   /* textureCompressionBC (every desktop GPU) */
    VIO_FEATURE_RENDER_TARGET_LAYERED => 1,   /* framebuffer per array layer (GEOMETRY-STAGES-PLAN 1a) */
    VIO_FEATURE_RENDER_TARGET_DEPTH_SAMPLE => 1,   /* SAMPLED depth, DEPTH_STENCIL_READ_ONLY between passes (test 223) */
    VIO_FEATURE_RENDER_TARGET_STORAGE => 1,   /* STORAGE colour images in GENERAL (test 225) */
    VIO_FEATURE_INSTANCED_DRAW     => 1,
    VIO_FEATURE_DEPTH_BIAS         => 1,
    /* TESSELLATION / GEOMETRY follow the device features geometryShader /
     * tessellationShader (MoltenVK has no geometry stage) and are therefore not
     * pinned; tests/render3d/109, 110 and 135 are the contract. */
    VIO_FEATURE_VERTEX_STORAGE     => 1,
    VIO_FEATURE_COMPUTE            => 1,
    VIO_FEATURE_READ_PIXELS        => 1,
    VIO_FEATURE_RENDER_TARGET      => 1,
    VIO_FEATURE_SCISSOR            => 1,
    VIO_FEATURE_TEXTURE_SWIZZLE    => 1,
    VIO_FEATURE_NATIVE_2D_BATCH    => 1,
    VIO_FEATURE_TEXTURE_3D         => 1,
    /* RAYTRACING follows VK_KHR_ray_tracing_pipeline (1 on RTX, 0 on lavapipe): test 164. */
    /* MULTIVIEW is device dependent here (GL_OVR_multiview2 / VkPhysicalDeviceMultiviewFeatures): test 158. */
]);

/* Metal (macOS) — full 3D pipeline + RT + 2D-batch + swizzle */
$mtl = @vio_create("metal", ["width" => 16, "height" => 16, "headless" => true]);
if ($mtl) {
    // Fold the per-feature asserts into one line so the matched output has the
    // same shape whether or not Metal is compiled in (it isn't on Linux /
    // Windows). The --EXPECTF-- `metal: %s` then accepts OK or the skip line.
    $ok = vio_supports_feature($mtl, VIO_FEATURE_3D_PIPELINE) === true
       && vio_supports_feature($mtl, VIO_FEATURE_INSTANCED_DRAW) === true
       && vio_supports_feature($mtl, VIO_FEATURE_CUBEMAP) === true
       && vio_supports_feature($mtl, VIO_FEATURE_RENDER_TARGET_MSAA) === true
       && vio_supports_feature($mtl, VIO_FEATURE_RENDER_TARGET_CUBE) === true
       && vio_supports_feature($mtl, VIO_FEATURE_MIPMAP_GEN) === true
       && vio_supports_feature($mtl, VIO_FEATURE_MRT) === true
       && vio_supports_feature($mtl, VIO_FEATURE_STORAGE_IMAGE) === vio_supports_feature($mtl, VIO_FEATURE_COMPUTE)
       /* tessellation = vertex + control kernels, geometry = vertex + geometry kernels
        * (both need SPIRV-Cross like compute) */
       && vio_supports_feature($mtl, VIO_FEATURE_TESSELLATION) === vio_supports_feature($mtl, VIO_FEATURE_COMPUTE)
       && vio_supports_feature($mtl, VIO_FEATURE_GEOMETRY) === vio_supports_feature($mtl, VIO_FEATURE_COMPUTE)
       && vio_supports_feature($mtl, VIO_FEATURE_GEOMETRY_INSTANCING) === vio_supports_feature($mtl, VIO_FEATURE_GEOMETRY)
       /* Depth32Float_Stencil8 on swapchain / colour targets; 2DArray + depth cubes */
       && vio_supports_feature($mtl, VIO_FEATURE_STENCIL) === true
       && vio_supports_feature($mtl, VIO_FEATURE_RENDER_TARGET_LAYERED) === true
       && vio_supports_feature($mtl, VIO_FEATURE_RENDER_TARGET_DEPTH_SAMPLE) === true
       && vio_supports_feature($mtl, VIO_FEATURE_RENDER_TARGET_STORAGE) === false
       && vio_supports_feature($mtl, VIO_FEATURE_UPSCALER_NATIVE) === false
       /* vertex-stage layer / viewport index travel together (Mac2 / Apple5) */
       && vio_supports_feature($mtl, VIO_FEATURE_LAYERED_RENDER) === vio_supports_feature($mtl, VIO_FEATURE_VERTEX_LAYER)
       && vio_supports_feature($mtl, VIO_FEATURE_MULTI_VIEWPORT) === vio_supports_feature($mtl, VIO_FEATURE_VERTEX_LAYER)
       && vio_supports_feature($mtl, VIO_FEATURE_NATIVE_2D_BATCH) === true
       /* frame-latency semaphore; RGB10A2 layer in the BT.2100 PQ colour space */
       && vio_supports_feature($mtl, VIO_FEATURE_FRAME_LATENCY) === true
       && vio_supports_feature($mtl, VIO_FEATURE_HDR_OUTPUT) === true
       && vio_supports_feature($mtl, VIO_FEATURE_RENDER_TARGET) === true
       && vio_supports_feature($mtl, VIO_FEATURE_TEXTURE_SWIZZLE) === true;
    vio_destroy($mtl);
    echo $ok ? "metal: OK\n" : "metal: FAIL\n";
} else {
    echo "metal: skip (unavailable)\n";
}

echo "DONE\n";
?>
--EXPECTF--
opengl: OK
null: OK
d3d11: %s
d3d12: %s
vulkan: %s
metal: %s
DONE
