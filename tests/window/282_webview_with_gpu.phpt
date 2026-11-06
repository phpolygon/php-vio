--TEST--
System webview (WEBVIEW-PLAN W3), next to GPU content in one window: frames render on with a webview in the window (vio_read_pixels exact, it is the swapchain only), the webview fills the client area and follows vio_set_window_size, 'bounds' / vio_webview_bounds place it (the page measures its viewport), null fills again; a transparent webview loads too
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio') || !vio_platform_info()['webview']) die('skip no system webview on this platform');
?>
--FILE--
<?php
$ctx = vio_create('auto', ['width' => 320, 'height' => 240, 'headless' => true]);
$page = '<!doctype html><html><head><meta name="viewport" content="width=device-width"><script>'
      . 'function size() { window.vio.postMessage([innerWidth, innerHeight]); }'
      . 'window.addEventListener("load", size); window.addEventListener("resize", size);'
      . '</script></head><body style="background: transparent"></body></html>';
$sizes = [];
$wv = vio_webview($ctx, ['url' => 'app://localhost/', 'transparent' => true,
                         'schemes' => ['app' => fn() => ['headers' => ['Content-Type' => 'text/html'], 'body' => $page]]]);
vio_webview_on_message($wv, function (string $json) use (&$sizes) { $sizes[] = json_decode($json); });
$waitFor = function (array $want) use ($ctx, &$sizes) {
    $end = microtime(true) + 10;
    while (microtime(true) < $end) {
        vio_wait_events($ctx, 0.05);
        if ($sizes && end($sizes) === $want) return true;
    }
    return end($sizes);
};
$frame = function () use ($ctx) {
    vio_begin($ctx);
    vio_clear($ctx, 1.0, 0.0, 0.0);
    vio_end($ctx);
    $px = vio_read_pixels($ctx);
    return strlen($px) === 320 * 240 * 4 && $px === str_repeat("\xff\x00\x00\xff", 320 * 240);
};
echo 'fill ', json_encode($waitFor([320, 240])), "\n";
echo 'gpu ', json_encode($frame()), "\n";
vio_webview_bounds($wv, [200, 10, 100, 120]);
echo 'bounds ', json_encode($waitFor([100, 120])), "\n";
echo 'gpu ', json_encode($frame()), "\n";
vio_webview_bounds($wv, null);
echo 'fill again ', json_encode($waitFor([320, 240])), "\n";
vio_set_window_size($ctx, 400, 300);
echo 'resized ', json_encode($waitFor([400, 300])), "\n";
vio_webview_destroy($wv);
echo 'gpu after ', json_encode((function () use ($ctx) { vio_begin($ctx); vio_end($ctx); return true; })()), "\n";
vio_destroy($ctx);
?>
--EXPECT--
fill true
gpu true
bounds true
gpu true
fill again true
resized true
gpu after true
