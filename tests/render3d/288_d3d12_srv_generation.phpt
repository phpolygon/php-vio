--TEST--
D3D12 SRV slots across contexts (fix/d3d12-srv-generation): the free list of a destroyed context does not hand its slots to the next one, and a texture freed after its context is gone gives nothing back
--EXTENSIONS--
vio
--FILE--
<?php
/* Before: the free list of static SRV slots outlived the device. Slots freed in one context
 * were handed out again in the next, whose own allocator starts at the top of a fresh heap
 * and later gives out the same indices: two live textures shared one descriptor and the
 * older one sampled the newer (a suite of tests sharing one process saw wrong images).
 * Textures that PHP freed after vio_destroy() of their context fed the same list. */
$VS = "#version 450\nlayout(location=0) in vec3 a_pos;\nvoid main(){ gl_Position = vec4(a_pos, 1.0); }";
$FS = "#version 450\nuniform sampler2D u_tex;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texture(u_tex, vec2(0.5)); }";

function open_d3d12(): ?VioContext
{
    $ctx = @vio_create('d3d12', ['width' => 4, 'height' => 4, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== 'd3d12') { vio_destroy($ctx); $ctx = null; }

    return $ctx ?: null;
}

function run_d3d12(): string
{
    global $VS, $FS;
    $first = open_d3d12();
    if (!$first) return "skip (unavailable)";
    // Slots freed while the first context lives, then reclaimed by the next allocation.
    $gone = [];
    for ($i = 0; $i < 16; $i++) {
        $gone[] = vio_texture($first, ['data' => "\x80\x80\x80\xff", 'width' => 1, 'height' => 1]);
    }
    $gone = [];
    for ($frame = 0; $frame < 4; $frame++) { vio_begin($first); vio_end($first); }
    $late = [vio_texture($first, ['data' => "\x80\x80\x80\xff", 'width' => 1, 'height' => 1])];
    vio_destroy($first);

    $ctx = open_d3d12();
    if (!$ctx) return "skip (unavailable)";
    // 40 textures, each its own colour; $late goes after its context is gone.
    $texs = [];
    for ($i = 0; $i < 40; $i++) {
        $texs[] = vio_texture($ctx, ['data' => chr(5 * $i) . chr(255 - 5 * $i) . "\x40\xff", 'width' => 1, 'height' => 1]);
        if ($i === 10) {
            $late = [];
            for ($frame = 0; $frame < 4; $frame++) { vio_begin($ctx); vio_end($ctx); }
        }
    }

    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    $p = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
    $fail = [];
    foreach ($texs as $n => $tex) {
        vio_begin($ctx);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $p);
        vio_bind_texture($ctx, $tex, 0);
        vio_set_uniform($ctx, 'u_tex', 0);
        vio_draw($ctx, $tri);
        vio_end($ctx);
        $px = vio_read_pixels($ctx);
        $o = (2 * 4 + 2) * 4;
        $got = [ord($px[$o]), ord($px[$o + 1])];
        if (abs($got[0] - 5 * $n) > 2 || abs($got[1] - (255 - 5 * $n)) > 2) {
            $fail[] = "texture #$n sampled " . json_encode($got);
        }
    }
    vio_destroy($ctx);

    return $fail ? 'FAIL: ' . implode('; ', array_slice($fail, 0, 3)) : 'OK';
}

echo "d3d12: ", run_d3d12(), "\n";
?>
--EXPECTF--
d3d12: %r(OK|skip \(.*\))%r
