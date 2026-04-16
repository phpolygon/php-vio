<?php
$ctx = vio_create("opengl", ["width" => 32, "height" => 32, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

// Create shader with GLSL (need layout location on output too)
$vs = <<<'GLSL'
#version 410 core
layout(location = 0) in vec3 aPos;
void main() {
    gl_Position = vec4(aPos, 1.0);
}
GLSL;

$fs = <<<'GLSL'
#version 410 core
layout(location = 0) out vec4 FragColor;
void main() {
    FragColor = vec4(1.0, 0.0, 0.0, 1.0);
}
GLSL;

$shader = vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs]);
var_dump($shader instanceof VioShader);

// Create pipeline
$pipeline = vio_pipeline($ctx, ['shader' => $shader]);
var_dump($pipeline instanceof VioPipeline);

// Bind in frame
vio_begin($ctx);
vio_bind_pipeline($ctx, $pipeline);
vio_end($ctx);
echo "pipeline bound OK\n";

// Shader reflection
$info = vio_shader_reflect($shader);
var_dump(is_array($info));
var_dump(isset($info['vertex']['inputs']));

vio_destroy($ctx);
echo "OK\n";
?>
