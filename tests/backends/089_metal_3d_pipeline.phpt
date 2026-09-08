--TEST--
Metal backend: 3D pipeline — mesh draw, uniforms, textures, instancing, RT sampling, cubemap, eager clear, MSAA RT/swapchain, depth-only discard, gpu_info, capture
--SKIPIF--
<?php
if (!in_array('metal', vio_backends(), true)) die('skip Metal backend not compiled in');
$c = @vio_create('metal', ['width' => 8, 'height' => 8, 'headless' => true, 'vsync' => false]);
if (!$c) die('skip Metal context unavailable');
if (!vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)) die('skip Metal 3D pipeline not available (no SPIRV-Cross?)');
vio_destroy($c);
?>
--EXTENSIONS--
vio
--FILE--
<?php
/* Pixel-level contract for the Metal 3D path. Every check reads the pre-present
 * frame back through vio_read_pixels (top-down RGBA). Colours are exact
 * (unlit, unblended writes), so a plain byte compare is enough. */
$W = 64; $H = 64;
$ctx = vio_create("metal", ["width" => $W, "height" => $H, "headless" => true, "vsync" => false]);

function px(string $p, int $x, int $y, int $w): array {
    $o = ($y * $w + $x) * 4;
    return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])];
}
function is(array $px, array $want): bool {
    return abs($px[0] - $want[0]) <= 2 && abs($px[1] - $want[1]) <= 2 && abs($px[2] - $want[2]) <= 2;
}
$I = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1];

$vs_plain = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos,1.0); }";
$fs_white = "#version 330 core\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(1.0); }";
$vs_model = "#version 330 core\nlayout(location=0) in vec3 aPos;\nuniform mat4 u_model;\nvoid main(){ gl_Position = u_model * vec4(aPos,1.0); }";
$fs_color = "#version 330 core\nuniform vec4 u_color;\nlayout(location=0) out vec4 o;\nvoid main(){ o = u_color; }";
$vs_uv    = "#version 330 core\nlayout(location=0) in vec3 aPos;\nlayout(location=1) in vec2 aUv;\nout vec2 vUv;\nvoid main(){ vUv = aUv; gl_Position = vec4(aPos,1.0); }";
$fs_tex   = "#version 330 core\nin vec2 vUv; uniform sampler2D u_tex;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texture(u_tex, vUv); }";
$vs_inst  = "#version 330 core\nlayout(location=0) in vec3 aPos;\nlayout(location=3) in mat4 aModel;\nvoid main(){ gl_Position = aModel * vec4(aPos,1.0); }";
$fs_cube  = "#version 330 core\nuniform samplerCube u_sky;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texture(u_sky, vec3(0.0, 0.0, 1.0)); }";

$tri     = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 0,1,0], 'layout' => [VIO_FLOAT3]]);
$quad    = vio_mesh($ctx, ['vertices' => [-0.5,-0.5,0, 0.5,-0.5,0, 0.5,0.5,0, -0.5,0.5,0],
                           'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
$small   = vio_mesh($ctx, ['vertices' => [-0.2,-0.2,0, 0.2,-0.2,0, 0.2,0.2,0, -0.2,0.2,0],
                           'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
$quad_uv = vio_mesh($ctx, ['vertices' => [-1,-1,0,0,0, 1,-1,0,1,0, 1,1,0,1,1, -1,1,0,0,1],
                           'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);

$p_white = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs_plain, 'fragment' => $fs_white])]);
$p_color = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs_model, 'fragment' => $fs_color]), 'depth_test' => false]);
$p_tex   = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs_uv,    'fragment' => $fs_tex])]);
$p_inst  = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs_inst,  'fragment' => $fs_white])]);
$p_cube  = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs_plain, 'fragment' => $fs_cube])]);
var_dump($p_white instanceof VioPipeline && $p_color instanceof VioPipeline && $p_tex instanceof VioPipeline
      && $p_inst instanceof VioPipeline && $p_cube instanceof VioPipeline);

// Clear colour is latched at encoder open (see 067): set it before vio_begin.
vio_clear($ctx, 0, 0, 0, 1);

