--TEST--
D3D12 shader model pinning: shader_model => 6x picks exactly that DXC profile (clamped to what device + DXC accept), version-gated features (SHADER_FLOAT16 from 6.2, COMPUTE_DERIVATIVES from 6.6) follow it, VIO_D3D12_SHADER_MODEL applies without the option, the option wins
--EXTENSIONS--
vio
--SKIPIF--
<?php
$c = @vio_create('d3d12', ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
if (!$c) die("skip d3d12 unavailable");
vio_destroy($c);
$dir = getenv("VIO_DXC_DIR") ?: "";
if ($dir === "") foreach (glob("C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll") ?: [] as $cand)
    if (is_file(dirname($cand) . "/dxil.dll")) $dir = dirname($cand);
$c = @vio_create("d3d12", ["width" => 8, "height" => 8, "headless" => true, "shader_model" => 6] + ($dir !== "" ? ["dxc_dir" => $dir] : []));
$sm = $c ? vio_swapchain_info($c)["shader_model"] : 0;
if ($c) vio_destroy($c);
if ($sm !== 6 && !getenv("VIO_REQUIRE_SM6")) die("skip no Shader Model 6 (DXC)");
?>
--FILE--
<?php
/* OPEN-ITEMS-PLAN A5. 'shader_model' => 6 keeps taking the highest 6.x; a value
 * 60..69 (major * 10 + minor) pins the profile, so a renderer can test the SM 6.0
 * path on a 6.8 device. Values above the maximum clamp to it, 5 / 51 stay FXC. */
function make(array $extra): ?VioContext {
    static $dir = null;
    if ($dir === null) {
        $dir = getenv('VIO_DXC_DIR') ?: '';
        if ($dir === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand)
            if (is_file(dirname($cand) . '/dxil.dll')) $dir = dirname($cand);
    }
    $o = ["width" => 16, "height" => 16, "headless" => true, "vsync" => false] + $extra;
    if ($dir !== '') $o['dxc_dir'] = $dir;
    return vio_create('d3d12', $o) ?: null;
}
function ver(?VioContext $c): int { return $c ? (vio_swapchain_info($c)['shader_model_version'] ?? 0) : -1; }

$top = make(['shader_model' => 6]);
$max = ver($top);
vio_destroy($top);
if ($max < 60) { echo "FAIL: SM 6 unavailable ($max)\n"; exit; }

$ok = true;
for ($p = 60; $p <= $max; $p++) {
    $c = make(['shader_model' => $p]);
    $v = ver($c);
    $f16 = vio_supports_feature($c, VIO_FEATURE_SHADER_FLOAT16);
    $cd = vio_supports_feature($c, VIO_FEATURE_COMPUTE_DERIVATIVES);
    if ($v !== $p) { echo "FAIL pin $p -> $v\n"; $ok = false; }
    if ($p < 62 && $f16) { echo "FAIL pin $p reports SHADER_FLOAT16\n"; $ok = false; }
    if ($p < 66 && $cd)  { echo "FAIL pin $p reports COMPUTE_DERIVATIVES\n"; $ok = false; }
    vio_destroy($c);
}
echo "pins: ", $ok ? "OK" : "FAIL", "\n";

/* Graphics at the lowest pin: the profile really compiles. */
$c = make(['shader_model' => 60]);
$vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$fs = "#version 330 core\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0.0, 1.0, 0.0, 1.0); }";
$pipe = vio_pipeline($c, ['shader' => vio_shader($c, ['vertex' => $vs, 'fragment' => $fs]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
$quad = vio_mesh($c, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
vio_clear($c, 0, 0, 0, 1);
vio_begin($c); vio_bind_pipeline($c, $pipe); vio_draw($c, $quad); vio_end($c);
$px = vio_read_pixels($c);
$o = (8 * 16 + 8) * 4;
echo "draw at 6.0: ", (ord($px[$o + 1]) > 250 && ord($px[$o]) < 5) ? "OK" : "FAIL", "\n";
vio_destroy($c);

$c = make(['shader_model' => 99]);
echo "above max clamps: ", ver($c) === $max ? "OK" : "FAIL " . ver($c), "\n";
vio_destroy($c);
foreach ([5, 51] as $p) {
    $c = make(['shader_model' => $p]);
    echo "pin $p: ", ver($c) === 51 ? "FXC" : "FAIL " . ver($c), "\n";
    vio_destroy($c);
}

putenv('VIO_D3D12_SHADER_MODEL=60');
$c = make([]);
echo "env without option: ", ver($c) === 60 ? "OK" : "FAIL " . ver($c), "\n";
vio_destroy($c);
$c = make(['shader_model' => $max]);
echo "option wins: ", ver($c) === $max ? "OK" : "FAIL " . ver($c), "\n";
vio_destroy($c);
putenv('VIO_D3D12_SHADER_MODEL');
echo "DONE\n";
?>
--EXPECT--
pins: OK
draw at 6.0: OK
above max clamps: OK
pin 5: FXC
pin 51: FXC
env without option: OK
option wins: OK
DONE
