--TEST--
OpenGL: objects of a destroyed context freed while a NEW context is current must not delete the new context's GL names (second context renders)
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
require __DIR__ . '/../skipif_gl.inc';
?>
--FILE--
<?php
// Regression: GL object names are per context and start from 1 again in a fresh
// context. A VioShader / VioMesh / VioTexture / ... created under context 1 and
// freed only AFTER context 2 is current (PHP frees on reassignment, or in GC
// after vio_destroy) used to run glDelete* against names that context 2 had
// already handed out again -> the new shader/VAO vanished and context 2 drew
// black. Every GL-owned object is now stamped with its context generation and
// skips the delete when that context is gone. This hit every test that iterates
// over ['auto', ..., 'opengl'] once 'auto' resolved to OpenGL on Linux CI.

$vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$fs = "#version 330 core\nuniform sampler2D u_tex;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texture(u_tex, vec2(0.5)); }";

function scene($ctx, string $vs, string $fs): array
{
    // 1x1 green texture sampled by a full-screen triangle -> every pixel green.
    $tex  = vio_texture($ctx, ['data' => pack('C4', 0, 255, 0, 255), 'width' => 1, 'height' => 1]);
    $sh   = vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => VIO_SHADER_GLSL_RAW]);
    $pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false]);
    $mesh = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
    $rt   = vio_render_target($ctx, ['width' => 4, 'height' => 4]);
    $ubo  = vio_uniform_buffer($ctx, ['size' => 16, 'binding' => 0]);
    return compact('tex', 'sh', 'pipe', 'mesh', 'rt', 'ubo');
}

function draw($ctx, array $s): string
{
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_viewport($ctx, 0, 0, 8, 8);
    vio_bind_pipeline($ctx, $s['pipe']);
    vio_bind_texture($ctx, $s['tex'], 0);
    vio_set_uniform($ctx, 'u_tex', 0);
    vio_draw($ctx, $s['mesh']);
    $px = vio_read_pixels($ctx);
    vio_end($ctx);
    $o = (4 * 8 + 4) * 4;
    return sprintf('[%d,%d,%d]', ord($px[$o]), ord($px[$o + 1]), ord($px[$o + 2]));
}

$opts = ['width' => 8, 'height' => 8, 'headless' => true, 'vsync' => false];

$ctx1 = vio_create('opengl', $opts);
if (!$ctx1) die('skip cannot create OpenGL context');
$s1 = scene($ctx1, $vs, $fs);
echo "ctx1: ", draw($ctx1, $s1), "\n";
vio_destroy($ctx1);          // $s1 objects still alive — their GL names are now stale

$ctx2 = vio_create('opengl', $opts);
$s2 = scene($ctx2, $vs, $fs); // context 2 reuses the same GL name range
unset($s1);                   // frees context-1 objects while context 2 is current
echo "ctx2 after freeing ctx1 objects: ", draw($ctx2, $s2), "\n";

// A second scene on context 2 must be unaffected by the frees above.
$s3 = scene($ctx2, $vs, $fs);
echo "ctx2 second scene: ", draw($ctx2, $s3), "\n";
unset($s2, $s3);
vio_destroy($ctx2);
echo "OK\n";
?>
--EXPECT--
ctx1: [0,255,0]
ctx2 after freeing ctx1 objects: [0,255,0]
ctx2 second scene: [0,255,0]
OK