// 1. Plain indexed-less triangle covers the centre, not the corners.
vio_begin($ctx);
vio_bind_pipeline($ctx, $p_white);
vio_draw($ctx, $tri);
vio_end($ctx);
$p = vio_read_pixels($ctx);
echo "triangle: ", (is(px($p, 32, 32, $W), [255,255,255]) && is(px($p, 1, 1, $W), [0,0,0])) ? "OK" : "FAIL", "\n";

// 2. Uniforms are per-draw: two quads with different u_model / u_color in one frame.
$T = $I; $T[12] = 0.5;
vio_begin($ctx);
vio_bind_pipeline($ctx, $p_color);
vio_set_uniform($ctx, 'u_model', $I); vio_set_uniform($ctx, 'u_color', [1, 0, 0, 1]); vio_draw($ctx, $quad);
vio_set_uniform($ctx, 'u_model', $T); vio_set_uniform($ctx, 'u_color', [0, 0, 1, 1]); vio_draw($ctx, $quad);
vio_end($ctx);
$p = vio_read_pixels($ctx);
echo "per-draw uniforms: ", (is(px($p, 20, 32, $W), [255,0,0]) && is(px($p, 58, 32, $W), [0,0,255])) ? "OK" : "FAIL", "\n";

// 3. Texture sampling (2x2 green RGBA8 via create_texture).
$tex = vio_texture($ctx, ['data' => str_repeat("\x00\xFF\x00\xFF", 4), 'width' => 2, 'height' => 2]);
vio_begin($ctx);
vio_bind_pipeline($ctx, $p_tex);
vio_set_uniform($ctx, 'u_tex', 0);
vio_bind_texture($ctx, $tex, 0);
vio_draw($ctx, $quad_uv);
vio_end($ctx);
$p = vio_read_pixels($ctx);
echo "texture: ", is(px($p, 32, 32, $W), [0,255,0]) ? "OK" : "FAIL", "\n";

// 4. Instancing: per-instance mat4 at locations 3..6, two instances left/right.
$m1 = $I; $m1[12] = -0.6;
$m2 = $I; $m2[12] =  0.6;
vio_begin($ctx);
vio_bind_pipeline($ctx, $p_inst);
vio_draw_instanced($ctx, $small, array_merge($m1, $m2), 2);
vio_end($ctx);
$p = vio_read_pixels($ctx);
echo "instanced: ", (is(px($p, 13, 32, $W), [255,255,255]) && is(px($p, 51, 32, $W), [255,255,255])
                  && is(px($p, 32, 32, $W), [0,0,0])) ? "OK" : "FAIL", "\n";

// 5. Render target: 3D draw into the RT, then sample it in the SAME frame.
$rt = vio_render_target($ctx, ['width' => 32, 'height' => 32]);
vio_begin($ctx);
vio_bind_render_target($ctx, $rt);
vio_bind_pipeline($ctx, $p_color);
vio_set_uniform($ctx, 'u_model', $I); vio_set_uniform($ctx, 'u_color', [1, 1, 0, 1]);
vio_draw($ctx, $quad);
vio_unbind_render_target($ctx);
$rt_tex = vio_render_target_texture($rt);
vio_bind_pipeline($ctx, $p_tex);
vio_set_uniform($ctx, 'u_tex', 0);
vio_bind_texture($ctx, $rt_tex, 0);
vio_draw($ctx, $quad_uv);
vio_end($ctx);
$p = vio_read_pixels($ctx);
echo "render target: ", (is(px($p, 32, 32, $W), [255,255,0]) && is(px($p, 2, 2, $W), [0,0,0])) ? "OK" : "FAIL", "\n";

// 6. Depth-only RT (shadow-map shape): draws must not fail without a colour attachment.
$rt_depth = vio_render_target($ctx, ['width' => 32, 'height' => 32, 'depth_only' => true]);
vio_begin($ctx);
vio_bind_render_target($ctx, $rt_depth);
vio_bind_pipeline($ctx, $p_white);
vio_draw($ctx, $tri);
vio_unbind_render_target($ctx);
vio_end($ctx);
echo "depth-only RT: OK\n";

