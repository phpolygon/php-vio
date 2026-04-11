<?php
/**
 * Phase 2 example: Render a white triangle (position-only, no colors)
 *
 * Run with: php -d extension=modules/vio.so examples/phase2_white_triangle.php
 */

$ctx = vio_create('opengl', [
    'width'  => 800,
    'height' => 600,
    'title'  => 'php-vio - White Triangle',
]);

if ($ctx === false) {
    echo "Failed to create OpenGL context\n";
    exit(1);
}

echo "Backend: " . vio_backend_name($ctx) . "\n";

// Simple triangle - position only
$triangle = vio_mesh($ctx, [
    'vertices' => [
        -0.5, -0.5, 0.0,
         0.5, -0.5, 0.0,
         0.0,  0.5, 0.0,
    ],
    'layout' => [VIO_FLOAT3],
]);

vio_clear($ctx, 0.2, 0.2, 0.2, 1.0);

while (!vio_should_close($ctx)) {
    vio_begin($ctx);
    vio_draw($ctx, $triangle);
    vio_end($ctx);
    vio_poll_events($ctx);
}

vio_destroy($ctx);
