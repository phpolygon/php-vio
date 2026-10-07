--TEST--
Ray tracing pipeline (VIO_FEATURE_RAYTRACING): vio_rt_pipeline (raygen / miss / closest-hit, optional any-hit) traces against a vio_acceleration_structure; vio_trace_rays writes through vio_rt_bind_buffer into a storage buffer; refused where the flag is 0
--EXTENSIONS--
vio
--FILE--
<?php
/* An occluder quad at z = 0.5 covers x in [-1, 0]. One ray per launch pixel
 * (16 x 4) from (x, y, 0) toward +z; the raygen stores the payload per pixel:
 *   miss 0, closest hit 100 + round(10 * gl_HitTEXT) = 105, untouched 7.
 *   A. one instance: the left half 105, the right half 0
 *   B. the occluder moved by +1 in x: the right half 105, the left half 0
 *   C. an any-hit shader that ignores every intersection, rays forced
 *      non-opaque: everything 0 (the any-hit stage runs)
 *   D. vio_trace_rays with a depth of 2 covers the second slice too
 *   E. the contract: vio_rt_pipeline refused (false + warning) where the flag is 0
 * Vulkan takes the GLSL stages (GL_EXT_ray_tracing); D3D12 the 'hlsl' library
 * (DXR 1.0, DXC lib_6_3: exports vio_raygen / vio_miss / vio_closest_hit /
 * vio_any_hit, TLAS t0, buffers u<binding>). VIO_REQUIRE_RAYTRACING makes the
 * listed backends mandatory. */
$W = 16; $H = 4;
$HDR = "#version 460\n#extension GL_EXT_ray_tracing : require\n";
$RGEN = fn(string $flags) => $HDR
    . "layout(binding = 0) uniform accelerationStructureEXT u_tlas;\n"
    . "layout(std430, binding = 1) buffer B { uint v[]; } b;\n"
    . "layout(location = 0) rayPayloadEXT uint hit;\n"
    . "void main(){ uvec3 id = gl_LaunchIDEXT; uvec3 n = gl_LaunchSizeEXT;\n"
    . "  vec2 uv = (vec2(id.xy) + 0.5) / vec2(n.xy) * 2.0 - 1.0;\n"
    . "  hit = 7u;\n"
    . "  traceRayEXT(u_tlas, $flags, 0xFF, 0, 0, 0, vec3(uv, 0.0), 0.001, vec3(0.0, 0.0, 1.0), 100.0, 0);\n"
    . "  b.v[(id.z * n.y + id.y) * n.x + id.x] = hit; }";
$MISS = $HDR . "layout(location = 0) rayPayloadInEXT uint hit;\nvoid main(){ hit = 0u; }";
$CHIT = $HDR . "layout(location = 0) rayPayloadInEXT uint hit;\nhitAttributeEXT vec2 bary;\n"
      . "void main(){ hit = 100u + uint(gl_HitTEXT * 10.0 + 0.5); }";
$AHIT = $HDR . "layout(location = 0) rayPayloadInEXT uint hit;\nhitAttributeEXT vec2 bary;\n"
      . "void main(){ ignoreIntersectionEXT; }";
$HLSL = fn(string $flags) => <<<HLSL
RaytracingAccelerationStructure u_tlas : register(t0);
RWStructuredBuffer<uint> b : register(u1);
struct Payload { uint hit; };
[shader("raygeneration")] void vio_raygen() {
    uint3 id = DispatchRaysIndex(); uint3 n = DispatchRaysDimensions();
    float2 uv = (float2(id.xy) + 0.5) / float2(n.xy) * 2.0 - 1.0;
    RayDesc r; r.Origin = float3(uv, 0.0); r.Direction = float3(0.0, 0.0, 1.0); r.TMin = 0.001; r.TMax = 100.0;
    Payload p; p.hit = 7;
    TraceRay(u_tlas, $flags, 0xFF, 0, 0, 0, r, p);
    b[(id.z * n.y + id.y) * n.x + id.x] = p.hit;
}
[shader("miss")] void vio_miss(inout Payload p) { p.hit = 0; }
[shader("closesthit")] void vio_closest_hit(inout Payload p, in BuiltInTriangleIntersectionAttributes a) {
    p.hit = 100 + (uint)(RayTCurrent() * 10.0 + 0.5);
}
[shader("anyhit")] void vio_any_hit(inout Payload p, in BuiltInTriangleIntersectionAttributes a) { IgnoreHit(); }
HLSL;

