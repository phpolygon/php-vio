--TEST--
vio_trace_rays inside vio_begin / vio_end (VIO_FEATURE_RAYTRACING): recorded in order with the frame's draws and async compute, readable mid-frame and after the frame, the render pass continues with its content
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A13 (trace in the frame). One frame:
 *   1. draw a quad over the left half (red)
 *   2. vio_trace_rays: hit (lower half of the launch) 105, miss 0
 *   3. async compute: c[i] = b[i] * 2 (must see the trace's writes)
 *   4. draw a quad over the right half (green)
 *   5. vio_storage_buffer_read(b) mid-frame (flushes), then a second trace
 *      with the occluder's mirror pipeline result checked after vio_end
 * The image keeps both halves (the pass resumed with LOAD), b and c hold the
 * trace / compute results. Before, vio_trace_rays refused to run in a frame. */
$W = 8; $H = 4;
$HDR = "#version 460\n#extension GL_EXT_ray_tracing : require\n";
$RGEN = $HDR
    . "layout(binding = 0) uniform accelerationStructureEXT u_tlas;\n"
    . "layout(std430, binding = 1) buffer B { uint v[]; } b;\n"
    . "layout(location = 0) rayPayloadEXT uint hit;\n"
    . "void main(){ uvec3 id = gl_LaunchIDEXT; uvec3 n = gl_LaunchSizeEXT;\n"
    . "  vec2 uv = (vec2(id.xy) + 0.5) / vec2(n.xy) * 2.0 - 1.0;\n"
    . "  hit = 7u;\n"
    . "  traceRayEXT(u_tlas, gl_RayFlagsOpaqueEXT, 0xFF, 0, 1, 0, vec3(uv, 0.0), 0.001, vec3(0.0, 0.0, 1.0), 100.0, 0);\n"
    . "  b.v[id.y * n.x + id.x] = hit; }";
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
    TraceRay(u_tlas, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 1, 0, r, p);
    b[id.y * n.x + id.x] = p.hit;
}
[shader("miss")] void vio_miss(inout Payload p) { p.hit = 0; }
[shader("closesthit")] void vio_closest_hit(inout Payload p, in BuiltInTriangleIntersectionAttributes a) {
    p.hit = 100 + (uint)(RayTCurrent() * 10.0 + 0.5);
}
HLSL;
$CS = "#version 450\nlayout(local_size_x = 32) in;\n"
    . "layout(std430, binding = 0) readonly buffer B { uint v[]; } b;\n"
    . "layout(std430, binding = 1) buffer C { uint v[]; } c;\n"
    . "void main(){ uint i = gl_GlobalInvocationID.x; c.v[i] = b.v[i] * 2u; }";
$VS = "#version 450\nlayout(location = 0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$FS = "#version 450\nlayout(location = 0) out vec4 o;\nuniform vec4 u_col;\nvoid main(){ o = u_col; }";

$opts = ["width" => 16, "height" => 16, "headless" => true, "vsync" => false, "shader_model" => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_RAYTRACING') ?: ''));

function px(string $p, int $x, int $y, int $w): array { $o = ($y * $w + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }
function want_trace(int $w, int $h): array { $v = []; for ($y = 0; $y < $h; $y++) for ($x = 0; $x < $w; $x++) $v[] = $y < $h / 2 ? 105 : 0; return $v; }

function run_backend(string $name): string {
    global $W, $H, $RGEN, $MISS, $CHIT, $HLSL, $CS, $VS, $FS, $opts, $require;
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
    $rt = vio_rt_pipeline($ctx, ['raygen' => $RGEN, 'miss' => $MISS, 'closest_hit' => $CHIT, 'hlsl' => $HLSL]);
    $cp = vio_compute_pipeline($ctx, ['source' => $CS]);
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    $pipe = $sh ? vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]) : false;
    if (!$as || !$rt || !$cp || !$pipe) { vio_destroy($ctx); return "FAIL\n  objects not created"; }
    $left  = vio_mesh($ctx, ['vertices' => [-1,-1,0, 0,-1,0, 0,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $right = vio_mesh($ctx, ['vertices' => [0,-1,0, 1,-1,0, 1,1,0, 0,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $b  = vio_storage_buffer($ctx, ['size' => 32 * 4]);
    $b2 = vio_storage_buffer($ctx, ['size' => 32 * 4]);
    $c  = vio_storage_buffer($ctx, ['size' => 32 * 4]);
    vio_bind_acceleration_structure($ctx, $as, 0);
    vio_compute_bind_buffer($ctx, $cp, $b, 0, VIO_COMPUTE_READ);
    vio_compute_bind_buffer($ctx, $cp, $c, 1, VIO_COMPUTE_WRITE);

    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pipe);
    vio_set_uniform($ctx, 'u_col', [1.0, 0.0, 0.0, 1.0]);
    vio_draw($ctx, $left);
    vio_rt_bind_buffer($ctx, $rt, $b, 1);
    vio_trace_rays($ctx, $rt, $W, $H);
    vio_compute_dispatch($ctx, $cp, 1, 1, 1, ['async' => true]);
    vio_bind_pipeline($ctx, $pipe);
    vio_set_uniform($ctx, 'u_col', [0.0, 1.0, 0.0, 1.0]);
    vio_draw($ctx, $right);
    $mid = array_values(unpack('V*', vio_storage_buffer_read($ctx, $b)));
    vio_rt_bind_buffer($ctx, $rt, $b2, 1);
    vio_trace_rays($ctx, $rt, $W, $H);
    vio_end($ctx);

    $want = want_trace($W, $H);
    if ($mid !== $want) $fail[] = "mid-frame read of the trace: " . json_encode($mid);
    $v2 = array_values(unpack('V*', vio_storage_buffer_read($ctx, $b2)));
    if ($v2 !== $want) $fail[] = "second trace after vio_end: " . json_encode($v2);
    $vc = array_values(unpack('V*', vio_storage_buffer_read($ctx, $c)));
    if ($vc !== array_map(fn($x) => $x * 2, $want)) $fail[] = "async compute after the trace: " . json_encode($vc);
    $p = vio_read_pixels($ctx);
    $l = px($p, 3, 8, 16); $r = px($p, 12, 8, 16);
    if ($l !== [255, 0, 0] || $r !== [0, 255, 0]) $fail[] = "image halves " . json_encode([$l, $r]) . " (draws around the trace)";

    /* Free the pipeline right after a frame that traced: no crash, no validation error. */
    $rt = null;
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
