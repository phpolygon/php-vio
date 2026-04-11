--TEST--
Headless GPU rendering: clear, read_pixels, screenshot, image comparison
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 8, "height" => 8, "headless" => true]);
if (!$ctx) { echo "SKIP: cannot create headless OpenGL context\n"; exit; }

echo "backend: " . vio_backend_name($ctx) . "\n";

// === Clear red and verify pixels ===
vio_clear($ctx, 1.0, 0.0, 0.0, 1.0);
vio_begin($ctx);
$pixels = vio_read_pixels($ctx);
vio_end($ctx);

var_dump(strlen($pixels) === 8 * 8 * 4); // 256 bytes RGBA
$r = ord($pixels[0]); $g = ord($pixels[1]); $b = ord($pixels[2]); $a = ord($pixels[3]);
var_dump($r === 255 && $g === 0 && $b === 0 && $a === 255); // pure red

// === Clear green ===
vio_clear($ctx, 0.0, 1.0, 0.0, 1.0);
vio_begin($ctx);
$pixels = vio_read_pixels($ctx);
vio_end($ctx);

$r = ord($pixels[0]); $g = ord($pixels[1]); $b = ord($pixels[2]);
var_dump($r === 0 && $g === 255 && $b === 0); // pure green

// === Clear blue ===
vio_clear($ctx, 0.0, 0.0, 1.0, 1.0);
vio_begin($ctx);
$pixels = vio_read_pixels($ctx);
vio_end($ctx);

$r = ord($pixels[0]); $g = ord($pixels[1]); $b = ord($pixels[2]);
var_dump($r === 0 && $g === 0 && $b === 255); // pure blue

// === All pixels same color ===
vio_clear($ctx, 0.5, 0.5, 0.5, 1.0);
vio_begin($ctx);
$pixels = vio_read_pixels($ctx);
vio_end($ctx);

$first_r = ord($pixels[0]);
$last_off = (8 * 8 - 1) * 4;
$last_r = ord($pixels[$last_off]);
var_dump($first_r === $last_r); // uniform color

// === Screenshot to file ===
vio_clear($ctx, 1.0, 1.0, 0.0, 1.0); // yellow
vio_begin($ctx); vio_end($ctx);
$path = tempnam(sys_get_temp_dir(), 'vio_') . '.png';
$ok = vio_save_screenshot($ctx, $path);
var_dump($ok === true);
var_dump(file_exists($path));
var_dump(filesize($path) > 0);

// === Image comparison: identical images ===
$path2 = tempnam(sys_get_temp_dir(), 'vio_') . '.png';
vio_save_screenshot($ctx, $path2); // same yellow
$result = vio_compare_images($path, $path2);
var_dump($result['passed'] === true);
var_dump($result['diff_ratio'] === 0.0);
var_dump($result['diff_pixels'] === 0);

// === Image comparison: different images ===
vio_clear($ctx, 0.0, 0.0, 1.0, 1.0); // blue
vio_begin($ctx); vio_end($ctx);
$path3 = tempnam(sys_get_temp_dir(), 'vio_') . '.png';
vio_save_screenshot($ctx, $path3);

$result = vio_compare_images($path, $path3);
var_dump($result['passed'] === false);
var_dump($result['diff_ratio'] > 0);
var_dump($result['diff_pixels'] === 64); // all 8x8 pixels differ

// === Save diff image ===
$diff_path = tempnam(sys_get_temp_dir(), 'vio_') . '.png';
$ok = vio_save_diff_image($result, $diff_path);
var_dump($ok === true);
var_dump(file_exists($diff_path));

// === Comparison with threshold ===
$result = vio_compare_images($path, $path3, ["threshold" => 1.0]);
var_dump($result['passed'] === true); // 100% threshold = always pass

// Cleanup
unlink($path); unlink($path2); unlink($path3); unlink($diff_path);
vio_destroy($ctx);

echo "OK\n";
?>
--EXPECT--
backend: opengl
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
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
OK
