<?php
/**
 * Phase 2 example: Render a colored triangle with OpenGL
 *
 * Run with: php -d extension=modules/vio.so examples/phase2_triangle.php
 */

$ctx = vio_create('opengl', [
    'width'  => 800,
    'height' => 600,
    'title'  => 'php-vio - Triangle',
    'vsync'  => 1,
]);

if ($ctx === false) {
    echo "Failed to create OpenGL context\n";
    exit(1);
}

echo "Backend: " . vio_backend_name($ctx) . "\n";

// Triangle with per-vertex colors (position xyz + color rgba)
$triangle = vio_mesh($ctx, [
    'vertices' => [
        // x,    y,    z,     r,   g,   b,   a
        -0.5, -0.5,  0.0,   1.0, 0.0, 0.0, 1.0,  // bottom-left (red)
         0.5, -0.5,  0.0,   0.0, 1.0, 0.0, 1.0,  // bottom-right (green)
         0.0,  0.5,  0.0,   0.0, 0.0, 1.0, 1.0,  // top (blue)
    ],
    'layout' => [VIO_FLOAT3, VIO_FLOAT4],
]);

// Dark blue background
vio_clear($ctx, 0.1, 0.1, 0.2, 1.0);

$frames = 0;
while (!vio_should_close($ctx)) {
    vio_begin($ctx);
    vio_draw($ctx, $triangle);
    vio_end($ctx);
    vio_poll_events($ctx);
    $frames++;
}

echo "Rendered $frames frames\n";
vio_destroy($ctx);
echo "Done.\n";
