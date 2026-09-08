--TEST--
vio_texture_update sub-region uploads; pipelines release their GPU state when freed
--EXTENSIONS--
vio
--SKIPIF--
<?php
$c = @vio_create("auto", ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
if (!$c) die("skip no headless GPU context");
if (!vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)) die("skip backend lacks 3D pipeline");
$t = vio_texture($c, ['data' => "\xFF\xFF\xFF\xFF", 'width' => 1, 'height' => 1]);
if (!$t || @vio_texture_update($c, $t, "\x00\x00\x00\xFF") === false) { vio_destroy($c); die("skip backend has no vio_texture_update (D3D12 follow-up)"); }
vio_destroy($c);
?>
--FILE--
<?php
$W = 32; $H = 32;
$ctx = vio_create("auto", ["width" => $W, "height" => $H, "headless" => true, "vsync" => false]);
echo "backend=", vio_backend_name($ctx), "\n";
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b): bool { return abs($a[0]-$b[0]) <= 3 && abs($a[1]-$b[1]) <= 3 && abs($a[2]-$b[2]) <= 3; }
$fmt = vio_backend_name($ctx) === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
$vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nlayout(location=1) in vec2 aUv;\nout vec2 vUv;\nvoid main(){ vUv = aUv; gl_Position = vec4(aPos, 1.0); }";
$fs = "#version 330 core\nin vec2 vUv; uniform sampler2D u_tex;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texture(u_tex, vUv); }";
$pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]), 'depth_test' => false, 'blend' => VIO_BLEND_NONE]);
// UV v=0 at the bottom (GL convention) so texture row 0 (top-down data) lands at the TOP of the frame.
$quad = vio_mesh($ctx, ['vertices' => [-1,-1,0,0,1, 1,-1,0,1,1, 1,1,0,1,0, -1,1,0,0,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);

// 4x4 red texture; update the bottom-right 2x2 to green, then a single top-left pixel to blue.
$tex = vio_texture($ctx, ['data' => str_repeat("\xFF\x00\x00\xFF", 16), 'width' => 4, 'height' => 4, 'filter' => VIO_FILTER_NEAREST]);
var_dump(vio_texture_update($ctx, $tex, str_repeat("\x00\xFF\x00\xFF", 4), 2, 2, 2, 2));
var_dump(vio_texture_update($ctx, $tex, "\x00\x00\xFF\xFF", 0, 0, 1, 1));
var_dump(@vio_texture_update($ctx, $tex, "\x00", 3, 3, 2, 2) === false);   // out of range
var_dump(@vio_texture_update($ctx, $tex, "\x00") === false);                // too little data for full update

vio_clear($ctx, 0, 0, 0, 1);
vio_begin($ctx);
vio_bind_pipeline($ctx, $pipe); vio_set_uniform($ctx, 'u_tex', 0); vio_bind_texture($ctx, $tex, 0); vio_draw($ctx, $quad);
vio_end($ctx);
$p = vio_read_pixels($ctx);
// texel (0,0) top-left -> frame (4,4); texel (1,1) -> (12,12) still red; texel (3,3) bottom-right -> (28,28) green
$ok = near(px($p, 4, 4, $W), [0,0,255]) && near(px($p, 12, 12, $W), [255,0,0]) && near(px($p, 28, 28, $W), [0,255,0]) && near(px($p, 20, 4, $W), [255,0,0]);
echo "texture update: ", $ok ? "OK" : "FAIL " . json_encode([px($p,4,4,$W), px($p,12,12,$W), px($p,28,28,$W), px($p,20,4,$W)]), "\n";

// Full-texture update (no region) replaces everything.
var_dump(vio_texture_update($ctx, $tex, str_repeat("\xFF\xFF\x00\xFF", 16)));
vio_begin($ctx); vio_bind_pipeline($ctx, $pipe); vio_set_uniform($ctx, 'u_tex', 0); vio_bind_texture($ctx, $tex, 0); vio_draw($ctx, $quad); vio_end($ctx);
echo "full update: ", near(px(vio_read_pixels($ctx), 16, 16, $W), [255,255,0]) ? "OK" : "FAIL", "\n";

// Pipelines are created and dropped in a loop, including while bound inside a
// frame — the backend must release its state objects without touching a bound one.
$sh = vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]);
for ($i = 0; $i < 200; $i++) {
    $tmp = vio_pipeline($ctx, ['shader' => $sh, 'blend' => $i % 3]);
    if ($i % 50 === 0) {
        vio_begin($ctx); vio_bind_pipeline($ctx, $tmp); vio_set_uniform($ctx, 'u_tex', 0); vio_bind_texture($ctx, $tex, 0); vio_draw($ctx, $quad);
        unset($tmp);        // freed while bound, mid-frame
        vio_end($ctx);
    }
    unset($tmp);
}
echo "pipeline churn: OK\n";

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECTF--
backend=%s
bool(true)
bool(true)
bool(true)
bool(true)
texture update: OK
bool(true)
full update: OK
pipeline churn: OK
OK
