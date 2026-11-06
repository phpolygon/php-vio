--TEST--
System webview (WEBVIEW-PLAN W3), cookies and storage on the custom scheme (what Laravel needs): Set-Cookie from a PHP answer comes back as Cookie on later requests (fetch, navigation), document.cookie shows the cookies that are not HttpOnly (axios reads XSRF-TOKEN there) and never the HttpOnly session, a cookie set from script reaches PHP, an expired cookie is gone, localStorage survives a navigation
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio') || !vio_platform_info()['webview']) die('skip no system webview on this platform');
?>
--FILE--
<?php
$ctx = vio_create('auto', ['width' => 320, 'height' => 240, 'headless' => true]);
$one = <<<'HTML'
<!doctype html><html><head><script>
window.addEventListener('load', async () => {
  localStorage.setItem('k', 'v1');
  document.cookie = 'fromjs=1; path=/';
  const seen = document.cookie.split('; ').sort().join('; ');
  const res = await fetch('/check');
  window.vio.postMessage({page: 1, cookie: seen, server: await res.text()});
  location.href = '/two';
});
</script></head><body></body></html>
HTML;
$two = <<<'HTML'
<!doctype html><html><head><script>
window.addEventListener('load', async () => {
  const res = await fetch('/drop');
  await res.text();
  const res2 = await fetch('/check');
  window.vio.postMessage({page: 2, storage: localStorage.getItem('k'), cookie: document.cookie.split('; ').sort().join('; '), server: await res2.text()});
});
</script></head><body></body></html>
HTML;
$cookieOf = function (array $req): string {
    $c = array_change_key_case($req['headers'])['cookie'] ?? '';
    $parts = array_filter(array_map('trim', explode(';', $c)));
    sort($parts);
    return implode('; ', $parts);
};
$log = [];
$handler = function (array $req) use ($one, $two, $cookieOf, &$log) {
    $path = parse_url($req['url'], PHP_URL_PATH);
    $log[] = "$path [" . $cookieOf($req) . ']';
    return match ($path) {
        '/' => ['headers' => ['Content-Type' => 'text/html', 'Set-Cookie' => [
                    'laravel_session=abc; path=/; httponly; samesite=lax',
                    'XSRF-TOKEN=tok1; path=/; samesite=lax',
                    'old=1; path=/']], 'body' => $one],
        '/two' => ['headers' => ['Content-Type' => 'text/html'], 'body' => $two],
        '/drop' => ['headers' => ['Set-Cookie' => 'old=; expires=Thu, 01 Jan 1970 00:00:00 GMT; path=/'], 'body' => ''],
        '/check' => $cookieOf($req),
        default => ['status' => 404],
    };
};
$msgs = [];
$wv = vio_webview($ctx, ['url' => 'app://localhost/', 'schemes' => ['app' => $handler]]);
vio_webview_on_message($wv, function (string $json) use (&$msgs) { $msgs[] = json_decode($json, true); });
$end = microtime(true) + 20;
while (count($msgs) < 2 && microtime(true) < $end) vio_wait_events($ctx, 0.1);
foreach ($msgs as $m) echo json_encode($m, JSON_UNESCAPED_SLASHES), "\n";
echo implode("\n", array_filter($log, fn($l) => !str_starts_with($l, '/favicon'))), "\n";
vio_webview_destroy($wv);
vio_destroy($ctx);
?>
--EXPECT--
{"page":1,"cookie":"XSRF-TOKEN=tok1; fromjs=1; old=1","server":"XSRF-TOKEN=tok1; fromjs=1; laravel_session=abc; old=1"}
{"page":2,"storage":"v1","cookie":"XSRF-TOKEN=tok1; fromjs=1","server":"XSRF-TOKEN=tok1; fromjs=1; laravel_session=abc"}
/ []
/check [XSRF-TOKEN=tok1; fromjs=1; laravel_session=abc; old=1]
/two [XSRF-TOKEN=tok1; fromjs=1; laravel_session=abc; old=1]
/drop [XSRF-TOKEN=tok1; fromjs=1; laravel_session=abc; old=1]
/check [XSRF-TOKEN=tok1; fromjs=1; laravel_session=abc]
