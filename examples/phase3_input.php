<?php
/**
 * Phase 3 - Input Handling Demo
 *
 * Opens a window and prints input events to the terminal.
 * Press ESCAPE to close, move the mouse, click, scroll.
 */

$ctx = vio_create('opengl', [
    'width'  => 800,
    'height' => 600,
    'title'  => 'php-vio Phase 3 – Input Demo',
]);

if (!$ctx) {
    die("Failed to create context\n");
}

echo "Backend: " . vio_backend_name($ctx) . "\n";
echo "Press ESCAPE to quit, WASD to move, mouse to look around\n\n";

// Register key callback
vio_on_key($ctx, function(int $key, int $action, int $mods) {
    $actionStr = match($action) {
        VIO_ACTION_PRESS   => 'PRESS',
        VIO_ACTION_RELEASE => 'RELEASE',
        VIO_ACTION_REPEAT  => 'REPEAT',
        default            => "?($action)",
    };
    echo "  [KEY] key=$key action=$actionStr mods=$mods\n";
});

// Register resize callback
vio_on_resize($ctx, function(int $width, int $height) {
    echo "  [RESIZE] {$width}x{$height}\n";
});

$frame = 0;
while (!vio_should_close($ctx)) {
    vio_poll_events($ctx);
    vio_begin($ctx);

    // Clear with a dark blue
    vio_clear($ctx, 0.05, 0.05, 0.15);

    // Print mouse state every 60 frames
    if ($frame % 60 === 0) {
        [$mx, $my] = vio_mouse_position($ctx);
        [$dx, $dy] = vio_mouse_delta($ctx);
        [$sx, $sy] = vio_mouse_scroll($ctx);
        printf("  [MOUSE] pos=(%.0f,%.0f) delta=(%.1f,%.1f) scroll=(%.1f,%.1f) left=%s right=%s\n",
            $mx, $my, $dx, $dy, $sx, $sy,
            vio_mouse_button($ctx, VIO_MOUSE_LEFT) ? 'DOWN' : 'up',
            vio_mouse_button($ctx, VIO_MOUSE_RIGHT) ? 'DOWN' : 'up',
        );
    }

    // Close on ESCAPE
    if (vio_key_just_pressed($ctx, VIO_KEY_ESCAPE)) {
        echo "ESCAPE pressed, closing...\n";
        vio_close($ctx);
    }

    // Toggle fullscreen on F11
    if (vio_key_just_pressed($ctx, VIO_KEY_F11)) {
        echo "Toggling fullscreen...\n";
        vio_toggle_fullscreen($ctx);
    }

    vio_end($ctx);
    $frame++;
}

vio_destroy($ctx);
echo "Done. $frame frames rendered.\n";
