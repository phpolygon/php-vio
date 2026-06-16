--TEST--
vio_touch_inject / vio_touch_count / vio_touch_get round-trip (synthetic touches)
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("null", ["width" => 100, "height" => 100, "headless" => true]);

var_dump(vio_touch_count($ctx) === 0);

// phase 1 = BEGAN (VIO_TOUCH_* are integer phases, see vio_touch_get docs)
var_dump(vio_touch_inject($ctx, 7, 1, 10.0, 20.0));
var_dump(vio_touch_count($ctx) === 1);

$t = vio_touch_get($ctx, 0);
var_dump($t["id"] === 7);
var_dump($t["x"] === 10.0 && $t["y"] === 20.0);
var_dump($t["phase"] === 1);

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
bool(false)
OK
