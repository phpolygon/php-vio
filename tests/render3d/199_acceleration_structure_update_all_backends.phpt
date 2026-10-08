--TEST--
Acceleration structure updates (VIO_FEATURE_RAY_QUERY): vio_acceleration_structure_update() refits the TLAS in place when only transforms change, rebuilds it over the existing BLASes when the instance list changes, and builds new BLASes for new meshes
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A14. An occluder quad at z = 0.5 covers x in [-1, 0]; a
 * full-screen fragment shader casts rays toward +z (red = occluded, green =
 * free), as in test 163. One structure object is updated step by step:
 *   A. transform moved by +1 in x                -> 'refit',   green | red
 *   B. two instances of the same mesh            -> 'rebuild', red   | red
 *   C. back to one instance at identity          -> 'rebuild', red   | green
 *   D. a new mesh (right half)                   -> 'full',    green | red
 *   E. 20 refits alternating left / right, ends right -> green | red
 *   F. inside vio_begin / vio_end                -> false + warning, unchanged
 *   G. an empty instance list                    -> false + warning
 * VIO_REQUIRE_RAY_QUERY makes the listed backends mandatory. */
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

$opts = ["width" => $W, "height" => $W, "headless" => true, "vsync" => false, "shader_model" => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_RAY_QUERY') ?: ''));

function px(string $p, int $x, int $y, int $w): array { $o = ($y * $w + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }
function near(array $a, array $b): bool { return abs($a[0] - $b[0]) <= 3 && abs($a[1] - $b[1]) <= 3 && abs($a[2] - $b[2]) <= 3; }
function translate_x(float $tx): array { return [1,0,0,0, 0,1,0,0, 0,0,1,0, $tx,0,0,1]; }

function run_backend(string $name): string {
    global $W, $VS, $FS, $opts, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_RAY_QUERY)) {
        vio_destroy($ctx);
        return $req ? "FAIL\n  required but VIO_FEATURE_RAY_QUERY is 0" : "skip (no ray query)";
    }
    $occ   = vio_mesh($ctx, ['vertices' => [-1,-1,0.5, 0,-1,0.5, 0,1,0.5, -1,1,0.5], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $right = vio_mesh($ctx, ['vertices' => [0,-1,0.5, 1,-1,0.5, 1,1,0.5, 0,1,0.5], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $fail = [];
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    $pipe = $sh ? vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]) : false;
    if (!$pipe) { vio_destroy($ctx); return "FAIL\n  ray query pipeline not created"; }
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
    $as = vio_acceleration_structure($ctx, [['mesh' => $occ]]);
    if (!$as) { vio_destroy($ctx); return "FAIL\n  acceleration structure not built"; }
    [$l, $r] = $cols($as);
    if (!near($l, $RED) || !near($r, $GREEN)) $fail[] = "start: columns " . json_encode([$l, $r]);

    $steps = [
        ['A', [['mesh' => $occ, 'transform' => translate_x(1.0)]], 'refit', $GREEN, $RED],
        ['B', [['mesh' => $occ], ['mesh' => $occ, 'transform' => translate_x(1.0)]], 'rebuild', $RED, $RED],
        ['C', [['mesh' => $occ]], 'rebuild', $RED, $GREEN],
        ['D', [['mesh' => $right]], 'full', $GREEN, $RED],
    ];
    foreach ($steps as [$tag, $inst, $kind, $wl, $wr]) {
        $got = vio_acceleration_structure_update($ctx, $as, $inst);
        if ($got !== $kind) $fail[] = "$tag: update returned " . var_export($got, true) . ", want '$kind'";
        [$l, $r] = $cols($as);
        if (!near($l, $wl) || !near($r, $wr)) $fail[] = "$tag: columns " . json_encode([$l, $r]) . ", want " . json_encode([$wl, $wr]);
    }

    /* E: many refits; the last one leaves the right-half mesh at its place. */
    for ($i = 0; $i < 20; $i++) {
        $got = vio_acceleration_structure_update($ctx, $as, [['mesh' => $right, 'transform' => translate_x($i % 2 ? 0.0 : -1.0)]]);
        if ($got !== 'refit') { $fail[] = "E: refit $i returned " . var_export($got, true); break; }
    }
    [$l, $r] = $cols($as);
    if (!near($l, $GREEN) || !near($r, $RED)) $fail[] = "E: columns " . json_encode([$l, $r]);

    /* F: refused inside a frame, the structure keeps its state. */
    vio_begin($ctx);
    $got = @vio_acceleration_structure_update($ctx, $as, [['mesh' => $right, 'transform' => translate_x(-1.0)]]);
    vio_end($ctx);
    if ($got !== false) $fail[] = "F: update inside a frame returned " . var_export($got, true);
    [$l, $r] = $cols($as);
    if (!near($l, $GREEN) || !near($r, $RED)) $fail[] = "F: columns after the refused update " . json_encode([$l, $r]);

    /* G: empty list. */
    if (@vio_acceleration_structure_update($ctx, $as, []) !== false) $fail[] = "G: empty instance list accepted";

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
