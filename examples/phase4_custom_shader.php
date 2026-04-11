<?php
/**
 * Phase 4 - Custom Shader & Pipeline Demo
 *
 * Renders a triangle using a custom shader with a time-based color animation.
 * Demonstrates: vio_shader, vio_pipeline, vio_bind_pipeline, vio_uniform_buffer
 */

$ctx = vio_create('opengl', [
    'width'  => 800,
    'height' => 600,
    'title'  => 'php-vio Phase 4 – Custom Shader',
]);

if (!$ctx) {
    die("Failed to create context\n");
}

// Custom vertex shader with uniform block
$vertSrc = <<<'GLSL'
#version 410 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColor;

layout(std140) uniform Globals {
    float uTime;
    float _pad1;
    float _pad2;
    float _pad3;
};

out vec4 vColor;

void main() {
    // Rotate triangle over time
    float angle = uTime * 0.5;
    float c = cos(angle);
    float s = sin(angle);
    vec3 p = vec3(
        aPosition.x * c - aPosition.y * s,
        aPosition.x * s + aPosition.y * c,
        aPosition.z
    );
    gl_Position = vec4(p, 1.0);
    vColor = aColor;
}
GLSL;

$fragSrc = <<<'GLSL'
#version 410 core
in vec4 vColor;
out vec4 FragColor;

layout(std140) uniform Globals {
    float uTime;
    float _pad1;
    float _pad2;
    float _pad3;
};

void main() {
    // Pulsate alpha with time
    float pulse = 0.5 + 0.5 * sin(uTime * 3.0);
    FragColor = vec4(vColor.rgb, vColor.a * pulse);
}
GLSL;

// Compile custom shader
$shader = vio_shader($ctx, [
    'vertex'   => $vertSrc,
    'fragment' => $fragSrc,
]);
if (!$shader) die("Failed to compile shader\n");
echo "Custom shader compiled\n";

// Create pipeline
$pipeline = vio_pipeline($ctx, [
    'shader'     => $shader,
    'topology'   => VIO_TRIANGLES,
    'depth_test' => true,
    'blend'      => VIO_BLEND_ALPHA,
]);
if (!$pipeline) die("Failed to create pipeline\n");
echo "Pipeline created\n";

// Create uniform buffer (16 bytes = 1 float + 3 padding for std140)
$ubo = vio_uniform_buffer($ctx, [
    'size'    => 16,
    'binding' => 0,
]);
echo "UBO created\n";

// Create a colored triangle
$triangle = vio_mesh($ctx, [
    'vertices' => [
        // x,    y,    z,    r,   g,   b,   a
        -0.5, -0.5,  0.0,  1.0, 0.0, 0.0, 1.0,  // red
         0.5, -0.5,  0.0,  0.0, 1.0, 0.0, 1.0,  // green
         0.0,  0.5,  0.0,  0.0, 0.0, 1.0, 1.0,  // blue
    ],
    'layout' => [VIO_FLOAT3, VIO_FLOAT4],
]);
echo "Mesh created\n";

$startTime = microtime(true);
$frame = 0;

while (!vio_should_close($ctx)) {
    vio_poll_events($ctx);

    if (vio_key_just_pressed($ctx, VIO_KEY_ESCAPE)) {
        vio_close($ctx);
    }

    vio_begin($ctx);
    vio_clear($ctx, 0.05, 0.05, 0.1);

    // Update time uniform
    $time = (float)(microtime(true) - $startTime);
    vio_update_buffer($ubo, pack('f', $time));

    // Bind our custom pipeline and draw
    vio_bind_pipeline($ctx, $pipeline);
    vio_bind_buffer($ctx, $ubo, 0);
    vio_draw($ctx, $triangle);

    vio_end($ctx);
    $frame++;
}

vio_destroy($ctx);
echo "Done. $frame frames rendered.\n";
