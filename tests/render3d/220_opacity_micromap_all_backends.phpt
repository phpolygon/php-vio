--TEST--
Opacity micromaps (VIO_FEATURE_OPACITY_MICROMAP, DXR 1.2 / VK_EXT_opacity_micromap, SM69-PLAN Phase 4): vio_acceleration_structure(['opacity_micromap' => ...]) makes transparent triangles let rays through without an any-hit shader, unknown ones run it, and RAY_FLAG_FORCE_OMM_2_STATE resolves unknown states without it; malformed maps throw
--EXTENSIONS--
vio
--FILE--
<?php
/* The occluder of test 164 (x in [-1, 0], z = 0.5) split along its diagonal:
 * triangle 0 (v0 v1 v2) lies below the line y = 2x + 1, triangle 1 above it.
 * Subdivision 0: one state per triangle, independent of the micro-triangle
 * order. Rays go toward +z without RAY_FLAG_FORCE_OPAQUE (which would ignore
 * the map); a hit writes 100 + round(10 t) = 105, a miss 0.
 *   A. 2-state, triangle 0 opaque, triangle 1 transparent, no any-hit:
 *      105 inside triangle 0 only.
 *   B. 4-state, unknown-opaque / unknown-transparent, an any-hit that ignores
 *      every intersection: unknown runs it - nothing is hit.
 *   C. B with RAY_FLAG_FORCE_OMM_2_STATE: unknown-opaque counts as opaque,
 *      unknown-transparent as transparent, no any-hit - as A.
 *   D. A as an inline ray query in a compute kernel (no any-hit stage there:
 *      only the opaque triangle is committed). D3D12 has to declare the query
 *      with RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS, or it ignores the map.
 * VIO_REQUIRE_OPACITY_MICROMAP makes backends mandatory. */
$W = 16; $H = 4;
$HDR = "#version 460\n#extension GL_EXT_ray_tracing : require\n#extension GL_EXT_opacity_micromap : require\n";
$RGEN = fn(string $flags) => $HDR
    . "layout(binding = 0) uniform accelerationStructureEXT u_tlas;\n"
    . "layout(std430, binding = 1) buffer B { uint v[]; } b;\n"
    . "layout(location = 0) rayPayloadEXT uint hit;\n"
    . "void main(){ uvec3 id = gl_LaunchIDEXT; uvec3 n = gl_LaunchSizeEXT;\n"
    . "  vec2 uv = (vec2(id.xy) + 0.5) / vec2(n.xy) * 2.0 - 1.0;\n"
    . "  hit = 7u;\n"
    . "  traceRayEXT(u_tlas, $flags, 0xFF, 0, 0, 0, vec3(uv, 0.0), 0.001, vec3(0.0, 0.0, 1.0), 100.0, 0);\n"
    . "  b.v[id.y * n.x + id.x] = hit; }";
$MISS = $HDR . "layout(location = 0) rayPayloadInEXT uint hit;\nvoid main(){ hit = 0u; }";
$CHIT = $HDR . "layout(location = 0) rayPayloadInEXT uint hit;\nhitAttributeEXT vec2 bary;\n"
      . "void main(){ hit = 100u + uint(gl_HitTEXT * 10.0 + 0.5); }";
$AHIT = $HDR . "layout(location = 0) rayPayloadInEXT uint hit;\nhitAttributeEXT vec2 bary;\nvoid main(){ ignoreIntersectionEXT; }";
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
    b[id.y * n.x + id.x] = p.hit;
}
[shader("miss")] void vio_miss(inout Payload p) { p.hit = 0; }
[shader("closesthit")] void vio_closest_hit(inout Payload p, in BuiltInTriangleIntersectionAttributes a) {
    p.hit = 100 + (uint)(RayTCurrent() * 10.0 + 0.5);
}
[shader("anyhit")] void vio_any_hit(inout Payload p, in BuiltInTriangleIntersectionAttributes a) { IgnoreHit(); }
HLSL;

$CS = "#version 460\n#extension GL_EXT_ray_query : require\nlayout(local_size_x = 64) in;\n"
    . "layout(binding = 0) uniform accelerationStructureEXT u_tlas;\n"
    . "layout(std430, binding = 1) buffer B { uint v[]; } b;\n"
    . "void main(){ uint i = gl_GlobalInvocationID.x; uvec2 id = uvec2(i % 16u, i / 16u);\n"
    . "  vec2 uv = (vec2(id) + 0.5) / vec2(16.0, 4.0) * 2.0 - 1.0; rayQueryEXT q;\n"
    . "  rayQueryInitializeEXT(q, u_tlas, gl_RayFlagsNoneEXT, 0xFF, vec3(uv, 0.0), 0.001, vec3(0.0, 0.0, 1.0), 100.0);\n"
    . "  while (rayQueryProceedEXT(q)) {}\n"
    . "  b.v[i] = rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT ? 105u : 0u; }";
$opts = ["width" => 16, "height" => 16, "headless" => true, "vsync" => false, "shader_model" => 6];
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_OPACITY_MICROMAP') ?: ''));

/* 105 where the ray meets an opaque triangle of the given set, else 0 */
function expect(int $w, int $h, array $opaque): array {
    $out = [];
    for ($y = 0; $y < $h; $y++) for ($x = 0; $x < $w; $x++) {
        $u = ($x + 0.5) / $w * 2 - 1; $v = ($y + 0.5) / $h * 2 - 1;
        $tri = $u >= 0 ? -1 : ($v <= 2 * $u + 1 ? 0 : 1);
        $out[] = ($tri >= 0 && in_array($tri, $opaque, true)) ? 105 : 0;
    }
    return $out;
}

