--TEST--
Per-primitive shading rate (VIO_FEATURE_SHADING_RATE_PRIMITIVE): a vertex stage that writes gl_PrimitiveShadingRateEXT shades its primitives at that rate, overriding vio_set_shading_rate; pipelines that do not write it keep the set rate
--EXTENSIONS--
vio
--FILE--
<?php
/* GL_EXT_fragment_shading_rate. The fragment shader writes its pixel coordinate
 * (as in test 122): at 2x2 the two pixels of a block are equal, at 1x1 they
 * differ. The rate code is the same on Vulkan and D3D12 (log2 width << 2 |
 * log2 height): 0 = 1x1, 5 = 2x2.
 *   A. VS writes 2x2, set rate 1x1        -> coarse
 *   B. VS writes 1x1, set rate 2x2        -> fine (the primitive rate wins)
 *   C. a pipeline without the write, 2x2  -> coarse (the set rate still applies)
 * D3D12 needs VRS Tier 2 and SM 6.4 (SV_ShadingRate); Vulkan
 * primitiveFragmentShadingRate. VIO_REQUIRE_SHADING_RATE_PRIMITIVE makes the
 * listed backends mandatory. */
$W = 16;
/* The rate is a constant per shader (no int-uniform round trip). */
$VSP = fn(int $code) => "#version 450\n#extension GL_EXT_fragment_shading_rate : require\n"
     . "layout(location=0) in vec3 aPos;\n"
     . "void main(){ gl_PrimitiveShadingRateEXT = $code; gl_Position = vec4(aPos, 1.0); }";
$VS  = "#version 450\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$FS  = "#version 450\nlayout(location=0) out vec4 o;\n"
     . "void main(){ o = vec4(floor(gl_FragCoord.x) / 16.0, floor(gl_FragCoord.y) / 16.0, 0.0, 1.0); }";

$opts = ["width" => $W, "height" => $W, "headless" => true, "vsync" => false, "shader_model" => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_SHADING_RATE_PRIMITIVE') ?: ''));

function px(string $p, int $x, int $y, int $w): array { $o = ($y * $w + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }
/* 1 when pixels 4 and 5 of row 8 share a shading invocation (2x2), 0 when they differ. */
function coarse(string $p, int $w): int { return px($p, 4, 8, $w) === px($p, 5, 8, $w) ? 1 : 0; }

function run_backend(string $name): string {
    global $W, $VSP, $VS, $FS, $opts, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_SHADING_RATE_PRIMITIVE)) {
        vio_destroy($ctx);
        return $req ? "FAIL\n  required but VIO_FEATURE_SHADING_RATE_PRIMITIVE is 0" : "skip (no per-primitive shading rate)";
    }
    $fail = [];
    if (!vio_supports_feature($ctx, VIO_FEATURE_SHADING_RATE)) $fail[] = "per-primitive rate without VIO_FEATURE_SHADING_RATE";
    $sh2 = vio_shader($ctx, ['vertex' => $VSP(5), 'fragment' => $FS]);
    $sh1 = vio_shader($ctx, ['vertex' => $VSP(0), 'fragment' => $FS]);
    $sh  = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    if (!$sh2 || !$sh1 || !$sh) { vio_destroy($ctx); return "FAIL\n  shader not created"; }
    $base = ['depth_test' => false, 'cull_mode' => VIO_CULL_NONE];
    $p2 = vio_pipeline($ctx, ['shader' => $sh2] + $base);
    $p1 = vio_pipeline($ctx, ['shader' => $sh1] + $base);
    $p  = vio_pipeline($ctx, ['shader' => $sh] + $base);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $draw = function ($pipe, int $set) use ($ctx, $quad, $W): int {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_set_shading_rate($ctx, $set);
        vio_bind_pipeline($ctx, $pipe);
        vio_draw($ctx, $quad);
        vio_end($ctx);
        return coarse(vio_read_pixels($ctx), $W);
    };
    if ($draw($p2, VIO_SHADING_RATE_1X1) !== 1) $fail[] = "A: the primitive's 2x2 did not apply";
    if ($draw($p1, VIO_SHADING_RATE_2X2) !== 0) $fail[] = "B: the primitive's 1x1 did not override the set 2x2";
    if ($draw($p,  VIO_SHADING_RATE_2X2) !== 1) $fail[] = "C: a pipeline without the write lost the set 2x2";
    vio_set_shading_rate($ctx, VIO_SHADING_RATE_1X1);
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
