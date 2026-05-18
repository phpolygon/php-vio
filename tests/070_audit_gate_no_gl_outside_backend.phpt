--TEST--
Audit gate: no glXxx() or GL_* references outside src/backends/opengl/
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
$src_root = __DIR__ . '/..';
if (!is_dir("$src_root/src/backends/opengl")) {
    die('skip source tree not available (installed extension)');
}
?>
--FILE--
<?php
/* Walks the source tree once and verifies that no functional OpenGL call
 * leaks out of src/backends/opengl/. This is the post-Phase-1 contract for
 * the OpenGL backend split — every glXxx() / GL_* token must be fenced in
 * its own backend directory. Header files in include/ are exempt because
 * they only declare the cross-backend vtable.
 *
 * The test grep-pattern is intentionally generous: any identifier matching
 * /gl[A-Z][a-zA-Z]+\(/ or /GL_[A-Z]/. False positives in comments are
 * stripped via a second pass. */

$root = realpath(__DIR__ . '/..');
$exempt_dirs = [
    "$root/src/backends/opengl",
    "$root/vendor",
];
$exempt_files = [
    "$root/src/vio_window.c",       // GLFW window/context setup (lib-level)
    "$root/include/vio_backend.h",  // vtable type declarations
    "$root/include/vio_types.h",    // enum / struct declarations
    "$root/include/vio_plugin.h",   // plugin vtable
];

function is_exempt(string $path, array $dirs, array $files): bool {
    foreach ($files as $f) if ($path === $f) return true;
    foreach ($dirs as $d) if (str_starts_with($path, $d . '/')) return true;
    return false;
}

function strip_comments(string $code): string {
    // Strip /* ... */ block comments (single line of multi-line).
    $out = preg_replace('#/\*.*?\*/#s', '', $code);
    // Strip // line comments.
    $out = preg_replace('#//[^\n]*#', '', $out);
    return $out;
}

$violations = [];
$it = new RecursiveIteratorIterator(new RecursiveDirectoryIterator($root,
    FilesystemIterator::SKIP_DOTS));

foreach ($it as $file) {
    if (!$file->isFile()) continue;
    $path = $file->getPathname();
    if (!preg_match('/\.(c|m|h|cpp)$/', $path)) continue;
    if (is_exempt($path, $exempt_dirs, $exempt_files)) continue;

    $code = strip_comments(file_get_contents($path));
    if (preg_match_all('/\bgl[A-Z][a-zA-Z]+\s*\(|\bGL_[A-Z][A-Z0-9_]*/', $code, $m)) {
        foreach ($m[0] as $hit) {
            $violations[] = substr($path, strlen($root) + 1) . " :: " . $hit;
        }
    }
}

if (empty($violations)) {
    echo "OK\n";
} else {
    echo "VIOLATIONS:\n";
    foreach (array_slice($violations, 0, 20) as $v) echo "  $v\n";
    if (count($violations) > 20) echo "  ... and " . (count($violations) - 20) . " more\n";
}
?>
--EXPECT--
OK
