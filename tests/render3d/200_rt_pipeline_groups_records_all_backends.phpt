--TEST--
Ray tracing pipeline groups (VIO_FEATURE_RAYTRACING): several miss shaders and hit groups, callables, shader records per group, per-instance hit group and mask
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A13 (groups + records). Two occluders over the lower half:
 * left (x < 0, hit group 0, mask 1) and right (x > 0, hit group 1, mask 2).
 * Every group reads a uint from its shader record:
 *   raygen 100000, miss 0 / 1 -> 40 / 41, hit group 0 / 1 -> 11 / 22, callable 0 -> 1000.
 * The raygen traces with missIndex = row & 1 and adds the callable (which adds
 * its record to the callable data) and its own record:
 *   value = 100000 + 1000 + (hit group record | miss record).
 * Slice z = 0 traces with cull mask 0xFF, slice z = 1 with 0x01 (the right
 * occluder disappears). Rows: the lower half (rows 2..3 of 4) is covered.
 * D3D12: the 'hlsl' library exports vio_raygen, vio_miss / vio_miss1,
 * vio_closest_hit / vio_closest_hit1, vio_callable0; records are the root
 * constants of a local root signature at b0, space1. */
$W = 8; $H = 4;
$HDR = "#version 460\n#extension GL_EXT_ray_tracing : require\n";
$REC = "layout(shaderRecordEXT, std430) buffer Rec { uint v; } rec;\n";
$RGEN = $HDR . $REC
    . "layout(binding = 0) uniform accelerationStructureEXT u_tlas;\n"
    . "layout(std430, binding = 1) buffer B { uint v[]; } b;\n"
    . "layout(location = 0) rayPayloadEXT uint hit;\n"
    . "layout(location = 1) callableDataEXT uint cd;\n"
    . "void main(){ uvec3 id = gl_LaunchIDEXT; uvec3 n = gl_LaunchSizeEXT;\n"
    . "  vec2 uv = (vec2(id.xy) + 0.5) / vec2(n.xy) * 2.0 - 1.0;\n"
    . "  hit = 7u;\n"
    . "  uint mask = id.z == 0u ? 0xFFu : 0x01u;\n"
    . "  traceRayEXT(u_tlas, gl_RayFlagsOpaqueEXT, mask, 0, 1, id.y & 1u, vec3(uv, 0.0), 0.001, vec3(0.0, 0.0, 1.0), 100.0, 0);\n"
    . "  cd = 0u; executeCallableEXT(0, 1);\n"
    . "  b.v[(id.z * n.y + id.y) * n.x + id.x] = hit + cd + rec.v; }";
$MISS = $HDR . $REC . "layout(location = 0) rayPayloadInEXT uint hit;\nvoid main(){ hit = rec.v; }";
$CHIT = $HDR . $REC . "layout(location = 0) rayPayloadInEXT uint hit;\nhitAttributeEXT vec2 bary;\nvoid main(){ hit = rec.v; }";
$CALL = $HDR . $REC . "layout(location = 1) callableDataInEXT uint cd;\nvoid main(){ cd += rec.v; }";
$HLSL = <<<'HLSL'
RaytracingAccelerationStructure u_tlas : register(t0);
RWStructuredBuffer<uint> b : register(u1);
struct Rec { uint v; };
ConstantBuffer<Rec> rec : register(b0, space1);
struct Payload { uint hit; };
struct Data { uint v; };
[shader("raygeneration")] void vio_raygen() {
    uint3 id = DispatchRaysIndex(); uint3 n = DispatchRaysDimensions();
    float2 uv = (float2(id.xy) + 0.5) / float2(n.xy) * 2.0 - 1.0;
    RayDesc r; r.Origin = float3(uv, 0.0); r.Direction = float3(0.0, 0.0, 1.0); r.TMin = 0.001; r.TMax = 100.0;
    Payload p; p.hit = 7;
    TraceRay(u_tlas, RAY_FLAG_FORCE_OPAQUE, id.z == 0 ? 0xFF : 0x01, 0, 1, id.y & 1, r, p);
    Data d; d.v = 0; CallShader(0, d);
    b[(id.z * n.y + id.y) * n.x + id.x] = p.hit + d.v + rec.v;
}
[shader("miss")] void vio_miss(inout Payload p) { p.hit = rec.v; }
[shader("miss")] void vio_miss1(inout Payload p) { p.hit = rec.v; }
[shader("closesthit")] void vio_closest_hit(inout Payload p, in BuiltInTriangleIntersectionAttributes a) { p.hit = rec.v; }
[shader("closesthit")] void vio_closest_hit1(inout Payload p, in BuiltInTriangleIntersectionAttributes a) { p.hit = rec.v; }
[shader("callable")] void vio_callable0(inout Data d) { d.v += rec.v; }
HLSL;

