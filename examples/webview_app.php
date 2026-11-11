<?php
/**
 * A desktop app whose UI is a system webview, served by PHP in this process
 * (no HTTP server): a page with a form, a fetch() to PHP, a message from PHP to
 * the page and a GPU-drawn strip above the webview in the same window.
 *
 * Run with: php -d extension=modules/vio.so examples/webview_app.php
 */

if (!vio_platform_info()['webview']) {
    fwrite(STDERR, "No system webview on this platform (" . vio_platform() . ")\n");
    exit(1);
}

$ctx = vio_create('auto', ['width' => 900, 'height' => 640, 'title' => 'php-vio webview']);

$page = <<<'HTML'
<!doctype html>
<html><head><meta charset="utf-8"><title>php-vio webview</title>
<style>
  body { font: 15px system-ui, sans-serif; margin: 24px; background: #f6f6f4; color: #222; }
  button { font: inherit; padding: 6px 14px; }
  #log { margin-top: 16px; white-space: pre-wrap; font-family: ui-monospace, monospace; }
</style></head>
<body>
  <h1>Served by PHP</h1>
  <form method="post" action="/greet"><input name="name" value="vio"> <button>POST</button></form>
  <p><button id="time">fetch('/time')</button> <button id="ping">window.vio.postMessage</button></p>
  <div id="log"></div>
  <script>
    const log = (s) => document.getElementById('log').textContent += s + '\n';
    document.getElementById('time').onclick = async () => log(await (await fetch('/time')).text());
    document.getElementById('ping').onclick = () => window.vio.postMessage({ping: Date.now()});
    window.addEventListener('native', (e) => log('from PHP: ' + JSON.stringify(e.detail)));
  </script>
</body></html>
HTML;

$handler = function (array $req) use ($page): array|string {
    $path = parse_url($req['url'], PHP_URL_PATH);
    if ($req['method'] === 'POST' && $path === '/greet') {
        parse_str($req['body'], $form);
        $name = htmlspecialchars($form['name'] ?? '?');
        return ['headers' => ['Content-Type' => 'text/html; charset=utf-8'], 'body' => "<h1>Hello, $name</h1><a href='/'>back</a>"];
    }
    return match ($path) {
        '/' => ['headers' => ['Content-Type' => 'text/html; charset=utf-8'], 'body' => $page],
        '/time' => date('H:i:s') . ' from PHP ' . PHP_VERSION,
        default => ['status' => 404, 'body' => 'not found'],
    };
};

$strip = 48;   // GPU strip above the webview
$wv = vio_webview($ctx, ['url' => 'app://localhost/', 'devtools' => true, 'bounds' => [0, $strip, 900, 640 - $strip],
                         'schemes' => ['app' => $handler]]);
vio_webview_on_message($wv, function (string $json) use ($wv) {
    $detail = json_encode(['got' => json_decode($json, true), 'at' => microtime(true)]);
    vio_webview_eval($wv, "window.dispatchEvent(new CustomEvent('native', {detail: $detail}))");
});
vio_webview_on_event($wv, function (string $type, array $data) use ($ctx) {
    if ($type === 'title') vio_set_title($ctx, $data['title']);
});
vio_on_resize($ctx, function (int $w, int $h) use ($wv, $strip) {
    vio_webview_bounds($wv, [0, $strip, $w, max(1, $h - $strip)]);
});

$t = 0.0;
while (!vio_should_close($ctx)) {
    vio_wait_events($ctx, 1 / 30);
    $t += 1 / 30;
    vio_begin($ctx);
    vio_clear($ctx, 0.12, 0.13, 0.16);
    [$w] = vio_window_size($ctx);
    vio_rect($ctx, (int)((sin($t) * 0.5 + 0.5) * ($w - 120)), 12, 120, 24, ['color' => 0xFF4F8EF7]);
    vio_end($ctx);
}
vio_webview_destroy($wv);
vio_destroy($ctx);
