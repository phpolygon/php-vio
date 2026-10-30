--TEST--
vio_opacity_micromap_bake: an alpha texture becomes micro-triangle states in the GPU's curve order - rays through a subdivided 2-state map reproduce an asymmetric mask at its place, 4-state known states are never wrong and an any-hit shader settles the rest, malformed input throws
--EXTENSIONS--
vio
--FILE--
<?php
/* The mask is a 64x64 "L" with a hole: asymmetric, so a wrong micro-triangle
 * order, a swapped barycentric or a flipped v lands on the wrong rays. Quad
 * x, y in [-1, 1] at z = 0.5, uv = (x + 1) / 2, (y + 1) / 2. Rays toward +z
 * write 105 on a hit, 0 on a miss.
 *   A. CPU: uniform alpha, sizes, state counts, deterministic bytes, indices.
 *   B. 2-state, subdivision 6, no any-hit: the image equals the mask but for
 *      a thin edge band.
 *   C. 4-state, subdivision 6, any-hit ignores: only known-opaque stays - each
 *      hit lies inside the mask, and the mask's interior is hit.
 *   D. as C, any-hit accepts: known-transparent stays empty, every other
 *      covered ray is a hit.
 * VIO_REQUIRE_OPACITY_MICROMAP makes backends mandatory. */
const N = 64;
function mask(float $u, float $v): bool {
    $x = $u * 8; $y = $v * 8;
    if ($x >= 1 && $x < 3 && $y >= 1 && $y < 7) return !($x >= 1.5 && $x < 2.5 && $y >= 4 && $y < 5.5);   // long bar with a hole
    return $x >= 1 && $x < 7 && $y >= 1 && $y < 3;                                                         // short foot
}
$rgba = '';
for ($y = 0; $y < N; $y++) for ($x = 0; $x < N; $x++)
    $rgba .= "\xFF\xFF\xFF" . (mask(($x + 0.5) / N, ($y + 0.5) / N) ? "\xFF" : "\x00");
$uvs = [0, 0, 1, 0, 1, 1, 0, 1];
$idx = [0, 1, 2, 0, 2, 3];
$bake = fn(int $fmt, int $sub) => vio_opacity_micromap_bake(['uvs' => $uvs, 'indices' => $idx, 'data' => $rgba, 'width' => N, 'height' => N,
                                                             'subdivision' => $sub, 'format' => $fmt]);

/* A */
$ok = [];
$b = $bake(4, 3);
$ok[] = strlen($b['states']) === 2 * 64 && array_sum($b['stats']) === 2 * 64 && $b['subdivision'] === 3 && $b['format'] === 4;
$ok[] = $bake(4, 3)['states'] === $b['states'];
$full = vio_opacity_micromap_bake(['uvs' => [0, 0, 1, 0, 0, 1], 'data' => str_repeat("\xFF", 16), 'width' => 4, 'height' => 4, 'channels' => 1, 'subdivision' => 2]);
$none = vio_opacity_micromap_bake(['uvs' => [0, 0, 1, 0, 0, 1], 'data' => str_repeat("\x00", 16), 'width' => 4, 'height' => 4, 'channels' => 1, 'subdivision' => 2, 'format' => 2]);
$ok[] = $full['states'] === str_repeat("\1", 16) && $none['states'] === str_repeat("\0", 16);
echo "cpu: ", in_array(false, $ok, true) ? "FAIL " . json_encode($ok) : "OK", "\n";
foreach ([['data' => 'x'], ['width' => 0], ['channels' => 3], ['subdivision' => 13], ['format' => 3], ['threshold' => 2], ['uvs' => [0, 0, 1]],
          ['indices' => [0, 1]], ['indices' => [0, 1, 9]], ['indices' => 7], ['subdivision' => 12, 'indices' => array_fill(0, 96, 0)]] as $i => $bad) {
    try { vio_opacity_micromap_bake($bad + ['uvs' => $uvs, 'indices' => $idx, 'data' => $rgba, 'width' => N, 'height' => N]); echo "bad $i accepted\n"; }
    catch (ValueError $e) {}
}

