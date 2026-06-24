--TEST--
Video modes: vio_video_modes enumerates a monitor; vio_set_fullscreen accepts a resolution
--EXTENSIONS--
vio
--FILE--
<?php
// The null backend still has GLFW initialised (MINIT calls glfwInit), so on a
// machine with a display vio_video_modes returns the primary monitor's modes.
// Headless/no-monitor CI without a display yields an empty list — both are
// valid; we only assert the SHAPE of whatever comes back.
$ctx = vio_create("null");

$modes = vio_video_modes($ctx);
var_dump(is_array($modes));

$shape_ok = true;
$ascending = true;
$prev = -1;
foreach ($modes as $m) {
    if (!is_array($m)
        || !isset($m['width'], $m['height'], $m['refresh_rate'])
        || !is_int($m['width']) || !is_int($m['height']) || !is_int($m['refresh_rate'])
        || $m['width'] <= 0 || $m['height'] <= 0) {
        $shape_ok = false;
        break;
    }
    $px = $m['width'] * $m['height'];
    if ($px < $prev) $ascending = false;
    $prev = $px;
}
var_dump($shape_ok);
var_dump($ascending);

// Explicit monitor index argument is accepted (−1 = primary).
$modes_primary = vio_video_modes($ctx, -1);
var_dump(is_array($modes_primary));

// vio_set_fullscreen now takes an optional resolution + refresh. On the null
// backend there is no window, so these are absorbed without crashing. The
// contract under test is purely "the extended signature is callable".
vio_set_fullscreen($ctx);                 // native (back-compat)
vio_set_fullscreen($ctx, -1);             // monitor only (back-compat)
vio_set_fullscreen($ctx, -1, 1280, 720);  // resolution
vio_set_fullscreen($ctx, -1, 1920, 1080, 144); // resolution + refresh
echo "set_fullscreen overloads OK\n";

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
set_fullscreen overloads OK
OK
