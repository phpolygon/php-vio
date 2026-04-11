<?php
/**
 * Phase 5 - 2D API Demo
 *
 * Demonstrates shapes, sprites, and text rendering.
 * Requires a TTF font file - pass path as first argument.
 *
 * Usage: php phase5_2d.php [path/to/font.ttf]
 */

$fontPath = $argv[1] ?? null;

$ctx = vio_create('opengl', [
    'width'  => 800,
    'height' => 600,
    'title'  => 'php-vio Phase 5 – 2D API',
]);

if (!$ctx) {
    die("Failed to create context\n");
}

// Load font if available
$font = null;
if ($fontPath && file_exists($fontPath)) {
    $font = vio_font($ctx, $fontPath, 32.0);
    if ($font) echo "Font loaded: $fontPath\n";
}

$frame = 0;
$startTime = microtime(true);

while (!vio_should_close($ctx)) {
    vio_poll_events($ctx);

    if (vio_key_just_pressed($ctx, VIO_KEY_ESCAPE)) {
        vio_close($ctx);
    }

    vio_begin($ctx);
    vio_clear($ctx, 0.1, 0.1, 0.15);

    $t = microtime(true) - $startTime;

    // Background rectangles
    vio_rect($ctx, 20, 20, 760, 560, ['color' => 0x40FFFFFF, 'outline' => true]);

    // Moving rectangle
    $rx = 100 + sin($t * 2.0) * 200;
    vio_rect($ctx, $rx, 100, 80, 60, ['fill' => 0xFF3366CC]);

    // Pulsing circle
    $radius = 30 + sin($t * 3.0) * 15;
    vio_circle($ctx, 400, 300, $radius, ['fill' => 0xFFFF6633]);

    // Outlined circle
    vio_circle($ctx, 600, 200, 50, ['color' => 0xFF00FF88, 'outline' => true, 'segments' => 48]);

    // Cross lines
    vio_line($ctx, 350, 250, 450, 350, ['color' => 0xFFFFFF00]);
    vio_line($ctx, 450, 250, 350, 350, ['color' => 0xFFFFFF00]);

    // Grid of small rects
    for ($i = 0; $i < 8; $i++) {
        $color = 0xFF000000 | (($i * 32) << 16) | ((255 - $i * 32) << 8) | 128;
        vio_rect($ctx, 50 + $i * 90, 450, 70, 40, ['fill' => $color]);
    }

    // Text
    if ($font) {
        vio_text($ctx, $font, 'Hello php-vio!', 50, 550, ['color' => 0xFFFFFFFF]);
        vio_text($ctx, $font, sprintf('Frame: %d  FPS: %.0f', $frame, $frame / max($t, 0.001)), 50, 40, ['color' => 0xFF88FF88]);
    }

    // Flush all 2D items
    vio_draw_2d($ctx);

    vio_end($ctx);
    $frame++;
}

vio_destroy($ctx);
echo "Done. $frame frames rendered.\n";
