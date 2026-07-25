--TEST--
Graphics-stage storage buffers (Path B): compute writes instance matrices, vertex shader reads them via gl_InstanceIndex, NO readback
--SKIPIF--
<?php
require __DIR__ . '/skipif_gl.inc';
// Also needs compute (SSBO in the vertex stage is GL 4.3+).
$c = @vio_create('opengl', ['width' => 8, 'height' => 8, 'headless' => true]);
if (!$c || !vio_supports_feature($c, VIO_FEATURE_VERTEX_STORAGE)) {
    die('skip no VIO_FEATURE_VERTEX_STORAGE (needs GL 4.3+ compute)');
}
vio_destroy($c);
?>
--FILE--
<?php
$VP = 64; $N = 4;
$ctx = vio_create('opengl', ['width' => $VP, 'height' => $VP, 'vsync' => false, 'headless' => true]);

// Reflection must expose the vertex-stage SSBO (Path B validation surface).
$vs = "#version 450\n".
      "layout(location=0) in vec3 aPos;\n".
      "layout(std430, binding=0) readonly buffer Instances { mat4 models[]; };\n".
      "void main(){ gl_Position = models[gl_InstanceIndex] * vec4(aPos, 1.0); }\n";
$fs = "#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(1.0); }\n";
$sh = vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs]);
$info = vio_shader_reflect($sh);
var_dump(count($info['vertex']['storage_buffers']) === 1);

// Compute writes one scale+translate matrix per instance into the SSBO.
$cs = "#version 450\n".
  "layout(local_size_x=64) in;\n".
  "layout(std430, binding=0) writeonly buffer OutM { float m[]; };\n".
  "layout(std140, binding=2) uniform P { int count; float scale; int p0; int p1; };\n".
  "void main(){ uint g=gl_GlobalInvocationID.x; if(g>=uint(count)) return;\n".
  "  float x=((g&1u)!=0u)?0.5:-0.5; float y=((g&2u)!=0u)?0.5:-0.5;\n".
  "  uint o=g*16u; for(uint k=0u;k<16u;k++) m[o+k]=0.0;\n".
  "  m[o+0u]=scale; m[o+5u]=scale; m[o+10u]=1.0; m[o+15u]=1.0; m[o+12u]=x; m[o+13u]=y; }\n";
$cp  = vio_compute_pipeline($ctx, ['source' => $cs]);
$buf = vio_storage_buffer($ctx, ['size' => $N*16*4, 'stride' => 4]);
$pipe = vio_pipeline($ctx, ['shader' => $sh]);
$mesh = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2,0,2,3], 'layout' => [VIO_FLOAT3]]);

vio_begin($ctx);
vio_compute_set_uniforms($ctx, $cp, pack('l', $N).pack('f', 0.15).pack('l2', 0, 0));
vio_compute_bind_buffer($ctx, $cp, $buf, 0, VIO_COMPUTE_WRITE);
vio_compute_dispatch($ctx, $cp, 1, 1, 1);

vio_viewport($ctx, 0, 0, $VP, $VP);
vio_clear($ctx, 0, 0, 0, 1);
vio_bind_pipeline($ctx, $pipe);
// The whole point: bind the compute output as the graphics instance source and
// draw from it — no vio_storage_buffer_read of the matrices.
vio_bind_storage_buffer($ctx, $buf, 0, VIO_COMPUTE_READ);
vio_draw_instanced_from_buffer($ctx, $mesh, $N);
$px = vio_read_pixels($ctx);
vio_end($ctx);

$stride = intdiv(strlen($px), $VP * 4);
$white = 0;
foreach ([[0.5,0.5],[-0.5,0.5],[0.5,-0.5],[-0.5,-0.5]] as $c) {
    $x = (int)round(($c[0]*0.5+0.5)*$VP); $y = (int)round(($c[1]*0.5+0.5)*$VP);
    $x = max(0, min($VP-1, $x)); $y = max(0, min($VP-1, $y));
    $off = ($y*$stride + $x) * 4;
    if (ord($px[$off]) > 200) $white++;
}
var_dump($white === 4);

// Center between the quads stays background (proves the quads came from the
// buffer, not a fullscreen fill).
$cx = (int)round(0.5*$VP); $off = ($cx*$stride + $cx) * 4;
var_dump(ord($px[$off]) < 40);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
OK
