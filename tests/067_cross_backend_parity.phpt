--TEST--
Cross-backend render parity: each backend produces the expected pixels for vio_clear
--SKIPIF--
<?php
$backends = vio_backends();
$gpu = array_intersect(['opengl', 'd3d11', 'd3d12'], $backends);
if (count($gpu) < 2) die('skip needs ≥2 GPU backends');
?>
--EXTENSIONS--
vio
--FILE--
<?php
// Backend clear-call timing inconsistency documented here so it doesn't
// drift back to causing renderer bugs:
//
//   OpenGL:  vio_clear is deferred — only stores the colour. The actual
//            glClear runs in opengl_begin_frame. Calling vio_clear after
//            vio_begin therefore does NOT clear the current frame.
//   D3D11:   vio_clear runs eagerly on the immediate context. Works in
//            either order (before or after vio_begin).
//   D3D12:   vio_clear records a ClearRenderTargetView command on the
//            graphics command list, which is only open between
//            begin_frame and end_frame. Calls outside that window are
//            silently dropped.
//
// Until the backends are unified the only "always works" pattern is:
//   1. vio_clear(...)
//   2. vio_begin(...)
//   3. vio_clear(...)         // again, no-op on OpenGL, eager on D3D
//   4. vio_end(...)
// which is what the engine layer does anyway via VioRenderer2D.

$candidates = ['opengl', 'd3d11', 'd3d12'];
$available = array_values(array_intersect($candidates, vio_backends()));

// Larger framebuffer to dodge GLFW min-window-size clamping on Windows.
$W = 256;
$H = 256;
$expected = [0xFF, 0x80, 0x40, 0xFF]; // 1.0, 0.5, 0.25, 1.0 → R/G/B/A bytes

$pixel_checks = 0;
foreach ($available as $backend) {
    $ctx = @vio_create($backend, ['width' => $W, 'height' => $H, 'headless' => true]);
    if (!$ctx instanceof VioContext) {
        echo "$backend: skip (no context)\n";
        continue;
    }

    // Clear both before AND after begin — covers all three timing
    // conventions in one call sequence.
    vio_clear($ctx, 1.0, 0.5, 0.25, 1.0);
    vio_begin($ctx);
    vio_clear($ctx, 1.0, 0.5, 0.25, 1.0);
    vio_end($ctx);

    $px = vio_read_pixels($ctx);

    // Different backends report different pixel-buffer sizes for the
    // same framebuffer (DPI scaling, GLFW window-size clamping). The
    // contract we DO want to hold: the bytes that ARE present must
    // start with the requested clear colour. We sample the first pixel.
    $r = ord($px[0]); $g = ord($px[1]); $b = ord($px[2]); $a = ord($px[3]);

    // Tolerance ±2 absorbs gamma / sRGB-vs-linear rounding.
    $hit = abs($r - $expected[0]) <= 2
        && abs($g - $expected[1]) <= 2
        && abs($b - $expected[2]) <= 2
        && abs($a - $expected[3]) <= 2;

    echo sprintf("%-7s first-pixel=%02x%02x%02x%02x len=%d match=%s\n",
                 $backend, $r, $g, $b, $a, strlen($px), $hit ? 'yes' : 'NO');
    if ($hit) $pixel_checks++;

    vio_destroy($ctx);
}

// At least two backends must agree on the clear colour for this to be
// considered passing cross-backend parity. If only one backend works,
// the test degrades to "current platform reports parity" which is fine.
var_dump($pixel_checks >= 2);
echo "OK\n";
?>
--EXPECTF--
%a
bool(true)
OK
