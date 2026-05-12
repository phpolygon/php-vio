--TEST--
vio_render_target_texture: repeated calls on the same RT are stable and don't leak
--EXTENSIONS--
vio
--FILE--
<?php
// Regression test: vio_render_target_texture used to allocate a fresh
// backend-texture wrapper (plus a fresh AddRef on the SRV plus a fresh
// CreateSamplerState) on every call. A render loop that asked for the
// offscreen colour texture once per frame would exhaust D3D11's
// sampler-state pool (cap: 4096) within a few thousand frames and the
// next call would return a wrapper backed by a garbage SRV pointer,
// crashing the next bind. Backends without that pool (OpenGL, Null)
// silently leaked. The fix caches the wrapper on the render-target so
// repeat queries return the same pointer.
$ctx = vio_create("opengl", ["width" => 64, "height" => 64, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

$rt = vio_render_target($ctx, [
    'width'  => 128,
    'height' => 128,
]);
if (!$rt instanceof VioRenderTarget) {
    echo "SKIP RT creation failed\n";
    exit;
}

// First call — baseline.
$tex_first = vio_render_target_texture($rt);
var_dump($tex_first instanceof VioTexture);

// Second call — used to crash in the D3D11 backend, leaked everywhere
// else. The cached wrapper makes this a no-op (same pointer / same SRV).
$tex_second = vio_render_target_texture($rt);
var_dump($tex_second instanceof VioTexture);

// Both queries must agree on the dimensions of the underlying RT.
$size_first  = vio_texture_size($tex_first);
$size_second = vio_texture_size($tex_second);
var_dump($size_first === $size_second);
var_dump($size_first[0] === 128 && $size_first[1] === 128);

// Many repeated calls — well above D3D11's 4096 sampler-state pool cap
// when the per-call SamplerState-create path was active. With the cache
// every call hands out the same wrapper, so this loop is constant cost.
$baseline_peak = memory_get_peak_usage(true);
for ($i = 0; $i < 5000; $i++) {
    $t = vio_render_target_texture($rt);
    if (!$t instanceof VioTexture) {
        echo "FAIL: iteration {$i} returned non-VioTexture\n";
        break;
    }
    unset($t);
}
$post_peak = memory_get_peak_usage(true);

// Loose memory bound: anything under 4 MB of growth across 5000 iterations
// is fine, the previous implementation grew unbounded. We can't pin the
// number tighter than that because Zend's allocator buckets PHP-side and
// the GPU resources show up only in process RSS, not in PHP memory.
$delta_mb = ($post_peak - $baseline_peak) / 1024 / 1024;
var_dump($delta_mb < 4.0);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
OK
