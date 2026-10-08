--TEST--
Shader Execution Reordering (VIO_FEATURE_SHADER_EXECUTION_REORDER, Shader Model 6.9 / DXR 1.2, SM69-PLAN Phase 3): a raygen traces through a hit object, reorders by a hit / miss hint, queries the hit object and then runs its closest-hit / miss shader - the same picture as an ordinary trace; Vulkan with GL_EXT_shader_invocation_reorder, D3D12 with dx::HitObject in the lib_6_9 library
--EXTENSIONS--
vio
--FILE--
<?php
/* The scene of test 164: an occluder quad at z = 0.5 over x in [-1, 0], one
 * ray per launch pixel toward +z. The raygen
 *   - traces into a hit object (no shader runs yet),
 *   - reorders the threads by whether the ray hit (the hint must not change
 *     any result),
 *   - asks the hit object itself (IsHit: +1000, without a closest-hit shader),
 *   - runs the hit object's shader: closest hit 100 + round(10 t) = 105, miss 0.
 * A: occluder in place, left half 1105, right half 0; B: moved by +1 in x,
 * mirrored. VIO_REQUIRE_SHADER_EXECUTION_REORDER makes backends mandatory. */
$W = 16; $H = 4;
$HDR = "#version 460\n#extension GL_EXT_ray_tracing : require\n";
$RGEN = $HDR . "#extension GL_EXT_shader_invocation_reorder : require\n"
    . "layout(binding = 0) uniform accelerationStructureEXT u_tlas;\n"
    . "layout(std430, binding = 1) buffer B { uint v[]; } b;\n"
    . "layout(location = 0) rayPayloadEXT uint hit;\n"
    . "void main(){ uvec3 id = gl_LaunchIDEXT; uvec3 n = gl_LaunchSizeEXT;\n"
    . "  vec2 uv = (vec2(id.xy) + 0.5) / vec2(n.xy) * 2.0 - 1.0;\n"
    . "  hitObjectEXT h;\n"
    . "  hitObjectTraceRayEXT(h, u_tlas, gl_RayFlagsOpaqueEXT, 0xFF, 0, 0, 0, vec3(uv, 0.0), 0.001, vec3(0.0, 0.0, 1.0), 100.0, 0);\n"
    . "  reorderThreadEXT(h, hitObjectIsHitEXT(h) ? 1u : 0u, 1u);\n"
    . "  uint q = hitObjectIsHitEXT(h) ? 1000u : 0u;\n"
    . "  hit = 7u;\n"
    . "  hitObjectExecuteShaderEXT(h, 0);\n"
    . "  b.v[(id.z * n.y + id.y) * n.x + id.x] = hit + q; }";
$MISS = $HDR . "layout(location = 0) rayPayloadInEXT uint hit;\nvoid main(){ hit = 0u; }";
$CHIT = $HDR . "layout(location = 0) rayPayloadInEXT uint hit;\nhitAttributeEXT vec2 bary;\n"
      . "void main(){ hit = 100u + uint(gl_HitTEXT * 10.0 + 0.5); }";
$HLSL = <<<'HLSL'
RaytracingAccelerationStructure u_tlas : register(t0);
RWStructuredBuffer<uint> b : register(u1);
struct Payload { uint hit; };
[shader("raygeneration")] void vio_raygen() {
    uint3 id = DispatchRaysIndex(); uint3 n = DispatchRaysDimensions();
    float2 uv = (float2(id.xy) + 0.5) / float2(n.xy) * 2.0 - 1.0;
    RayDesc r; r.Origin = float3(uv, 0.0); r.Direction = float3(0.0, 0.0, 1.0); r.TMin = 0.001; r.TMax = 100.0;
    Payload p; p.hit = 7;
    dx::HitObject h = dx::HitObject::TraceRay(u_tlas, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 0, 0, r, p);
    dx::MaybeReorderThread(h, h.IsHit() ? 1 : 0, 1);
    uint q = h.IsHit() ? 1000 : 0;
    dx::HitObject::Invoke(h, p);
    b[(id.z * n.y + id.y) * n.x + id.x] = p.hit + q;
}
[shader("miss")] void vio_miss(inout Payload p) { p.hit = 0; }
[shader("closesthit")] void vio_closest_hit(inout Payload p, in BuiltInTriangleIntersectionAttributes a) {
    p.hit = 100 + (uint)(RayTCurrent() * 10.0 + 0.5);
}
HLSL;

$opts = ["width" => 16, "height" => 16, "headless" => true, "vsync" => false, "shader_model" => 6];
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_SHADER_EXECUTION_REORDER') ?: ''));

function translate_x(float $tx): array { return [1,0,0,0, 0,1,0,0, 0,0,1,0, $tx,0,0,1]; }
function halves(string $bytes, int $w, int $h): array {
    $v = array_values(unpack('V*', $bytes));
    $l = []; $r = [];
    for ($y = 0; $y < $h; $y++) for ($x = 0; $x < $w; $x++) {
        $k = $v[$y * $w + $x];
        if ($x < $w / 2) $l[$k] = 1; else $r[$k] = 1;
    }
    ksort($l); ksort($r);
    return [array_keys($l), array_keys($r)];
}

function run_backend(string $name): string {
    global $W, $H, $RGEN, $MISS, $CHIT, $HLSL, $opts, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    $flag = vio_supports_feature($ctx, VIO_FEATURE_SHADER_EXECUTION_REORDER);
    if ($flag && !vio_supports_feature($ctx, VIO_FEATURE_RAYTRACING)) { vio_destroy($ctx); return "FAIL\n  reorder without a ray tracing pipeline"; }
    if (!$flag) { vio_destroy($ctx); return $req ? "FAIL\n  required but VIO_FEATURE_SHADER_EXECUTION_REORDER is 0" : "skip (no shader execution reordering)"; }
    $fail = [];
    $occ = vio_mesh($ctx, ['vertices' => [-1,-1,0.5, 0,-1,0.5, 0,1,0.5, -1,1,0.5], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $asA = vio_acceleration_structure($ctx, [['mesh' => $occ]]);
    $asB = vio_acceleration_structure($ctx, [['mesh' => $occ, 'transform' => translate_x(1.0)]]);
    $p = vio_rt_pipeline($ctx, ['raygen' => $RGEN, 'miss' => $MISS, 'closest_hit' => $CHIT, 'hlsl' => $HLSL]);
    if (!$asA || !$asB || !$p) { vio_destroy($ctx); return "FAIL\n  acceleration structure or reorder pipeline not created"; }
    foreach ([['A', $asA, [1105], [0]], ['B', $asB, [0], [1105]]] as [$tag, $as, $wl, $wr]) {
        $buf = vio_storage_buffer($ctx, ['size' => $W * $H * 4]);
        vio_rt_bind_buffer($ctx, $p, $buf, 1);
        vio_bind_acceleration_structure($ctx, $as, 0);
        vio_trace_rays($ctx, $p, $W, $H, 1);
        [$l, $r] = halves(vio_storage_buffer_read($ctx, $buf), $W, $H);
        if ($l !== $wl || $r !== $wr) $fail[] = "$tag: halves " . json_encode([$l, $r]) . ", want " . json_encode([$wl, $wr]);
    }
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) echo "$b: ", run_backend($b), "\n";
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE
