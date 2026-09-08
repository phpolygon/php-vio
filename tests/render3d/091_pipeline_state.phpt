--TEST--
Pipeline state: depth_write, color_mask and the extended blend modes
--EXTENSIONS--
vio
--SKIPIF--
<?php
$c = @vio_create("auto", ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
if (!$c) die("skip no headless GPU context");
if (!vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)) die("skip backend lacks 3D pipeline");
vio_destroy($c);
?>
--FILE--
<?php
$W = 32; $H = 32;
$ctx = vio_create("auto", ["width" => $W, "height" => $H, "headless" => true, "vsync" => false]);
echo "backend=", vio_backend_name($ctx), "\n";
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2]), ord($p[$o+3])]; }
function near(array $a, array $b, int $tol = 3): bool { for ($i = 0; $i < 4; $i++) if (abs($a[$i]-$b[$i]) > $tol) return false; return true; }

$fmt = vio_backend_name($ctx) === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
$vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nuniform float u_z;\nvoid main(){ gl_Position = vec4(aPos.xy, u_z, 1.0); }";
$fs = "#version 330 core\nuniform vec4 u_color;\nlayout(location=0) out vec4 o;\nvoid main(){ o = u_color; }";
$sh = vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]);
$quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
function draw($ctx, $pipe, $quad, array $rgba, float $z): void {
    vio_bind_pipeline($ctx, $pipe); vio_set_uniform($ctx, 'u_color', $rgba); vio_set_uniform($ctx, 'u_z', $z); vio_draw($ctx, $quad);
}
function frame($ctx, callable $body): string { vio_clear($ctx, 0, 0, 0, 1); vio_begin($ctx); $body(); vio_end($ctx); return vio_read_pixels($ctx); }

// 1. depth_write=false: a near quad that doesn't write depth must not occlude a later far quad.
$p_nowrite = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => true, 'depth_write' => false, 'blend' => VIO_BLEND_NONE]);
$p_write   = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => true, 'depth_write' => true,  'blend' => VIO_BLEND_NONE]);
$p = frame($ctx, function () use ($ctx, $p_nowrite, $p_write, $quad) {
    draw($ctx, $p_nowrite, $quad, [1, 0, 0, 1], 0.2);   // near, no depth write
    draw($ctx, $p_write,   $quad, [0, 1, 0, 1], 0.8);   // far, passes because depth is still 1.0
});
$a = px($p, 16, 16, $W);
$p = frame($ctx, function () use ($ctx, $p_write, $quad) {
    draw($ctx, $p_write, $quad, [1, 0, 0, 1], 0.2);     // near, writes depth
    draw($ctx, $p_write, $quad, [0, 1, 0, 1], 0.8);     // far, fails depth test
});
$b = px($p, 16, 16, $W);
echo "depth_write: ", (near($a, [0,255,0,255]) && near($b, [255,0,0,255])) ? "OK" : "FAIL " . json_encode([$a, $b]), "\n";

// 2. color_mask=RGB: alpha stays at the clear value (1.0) although the shader writes 0.
$p_rgb = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'blend' => VIO_BLEND_NONE, 'color_mask' => VIO_COLOR_RGB]);
$p_g   = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'blend' => VIO_BLEND_NONE, 'color_mask' => VIO_COLOR_G]);
$p = frame($ctx, function () use ($ctx, $p_rgb, $quad) { draw($ctx, $p_rgb, $quad, [0, 0, 1, 0], 0.0); });
$a = px($p, 16, 16, $W);
$p = frame($ctx, function () use ($ctx, $p_g, $quad) { draw($ctx, $p_g, $quad, [1, 1, 1, 1], 0.0); });
$b = px($p, 16, 16, $W);
echo "color_mask: ", (near($a, [0,0,255,255]) && near($b, [0,255,0,255])) ? "OK" : "FAIL " . json_encode([$a, $b]), "\n";

// 3. Blend modes over a known base colour.
$base = [0.5, 0.5, 0.5, 1.0];
$p_none = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'blend' => VIO_BLEND_NONE]);
$modes = [
    'premultiplied' => [VIO_BLEND_PREMULTIPLIED, [0.5, 0.0, 0.0, 0.5], [191, 64, 64, 255]],   // 0.5 + 0.5*(1-0.5)
    'multiply'      => [VIO_BLEND_MULTIPLY,      [0.5, 1.0, 0.0, 1.0], [64, 128, 0, 255]],
    'screen'        => [VIO_BLEND_SCREEN,        [0.5, 0.0, 0.0, 1.0], [191, 128, 128, 255]], // 0.5 + 0.5*(1-0.5)
    'min'           => [VIO_BLEND_MIN,           [0.2, 0.8, 0.5, 1.0], [51, 128, 128, 255]],
    'max'           => [VIO_BLEND_MAX,           [0.2, 0.8, 0.5, 1.0], [128, 204, 128, 255]],
];
$ok = true;
foreach ($modes as $name => [$mode, $src, $want]) {
    $pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'blend' => $mode]);
    $p = frame($ctx, function () use ($ctx, $p_none, $pipe, $quad, $base, $src) {
        draw($ctx, $p_none, $quad, $base, 0.0);
        draw($ctx, $pipe, $quad, $src, 0.0);
    });
    $got = px($p, 16, 16, $W);
    if (!near($got, $want, 4)) { echo "blend $name: got ", json_encode($got), " want ", json_encode($want), "\n"; $ok = false; }
}
echo "blend modes: ", $ok ? "OK" : "FAIL", "\n";

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECTF--
backend=%s
depth_write: OK
color_mask: OK
blend modes: OK
OK
