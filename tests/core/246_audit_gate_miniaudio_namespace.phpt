--TEST--
Audit gate: php-vio's miniaudio is renamed to vio_ma_* (src/vio_miniaudio_ns.h matches vendor/miniaudio/miniaudio.h) and only reached through src/vio_miniaudio.h
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
if (!is_file(__DIR__ . '/../../vendor/miniaudio/miniaudio.h')) die('skip source tree not available (installed extension)');
?>
--FILE--
<?php
/* The static runtimes link php-glfw into the same binary, and php-glfw compiles
 * its own, older miniaudio under the plain ma_* names. Before the rename, the
 * linker bound php-vio's ma_* calls to that copy - structs of another layout
 * than php-vio's header (ma_engine 1288 vs 1360 bytes, ma_sound 952 vs 1024,
 * ma_engine_config 160 vs 264 on 64-bit). Three things keep that from coming
 * back silently:
 *   1. src/vio_miniaudio_ns.h renames every external symbol (MA_API
 *      functions, globals) of exactly the vendored miniaudio.h (stale after a
 *      vendor update -> this test fails, and vio_miniaudio.h #errors on a
 *      version mismatch);
 *   2. no php-vio source includes miniaudio.h except through vio_miniaudio.h;
 *   3. the implementation is compiled from src/ (static-php-cli strips vio's
 *      vendor/* sources that php-glfw also ships), never from vendor/miniaudio.
 * Paths use forward slashes (see test 070 for why). */
function norm(string $p): string { return str_replace('\\', '/', $p); }
$root = norm(realpath(__DIR__ . '/../..'));

/* 1. rename list == external symbols of the vendored header (MA_API
 *    functions + non-static globals such as ma_atomic_global_lock) */
$hdr = str_replace("\r\n", "\n", file_get_contents("$root/vendor/miniaudio/miniaudio.h"));
preg_match_all('/^MA_API\b[^;{(]*?\b(ma_\w+)\s*\(/m', $hdr, $m);
preg_match_all('/^ma_\w+\s+(ma_\w+)\s*;/m', $hdr, $v);
$want = array_values(array_unique(array_merge($m[1], $v[1])));
sort($want);
$ns = str_replace("\r\n", "\n", file_get_contents("$root/src/vio_miniaudio_ns.h"));
preg_match_all('/^#define (ma_\w+) vio_(ma_\w+)$/m', $ns, $d, PREG_SET_ORDER);
$have = [];
$bad = [];
foreach ($d as $row) {
    if ($row[1] !== $row[2]) $bad[] = $row[1];
    $have[] = $row[1];
}
sort($have);
$missing = array_diff($want, $have);
$extra = array_diff($have, $want);
echo "symbols: ", count($want) > 1000 ? 'many' : count($want), "\n";
echo "missing: ", $missing ? implode(',', array_slice($missing, 0, 10)) : 'none', "\n";
echo "extra: ", $extra ? implode(',', array_slice($extra, 0, 10)) : 'none', "\n";
echo "bad: ", $bad ? implode(',', $bad) : 'none', "\n";
foreach (['MAJOR', 'MINOR', 'REVISION'] as $k) {
    preg_match('/^#define MA_VERSION_' . $k . '\s+(\d+)/m', $hdr, $a);
    preg_match('/^#define VIO_MINIAUDIO_NS_VERSION_' . $k . '\s+(\d+)/m', $ns, $b);
    if (($a[1] ?? 'x') !== ($b[1] ?? 'y')) echo "version $k differs: header ", $a[1] ?? '?', " list ", $b[1] ?? '?', "\n";
}

/* 2. miniaudio.h only through vio_miniaudio.h */
$files = ["$root/php_vio.c", "$root/php_vio.h"];
foreach (["$root/include", "$root/src"] as $dir) {
    foreach (new RecursiveIteratorIterator(new RecursiveDirectoryIterator($dir, FilesystemIterator::SKIP_DOTS)) as $f) {
        if ($f->isFile() && preg_match('/\.(c|m|h|cpp)$/', $f->getFilename())) $files[] = norm($f->getPathname());
    }
}
$direct = [];
foreach ($files as $path) {
    if ($path === "$root/src/vio_miniaudio.h") continue;
    if (preg_match('#^\s*\#\s*include\s*[<"](?:[^>"]*/)?miniaudio\.h[>"]#m', file_get_contents($path))) {
        $direct[] = substr($path, strlen($root) + 1);
    }
}
echo "direct includes: ", $direct ? implode(',', $direct) : 'none', "\n";

/* 3. the implementation is built from src/, nowhere else */
foreach (['config.m4', 'config.w32'] as $cfg) {
    $c = norm(file_get_contents("$root/$cfg"));
    echo "$cfg: ", str_contains($c, 'src//vio_miniaudio_impl.c') || str_contains($c, 'src/vio_miniaudio_impl.c') ? 'impl' : 'NO impl',
        preg_match('#vendor/+miniaudio/+\w+\.c#', $c) ? ' + vendor .c' : '', "\n";
}
echo is_file("$root/vendor/miniaudio/miniaudio_impl.c") ? "vendor impl present\n" : "no vendor impl\n";

/* the renamed build still plays its part: the audio API is there */
var_dump(function_exists('vio_audio_load'), class_exists('VioSound'));
?>
--EXPECT--
symbols: many
missing: none
extra: none
bad: none
direct includes: none
config.m4: impl
config.w32: impl
no vendor impl
bool(true)
bool(true)
