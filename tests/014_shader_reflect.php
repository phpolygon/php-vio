<?php
$ctx = vio_create('null');

$vert = "#version 450 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aTexCoord;
layout(std140, binding=0) uniform Matrices { mat4 uMVP; };
void main() { gl_Position = uMVP * vec4(aPos, 1.0); }";

$frag = "#version 450 core
layout(location=0) out vec4 FragColor;
layout(binding=1) uniform sampler2D uTexture;
void main() { FragColor = texture(uTexture, vec2(0.0)); }";

$shader = vio_shader($ctx, [
    'vertex'   => $vert,
    'fragment' => $frag,
]);
var_dump($shader instanceof VioShader);

// Reflect
$info = vio_shader_reflect($shader);
var_dump(is_array($info));

// Vertex inputs
var_dump(count($info['vertex']['inputs']));
echo $info['vertex']['inputs'][0]['name'] . "\n";
echo $info['vertex']['inputs'][1]['name'] . "\n";

// Vertex UBO
var_dump(count($info['vertex']['ubos']));
var_dump($info['vertex']['ubos'][0]['set']);
var_dump($info['vertex']['ubos'][0]['binding']);

// Fragment textures
var_dump(count($info['fragment']['textures']));
echo $info['fragment']['textures'][0]['name'] . "\n";

// Error: invalid shader
$bad = vio_shader($ctx, ['vertex' => 'invalid', 'fragment' => 'invalid']);
var_dump($bad);

vio_destroy($ctx);
echo "OK\n";
?>