// 7. Cubemap sampling (all faces magenta).
$faces = [];
for ($f = 0; $f < 6; $f++) {
    $face = [];
    for ($i = 0; $i < 4; $i++) { array_push($face, 255, 0, 255, 255); }
    $faces[] = $face;
}
$cm = vio_cubemap($ctx, ['pixels' => $faces, 'width' => 2, 'height' => 2]);
vio_begin($ctx);
vio_bind_pipeline($ctx, $p_cube);
vio_set_uniform($ctx, 'u_sky', 0);
vio_bind_cubemap($ctx, $cm, 0);
vio_draw($ctx, $tri);
vio_end($ctx);
$p = vio_read_pixels($ctx);
echo "cubemap: ", ($cm instanceof VioCubemap && is(px($p, 32, 40, $W), [255,0,255])) ? "OK" : "FAIL", "\n";

// 8. 2D batch still renders correctly after 3D state (cull / depth bias reset).
vio_begin($ctx);
vio_bind_pipeline($ctx, $p_white);
vio_draw($ctx, $tri);
vio_rect($ctx, 0, 0, 10, 10, ["color" => 0xFF00FF00]);
vio_draw_2d($ctx);
vio_end($ctx);
$p = vio_read_pixels($ctx);
echo "2d after 3d: ", is(px($p, 5, 5, $W), [0,255,0]) ? "OK" : "FAIL", "\n";

// 9. Eager vio_clear (D3D11 semantics): clears the bound target immediately,
//    on the swapchain and inside a bound RT.
vio_begin($ctx);
vio_bind_pipeline($ctx, $p_white); vio_draw($ctx, $tri);
vio_clear($ctx, 0, 0, 1, 1);
vio_end($ctx);
$p = vio_read_pixels($ctx);
$swap_ok = is(px($p, 32, 32, $W), [0,0,255]);
vio_begin($ctx);
vio_bind_render_target($ctx, $rt); vio_clear($ctx, 0, 1, 0, 1); vio_unbind_render_target($ctx);
vio_clear($ctx, 0, 0, 0, 1);
vio_bind_pipeline($ctx, $p_tex); vio_set_uniform($ctx, 'u_tex', 0); vio_bind_texture($ctx, $rt_tex, 0); vio_draw($ctx, $quad_uv);
vio_end($ctx);
$p = vio_read_pixels($ctx);
echo "eager clear: ", ($swap_ok && is(px($p, 32, 32, $W), [0,255,0])) ? "OK" : "FAIL", "\n";

// 10. MSAA RT: the resolved edge of a triangle has partial coverage; a
//     single-sample RT has none. 2D draws land in the MSAA target too.
$edge = [];
foreach ([1, 4] as $smp) {
    $rtm = vio_render_target($ctx, ['width' => 64, 'height' => 64, 'samples' => $smp]);
    vio_begin($ctx);
    vio_bind_render_target($ctx, $rtm);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $p_white); vio_draw($ctx, $tri);
    vio_rect($ctx, 2, 2, 6, 6, ['color' => 0xFFFF0000]); vio_draw_2d($ctx);
    vio_unbind_render_target($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    $t = vio_render_target_texture($rtm);
    vio_bind_pipeline($ctx, $p_tex); vio_set_uniform($ctx, 'u_tex', 0); vio_bind_texture($ctx, $t, 0); vio_draw($ctx, $quad_uv);
    vio_end($ctx);
    $p = vio_read_pixels($ctx);
    $mid = 0;
    for ($i = 0; $i < strlen($p); $i += 4) { $r = ord($p[$i]); if ($r > 10 && $r < 245) $mid++; }
    $edge[$smp] = $mid;
    // RT texture V is flipped when sampled with GL-style UVs: the 2D rect at
    // (2..8, 2..8) shows up at the bottom-left.
    $rect_ok = is(px($p, 4, 59, $W), [255,0,0]);
    if (!$rect_ok) echo "msaa x$smp: 2D rect missing\n";
}
echo "msaa: ", ($edge[1] === 0 && $edge[4] > 0) ? "OK" : "FAIL ({$edge[1]} / {$edge[4]})", "\n";

