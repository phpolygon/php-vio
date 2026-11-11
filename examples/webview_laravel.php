<?php
/**
 * A Laravel app in a system webview, in one PHP process: no HTTP server, no
 * Electron. Requests to app://localhost go through the app's HTTP kernel;
 * files under public/ are served directly.
 *
 * Run with: php -d extension=modules/vio.so examples/webview_laravel.php /path/to/laravel-app
 *           ... --smoke   (headless: load '/', print status and title, exit)
 *
 * Limits of this example: multipart uploads are not parsed, streamed responses
 * are sent once they are complete. A NativePHP driver package does the same
 * with the full request mapping.
 */

use Illuminate\Contracts\Http\Kernel;
use Illuminate\Http\Request;
use Symfony\Component\HttpFoundation\BinaryFileResponse;
use Symfony\Component\HttpFoundation\StreamedResponse;

$appPath = $argv[1] ?? '';
$smoke = in_array('--smoke', $argv, true);
if (!is_file("$appPath/bootstrap/app.php")) {
    fwrite(STDERR, "usage: php examples/webview_laravel.php /path/to/laravel-app [--smoke]\n");
    exit(1);
}
if (!vio_platform_info()['webview']) {
    fwrite(STDERR, 'No system webview on this platform (' . vio_platform() . ")\n");
    exit(1);
}

require "$appPath/vendor/autoload.php";
$app = require "$appPath/bootstrap/app.php";
$kernel = $app->make(Kernel::class);
$kernel->bootstrap();

$public = realpath("$appPath/public");
$types = ['css' => 'text/css', 'js' => 'text/javascript', 'mjs' => 'text/javascript', 'json' => 'application/json',
          'svg' => 'image/svg+xml', 'png' => 'image/png', 'jpg' => 'image/jpeg', 'jpeg' => 'image/jpeg', 'gif' => 'image/gif',
          'webp' => 'image/webp', 'ico' => 'image/x-icon', 'woff' => 'font/woff', 'woff2' => 'font/woff2', 'txt' => 'text/plain'];

$handler = function (array $req) use ($app, $kernel, $public, $types): array {
    $path = rawurldecode(parse_url($req['url'], PHP_URL_PATH) ?: '/');
    $query = parse_url($req['url'], PHP_URL_QUERY) ?: '';

    /* public/ files, never PHP and never outside it */
    $file = $path !== '/' ? realpath($public . $path) : false;
    if ($file && is_file($file) && str_starts_with($file, $public . DIRECTORY_SEPARATOR) && !str_ends_with($file, '.php')) {
        $ext = strtolower(pathinfo($file, PATHINFO_EXTENSION));
        return ['headers' => ['Content-Type' => $types[$ext] ?? 'application/octet-stream'], 'body' => file_get_contents($file)];
    }

    $server = ['HTTP_HOST' => 'localhost', 'SERVER_NAME' => 'localhost', 'SERVER_PORT' => 80, 'REMOTE_ADDR' => '127.0.0.1'];
    $cookies = [];
    foreach ($req['headers'] as $name => $value) {
        $key = strtoupper(str_replace('-', '_', $name));
        if ($key === 'CONTENT_TYPE' || $key === 'CONTENT_LENGTH') $server[$key] = $value;
        else $server["HTTP_$key"] = $value;
        if ($key === 'COOKIE') {
            foreach (explode(';', $value) as $pair) {
                [$k, $v] = array_pad(explode('=', trim($pair), 2), 2, '');
                if ($k !== '') $cookies[$k] = urldecode($v);
            }
        }
    }
    $params = [];
    if (str_starts_with($server['CONTENT_TYPE'] ?? '', 'application/x-www-form-urlencoded')) parse_str($req['body'], $params);
    $request = Request::create('http://localhost' . $path . ($query !== '' ? "?$query" : ''), $req['method'], $params,
                               $cookies, [], $server, $req['body']);

    /* links and assets the app generates must stay on the scheme */
    $app->instance('request', $request);
    $app['url']->forceRootUrl('app://localhost');
    $app['url']->forceScheme('app');
    $response = $kernel->handle($request);
    if ($response instanceof StreamedResponse) {
        ob_start();
        $response->sendContent();
        $body = ob_get_clean();
    } elseif ($response instanceof BinaryFileResponse) {
        $body = file_get_contents($response->getFile()->getPathname());
    } else {
        $body = $response->getContent();
    }
    $headers = $response->headers->allPreserveCaseWithoutCookies();
    $headers['Set-Cookie'] = array_map('strval', $response->headers->getCookies());
    $kernel->terminate($request, $response);
    return ['status' => $response->getStatusCode(), 'headers' => $headers, 'body' => (string)$body];
};

$ctx = vio_create('auto', ['width' => 1100, 'height' => 760, 'title' => 'Laravel', 'headless' => $smoke]);
$wv = vio_webview($ctx, ['url' => 'app://localhost/', 'devtools' => true, 'schemes' => ['app' => function (array $req) use ($handler, $smoke) {
    $res = $handler($req);
    if ($smoke) echo $req['method'], ' ', $req['url'], ' -> ', ($res['status'] ?? 200), "\n";
    return $res;
}]]);

$title = null;
vio_webview_on_event($wv, function (string $type, array $data) use ($ctx, &$title) {
    if ($type !== 'title') return;
    $title = $data['title'];
    vio_set_title($ctx, $title);
});

$end = microtime(true) + 15;
while (!vio_should_close($ctx)) {
    vio_wait_events($ctx, $smoke ? 0.1 : -1.0);
    if ($smoke && ($title !== null || microtime(true) > $end)) break;
}
if ($smoke) echo 'title: ', $title ?? '(none)', "\n";
vio_webview_destroy($wv);
vio_destroy($ctx);
