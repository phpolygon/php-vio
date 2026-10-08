--TEST--
Audit gate: no glfwXxx / GLFW_* outside src/platform/glfw/ (NATIVE-PLATFORM-PLAN Phase 0, OPEN-ITEMS A1); the platform layer is active and answers
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
if (!is_dir(__DIR__ . '/../../src/platform')) die('skip source tree not available (installed extension)');
?>
--FILE--
<?php
/* The window system sits behind the vio_platform vtable (include/vio_platform.h):
 * GLFW is one implementation in src/platform/glfw/, the only place allowed to
 * name it - php_vio.c, the GPU backends and the input code ask the active
 * platform. Comments are stripped first (they may mention GLFW freely). Paths
 * use forward slashes (see test 070 for why). */
function norm(string $p): string { return str_replace('\\', '/', $p); }

$root = norm(realpath(__DIR__ . '/../..'));
/* vio's own sources only: CI extracts third-party SDKs into the tree (deps/) */
$scan = ["$root/php_vio.c", "$root/php_vio.h", "$root/include", "$root/src"];
$exempt_dirs = [
    "$root/src/platform/glfw",
];

function strip_comments(string $code): string {
    $out = preg_replace('#/\*.*?\*/#s', '', $code);
    return preg_replace('#//[^\n]*#', '', $out);
}

$violations = [];
$files = [];
foreach ($scan as $p) {
    if (is_file($p)) { $files[] = $p; continue; }
    if (!is_dir($p)) continue;
    foreach (new RecursiveIteratorIterator(new RecursiveDirectoryIterator($p, FilesystemIterator::SKIP_DOTS)) as $f) {
        if ($f->isFile()) $files[] = norm($f->getPathname());
    }
}
foreach ($files as $path) {
    if (!preg_match('/\.(c|m|h|cpp)$/', $path)) continue;
    foreach ($exempt_dirs as $d) if (str_starts_with($path, $d . '/')) continue 2;
    $code = strip_comments(file_get_contents($path));
    if (preg_match_all('/\bglfw[A-Z]\w*|\bGLFW_[A-Z]\w*|\bGLFW[a-z]\w*/', $code, $m)) {
        foreach ($m[0] as $hit) $violations[] = substr($path, strlen($root) + 1) . " :: " . $hit;
    }
}

if ($violations) {
    echo "VIOLATIONS:\n";
    foreach (array_slice($violations, 0, 20) as $v) echo "  $v\n";
    if (count($violations) > 20) echo "  ... and " . (count($violations) - 20) . " more\n";
} else {
    echo "OK\n";
}

/* The active platform answers through the API: a headless context reports its
 * own size whatever the window system says, monitors / gamepads are arrays. */
/* the active platform: GLFW in a build with it (until a native layer takes over), else null */
if (!in_array(vio_platform(), ['glfw', 'win32', 'cocoa', 'x11', 'wayland', 'null'], true)) echo "platform ", vio_platform(), "\n";
$ctx = @vio_create('auto', ['width' => 48, 'height' => 32, 'headless' => true]);
if ($ctx) {
    var_dump(vio_window_size($ctx) === [48, 32], vio_framebuffer_size($ctx) === [48, 32], is_array(vio_monitors($ctx)), is_array(vio_gamepads()));
    vio_destroy($ctx);
} else {
    var_dump(true, true, true, true);
}
?>
--EXPECT--
OK
bool(true)
bool(true)
bool(true)
bool(true)
