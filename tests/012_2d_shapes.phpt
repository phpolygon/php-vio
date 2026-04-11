--TEST--
vio_rect / vio_circle / vio_line 2D shape rendering with null backend
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create('null');
vio_begin($ctx);

// Filled rect
vio_rect($ctx, 10, 10, 200, 100, ['fill' => 0xFF3366CC]);
echo "rect ok\n";

// Outlined rect
vio_rect($ctx, 50, 50, 100, 80, ['color' => 0xFFFF0000, 'outline' => true]);
echo "rect outline ok\n";

// Filled circle
vio_circle($ctx, 200, 200, 50, ['fill' => 0xFF00FF00]);
echo "circle ok\n";

// Outlined circle with custom segments
vio_circle($ctx, 300, 200, 30, ['color' => 0xFFFFFF00, 'outline' => true, 'segments' => 16]);
echo "circle outline ok\n";

// Line
vio_line($ctx, 0, 0, 400, 300, ['color' => 0xFFFFFFFF]);
echo "line ok\n";

// Flush
vio_draw_2d($ctx);
echo "draw_2d ok\n";

vio_end($ctx);
vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
rect ok
rect outline ok
circle ok
circle outline ok
line ok
draw_2d ok
OK
