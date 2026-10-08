--TEST--
Inline ray tracing (VIO_FEATURE_RAY_QUERY): vio_acceleration_structure() builds a BLAS per mesh and a TLAS over the instances, vio_bind_acceleration_structure() binds it, GL_EXT_ray_query in the fragment and compute stages sees the occluders
--EXTENSIONS--
vio
--FILE--
<?php
/* An occluder quad at z = 0.5 covers x in [-1, 0]. A full-screen fragment
 * shader casts a ray from each pixel's (x, y, 0) toward +z: occluded pixels
 * write red, free ones green.
 *   A. one instance, identity transform: left half red, right half green
 *   B. the same mesh moved by +1 in x (instance transform): right red, left green
 *   C. two instances (left and moved): everything red
 *   D. compute: four rays at x = -0.75, -0.25, 0.25, 0.75 against A -> 1 1 0 0
 *   E. the contract: refused (false + warning) where the flag is 0
 * Columns are compared (x never flips between backends).
 * D3D12 needs DXR 1.1 + SM 6.5 (RayQuery); Metal MSL 2.4 for ray queries in
 * fragment functions. VIO_REQUIRE_RAY_QUERY makes the listed backends mandatory. */
$W = 16;
$VS = "#version 460\nlayout(location=0) in vec3 aPos;\nlayout(location=0) out vec3 wpos;\n"
    . "void main(){ wpos = aPos; gl_Position = vec4(aPos.xy, 0.0, 1.0); }";
$FS = "#version 460\n#extension GL_EXT_ray_query : require\n"
    . "layout(location=0) in vec3 wpos;\nlayout(location=0) out vec4 o;\n"
    . "layout(binding = 0) uniform accelerationStructureEXT u_tlas;\n"
    . "void main(){ rayQueryEXT q;\n"
    . "  rayQueryInitializeEXT(q, u_tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFF,\n"
    . "                        vec3(wpos.xy, 0.0), 0.001, vec3(0.0, 0.0, 1.0), 100.0);\n"
    . "  while (rayQueryProceedEXT(q)) {}\n"
    . "  bool hit = rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT;\n"
    . "  o = hit ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0, 1.0, 0.0, 1.0); }";
$CS = "#version 460\n#extension GL_EXT_ray_query : require\nlayout(local_size_x = 4) in;\n"
    . "layout(binding = 0) uniform accelerationStructureEXT u_tlas;\n"
    . "layout(std430, binding = 1) buffer B { uint v[]; } b;\n"
    . "void main(){ uint i = gl_GlobalInvocationID.x; rayQueryEXT q;\n"
    . "  rayQueryInitializeEXT(q, u_tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFF,\n"
    . "                        vec3(float(i) * 0.5 - 0.75, 0.0, 0.0), 0.001, vec3(0.0, 0.0, 1.0), 100.0);\n"
    . "  while (rayQueryProceedEXT(q)) {}\n"
    . "  b.v[i] = rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT ? 1u : 0u; }";

$opts = ["width" => $W, "height" => $W, "headless" => true, "vsync" => false, "shader_model" => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_RAY_QUERY') ?: ''));

function px(string $p, int $x, int $y, int $w): array { $o = ($y * $w + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }
function near(array $a, array $b): bool { return abs($a[0] - $b[0]) <= 3 && abs($a[1] - $b[1]) <= 3 && abs($a[2] - $b[2]) <= 3; }
function translate_x(float $tx): array { return [1,0,0,0, 0,1,0,0, 0,0,1,0, $tx,0,0,1]; }

function run_backend(string $name): string {
    global $W, $VS, $FS, $CS, $opts, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    $occ = vio_mesh($ctx, ['vertices' => [-1,-1,0.5, 0,-1,0.5, 0,1,0.5, -1,1,0.5], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    if (!vio_supports_feature($ctx, VIO_FEATURE_RAY_QUERY)) {
        $as = @vio_acceleration_structure($ctx, [['mesh' => $occ]]);
        vio_destroy($ctx);
        if ($as !== false) return "FAIL\n  E: vio_acceleration_structure succeeded without VIO_FEATURE_RAY_QUERY";
        return $req ? "FAIL\n  required but VIO_FEATURE_RAY_QUERY is 0" : "skip (no ray query)";
    }
    $fail = [];
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    if (!$sh) { vio_destroy($ctx); return "FAIL\n  ray query shader not created"; }
    $pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $cols = function ($as) use ($ctx, $pipe, $quad, $W): array {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_bind_acceleration_structure($ctx, $as, 0);
        vio_draw($ctx, $quad);
        vio_end($ctx);
        $p = vio_read_pixels($ctx);
        return [px($p, $W >> 2, $W >> 1, $W), px($p, $W - 1 - ($W >> 2), $W >> 1, $W)];
    };
    $RED = [255, 0, 0]; $GREEN = [0, 255, 0];
    $asA = vio_acceleration_structure($ctx, [['mesh' => $occ]]);
    $asB = vio_acceleration_structure($ctx, [['mesh' => $occ, 'transform' => translate_x(1.0)]]);
    $asC = vio_acceleration_structure($ctx, [['mesh' => $occ], ['mesh' => $occ, 'transform' => translate_x(1.0)]]);
    if (!$asA || !$asB || !$asC) { vio_destroy($ctx); return "FAIL\n  acceleration structure not built"; }
    if (!($asA instanceof VioAccelerationStructure)) $fail[] = "result is no VioAccelerationStructure";
    foreach ([['A', $asA, $RED, $GREEN], ['B', $asB, $GREEN, $RED], ['C', $asC, $RED, $RED]] as [$tag, $as, $wl, $wr]) {
        [$l, $r] = $cols($as);
        if (!near($l, $wl) || !near($r, $wr)) $fail[] = "$tag: columns " . json_encode([$l, $r]) . ", want " . json_encode([$wl, $wr]);
    }
    /* D: compute */
    if (vio_supports_feature($ctx, VIO_FEATURE_COMPUTE)) {
        $cp = vio_compute_pipeline($ctx, ['source' => $CS]);
        if (!$cp) $fail[] = "D: compute pipeline not created";
        else {
            $buf = vio_storage_buffer($ctx, ['size' => 16]);
            vio_compute_bind_buffer($ctx, $cp, $buf, 1, VIO_COMPUTE_WRITE);
            vio_bind_acceleration_structure($ctx, $asA, 0);
            vio_compute_dispatch($ctx, $cp, 1, 1, 1);
            $v = array_values(unpack('V4', vio_storage_buffer_read($ctx, $buf)));
            if ($v !== [1, 1, 0, 0]) $fail[] = "D: compute hits " . json_encode($v) . ", want [1,1,0,0]";
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