$opts = ["width" => 16, "height" => 16, "headless" => true, "vsync" => false, "shader_model" => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_RAYTRACING') ?: ''));
$u = fn(int $v) => pack('V', $v);

function run_backend(string $name): string {
    global $W, $H, $RGEN, $MISS, $CHIT, $CALL, $HLSL, $opts, $require, $u;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_RAYTRACING)) {
        vio_destroy($ctx);
        return $req ? "FAIL\n  required but VIO_FEATURE_RAYTRACING is 0" : "skip (no ray tracing pipeline)";
    }
    $fail = [];
    $left  = vio_mesh($ctx, ['vertices' => [-1,-1,0.5, 0,-1,0.5, 0,0,0.5, -1,0,0.5], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $right = vio_mesh($ctx, ['vertices' => [0,-1,0.5, 1,-1,0.5, 1,0,0.5, 0,0,0.5], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $as = vio_acceleration_structure($ctx, [['mesh' => $left, 'hit_group' => 0, 'mask' => 0x01],
                                            ['mesh' => $right, 'hit_group' => 1, 'mask' => 0x02]]);
    $p = vio_rt_pipeline($ctx, [
        'raygen' => $RGEN,
        'miss' => [$MISS, $MISS],
        'hit_groups' => [['closest_hit' => $CHIT], ['closest_hit' => $CHIT]],
        'callables' => [$CALL],
        'records' => ['raygen' => $u(100000), 'miss' => [$u(40), $u(41)], 'hit_groups' => [$u(11), $u(22)], 'callables' => [$u(1000)]],
        'hlsl' => $HLSL,
    ]);
    if (!$as || !$p) { vio_destroy($ctx); return "FAIL\n  acceleration structure or pipeline not created"; }
    $buf = vio_storage_buffer($ctx, ['size' => $W * $H * 2 * 4]);
    vio_rt_bind_buffer($ctx, $p, $buf, 1);
    vio_bind_acceleration_structure($ctx, $as, 0);
    vio_trace_rays($ctx, $p, $W, $H, 2);
    $v = array_values(unpack('V*', vio_storage_buffer_read($ctx, $buf)));
    /* Row 0 of the launch is uv.y < 0 = the lower half in clip space (covered). */
    for ($z = 0; $z < 2; $z++) for ($y = 0; $y < $H; $y++) for ($x = 0; $x < $W; $x++) {
        $covered = $y < $H / 2;
        if (!$covered) $want = $y & 1 ? 41 : 40;
        elseif ($x < $W / 2) $want = 11;
        else $want = $z == 0 ? 22 : ($y & 1 ? 41 : 40);
        $want += 101000;
        $got = $v[($z * $H + $y) * $W + $x];
        if ($got !== $want) { $fail[] = "z $z (x $x, y $y): $got, want $want"; break 3; }
    }

    /* The contract: an empty miss list is refused (ValueError). */
    try { $bad = @vio_rt_pipeline($ctx, ['raygen' => $RGEN, 'miss' => [], 'hit_groups' => [['closest_hit' => $CHIT]], 'hlsl' => $HLSL]); }
    catch (\ValueError $e) { $bad = false; }
    if ($bad !== false) $fail[] = "an empty miss list was accepted";
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
