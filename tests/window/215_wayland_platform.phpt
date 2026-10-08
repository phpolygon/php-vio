--TEST--
Wayland platform (NATIVE-PLATFORM-PLAN Phase 3, OPEN-ITEMS A3): OpenGL (EGL) and Vulkan open on an xdg-shell toplevel at exactly the requested size and read back what they drew, the native handle is the wl_surface, outputs are the monitors
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (PHP_OS_FAMILY !== 'Linux') die('skip Wayland platform');
if (vio_platform() !== 'wayland') die('skip active platform is ' . vio_platform());
?>
--FILE--
<?php
/* A Wayland client cannot send another client input (there is no XSendEvent),
 * so the input half of the platform is not exercised here; the Linux CI runs
 * the whole suite on a headless weston, where the injection tests drive the
 * same input paths. Two frames each: the first one maps the window. */
foreach (['opengl', 'vulkan'] as $b) {
    $ctx = @vio_create($b, ['width' => 48, 'height' => 32, 'headless' => true, 'vsync' => false]);
    if (!$ctx || vio_backend_name($ctx) !== $b) { echo "$b: skip\n"; continue; }
    for ($f = 0; $f < 2; $f++) {
        vio_begin($ctx);
        vio_clear($ctx, 1.0, 0.0, 0.0, 1.0);
        vio_end($ctx);
        vio_poll_events($ctx);
    }
    $px = vio_read_pixels($ctx);
    $ok = vio_framebuffer_size($ctx) === [48, 32] && vio_window_size($ctx) === [48, 32]
        && strlen($px) === 48 * 32 * 4 && ord($px[0]) > 240 && ord($px[1]) < 15 && vio_native_window_handle($ctx) > 0;
    if ($b === 'opengl') {
        $gl = vio_gl_info($ctx);
        /* EGL: the version ladder lands on a core context from 3.3 up */
        $ok = $ok && $gl && version_compare(preg_replace('/^(\d+\.\d+).*/', '$1', $gl['version']), '3.3', '>=') && $gl['profile'] === 'core';
    }
    echo "$b: ", $ok ? 'OK' : 'FAIL ' . json_encode([vio_framebuffer_size($ctx), vio_window_size($ctx), bin2hex(substr($px, 0, 4)),
                                                     vio_native_window_handle($ctx), $b === 'opengl' ? vio_gl_info($ctx) : null]), "\n";
    vio_destroy($ctx);
}

$ctx = vio_create('auto', ['width' => 64, 'height' => 64, 'headless' => true]);
$mons = vio_monitors($ctx);
$m0 = $mons[0] ?? null;
$modes = vio_video_modes($ctx, 0);
$area = array_map(fn($m) => $m['width'] * $m['height'], $modes);
$sorted = $area; sort($sorted);
$hasCurrent = (bool)array_filter($modes, fn($m) => $m0 && $m['width'] === $m0['width'] && $m['height'] === $m0['height']);
echo "monitors: ", json_encode($m0 && $m0['primary'] && $m0['width'] > 0 && $m0['height'] > 0 && $m0['name'] !== ''
     && $m0['scale_x'] >= 1.0 && $m0['work_width'] > 0 && $m0['work_width'] <= $m0['width']),
     " modes: ", json_encode($modes && $area === $sorted && $hasCurrent), "\n";
/* the client picks its size outside fullscreen */
vio_set_window_size($ctx, 80, 40);
echo "resize: ", json_encode(vio_window_size($ctx)), "\n";
vio_destroy($ctx);
?>
--EXPECTF--
opengl: %r(OK|skip)%r
vulkan: %r(OK|skip)%r
monitors: true modes: true
resize: [80,40]