function run_backend(string $name): string {
    global $W, $H, $RGEN, $MISS, $CHIT, $AHIT, $HLSL, $CS, $opts, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    $flag = vio_supports_feature($ctx, VIO_FEATURE_OPACITY_MICROMAP);
    $rt = vio_supports_feature($ctx, VIO_FEATURE_RAYTRACING);
    if ($flag && !$rt) { vio_destroy($ctx); return "FAIL\n  opacity micromaps without a ray tracing pipeline"; }
    if (!$rt) { vio_destroy($ctx); return $req ? "FAIL\n  required but no ray tracing pipeline" : "skip (no ray tracing pipeline)"; }
    $fail = [];
    $occ = vio_mesh($ctx, ['vertices' => [-1,-1,0.5, 0,-1,0.5, 0,1,0.5, -1,1,0.5], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    /* the argument contract holds with and without the flag */
    foreach ([['format' => 3, 'states' => "\1\0"], ['states' => "\1"], ['format' => 2, 'states' => "\1\2"], ['subdivision' => 13, 'states' => "\1\0"]] as $k => $bad) {
        try { @vio_acceleration_structure($ctx, [['mesh' => $occ, 'opacity_micromap' => $bad]]); $fail[] = "bad map $k accepted"; }
        catch (ValueError $e) {}
    }
    if (!$flag) {
        $r = @vio_acceleration_structure($ctx, [['mesh' => $occ, 'opacity_micromap' => ['format' => 2, 'states' => "\1\0"]]]);
        vio_destroy($ctx);
        if ($r !== false) $fail[] = "a map was accepted without VIO_FEATURE_OPACITY_MICROMAP";
        if ($fail) return "FAIL\n  " . implode("\n  ", $fail);
        return $req ? "FAIL\n  required but VIO_FEATURE_OPACITY_MICROMAP is 0" : "skip (no opacity micromaps)";
    }
    $as2 = vio_acceleration_structure($ctx, [['mesh' => $occ, 'opacity_micromap' => ['subdivision' => 0, 'format' => 2, 'states' => "\1\0"]]]);
    $as4 = vio_acceleration_structure($ctx, [['mesh' => $occ, 'opacity_micromap' => ['subdivision' => 0, 'format' => 4, 'states' => "\3\2"]]]);
    $plain = vio_rt_pipeline($ctx, ['raygen' => $RGEN('gl_RayFlagsNoneEXT'), 'miss' => $MISS, 'closest_hit' => $CHIT,
                                    'hlsl' => $HLSL('RAY_FLAG_NONE')]);
    $ahit = vio_rt_pipeline($ctx, ['raygen' => $RGEN('gl_RayFlagsNoneEXT'), 'miss' => $MISS, 'closest_hit' => $CHIT, 'any_hit' => $AHIT,
                                   'hlsl' => $HLSL('RAY_FLAG_NONE')]);
    $force = vio_rt_pipeline($ctx, ['raygen' => $RGEN('gl_RayFlagsForceOpacityMicromap2StateEXT'), 'miss' => $MISS, 'closest_hit' => $CHIT,
                                    'any_hit' => $AHIT, 'hlsl' => $HLSL('RAY_FLAG_FORCE_OMM_2_STATE')]);
    if (!$as2 || !$as4 || !$plain || !$ahit || !$force) { vio_destroy($ctx); return "FAIL\n  structures or pipelines not created"; }
    foreach ([['A', $plain, $as2, [0]], ['B', $ahit, $as4, []], ['C', $force, $as4, [0]]] as [$tag, $p, $as, $opaque]) {
        $buf = vio_storage_buffer($ctx, ['size' => $W * $H * 4]);
        vio_rt_bind_buffer($ctx, $p, $buf, 1);
        vio_bind_acceleration_structure($ctx, $as, 0);
        vio_trace_rays($ctx, $p, $W, $H, 1);
        $got = array_values(unpack('V*', vio_storage_buffer_read($ctx, $buf)));
        $want = expect($W, $H, $opaque);
        if ($got !== $want) {
            $rows = [];
            for ($y = 0; $y < $H; $y++) $rows[] = implode('', array_map(fn($v) => $v === 105 ? '#' : ($v === 0 ? '.' : '?'), array_slice($got, $y * $W, $W)));
            $want_rows = [];
            for ($y = 0; $y < $H; $y++) $want_rows[] = implode('', array_map(fn($v) => $v === 105 ? '#' : '.', array_slice($want, $y * $W, $W)));
            $fail[] = "$tag: got " . implode('|', $rows) . ", want " . implode('|', $want_rows);
        }
    }
    if (vio_supports_feature($ctx, VIO_FEATURE_RAY_QUERY) && vio_supports_feature($ctx, VIO_FEATURE_COMPUTE)) {
        $cp = vio_compute_pipeline($ctx, ['source' => $CS]);
        if (!$cp) $fail[] = "D: compute pipeline not created";
        else {
            $buf = vio_storage_buffer($ctx, ['size' => $W * $H * 4]);
            vio_compute_bind_buffer($ctx, $cp, $buf, 1, VIO_COMPUTE_WRITE);
            vio_bind_acceleration_structure($ctx, $as2, 0);
            vio_compute_dispatch($ctx, $cp, 1, 1, 1);
            $got = array_values(unpack('V*', vio_storage_buffer_read($ctx, $buf)));
            if ($got !== expect($W, $H, [0])) $fail[] = "D: ray query hits " . implode('', array_map(fn($v) => $v === 105 ? '#' : '.', $got));
        }
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
