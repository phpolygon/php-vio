--TEST--
A freed VioMesh gives its GPU buffers back: meshes created, drawn and dropped in a loop (also mid-frame and right after their draw, while the frame is still executing) leave the context fully usable
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with a 3D pipeline");
?>
--FILE--
<?php
/* D3D11 / D3D12 created a VioMesh's vertex and index buffers through create_buffer and never
 * destroyed them (a VRAM leak: measured +322 MB for 40 meshes of 8.4 MB on an RTX 2080).
 * vio_mesh_free_object now releases them (D3D12 parks them until the frames in flight are
 * done). This test cannot read VRAM; it exercises the release paths and checks the
 * context still renders correctly afterwards:
 *   - 120 meshes of ~1 MB created, drawn, dropped one by one (the draw is still in the
 *     frame's command list when the object dies),
 *   - a mesh dropped between vio_begin and vio_end after its draw,
 *   - a mesh that outlives its context (freed after vio_destroy).
 * tests/render3d/253 covers the mid-frame free of every other object kind. */
$W = 16;
$big = array_fill(0, 3 * 90000, 0.0);   // ~1 MB of vertices at the origin, only the first 4 vertices matter
$quadV = [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0];
foreach ($quadV as $i => $v) $big[$i] = $v;

function run_backend(string $name): string {
    global $W, $big, $quadV;
    $ctx = @vio_create($name, ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false]);
    if (!$ctx) return "skip (unavailable)";
    if (vio_backend_name($ctx) !== $name) { vio_destroy($ctx); return "skip (unavailable)"; }
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D)"; }
    $fail = [];
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $fs = "#version 330 core\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0.0, 1.0, 0.0, 1.0); }";
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);

    for ($i = 0; $i < 120; $i++) {
        $m = vio_mesh($ctx, ['vertices' => $big, 'indices' => [0, 1, 2, 0, 2, 3], 'layout' => [VIO_FLOAT3]]);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_draw($ctx, $m);
        unset($m);                     // the draw is recorded, the frame has not ended
        vio_end($ctx);
    }
    $px = vio_read_pixels($ctx);
    $o = (($W / 2) * $W + $W / 2) * 4;
    if (ord($px[$o + 1]) < 200) $fail[] = "the last frame did not draw (the mesh died after its draw was recorded)";

    $keep = vio_mesh($ctx, ['vertices' => $quadV, 'indices' => [0, 1, 2, 0, 2, 3], 'layout' => [VIO_FLOAT3]]);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pipe);
    vio_draw($ctx, $keep);
    vio_end($ctx);
    if (ord(vio_read_pixels($ctx)[$o + 1]) < 200) $fail[] = "a mesh created after the churn does not draw";

    $late = vio_mesh($ctx, ['vertices' => $big, 'indices' => [0, 1, 2], 'layout' => [VIO_FLOAT3]]);
    unset($keep, $pipe);
    vio_destroy($ctx);
    unset($late);                      // freed after its context is gone: must not crash
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'metal', 'vulkan'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(?:\[d3d12\] [^\r\n]*\r?\n)*(?:OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
DONE
