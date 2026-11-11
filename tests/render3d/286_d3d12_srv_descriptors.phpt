--TEST--
D3D12 SRV descriptors (fix/d3d12-srv-descriptors): freed textures and render targets give their view back, and a frame with thousands of texture switches keeps sampling the right texture instead of reusing a stale table
--EXTENSIONS--
vio
--FILE--
<?php
/* Before: every texture and render target took a static SRV for good (the slot was never
 * freed, VioTexture had no D3D12 free hook at all), so the per-frame table region below
 * them shrank over a session. When a frame's region ran out, the table flush returned
 * without binding anything: every later draw and post pass sampled the last table, and a
 * mirrored second scene pass put a label texture on the whole screen. The heap had
 * 32768 slots; one frame now holds 4000 distinct tables, and 40,000 create/free cycles
 * fit as well. */
$VS = "#version 450\nlayout(location=0) in vec3 a_pos;\nvoid main(){ gl_Position = vec4(a_pos, 1.0); }";
$FS = "#version 450\nuniform sampler2D u_tex;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texture(u_tex, vec2(0.5)); }";

function pixel(VioContext $ctx): array
{
    $px = vio_read_pixels($ctx);
    $o = (2 * 4 + 2) * 4;

    return [ord($px[$o]), ord($px[$o + 1]), ord($px[$o + 2])];
}

function run_d3d12(): string
{
    global $VS, $FS;
    $ctx = @vio_create('d3d12', ['width' => 4, 'height' => 4, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== 'd3d12') { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    $fail = [];
    $warn = '';
    set_error_handler(function ($no, $msg) use (&$warn) { $warn .= $msg . "\n"; return true; });

    // 40,000 textures and 2,000 render targets created and freed: more than the old heap had slots.
    for ($i = 0; $i < 40000; $i++) {
        $t = vio_texture($ctx, ['data' => "\x10\x20\x30\xff", 'width' => 1, 'height' => 1]);
        unset($t);
        if ($i % 1000 === 0) {
            vio_begin($ctx);
            vio_end($ctx);
        }
    }
    for ($i = 0; $i < 2000; $i++) {
        $rt = vio_render_target($ctx, ['width' => 2, 'height' => 2]);
        unset($rt);
        if ($i % 200 === 0) {
            vio_begin($ctx);
            vio_end($ctx);
        }
    }
    if (str_contains($warn, 'descriptor heap full')) $fail[] = 'heap full after freeing: ' . trim($warn);

    // One frame, 4000 draws alternating between 64 textures (no table dedup), the last one green.
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    $p = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
    $texs = [];
    for ($i = 0; $i < 64; $i++) {
        $texs[] = vio_texture($ctx, ['data' => chr(200) . chr($i) . chr(0) . "\xff", 'width' => 1, 'height' => 1]);
    }
    $green = vio_texture($ctx, ['data' => "\x00\xff\x00\xff", 'width' => 1, 'height' => 1]);
    for ($frame = 0; $frame < 3; $frame++) {
        vio_begin($ctx);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $p);
        vio_set_uniform($ctx, 'u_tex', 0);
        for ($i = 0; $i < 4000; $i++) {
            vio_bind_texture($ctx, $texs[$i % 64], 0);
            vio_draw($ctx, $quad);
        }
        vio_bind_texture($ctx, $green, 0);
        vio_draw($ctx, $quad);
        vio_end($ctx);
    }
    $got = pixel($ctx);
    if ($got[0] > 5 || $got[1] < 250 || $got[2] > 5) $fail[] = 'last draw sampled ' . json_encode($got) . ', want green';
    if (str_contains($warn, 'used up')) $fail[] = 'table region exhausted: ' . trim($warn);
    restore_error_handler();
    vio_destroy($ctx);

    return $fail ? 'FAIL: ' . implode('; ', $fail) : 'OK';
}

echo "d3d12: ", run_d3d12(), "\n";
?>
--EXPECTF--
d3d12: %r(OK|skip \(.*\))%r
