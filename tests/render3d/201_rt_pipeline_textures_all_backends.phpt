--TEST--
Ray tracing pipeline resources (VIO_FEATURE_RAYTRACING): vio_rt_bind_texture binds textures that the raygen and hit shaders sample next to the storage buffers
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A13 (resources). An occluder covers the lower half of the
 * launch. The raygen samples u_ramp (8 x 1, texel x = 10 * (x + 1)) at the
 * texel centre of its column; the closest hit samples u_hitcol (1 x 1, value
 * 200) - so the result is ramp + (hit ? 200 : 0), read from the red channel.
 * Rebinding a texture at the same binding replaces it (second trace: ramp 2,
 * texel x = 5 * (x + 1)). Bindings: TLAS 0, buffer 1, textures 2 and 3.
 * D3D12: Texture2D t<binding> with a linear / repeat SamplerState s<binding>. */
$W = 8; $H = 4;
$HDR = "#version 460\n#extension GL_EXT_ray_tracing : require\n";
$RGEN = $HDR
    . "layout(binding = 0) uniform accelerationStructureEXT u_tlas;\n"
    . "layout(std430, binding = 1) buffer B { uint v[]; } b;\n"
    . "layout(binding = 2) uniform sampler2D u_ramp;\n"
    . "layout(location = 0) rayPayloadEXT uint hit;\n"
    . "void main(){ uvec3 id = gl_LaunchIDEXT; uvec3 n = gl_LaunchSizeEXT;\n"
    . "  vec2 uv = (vec2(id.xy) + 0.5) / vec2(n.xy) * 2.0 - 1.0;\n"
    . "  hit = 0u;\n"
    . "  traceRayEXT(u_tlas, gl_RayFlagsOpaqueEXT, 0xFF, 0, 1, 0, vec3(uv, 0.0), 0.001, vec3(0.0, 0.0, 1.0), 100.0, 0);\n"
    . "  uint ramp = uint(textureLod(u_ramp, vec2((float(id.x) + 0.5) / float(n.x), 0.5), 0.0).r * 255.0 + 0.5);\n"
    . "  b.v[id.y * n.x + id.x] = ramp + hit; }";
$MISS = $HDR . "layout(location = 0) rayPayloadInEXT uint hit;\nvoid main(){ hit = 0u; }";
$CHIT = $HDR . "layout(binding = 3) uniform sampler2D u_hitcol;\n"
      . "layout(location = 0) rayPayloadInEXT uint hit;\nhitAttributeEXT vec2 bary;\n"
      . "void main(){ hit = uint(textureLod(u_hitcol, vec2(0.5), 0.0).r * 255.0 + 0.5); }";
$HLSL = <<<'HLSL'
RaytracingAccelerationStructure u_tlas : register(t0);
RWStructuredBuffer<uint> b : register(u1);
Texture2D u_ramp : register(t2);
SamplerState s_ramp : register(s2);
Texture2D u_hitcol : register(t3);
SamplerState s_hitcol : register(s3);
struct Payload { uint hit; };
[shader("raygeneration")] void vio_raygen() {
    uint3 id = DispatchRaysIndex(); uint3 n = DispatchRaysDimensions();
    float2 uv = (float2(id.xy) + 0.5) / float2(n.xy) * 2.0 - 1.0;
    RayDesc r; r.Origin = float3(uv, 0.0); r.Direction = float3(0.0, 0.0, 1.0); r.TMin = 0.001; r.TMax = 100.0;
    Payload p; p.hit = 0;
    TraceRay(u_tlas, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 1, 0, r, p);
    uint ramp = (uint)(u_ramp.SampleLevel(s_ramp, float2((id.x + 0.5) / n.x, 0.5), 0).r * 255.0 + 0.5);
    b[id.y * n.x + id.x] = ramp + p.hit;
}
[shader("miss")] void vio_miss(inout Payload p) { p.hit = 0; }
[shader("closesthit")] void vio_closest_hit(inout Payload p, in BuiltInTriangleIntersectionAttributes a) {
    p.hit = (uint)(u_hitcol.SampleLevel(s_hitcol, float2(0.5, 0.5), 0).r * 255.0 + 0.5);
}
HLSL;

$opts = ["width" => 16, "height" => 16, "headless" => true, "vsync" => false, "shader_model" => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_RAYTRACING') ?: ''));

function ramp(int $step): string { $s = ''; for ($x = 0; $x < 8; $x++) $s .= pack('C4', $step * ($x + 1), 0, 0, 255); return $s; }

function run_backend(string $name): string {
    global $W, $H, $RGEN, $MISS, $CHIT, $HLSL, $opts, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_RAYTRACING)) {
        vio_destroy($ctx);
        return $req ? "FAIL\n  required but VIO_FEATURE_RAYTRACING is 0" : "skip (no ray tracing pipeline)";
    }
    $fail = [];
    $occ = vio_mesh($ctx, ['vertices' => [-1,-1,0.5, 1,-1,0.5, 1,0,0.5, -1,0,0.5], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $as = vio_acceleration_structure($ctx, [['mesh' => $occ]]);
    $p = vio_rt_pipeline($ctx, ['raygen' => $RGEN, 'miss' => $MISS, 'closest_hit' => $CHIT, 'hlsl' => $HLSL]);
    if (!$as || !$p) { vio_destroy($ctx); return "FAIL\n  acceleration structure or pipeline not created"; }
    $ramp1 = vio_texture($ctx, ['data' => ramp(10), 'width' => 8, 'height' => 1, 'filter' => VIO_FILTER_NEAREST]);
    $ramp2 = vio_texture($ctx, ['data' => ramp(5), 'width' => 8, 'height' => 1, 'filter' => VIO_FILTER_NEAREST]);
    $hitcol = vio_texture($ctx, ['data' => pack('C4', 200, 0, 0, 255), 'width' => 1, 'height' => 1]);
    $buf = vio_storage_buffer($ctx, ['size' => $W * $H * 4]);
    vio_rt_bind_buffer($ctx, $p, $buf, 1);
    vio_rt_bind_texture($ctx, $p, $ramp1, 2);
    vio_rt_bind_texture($ctx, $p, $hitcol, 3);
    vio_bind_acceleration_structure($ctx, $as, 0);
    foreach ([[10, null], [5, $ramp2]] as $pass => [$step, $rebind]) {
        if ($rebind) vio_rt_bind_texture($ctx, $p, $rebind, 2);
        vio_trace_rays($ctx, $p, $W, $H);
        $v = array_values(unpack('V*', vio_storage_buffer_read($ctx, $buf)));
        for ($y = 0; $y < $H; $y++) for ($x = 0; $x < $W; $x++) {
            $want = $step * ($x + 1) + ($y < $H / 2 ? 200 : 0);
            $got = $v[$y * $W + $x];
            if ($got !== $want) { $fail[] = "pass $pass (x $x, y $y): $got, want $want"; break 3; }
        }
    }
    try { vio_rt_bind_texture($ctx, $p, $ramp1, 16); $fail[] = "binding 16 accepted"; } catch (\ValueError $e) {}
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
