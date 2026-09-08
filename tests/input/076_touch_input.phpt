--TEST--
vio_touch_inject / vio_touch_count / vio_touch_get round-trip + VIO_TOUCH_* constants
--EXTENSIONS--
vio
--FILE--
<?php
// Phase constants are exported (values 1..5)
var_dump(VIO_TOUCH_BEGAN === 1);
var_dump(VIO_TOUCH_MOVED === 2);
var_dump(VIO_TOUCH_STATIONARY === 3);
var_dump(VIO_TOUCH_ENDED === 4);
var_dump(VIO_TOUCH_CANCELLED === 5);

$ctx = vio_create("null", ["width" => 100, "height" => 100, "headless" => true]);

var_dump(vio_touch_count($ctx) === 0);

var_dump(vio_touch_inject($ctx, 7, VIO_TOUCH_BEGAN, 10.0, 20.0));
var_dump(vio_touch_count($ctx) === 1);

$t = vio_touch_get($ctx, 0);
var_dump($t["id"] === 7);
var_dump($t["x"] === 10.0 && $t["y"] === 20.0);
var_dump($t["phase"] === VIO_TOUCH_BEGAN);

// out-of-range index -> null
var_dump(vio_touch_get($ctx, 5) === null);

// unknown phase -> false
var_dump(@vio_touch_inject($ctx, 8, 99, 0.0, 0.0));

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(false)
OK
