<?php
/**
 * Phase 1 example: Basic window with clear loop
 *
 * Run with: php -d extension=modules/vio.so examples/phase1_window.php
 *
 * With only the null backend, this runs a headless loop.
 * Once an OpenGL backend is added (Phase 2), this will open a GLFW window.
 */

$ctx = vio_create('auto', [
    'width'  => 800,
    'height' => 600,
    'title'  => 'php-vio Phase 1',
]);

echo "Backend: " . vio_backend_name($ctx) . "\n";
echo "Backends registered: " . vio_backend_count() . "\n";

$frames = 0;
while (!vio_should_close($ctx)) {
    vio_begin($ctx);
    // Nothing to draw yet - just clearing
    vio_end($ctx);
    vio_poll_events($ctx);

    $frames++;
    if ($frames >= 60) {
        // Auto-exit after 60 frames for null backend
        if (vio_backend_name($ctx) === 'null') {
            break;
        }
    }
}

echo "Ran $frames frames\n";
vio_destroy($ctx);
echo "Done.\n";