$HDR = "#version 460\n#extension GL_EXT_ray_tracing : require\n";
$RGEN = $HDR . "layout(binding = 0) uniform accelerationStructureEXT u_tlas;\n"
    . "layout(std430, binding = 1) buffer B { uint v[]; } b;\n"
    . "layout(location = 0) rayPayloadEXT uint hit;\n"
    . "void main(){ uvec3 id = gl_LaunchIDEXT; uvec3 n = gl_LaunchSizeEXT;\n"
    . "  vec2 uv = (vec2(id.xy) + 0.5) / vec2(n.xy) * 2.0 - 1.0;\n"
    . "  hit = 7u;\n"
    . "  traceRayEXT(u_tlas, gl_RayFlagsNoneEXT, 0xFF, 0, 0, 0, vec3(uv, 0.0), 0.001, vec3(0.0, 0.0, 1.0), 100.0, 0);\n"
    . "  b.v[id.y * n.x + id.x] = hit; }";
$MISS = $HDR . "layout(location = 0) rayPayloadInEXT uint hit;\nvoid main(){ hit = 0u; }";
$CHIT = $HDR . "layout(location = 0) rayPayloadInEXT uint hit;\nhitAttributeEXT vec2 bary;\nvoid main(){ hit = 105u; }";
$AHIT_IGNORE = $HDR . "layout(location = 0) rayPayloadInEXT uint hit;\nhitAttributeEXT vec2 bary;\nvoid main(){ ignoreIntersectionEXT; }";
$AHIT_ACCEPT = $HDR . "layout(location = 0) rayPayloadInEXT uint hit;\nhitAttributeEXT vec2 bary;\nvoid main(){ }";
$HLSL = fn(string $ahit) => <<<HLSL
RaytracingAccelerationStructure u_tlas : register(t0);
RWStructuredBuffer<uint> b : register(u1);
struct Payload { uint hit; };
[shader("raygeneration")] void vio_raygen() {
    uint3 id = DispatchRaysIndex(); uint3 n = DispatchRaysDimensions();
    float2 uv = (float2(id.xy) + 0.5) / float2(n.xy) * 2.0 - 1.0;
    RayDesc r; r.Origin = float3(uv, 0.0); r.Direction = float3(0.0, 0.0, 1.0); r.TMin = 0.001; r.TMax = 100.0;
    Payload p; p.hit = 7;
    TraceRay(u_tlas, RAY_FLAG_NONE, 0xFF, 0, 0, 0, r, p);
    b[id.y * n.x + id.x] = p.hit;
}
[shader("miss")] void vio_miss(inout Payload p) { p.hit = 0; }
[shader("closesthit")] void vio_closest_hit(inout Payload p, in BuiltInTriangleIntersectionAttributes a) { p.hit = 105; }
[shader("anyhit")] void vio_any_hit(inout Payload p, in BuiltInTriangleIntersectionAttributes a) { $ahit }
HLSL;
$opts = ["width" => 16, "height" => 16, "headless" => true, "vsync" => false, "shader_model" => 6];
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_OPACITY_MICROMAP') ?: ''));

function trace($ctx, $p, $as): array {
    $buf = vio_storage_buffer($ctx, ['size' => N * N * 4]);
    vio_rt_bind_buffer($ctx, $p, $buf, 1);
    vio_bind_acceleration_structure($ctx, $as, 0);
    vio_trace_rays($ctx, $p, N, N, 1);
    return array_values(unpack('V*', vio_storage_buffer_read($ctx, $buf)));
}

