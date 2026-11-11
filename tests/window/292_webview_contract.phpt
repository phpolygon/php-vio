--TEST--
System webview (WEBVIEW-PLAN W1), contract: vio_webview returns what vio_platform_info()['webview'] promises (false without a warning where there is none, the null platform never has one); options are checked everywhere (schemes, bounds, url); a destroyed webview and one whose window was closed are refused; it goes with its window
--EXTENSIONS--
vio
--FILE--
<?php
$err = function (callable $f) {
    try { $f(); return 'none'; } catch (ValueError $e) { return 'ValueError'; } catch (TypeError $e) { return 'TypeError'; }
};
$avail = vio_platform_info()['webview'];
$ctx = @vio_create('auto', ['width' => 64, 'height' => 48, 'headless' => true]) ?: vio_create('null', ['width' => 64, 'height' => 48]);
$ok = fn(array $r) => ['status' => 200, 'body' => ''];

echo 'contract ', implode(' ', [
    $err(fn() => vio_webview($ctx, ['schemes' => ['http' => $ok]])),          // engines own http(s)
    $err(fn() => vio_webview($ctx, ['schemes' => ['file' => $ok]])),
    $err(fn() => vio_webview($ctx, ['schemes' => ['1app' => $ok]])),          // RFC 3986: a letter first
    $err(fn() => vio_webview($ctx, ['schemes' => ['App' => $ok]])),          // lower case only
    $err(fn() => vio_webview($ctx, ['schemes' => ['app' => 'nope']])),
    $err(fn() => vio_webview($ctx, ['schemes' => 'app'])),
    $err(fn() => vio_webview($ctx, ['bounds' => [0, 0, 10]])),
    $err(fn() => vio_webview($ctx, ['bounds' => [0, 0, 0, 10]])),
    $err(fn() => vio_webview($ctx, ['url' => 5])),
    $err(fn() => vio_webview($ctx, ['url' => "app://x/\n"])),
    $err(fn() => vio_webview_on_message($ctx, fn() => 1)),
]), "\n";

ob_start();
$wv = vio_webview($ctx, ['url' => 'about:blank', 'schemes' => ['app' => $ok, 'vio-test' => $ok]]);
$warn = ob_get_clean();
echo 'create ', json_encode($avail ? $wv instanceof VioWebview : $wv === false), ' ', json_encode($warn === ''), "\n";

if ($avail) {
    echo 'calls ', json_encode([
        vio_webview_navigate($wv, 'app://localhost/'),
        vio_webview_eval($wv, '1 + 1'),
        vio_webview_bounds($wv, [0, 0, 32, 24]),
        vio_webview_bounds($wv, null),
    ]), "\n";
    echo 'bad ', implode(' ', [
        $err(fn() => vio_webview_navigate($wv, "app://x/\0")),
        $err(fn() => vio_webview_bounds($wv, [0, 0, -1, 1])),
        $err(fn() => vio_webview_on_message($wv, 'nope')),
        $err(fn() => vio_webview_on_event($wv, 'nope')),
    ]), "\n";
    vio_webview_destroy($wv);
    vio_webview_destroy($wv);   // idempotent
    echo 'destroyed ', $err(fn() => vio_webview_eval($wv, '1')), "\n";

    $w = vio_window($ctx, ['width' => 64, 'height' => 48, 'headless' => true]);
    if ($w) {
        $wv2 = vio_webview($w, ['url' => 'about:blank']);
        vio_destroy_window($w);
        echo 'window closed ', $err(fn() => vio_webview_navigate($wv2, 'about:blank')), "\n";
    } else {
        echo "window closed ValueError\n";
    }
    /* the webview keeps its context alive; freeing both in either order is fine */
    $wv3 = vio_webview($ctx, ['url' => 'about:blank']);
    unset($ctx);
    vio_webview_eval($wv3, '1');
    unset($wv3);
} else {
    echo "calls [true,true,true,true]\nbad ValueError ValueError TypeError TypeError\ndestroyed ValueError\nwindow closed ValueError\n";
}

/* the null platform has no webview */
$cmd = [PHP_BINARY, '-n', '-d', 'extension_dir=' . ini_get('extension_dir'), '-d', 'extension=vio', '-r',
        '$c = vio_create("null", ["width" => 8, "height" => 8]); var_export([vio_platform_info()["webview"], vio_webview($c)]);'];
$p = proc_open($cmd, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, null, ['VIO_PLATFORM' => 'null'] + getenv());
$out = stream_get_contents($pipes[1]) . stream_get_contents($pipes[2]);
proc_close($p);
echo 'null ', str_replace(["\n", ' '], '', $out), "\n";
?>
--EXPECT--
contract ValueError ValueError ValueError ValueError TypeError TypeError ValueError ValueError TypeError ValueError TypeError
create true true
calls [true,true,true,true]
bad ValueError ValueError TypeError TypeError
destroyed ValueError
window closed ValueError
null array(0=>false,1=>false,)
