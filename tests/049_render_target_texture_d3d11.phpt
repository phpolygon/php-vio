--TEST--
vio_render_target_texture on D3D11: cached wrapper survives sampler-pool pressure
--SKIPIF--
<?php
if (PHP_OS_FAMILY !== 'Windows') die('skip Windows only');
if (!in_array('d3d11', vio_backends(), true)) die('skip D3D11 backend not compiled in');
?>
--EXTENSIONS--
vio
--FILE--
<?php
// D3D11-specific regression: each vio_render_target_texture call used to
// create a fresh ID3D11SamplerState. The runtime caps that pool at 4096,
// after which CreateSamplerState starts returning E_OUTOFMEMORY and the
// returned wrapper held a NULL sampler pointer that crashed on bind.
// Engines that query the offscreen colour texture once per frame (FXAA
// blit, render-scale present) hit the cap inside a minute of gameplay.
$ctx = @vio_create('d3d11', ['width' => 64, 'height' => 64, 'headless' => true]);
if (!$ctx instanceof VioContext) {
    echo "SKIP: WARP unavailable\n";
    exit;
}

$rt = vio_render_target($ctx, ['width' => 128, 'height' => 128]);
if (!$rt instanceof VioRenderTarget) {
    echo "SKIP: RT creation failed on D3D11\n";
    exit;
}

// 5000 calls — comfortably past the 4096 D3D11 sampler-state cap. A
// pre-fix run starts handing out wrappers with NULL samplers around
// iteration 4096 and the next bind segfaults.
for ($i = 0; $i < 5000; $i++) {
    $tex = vio_render_target_texture($rt);
    if (!$tex instanceof VioTexture) {
        echo "FAIL: iteration {$i} returned non-VioTexture\n";
        break;
    }

    // Bind every 100th texture so we actually exercise the sampler in
    // the device context, not just allocate it. Wrap in vio_begin/end
    // so the backend sees a frame context.
    if ($i % 100 === 0) {
        vio_begin($ctx);
        vio_bind_texture($ctx, $tex, 0);
        vio_end($ctx);
    }

    unset($tex);
}

echo "completed 5000 iterations\n";

// Final sanity: the RT is still valid and queryable.
$tex_final = vio_render_target_texture($rt);
var_dump($tex_final instanceof VioTexture);
$size = vio_texture_size($tex_final);
var_dump($size[0] === 128 && $size[1] === 128);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
completed 5000 iterations
bool(true)
bool(true)
OK
