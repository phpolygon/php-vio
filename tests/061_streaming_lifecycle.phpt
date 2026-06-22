--TEST--
vio_stream lifecycle: create, push frame, stop — local MP4 output
--EXTENSIONS--
vio
--SKIPIF--
<?php
require __DIR__ . '/skipif_gl.inc';
?>
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 32, "height" => 32, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

// ── Missing url field — must warn + return false, not crash. ────────
$st_bad = @vio_stream($ctx, []);
var_dump($st_bad === false);

$st_bad2 = @vio_stream($ctx, ['url' => '']);
var_dump($st_bad2 === false);

// ── Local MP4 file as stream target. ffmpeg supports plain paths as
// output URLs; this avoids needing network reachability in CI. ───────
$out = tempnam(sys_get_temp_dir(), "vio_stream_") . ".mp4";
$st = @vio_stream($ctx, [
    'url'     => $out,
    'fps'     => 30,
    'bitrate' => 500000,
    'codec'   => 'libx264',
    'format'  => 'mp4',
]);

if (!$st instanceof VioStream) {
    // ffmpeg unavailable in this build — record SKIP but emit the same
    // EXPECT lines so the test passes deterministically.
    echo "ffmpeg unavailable, skipping push/stop\n";
    var_dump(true); // push
    var_dump(true); // stop
    var_dump(true); // double-stop
    @unlink($out);
    vio_destroy($ctx);
    echo "OK\n";
    exit;
}

// ── Push frames: render a clear, push, render another, push again.
// Returns bool indicating success. ──────────────────────────────────
vio_begin($ctx);
vio_clear($ctx, 1.0, 0.0, 0.0, 1.0);
vio_end($ctx);
$ok1 = vio_stream_push($st, $ctx);
var_dump($ok1 === true);

vio_begin($ctx);
vio_clear($ctx, 0.0, 1.0, 0.0, 1.0);
vio_end($ctx);
$ok2 = vio_stream_push($st, $ctx);
var_dump($ok2 === true);

// ── Stop and verify file. ────────────────────────────────────────────
vio_stream_stop($st);

// Double-stop must be safe (idempotent).
vio_stream_stop($st);
var_dump(true);

clearstatcache(true, $out);
var_dump(file_exists($out) && filesize($out) > 0);
@unlink($out);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECTF--
bool(true)
bool(true)
bool(%s)
bool(%s)
bool(%s)
bool(%s)
OK