function run_backend(string $name): string {
    global $opts, $require, $bake, $RGEN, $MISS, $CHIT, $AHIT_IGNORE, $AHIT_ACCEPT, $HLSL;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_OPACITY_MICROMAP)) { vio_destroy($ctx); return $req ? "FAIL\n  required but no opacity micromaps" : "skip (no opacity micromaps)"; }
    $fail = [];
    $occ = vio_mesh($ctx, ['vertices' => [-1,-1,0.5, 1,-1,0.5, 1,1,0.5, -1,1,0.5], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $as2 = vio_acceleration_structure($ctx, [['mesh' => $occ, 'opacity_micromap' => $bake(2, 6)]]);
    $as4 = vio_acceleration_structure($ctx, [['mesh' => $occ, 'opacity_micromap' => $bake(4, 6)]]);
    $plain  = vio_rt_pipeline($ctx, ['raygen' => $RGEN, 'miss' => $MISS, 'closest_hit' => $CHIT, 'hlsl' => $HLSL('')]);
    $ignore = vio_rt_pipeline($ctx, ['raygen' => $RGEN, 'miss' => $MISS, 'closest_hit' => $CHIT, 'any_hit' => $AHIT_IGNORE, 'hlsl' => $HLSL('IgnoreHit();')]);
    $accept = vio_rt_pipeline($ctx, ['raygen' => $RGEN, 'miss' => $MISS, 'closest_hit' => $CHIT, 'any_hit' => $AHIT_ACCEPT, 'hlsl' => $HLSL('')]);
    if (!$as2 || !$as4 || !$plain || !$ignore || !$accept) { vio_destroy($ctx); return "FAIL\n  structures or pipelines not created"; }

    /* A ray at (i + 0.5) / N; "deep" = the mask agrees over a cell around it. */
    $deep = function (int $x, int $y, bool $want): bool {
        for ($dy = -2; $dy <= 2; $dy++) for ($dx = -2; $dx <= 2; $dx++)
            if (mask(($x + 0.5 + $dx) / N, ($y + 0.5 + $dy) / N) !== $want) return false;
        return true;
    };
    $render = function (array $got): string {
        $o = '';
        for ($y = N - 1; $y >= 0; $y -= 4) { $o .= "\n  "; for ($x = 0; $x < N; $x += 2) { $v = $got[$y * N + $x]; $o .= $v === 105 ? '#' : ($v === 0 ? '.' : '?'); } }
        return $o;
    };
    $count = function (array $got, callable $bad) { $n = 0; for ($y = 0; $y < N; $y++) for ($x = 0; $x < N; $x++) if ($bad($got[$y * N + $x] === 105, $x, $y)) $n++; return $n; };

    $b = trace($ctx, $plain, $as2);
    $wrong = $count($b, fn($hit, $x, $y) => ($deep($x, $y, true) && !$hit) || ($deep($x, $y, false) && $hit));
    if ($wrong) $fail[] = "B: $wrong rays wrong" . $render($b);

    $c = trace($ctx, $ignore, $as4);
    $outside = $count($c, fn($hit, $x, $y) => $hit && !mask(($x + 0.5) / N, ($y + 0.5) / N));
    $missed = $count($c, fn($hit, $x, $y) => !$hit && $deep($x, $y, true));
    if ($outside || $missed) $fail[] = "C: $outside hits outside the mask, $missed deep rays missed" . $render($c);

    $d = trace($ctx, $accept, $as4);
    $empty = $count($d, fn($hit, $x, $y) => $hit && $deep($x, $y, false));
    $lost = $count($d, fn($hit, $x, $y) => !$hit && mask(($x + 0.5) / N, ($y + 0.5) / N));
    if ($empty || $lost) $fail[] = "D: $empty hits in the empty area, $lost covered rays lost" . $render($d);

    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['d3d12', 'vulkan'] as $b) echo "$b: ", run_backend($b), "\n";
$s = $bake(4, 6)['stats'];
$unknown = ($s['unknown_opaque'] + $s['unknown_transparent']) / array_sum($s);
echo "unknown share below 25%: ", $unknown < 0.25 ? "yes" : "no ($unknown)", "\n";
echo "DONE\n";
?>
--EXPECTF--
cpu: OK
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
unknown share below 25%: yes
DONE
