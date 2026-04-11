--TEST--
vio_shader / vio_pipeline creation and binding with null backend
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create('null');

// Create shader (SPIR-V compatible GLSL with explicit locations)
$vert = "#version 410 core\nlayout(location=0) in vec3 aPos;\nvoid main() { gl_Position = vec4(aPos, 1.0); }";
$frag = "#version 410 core\nlayout(location=0) out vec4 FragColor;\nvoid main() { FragColor = vec4(1.0); }";
$shader = vio_shader($ctx, [
    'vertex'   => $vert,
    'fragment' => $frag,
    'format'   => VIO_SHADER_GLSL,
]);
var_dump($shader instanceof VioShader);

// Create pipeline
$pipe = vio_pipeline($ctx, [
    'shader'     => $shader,
    'topology'   => VIO_TRIANGLES,
    'cull_mode'  => VIO_CULL_BACK,
    'depth_test' => true,
    'blend'      => VIO_BLEND_ALPHA,
]);
var_dump($pipe instanceof VioPipeline);

// Bind pipeline within a frame
vio_begin($ctx);
vio_bind_pipeline($ctx, $pipe);
echo "bind_pipeline ok\n";
vio_end($ctx);

// Error: pipeline requires shader
$bad = vio_pipeline($ctx, ['topology' => VIO_TRIANGLES]);
var_dump($bad);

// Error: shader requires vertex
$bad2 = vio_shader($ctx, ['fragment' => 'test']);
var_dump($bad2);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECTF--
bool(true)
bool(true)
bind_pipeline ok

Warning: vio_pipeline(): vio_pipeline requires 'shader' (VioShader object) in %s on line %d
bool(false)

Warning: vio_shader(): vio_shader requires 'vertex' string in %s on line %d
bool(false)
OK
