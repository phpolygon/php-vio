--TEST--
OpenGL: per-program caches (shadow-sampler locations, uniform locations) do not outlive their context; a program kept past vio_destroy does not leak its entries into the next context that reuses its program id
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
require __DIR__ . '/../skipif_gl.inc';
$c = vio_create('opengl', ['width' => 8, 'height' => 8, 'headless' => true]);
$s = @vio_shader($c, ['vertex' => "#version 430 core\nlayout(location = 0) in vec3 a_pos;\nvoid main() { gl_Position = vec4(a_pos, 1.0); }\n",
    'fragment' => "#version 430 core\nlayout(location = 5) uniform vec4 u_c;\nout vec4 o;\nvoid main() { o = u_c; }\n",
    'format' => VIO_SHADER_GLSL_RAW]);
if (!$s) die('skip needs GLSL 4.30 (explicit uniform locations)');
unset($s);
vio_destroy($c);
?>
--FILE--
<?php
// Regression: the shadow-sampler cache (locations of a program's sampler*Shadow
// uniforms, read by gl_shadow_begin() before every draw) was keyed by program id
// only and not cleared by opengl_shutdown. A program whose VioShader is still
// referenced at vio_destroy never had its entry dropped; the next context hands
// out the same program id again and got the old program's locations:
//  - context 2: location 5 is a plain sampler2D -> the depth-compare sampler
//    (CLAMP_TO_BORDER, white border) was bound to its unit, so a REPEAT lookup at
//    u = 1.25 returned white instead of the red texel;
//  - context 3: location 5 is a mat4 -> glGetUniformiv() wrote 16 ints into one
//    GLint on the stack (access violation in opengl_draw_mesh on NVIDIA).
// Every entry now carries its context generation, shutdown clears the cache, a
// program of a dead context no longer "forgets" entries of the live one, and the
// sampler unit is read into a buffer large enough for any uniform.
$opts = ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false];
$vs = "#version 430 core\nlayout(location = 0) in vec3 a_pos;\nvoid main() { gl_Position = vec4(a_pos, 1.0); }\n";
$tri = ['vertices' => [-1.0, -1.0, 0.0, 3.0, -1.0, 0.0, -1.0, 3.0, 0.0], 'layout' => [VIO_FLOAT3]];

function frame(VioContext $ctx, VioPipeline $p, VioMesh $m, callable $uniforms): string
{
    vio_begin($ctx);
    vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
    vio_bind_pipeline($ctx, $p);
    $uniforms($ctx);
    vio_draw($ctx, $m);
    vio_end($ctx);
    return (string) vio_read_pixels($ctx);
}
function centre(string $px): string
{
    $o = (8 * 16 + 8) * 4;
    return sprintf('%d %d %d', ord($px[$o]), ord($px[$o + 1]), ord($px[$o + 2]));
}

/* Context 1: a shadow sampler at location 5; the draw fills the cache. All
 * objects stay referenced across vio_destroy. */
$ctx1 = vio_create('opengl', $opts);
$fsA = "#version 430 core\nlayout(location = 5) uniform sampler2DShadow u_shadow;\nout vec4 o;\n"
    . "void main() { o = vec4(texture(u_shadow, vec3(0.5, 0.5, 0.5))); }\n";
$shaderA = vio_shader($ctx1, ['vertex' => $vs, 'fragment' => $fsA, 'format' => VIO_SHADER_GLSL_RAW]);
$pipeA = vio_pipeline($ctx1, ['shader' => $shaderA, 'depth_test' => false]);
$meshA = vio_mesh($ctx1, $tri);
frame($ctx1, $pipeA, $meshA, static function (VioContext $c): void {});
echo "context 1 drawn\n";
vio_destroy($ctx1);

/* Context 2: the same program id; location 5 is an ordinary sampler2D. */
$ctx2 = vio_create('opengl', $opts);
$fsB = "#version 430 core\nlayout(location = 5) uniform sampler2D u_tex;\nout vec4 o;\n"
    . "void main() { o = texture(u_tex, vec2(1.25, 0.5)); }\n";
$shaderB = vio_shader($ctx2, ['vertex' => $vs, 'fragment' => $fsB, 'format' => VIO_SHADER_GLSL_RAW]);
$pipeB = vio_pipeline($ctx2, ['shader' => $shaderB, 'depth_test' => false]);
$meshB = vio_mesh($ctx2, $tri);
$tex = vio_texture($ctx2, ['data' => "\xFF\x00\x00\xFF\x00\x00\xFF\xFF", 'width' => 2, 'height' => 1,
    'filter' => VIO_FILTER_NEAREST, 'wrap' => VIO_WRAP_REPEAT]);
$drawB = static function () use ($ctx2, $pipeB, $meshB, $tex): string {
    return frame($ctx2, $pipeB, $meshB, static function (VioContext $c) use ($tex): void {
        vio_bind_texture($c, $tex, 0);
        vio_set_uniform($c, 'u_tex', 0);
    });
};
echo "context 2: ", centre($drawB()), "\n";
vio_destroy($ctx2);

/* Context 3: the same program id; location 5 is a mat4. */
$ctx3 = vio_create('opengl', $opts);
$fsC = "#version 430 core\nlayout(location = 5) uniform mat4 u_m;\nout vec4 o;\n"
    . "void main() { o = u_m[0]; }\n";
$shaderC = vio_shader($ctx3, ['vertex' => $vs, 'fragment' => $fsC, 'format' => VIO_SHADER_GLSL_RAW]);
$pipeC = vio_pipeline($ctx3, ['shader' => $shaderC, 'depth_test' => false]);
$meshC = vio_mesh($ctx3, $tri);
$drawC = static function () use ($ctx3, $pipeC, $meshC): string {
    return frame($ctx3, $pipeC, $meshC, static function (VioContext $c): void {
        vio_set_uniform($c, 'u_m', [0.0, 1.0, 0.0, 1.0, 1e9, 1e9, 1e9, 1e9, -1e9, -1e9, -1e9, -1e9, 1e9, -1e9, 1e9, -1e9]);
    });
};
echo "context 3: ", centre($drawC()), "\n";
/* Freeing the objects of the dead contexts now must not disturb context 3. */
unset($pipeA, $shaderA, $meshA, $pipeB, $shaderB, $meshB, $tex, $drawB);
echo "context 3 after freeing contexts 1 and 2: ", centre($drawC()), "\n";
vio_destroy($ctx3);
echo "done\n";
?>
--EXPECT--
context 1 drawn
context 2: 255 0 0
context 3: 0 255 0
context 3 after freeing contexts 1 and 2: 0 255 0
done