$opts = ["width" => 16, "height" => 16, "headless" => true, "vsync" => false, "shader_model" => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_RAYTRACING') ?: ''));

function translate_x(float $tx): array { return [1,0,0,0, 0,1,0,0, 0,0,1,0, $tx,0,0,1]; }
/* [left half, right half] of slice $z: the distinct values found there. */
function halves(string $bytes, int $w, int $h, int $z): array {
    $v = array_values(unpack('V*', $bytes));
    $l = []; $r = [];
    for ($y = 0; $y < $h; $y++) for ($x = 0; $x < $w; $x++) {
        $k = $v[($z * $h + $y) * $w + $x];
        if ($x < $w / 2) $l[$k] = 1; else $r[$k] = 1;
    }
    ksort($l); ksort($r);
    return [array_keys($l), array_keys($r)];
}

function run_backend(string $name): string {
    global $W, $H, $RGEN, $MISS, $CHIT, $AHIT, $HLSL, $opts, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    $opaque = ['raygen' => $RGEN('gl_RayFlagsOpaqueEXT'), 'miss' => $MISS, 'closest_hit' => $CHIT,
               'hlsl' => $HLSL('RAY_FLAG_FORCE_OPAQUE')];
    if (!vio_supports_feature($ctx, VIO_FEATURE_RAYTRACING)) {
        $p = @vio_rt_pipeline($ctx, $opaque);
        vio_destroy($ctx);
        if ($p !== false) return "FAIL\n  E: vio_rt_pipeline succeeded without VIO_FEATURE_RAYTRACING";
        return $req ? "FAIL\n  required but VIO_FEATURE_RAYTRACING is 0" : "skip (no ray tracing pipeline)";
    }
    $fail = [];
    $occ = vio_mesh($ctx, ['vertices' => [-1,-1,0.5, 0,-1,0.5, 0,1,0.5, -1,1,0.5], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $asA = vio_acceleration_structure($ctx, [['mesh' => $occ]]);
    $asB = vio_acceleration_structure($ctx, [['mesh' => $occ, 'transform' => translate_x(1.0)]]);
    if (!$asA || !$asB) { vio_destroy($ctx); return "FAIL\n  acceleration structure not built"; }
    $p = vio_rt_pipeline($ctx, $opaque);
    $pa = vio_rt_pipeline($ctx, ['raygen' => $RGEN('gl_RayFlagsNoOpaqueEXT'), 'miss' => $MISS, 'closest_hit' => $CHIT,
                                 'any_hit' => $AHIT, 'hlsl' => $HLSL('RAY_FLAG_FORCE_NON_OPAQUE')]);
    if (!$p || !$pa) { vio_destroy($ctx); return "FAIL\n  ray tracing pipeline not created"; }
    if (!($p instanceof VioRtPipeline)) $fail[] = "result is no VioRtPipeline";
    $trace = function ($pipe, $as, int $d = 1) use ($ctx, $W, $H): string {
        $buf = vio_storage_buffer($ctx, ['size' => $W * $H * $d * 4]);
        vio_rt_bind_buffer($ctx, $pipe, $buf, 1);
        vio_bind_acceleration_structure($ctx, $as, 0);
        vio_trace_rays($ctx, $pipe, $W, $H, $d);
        return vio_storage_buffer_read($ctx, $buf);
    };
    foreach ([['A', $p, $asA, [105], [0]], ['B', $p, $asB, [0], [105]], ['C', $pa, $asA, [0], [0]]] as [$tag, $pipe, $as, $wl, $wr]) {
        [$l, $r] = halves($trace($pipe, $as), $W, $H, 0);
        if ($l !== $wl || $r !== $wr) $fail[] = "$tag: halves " . json_encode([$l, $r]) . ", want " . json_encode([$wl, $wr]);
    }
    $bytes = $trace($p, $asA, 2);
    foreach ([0, 1] as $z) {
        [$l, $r] = halves($bytes, $W, $H, $z);
        if ($l !== [105] || $r !== [0]) $fail[] = "D: slice $z halves " . json_encode([$l, $r]);
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
