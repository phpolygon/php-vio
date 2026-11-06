--TEST--
System webview (WEBVIEW-PLAN W1/W3), JS <-> PHP bridge: window.vio.postMessage sends any JSON value (in order, Unicode intact), vio_webview_eval runs script in the page and the page answers, vio_webview_navigate loads another page; vio_webview_on_event reports navigated (URL), title and new_window (window.open / target=_blank, not opened by the engine)
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio') || !vio_platform_info()['webview']) die('skip no system webview on this platform');
?>
--FILE--
<?php
$ctx = vio_create('auto', ['width' => 320, 'height' => 240, 'headless' => true]);
$pages = [
    '/' => <<<'HTML'
<!doctype html><html><head><title>One</title><script>
window.addEventListener('load', () => {
  window.vio.postMessage({hello: 'wörld 😀', n: [1, 2.5, null, true]});
  window.vio.postMessage('plain');
  window.vio.postMessage(42);
  window.answer = (v) => window.vio.postMessage({answer: v * 2});
});
</script></head><body></body></html>
HTML,
    '/two' => <<<'HTML'
<!doctype html><html><head><title>Zwei</title><script>
window.addEventListener('load', () => {
  window.open('https://example.org/popup');
  window.vio.postMessage('two loaded');
});
</script></head><body></body></html>
HTML,
];
$handler = fn(array $req) => ['headers' => ['Content-Type' => 'text/html; charset=utf-8'],
                              'body' => $pages[parse_url($req['url'], PHP_URL_PATH)] ?? ''];
$msgs = [];
$events = [];
$wv = vio_webview($ctx, ['url' => 'app://localhost/', 'schemes' => ['app' => $handler]]);
vio_webview_on_message($wv, function (string $json) use (&$msgs, $wv) {
    $msgs[] = $json;
    if ($json === '42') vio_webview_eval($wv, 'window.answer(21)');
    if (str_contains($json, '"answer"')) vio_webview_navigate($wv, 'app://localhost/two');
});
vio_webview_on_event($wv, function (string $type, array $data) use (&$events) {
    $events[] = $type . ' ' . json_encode($data, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
});
$end = microtime(true) + 20;
while (microtime(true) < $end && !(in_array('"two loaded"', $msgs, true) && preg_grep('/^new_window /', $events) && preg_grep('/^title .*Zwei/', $events))) {
    vio_wait_events($ctx, 0.1);
}
echo implode("\n", $msgs), "\n";
$want = ['navigated {"url":"app://localhost/"}', 'title {"title":"One"}', 'navigated {"url":"app://localhost/two"}',
         'title {"title":"Zwei"}', 'new_window {"url":"https://example.org/popup"}'];
$got = array_values(array_unique(array_intersect($events, $want)));
$sorted = $got; sort($sorted); $w = $want; sort($w);
$ok = $sorted === $w && array_search($want[0], $got) < array_search($want[2], $got);
echo json_encode($ok ? 'events' : $events, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE), "\n";
vio_webview_destroy($wv);
vio_destroy($ctx);
?>
--EXPECT--
{"hello":"wörld 😀","n":[1,2.5,null,true]}
"plain"
42
{"answer":42}
"two loaded"
"events"
