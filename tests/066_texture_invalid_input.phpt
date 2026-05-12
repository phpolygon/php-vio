--TEST--
vio_texture: corrupt PNG, wrong size, missing file — graceful failure
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create('null');

// ── Nonexistent file ───────────────────────────────────────────────
$tex = @vio_texture($ctx, ['file' => '/no/such/file.png']);
var_dump($tex === false);

// ── Empty path ─────────────────────────────────────────────────────
$tex = @vio_texture($ctx, ['file' => '']);
var_dump($tex === false);

// ── Garbage bytes that look nothing like a PNG ─────────────────────
$path = tempnam(sys_get_temp_dir(), 'vio_garbage_') . '.png';
file_put_contents($path, "this is not a png — just plain text");
$tex = @vio_texture($ctx, ['file' => $path]);
var_dump($tex === false);
unlink($path);

// ── Truncated PNG (valid magic header, no IHDR chunk) ──────────────
$path = tempnam(sys_get_temp_dir(), 'vio_truncated_') . '.png';
file_put_contents($path, "\x89PNG\r\n\x1a\n"); // PNG signature only
$tex = @vio_texture($ctx, ['file' => $path]);
var_dump($tex === false);
unlink($path);

// ── PNG signature + IHDR with absurd dimensions ────────────────────
$path = tempnam(sys_get_temp_dir(), 'vio_huge_') . '.png';
$ihdr = pack("N", 13) . "IHDR"
      . pack("NN", 1 << 30, 1 << 30)  // 1B × 1B pixels
      . "\x08\x06\x00\x00\x00"
      . pack("N", 0); // bogus CRC
file_put_contents($path, "\x89PNG\r\n\x1a\n" . $ihdr);
$tex = @vio_texture($ctx, ['file' => $path]);
var_dump($tex === false);
unlink($path);

// ── Raw data mode: data length mismatches width × height × 4 ───────
$tex = @vio_texture($ctx, [
    'data'   => "abc",        // only 3 bytes
    'width'  => 4,
    'height' => 4,             // expects 64 bytes
]);
// Either rejects with false/null, or accepts and pads — implementation
// choice. We assert only "no crash, deterministic return shape".
var_dump($tex === false || $tex === null || $tex instanceof VioTexture);

// ── Zero / negative dimensions: documented loose contract — either
// reject (false/null) or accept with degenerate state. We just require
// "no crash, return a known sentinel or a VioTexture", never garbage. ─
$tex = @vio_texture($ctx, ['data' => "\x00\x00\x00\x00", 'width' => 0, 'height' => 1]);
var_dump($tex === false || $tex === null || $tex instanceof VioTexture);

$tex = @vio_texture($ctx, ['data' => "\x00\x00\x00\x00", 'width' => 1, 'height' => 0]);
var_dump($tex === false || $tex === null || $tex instanceof VioTexture);

$tex = @vio_texture($ctx, ['data' => "\x00\x00\x00\x00", 'width' => -1, 'height' => 1]);
var_dump($tex === false || $tex === null || $tex instanceof VioTexture);

// ── Missing required keys ──────────────────────────────────────────
$tex = @vio_texture($ctx, ['data' => "\xff\xff\xff\xff"]); // no width/height
var_dump($tex === false);

$tex = @vio_texture($ctx, ['width' => 1, 'height' => 1]); // no data
var_dump($tex === false);

// ── Empty options array ────────────────────────────────────────────
$tex = @vio_texture($ctx, []);
var_dump($tex === false);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
OK