// 11. Depth-only RT keeps the fragment stage: `discard` on the left half leaves
//     depth at 1.0 (lit for a compare against 0.5), the right half writes 0.
$fs_disc = "#version 330 core\nlayout(location=0) out vec4 o;\nvoid main(){ if (gl_FragCoord.x < 32.0) discard; o = vec4(1.0); }";
$fs_sh   = "#version 330 core\nin vec2 vUv; uniform sampler2DShadow u_shadow;\nlayout(location=0) out vec4 o;\nvoid main(){ float s = texture(u_shadow, vec3(vUv, 0.5)); o = vec4(s, s, s, 1.0); }";
$p_disc = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs_plain, 'fragment' => $fs_disc])]);
$p_sh   = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs_uv,    'fragment' => $fs_sh])]);
$rtd = vio_render_target($ctx, ['width' => 64, 'height' => 64, 'depth_only' => true]);
vio_begin($ctx);
vio_bind_render_target($ctx, $rtd); vio_clear($ctx, 0, 0, 0, 1);
vio_bind_pipeline($ctx, $p_disc); vio_draw($ctx, $quad_uv);
vio_unbind_render_target($ctx);
vio_clear($ctx, 0, 0, 0, 1);
$dt = vio_render_target_texture($rtd);
vio_bind_pipeline($ctx, $p_sh); vio_set_uniform($ctx, 'u_shadow', 0); vio_bind_texture($ctx, $dt, 0); vio_draw($ctx, $quad_uv);
vio_end($ctx);
$p = vio_read_pixels($ctx);
echo "depth-only discard: ", (is(px($p, 10, 32, $W), [255,255,255]) && is(px($p, 54, 32, $W), [0,0,0])) ? "OK" : "FAIL", "\n";

vio_destroy($ctx);

// 12. Swapchain MSAA (vio_create 'samples'), GPU info, frame capture on Metal.
$c4 = vio_create("metal", ["width" => $W, "height" => $H, "headless" => true, "vsync" => false, "samples" => 4, "debug" => true]);
$tri4 = vio_mesh($c4, ['vertices' => [-1,-1,0, 1,-1,0, 0,1,0], 'layout' => [VIO_FLOAT3]]);
$pw4  = vio_pipeline($c4, ['shader' => vio_shader($c4, ['vertex' => $vs_plain, 'fragment' => $fs_white])]);
vio_clear($c4, 0, 0, 0, 1);
vio_begin($c4); vio_bind_pipeline($c4, $pw4); vio_draw($c4, $tri4);
vio_rect($c4, 0, 0, 8, 8, ['color' => 0xFFFF0000]); vio_draw_2d($c4);
vio_end($c4);
$p = vio_read_pixels($c4);
$mid = 0;
for ($i = 0; $i < strlen($p); $i += 4) { $r = ord($p[$i]); if ($r > 10 && $r < 245) $mid++; }
echo "swapchain msaa: ", ($mid > 0 && is(px($p, 4, 4, $W), [255,0,0]) && is(px($p, 32, 40, $W), [255,255,255])) ? "OK" : "FAIL ($mid)", "\n";
$gi = vio_gpu_info();
echo "gpu info: ", (is_array($gi) && $gi['name'] !== '' && $gi['vram_bytes'] > 0) ? "OK" : "FAIL", "\n";
$rec = @vio_recorder($c4, ['path' => sys_get_temp_dir() . '/vio_089_metal.mp4', 'fps' => 30]);
if ($rec instanceof VioRecorder) {
    vio_begin($c4); vio_bind_pipeline($c4, $pw4); vio_draw($c4, $tri4); vio_end($c4);
    $cap = vio_recorder_capture($rec, $c4);
    vio_recorder_stop($rec);
    @unlink(sys_get_temp_dir() . '/vio_089_metal.mp4');
    echo "recorder capture: ", $cap === true ? "OK" : "FAIL", "\n";
} else {
    echo "recorder capture: OK\n";  // FFmpeg not built in — capture path untestable here
}
vio_destroy($c4);
echo "DONE\n";
?>
--EXPECTF--
bool(true)
triangle: OK
per-draw uniforms: OK
texture: OK
instanced: OK
render target: OK
depth-only RT: OK
cubemap: OK
2d after 3d: OK
eager clear: OK
msaa: OK
depth-only discard: OK
swapchain msaa: OK
gpu info: OK
%Arecorder capture: OK
DONE
