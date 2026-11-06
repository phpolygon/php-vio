--TEST--
System webview (WEBVIEW-PLAN W1), custom scheme served by PHP in the same process: a page loads HTML, JS, CSS and a PNG from the scheme callback, then fetch()es POST bodies (JSON and every byte value), reads status and response headers of a 404 and a binary answer, and a callback that throws answers 500; each request reaches PHP with method, URL, headers and body, only from vio_poll_events / vio_wait_events
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio') || !vio_platform_info()['webview']) die('skip no system webview on this platform');
?>
--FILE--
<?php
$ctx = vio_create('auto', ['width' => 320, 'height' => 240, 'headless' => true]);
$chunk = fn(string $t, string $d) => pack('N', strlen($d)) . $t . $d . pack('N', crc32($t . $d));
$png = "\x89PNG\r\n\x1a\n" . $chunk('IHDR', pack('NNCCCCC', 2, 3, 8, 2, 0, 0, 0))
     . $chunk('IDAT', gzcompress(str_repeat("\0" . str_repeat("\xff\0\0", 2), 3))) . $chunk('IEND', '');   // 2x3 red
$page = <<<'HTML'
<!doctype html><html><head><link rel="stylesheet" href="/style.css"><script src="/app.js"></script></head>
<body><p id="p">x</p><img id="img" src="/img/dot.png"></body></html>
HTML;
$js = <<<'JS'
window.addEventListener('load', async () => {
  const r = {};
  r.css = getComputedStyle(document.getElementById('p')).color;
  const img = document.getElementById('img');
  r.img = [img.naturalWidth, img.naturalHeight];
  let res = await fetch('/echo?q=1', {method: 'POST', headers: {'Content-Type': 'application/json', 'X-Test': 'yes-1'}, body: JSON.stringify({a: 1})});
  r.json = [res.status, res.headers.get('x-answer'), await res.text()];
  const all = new Uint8Array(256); for (let i = 0; i < 256; i++) all[i] = i;
  res = await fetch('/echo', {method: 'POST', body: all});
  r.bytes = [res.status, await res.text()];
  res = await fetch('/missing');
  r.missing = [res.status, await res.text()];
  res = await fetch('/bin');
  const b = new Uint8Array(await res.arrayBuffer());
  r.bin = [res.headers.get('content-type'), b.length, b[0], b[255]];
  res = await fetch('/throws');
  r.throws = res.status;
  window.vio.postMessage(r);
});
JS;
$log = [];
$inPoll = false;
$handler = function (array $req) use (&$log, &$inPoll, $page, $js, $png) {
    $path = parse_url($req['url'], PHP_URL_PATH);
    $log[] = $req['method'] . ' ' . $path . ($inPoll ? '' : ' (outside poll!)');
    $h = array_change_key_case($req['headers']);
    return match ($path) {
        '/' => ['headers' => ['Content-Type' => 'text/html; charset=utf-8'], 'body' => $page],
        '/app.js' => ['headers' => ['Content-Type' => 'text/javascript'], 'body' => $js],
        '/style.css' => ['headers' => ['Content-Type' => 'text/css'], 'body' => '#p { color: rgb(1, 2, 3); }'],
        '/img/dot.png' => ['headers' => ['Content-Type' => 'image/png'], 'body' => $png],
        '/echo' => ['headers' => ['Content-Type' => 'text/plain', 'X-Answer' => 'yes'],
                    'body' => json_encode([parse_url($req['url'], PHP_URL_QUERY), $h['x-test'] ?? null,
                                           strlen($req['body']), md5($req['body']) === md5(implode('', array_map('chr', range(0, 255)))) ? 'all bytes' : $req['body']],
                                          JSON_UNESCAPED_UNICODE)],
        '/bin' => ['headers' => ['Content-Type' => 'application/octet-stream'], 'body' => implode('', array_map('chr', range(0, 255)))],
        '/throws' => throw new RuntimeException('handler failed'),
        default => ['status' => 404, 'body' => 'not here'],
    };
};
$result = null;
$wv = vio_webview($ctx, ['url' => 'app://localhost/', 'schemes' => ['app' => $handler]]);
vio_webview_on_message($wv, function (string $json) use (&$result) { $result = $json; });
$end = microtime(true) + 20;
while ($result === null && microtime(true) < $end) {
    $inPoll = true;
    try { vio_wait_events($ctx, 0.1); } catch (RuntimeException $e) { echo 'exception ', $e->getMessage(), "\n"; }
    $inPoll = false;
}
echo $result === null ? "no message\n" : json_encode(json_decode($result), JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES), "\n";
sort($log);
echo implode("\n", $log), "\n";
vio_webview_destroy($wv);
vio_destroy($ctx);
?>
--EXPECT--
exception handler failed
{
    "css": "rgb(1, 2, 3)",
    "img": [
        2,
        3
    ],
    "json": [
        200,
        "yes",
        "[\"q=1\",\"yes-1\",7,\"{\\\"a\\\":1}\"]"
    ],
    "bytes": [
        200,
        "[null,null,256,\"all bytes\"]"
    ],
    "missing": [
        404,
        "not here"
    ],
    "bin": [
        "application/octet-stream",
        256,
        0,
        255
    ],
    "throws": 500
}
GET /
GET /app.js
GET /bin
GET /img/dot.png
GET /missing
GET /style.css
GET /throws
POST /echo
POST /echo
