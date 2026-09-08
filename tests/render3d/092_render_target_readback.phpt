--TEST--
vio_read_render_target: colour, HDR, depth-only and cube-face readback (mid-frame and after)
--EXTENSIONS--
vio
--SKIPIF--
<?php
$c = @vio_create("auto", ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
if (!$c) die("skip no headless GPU context");
if (!vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)) die("skip backend lacks 3D pipeline");
if (!vio_supports_feature($c, VIO_FEATURE_RENDER_TARGET)) die("skip backend lacks render targets");
$rt = vio_render_target($c, ['width' => 4, 'height' => 4]);
if (!$rt || @vio_read_render_target($rt) === false) { vio_destroy($c); die("skip backend has no vio_read_render_target (D3D12 follow-up)"); }
vio_destroy($c);
?>
--FILE--
<?php
$W = 32; $H = 32;
$ctx = vio_create("auto", ["width" => $W, "height" => $H, "headless" => true, "vsync" => false]);
echo "backend=", vio_backend_name($ctx), "\n";
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2]), ord($p[$o+3])]; }
function near(array $a, array $b, int $tol = 3): bool { for ($i = 0; $i < count($b); $i++) if (abs($a[$i]-$b[$i]) > $tol) return false; return true; }
$fmt = vio_backend_name($ctx) === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
$vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nuniform float u_z;\nvoid main(){ gl_Position = vec4(aPos.xy * 0.5, u_z, 1.0); }";
$fs = "#version 330 core\nuniform vec4 u_color;\nlayout(location=0) out vec4 o;\nvoid main(){ o = u_color; }";
$pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]), 'blend' => VIO_BLEND_NONE]);
$quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
function draw($ctx, $pipe, $quad, array $rgba, float $z): void { vio_bind_pipeline($ctx, $pipe); vio_set_uniform($ctx, 'u_color', $rgba); vio_set_uniform($ctx, 'u_z', $z); vio_draw($ctx, $quad); }

// 1. Colour RT: centre quad red, corners keep the clear colour. Read mid-frame AND after vio_end.
$rt = vio_render_target($ctx, ['width' => 64, 'height' => 64]);
vio_clear($ctx, 0, 0, 1, 1);
vio_begin($ctx);
vio_bind_render_target($ctx, $rt);
vio_clear($ctx, 0, 0, 1, 1);     // eager on Metal/D3D; latched (already blue from above) on GL
draw($ctx, $pipe, $quad, [1, 0, 0, 1], 0.0);
$mid = vio_read_render_target($rt);
vio_unbind_render_target($ctx);
vio_end($ctx);
$after = vio_read_render_target($rt);
$ok = strlen($mid) === 64*64*4 && strlen($after) === 64*64*4
   && near(px($mid, 32, 32, 64), [255,0,0,255]) && near(px($mid, 2, 2, 64), [0,0,255,255])
   && near(px($after, 32, 32, 64), [255,0,0,255]) && near(px($after, 2, 2, 64), [0,0,255,255]);
echo "colour: ", $ok ? "OK" : "FAIL " . json_encode([px($mid,32,32,64), px($mid,2,2,64), px($after,32,32,64)]), "\n";

// 2. HDR RT: values > 1 clamp to 255, fractions quantise.
$hdr = vio_render_target($ctx, ['width' => 16, 'height' => 16, 'hdr' => true]);
vio_clear($ctx, 0, 0, 0, 1);
vio_begin($ctx);
vio_bind_render_target($ctx, $hdr);
$pipeHdr = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]), 'blend' => VIO_BLEND_NONE, 'hdr' => true]);
draw($ctx, $pipeHdr, $quad, [4.0, 0.5, 0.25, 1], 0.0);
vio_unbind_render_target($ctx);
vio_end($ctx);
$p = vio_read_render_target($hdr);
echo "hdr: ", near(px($p, 8, 8, 16), [255, 128, 64, 255]) ? "OK" : "FAIL " . json_encode(px($p, 8, 8, 16)), "\n";

// 3. Depth-only RT: quad at z=0.25 (NDC) -> stored depth 0.625 (GL, [-1,1]) or 0.25 (0..1 backends).
$dep = vio_render_target($ctx, ['width' => 16, 'height' => 16, 'depth_only' => true]);
vio_clear($ctx, 0, 0, 0, 1);
vio_begin($ctx);
vio_bind_render_target($ctx, $dep);
vio_clear($ctx, 0, 0, 0, 1);
draw($ctx, $pipe, $quad, [1, 1, 1, 1], 0.25);
vio_unbind_render_target($ctx);
vio_end($ctx);
$p = vio_read_render_target($dep);
$c = px($p, 8, 8, 16); $e = px($p, 1, 1, 16);
// Stored depth is 0.25 on a native 0..1 backend (Metal) and 0.625 where the
// transpiled vertex stage keeps the GL [-1,1] -> [0,1] remap (OpenGL, D3D).
$depthOk = abs($c[0] - 64) <= 3 || abs($c[0] - 159) <= 3;
echo "depth-only: ", ($depthOk && $c[0] === $c[1] && $c[1] === $c[2] && $e[0] === 255) ? "OK" : "FAIL " . json_encode([$c, $e]), "\n";

// 4. Cube face readback (when supported).
if (vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_CUBE)) {
    $cube = vio_render_target($ctx, ['cube' => true, 'size' => 8]);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    $pipeFull = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }", 'fragment' => $fs, 'format' => $fmt]), 'blend' => VIO_BLEND_NONE, 'depth_test' => false]);
    for ($f = 0; $f < 6; $f++) {
        vio_bind_render_target($ctx, $cube, $f);
        vio_bind_pipeline($ctx, $pipeFull); vio_set_uniform($ctx, 'u_color', [$f / 5.0, 0, 0, 1]); vio_draw($ctx, $quad);
    }
    vio_unbind_render_target($ctx);
    vio_end($ctx);
    $ok = true;
    for ($f = 0; $f < 6; $f++) {
        $p = vio_read_render_target($cube, $f);
        if (!near(px($p, 4, 4, 8), [(int)round($f / 5 * 255), 0, 0, 255])) { $ok = false; echo "face $f: ", json_encode(px($p, 4, 4, 8)), "\n"; }
    }
    echo "cube faces: ", $ok ? "OK" : "FAIL", "\n";
} else {
    echo "cube faces: OK\n";
}

var_dump(@vio_read_render_target($rt, 3) === false);   // face on a non-cube target
vio_destroy($ctx);
echo "OK\n";
?>
--EXPECTF--
backend=%s
colour: OK
hdr: OK
depth-only: OK
cube faces: OK
bool(true)
OK
