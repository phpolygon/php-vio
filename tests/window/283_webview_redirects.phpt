--TEST--
System webview (WEBVIEW-PLAN W3), redirects on the custom scheme (Laravel answers a form POST with 302): fetch follows 302/303 as GET and 307 with method and body (Response.url stays the first URL: WebKit has no redirect for scheme tasks); a form POST that is redirected shows the target page (location.href, GET), a cookie set on the redirect reaches the target; a redirect loop ends with an error; a Location on another scheme is not followed
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio') || !vio_platform_info()['webview']) die('skip no system webview on this platform');
?>
--FILE--
<?php
$ctx = vio_create('auto', ['width' => 320, 'height' => 240, 'headless' => true]);
$page = <<<'HTML'
<!doctype html><html><head><script>
window.addEventListener('load', async () => {
  const r = {};
  let res = await fetch('/r302');
  r.r302 = [res.status, await res.text()];
  res = await fetch('/r303', {method: 'POST', body: 'x=1', headers: {'Content-Type': 'application/x-www-form-urlencoded'}});
  r.r303 = [res.status, await res.text()];
  res = await fetch('/r307', {method: 'POST', body: 'keep=me'});
  r.r307 = [res.status, await res.text()];
  try { res = await fetch('/loop'); r.loop = res.status; } catch (e) { r.loop = 'network error'; }
  res = await fetch('/away', {redirect: 'manual'});
  r.away = res.status === 302 || res.type === 'opaqueredirect' ? 'not followed' : res.status;
  window.vio.postMessage(r);
  document.forms[0].submit();
});
</script></head><body><form method="post" action="/login"><input name="user" value="vio"></form></body></html>
HTML;
$log = [];
$handler = function (array $req) use ($page, &$log): array|string {
    $path = parse_url($req['url'], PHP_URL_PATH);
    $cookie = array_change_key_case($req['headers'])['cookie'] ?? '';
    $log[] = "$req[method] $path" . ($req['body'] !== '' ? " body=$req[body]" : '') . ($cookie !== '' ? " cookie=$cookie" : '');
    return match ($path) {
        '/' => ['headers' => ['Content-Type' => 'text/html'], 'body' => $page],
        '/r302' => ['status' => 302, 'headers' => ['Location' => '/target']],
        '/r303' => ['status' => 303, 'headers' => ['Location' => 'app://localhost/target?from=303']],
        '/r307' => ['status' => 307, 'headers' => ['Location' => 'echo']],
        '/echo' => "$req[method] $req[body]",
        '/target' => "target $req[method]",
        '/loop' => ['status' => 302, 'headers' => ['Location' => '/loop']],
        '/away' => ['status' => 302, 'headers' => ['Location' => 'https://example.org/']],
        '/login' => ['status' => 302, 'headers' => ['Location' => '/home', 'Set-Cookie' => 'session=s1; path=/; httponly']],
        '/home' => ['headers' => ['Content-Type' => 'text/html'],
                    'body' => '<script>window.vio.postMessage({home: location.href})</script>'],
        default => ['status' => 404],
    };
};
$msgs = [];
$wv = vio_webview($ctx, ['url' => 'app://localhost/', 'schemes' => ['app' => $handler]]);
vio_webview_on_message($wv, function (string $json) use (&$msgs) { $msgs[] = $json; });
$end = microtime(true) + 60;
while (count($msgs) < 2 && microtime(true) < $end) vio_wait_events($ctx, 0.1);
echo implode("\n", $msgs), "\n";
echo implode("\n", array_filter($log, fn($l) => !str_starts_with($l, 'GET /loop'))), "\n";
echo 'loop requests ', count(array_filter($log, fn($l) => str_starts_with($l, 'GET /loop'))), "\n";
vio_webview_destroy($wv);
vio_destroy($ctx);
?>
--EXPECT--
{"r302":[200,"target GET"],"r303":[200,"target GET"],"r307":[200,"POST keep=me"],"loop":"network error","away":"not followed"}
{"home":"app://localhost/home"}
GET /
GET /r302
GET /target
POST /r303 body=x=1
GET /target
POST /r307 body=keep=me
POST /echo body=keep=me
GET /away
POST /login body=user=vio
GET /home cookie=session=s1
loop requests 21
