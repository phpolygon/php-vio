--TEST--
System webview (WEBVIEW-PLAN W3), streamed and large answers: an iterable body reaches the page chunk by chunk (one per poll, the page reads them before the stream ends, the PHP loop keeps running), Server-Sent Events over the custom scheme, a generator that throws ends its stream; a 60 MB answer and a 20 MB POST body arrive whole
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
  /* chunks: the page sees the first one while PHP has not produced the last */
  let res = await fetch('/chunks');
  const reader = res.body.getReader();
  const dec = new TextDecoder();
  let text = '', reads = 0, early = null;
  for (;;) {
    const {value, done} = await reader.read();
    if (done) break;
    reads++;
    text += dec.decode(value, {stream: true});
    if (early === null) { early = (await (await fetch('/progress')).text()); }
  }
  r.chunks = [text, reads > 1, early];
  /* SSE */
  r.sse = await new Promise((resolve) => {
    const got = [];
    const es = new EventSource('/events');
    es.onmessage = (e) => { got.push(e.data); if (got.length === 3) { es.close(); resolve(got); } };
    es.onerror = () => { es.close(); resolve(got.length ? got : 'error'); };
  });
  /* a generator that throws: what came before it arrives, then the stream ends */
  try { res = await fetch('/broken'); r.broken = await res.text(); } catch (e) { r.broken = 'network error'; }
  /* big */
  res = await fetch('/big');
  const big = new Uint8Array(await res.arrayBuffer());
  r.big = [big.length, big[0], big[big.length - 1]];
  const up = new Uint8Array(20 * 1024 * 1024); up[0] = 7; up[up.length - 1] = 9;
  res = await fetch('/upload', {method: 'POST', body: up});
  r.upload = await res.text();
  window.vio.postMessage(r);
});
</script></head><body></body></html>
HTML;
$produced = 0;
$polls = 0;
$handler = function (array $req) use ($page, &$produced) {
    switch (parse_url($req['url'], PHP_URL_PATH)) {
    case '/': return ['headers' => ['Content-Type' => 'text/html'], 'body' => $page];
    case '/chunks':
        return ['headers' => ['Content-Type' => 'text/plain'], 'body' => (function () use (&$produced) {
            foreach (['a', 'b', 'c', 'd', 'e'] as $c) { $produced++; yield $c; }
        })()];
    case '/progress': return (string)$produced;   /* answered while /chunks is still streaming */
    case '/events':
        return ['headers' => ['Content-Type' => 'text/event-stream', 'Cache-Control' => 'no-cache'],
                'body' => (function () { foreach (['one', 'two', 'three'] as $m) yield "data: $m\n\n"; })()];
    case '/broken':
        return ['headers' => ['Content-Type' => 'text/plain'], 'body' => (function () {
            yield 'part1';
            throw new LogicException('generator failed');
        })()];
    case '/big': return ['headers' => ['Content-Type' => 'application/octet-stream'], 'body' => "\x01" . str_repeat("\0", 60 * 1024 * 1024 - 2) . "\x02"];
    case '/upload': return strlen($req['body']) . ' ' . ord($req['body'][0]) . ' ' . ord($req['body'][-1]);
    }
    return ['status' => 404];
};
$result = null;
$wv = vio_webview($ctx, ['url' => 'app://localhost/', 'schemes' => ['app' => $handler]]);
vio_webview_on_message($wv, function (string $json) use (&$result) { $result = $json; });
$end = microtime(true) + 30;
while ($result === null && microtime(true) < $end) {
    $polls++;
    try { vio_wait_events($ctx, 0.1); } catch (LogicException $e) { echo 'exception ', $e->getMessage(), "\n"; }
}
$r = json_decode((string)$result, true);
echo json_encode($r, JSON_UNESCAPED_SLASHES), "\n";
echo 'early ', json_encode(isset($r['chunks'][2]) && (int)$r['chunks'][2] < 5), "\n";
echo 'polls ', json_encode($polls > 5), "\n";
vio_webview_destroy($wv);
vio_destroy($ctx);
?>
--EXPECTF--
exception generator failed
{"chunks":["abcde",true,"%d"],"sse":["one","two","three"],"broken":"part1","big":[62914560,1,2],"upload":"20971520 7 9"}
early true
polls true
