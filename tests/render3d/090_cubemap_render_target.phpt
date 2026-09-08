--TEST--
Cubemap render targets: per-face bind, vio_render_target_cubemap, vio_generate_mipmaps + textureLod
--EXTENSIONS--
vio
--SKIPIF--
<?php
$c = @vio_create("auto", ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
if (!$c) die("skip no headless GPU context");
if (!vio_supports_feature($c, VIO_FEATURE_RENDER_TARGET_CUBE)) die("skip backend lacks cube render targets");
if (!vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)) die("skip backend lacks 3D pipeline");
vio_destroy($c);
?>
--FILE--
<?php
/* Renders a distinct flat colour into each of the six faces of a cube render
 * target (drawn — not cleared — so the check is portable across the eager /
 * latched vio_clear backends), generates the mip chain, then samples the cube
 * through a samplerCube with textureLod at level 0 and at the last level.
 * A uniformly coloured face averages to itself, so both LODs must match. */
$W = 32; $H = 32; $SIZE = 16;
$ctx = vio_create("auto", ["width" => $W, "height" => $H, "headless" => true, "vsync" => false]);
echo "backend=", vio_backend_name($ctx), "\n";

function px(string $p, int $x, int $y, int $w): array {
    $o = ($y * $w + $x) * 4;
    return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])];
}
function near(array $a, array $b): bool {
    return abs($a[0] - $b[0]) <= 3 && abs($a[1] - $b[1]) <= 3 && abs($a[2] - $b[2]) <= 3;
}

$vs_fill = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$fs_fill = "#version 330 core\nuniform vec4 u_color;\nlayout(location=0) out vec4 o;\nvoid main(){ o = u_color; }";
$fs_cube = "#version 330 core\nuniform samplerCube u_env; uniform vec3 u_dir; uniform float u_lod;\n"
         . "layout(location=0) out vec4 o;\nvoid main(){ o = textureLod(u_env, u_dir, u_lod); }";
$fmt = vio_backend_name($ctx) === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
$p_fill = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs_fill, 'fragment' => $fs_fill, 'format' => $fmt]), 'depth_test' => false]);
$p_cube = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs_fill, 'fragment' => $fs_cube, 'format' => $fmt]), 'depth_test' => false]);
$quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);

$rt = vio_render_target($ctx, ['cube' => true, 'size' => $SIZE, 'mipmaps' => true]);
var_dump($rt instanceof VioRenderTarget);

$colors = [[1,0,0], [0,1,0], [0,0,1], [1,1,0], [1,0,1], [0,1,1]];
$dirs   = [[1,0,0], [-1,0,0], [0,1,0], [0,-1,0], [0,0,1], [0,0,-1]];

vio_clear($ctx, 0, 0, 0, 1);
vio_begin($ctx);
for ($f = 0; $f < 6; $f++) {
    vio_bind_render_target($ctx, $rt, $f);
    vio_bind_pipeline($ctx, $p_fill);
    vio_set_uniform($ctx, 'u_color', [$colors[$f][0], $colors[$f][1], $colors[$f][2], 1.0]);
    vio_draw($ctx, $quad);
}
vio_unbind_render_target($ctx);
var_dump(vio_generate_mipmaps($ctx, $rt));
vio_end($ctx);

$cm = vio_render_target_cubemap($rt);
var_dump($cm instanceof VioCubemap);

$maxLod = (int)floor(log($SIZE, 2));
$ok = true;
foreach ([0.0, (float)$maxLod] as $lod) {
    for ($f = 0; $f < 6; $f++) {
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $p_cube);
        vio_set_uniform($ctx, 'u_env', 0);
        vio_bind_cubemap($ctx, $cm, 0);
        vio_set_uniform($ctx, 'u_dir', $dirs[$f]);
        vio_set_uniform($ctx, 'u_lod', $lod);
        vio_draw($ctx, $quad);
        vio_end($ctx);
        $px = px(vio_read_pixels($ctx), 16, 16, $W);
        $want = [$colors[$f][0] * 255, $colors[$f][1] * 255, $colors[$f][2] * 255];
        if (!near($px, $want)) {
            echo "face $f lod $lod: got ", json_encode($px), " want ", json_encode($want), "\n";
            $ok = false;
        }
    }
}
echo "faces+lod: ", $ok ? "OK" : "FAIL", "\n";

// Rendering into a mip level > 0 (no depth attachment) must work too.
vio_begin($ctx);
vio_bind_render_target($ctx, $rt, 2, 1);
vio_bind_pipeline($ctx, $p_fill);
vio_set_uniform($ctx, 'u_color', [1.0, 1.0, 1.0, 1.0]);
vio_draw($ctx, $quad);
vio_unbind_render_target($ctx);
vio_end($ctx);
vio_begin($ctx);
vio_bind_pipeline($ctx, $p_cube);
vio_set_uniform($ctx, 'u_env', 0);
vio_bind_cubemap($ctx, $cm, 0);
vio_set_uniform($ctx, 'u_dir', [0, 1, 0]);
vio_set_uniform($ctx, 'u_lod', 1.0);
vio_draw($ctx, $quad);
vio_end($ctx);
echo "level-1 face: ", near(px(vio_read_pixels($ctx), 16, 16, $W), [255,255,255]) ? "OK" : "FAIL", "\n";

// Bad arguments are rejected without touching the GPU.
var_dump(@vio_render_target($ctx, ['cube' => true, 'size' => 8, 'depth_only' => true]) === false);
vio_begin($ctx);
@vio_bind_render_target($ctx, $rt, 6);   // face out of range -> warning, no bind
vio_end($ctx);

// vio_generate_mipmaps on an ordinary mipmapped texture and CPU cubemap.
$tex = vio_texture($ctx, ['data' => str_repeat("\xFF\x00\x00\xFF", 16), 'width' => 4, 'height' => 4, 'mipmaps' => true]);
var_dump(vio_generate_mipmaps($ctx, $tex));
$faces = [];
for ($f = 0; $f < 6; $f++) { $face = []; for ($i = 0; $i < 4; $i++) array_push($face, 0, 0, 255, 255); $faces[] = $face; }
$cm2 = vio_cubemap($ctx, ['pixels' => $faces, 'width' => 2, 'height' => 2, 'mipmaps' => true]);
var_dump($cm2 instanceof VioCubemap && vio_generate_mipmaps($ctx, $cm2));

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECTF--
backend=%s
bool(true)
bool(true)
bool(true)
faces+lod: OK
level-1 face: OK
bool(true)
bool(true)
bool(true)
OK
