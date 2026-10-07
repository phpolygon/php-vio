--TEST--
Shading-rate image (VIO_FEATURE_SHADING_RATE_IMAGE): vio_set_shading_rate_image() shades each screen tile at its own rate (combined with the set rate by MAX), null clears it; vio_shading_rate_tile_size() reports the tile edge
--EXTENSIONS--
vio
--FILE--
<?php
/* The fragment shader writes its pixel coordinate (as in test 122): two pixels
 * of one 2x2 block are equal at a 2x2 rate and differ at 1x1. The image holds
 * one VIO_SHADING_RATE_* byte per tile: 2X2 in the left half, 1X1 in the right.
 *   A. set rate 1X1 + image             -> left coarse, right fine
 *   B. image cleared (null)              -> both fine
 *   C. contract: wrong byte count / bad rate -> false; without the feature
 *      vio_set_shading_rate_image is false and the tile size 0
 * D3D12: VRS Tier 2 (RSSetShadingRateImage, combiner MAX). VIO_REQUIRE_SHADING_RATE_IMAGE
 * makes the listed backends mandatory. */
$W = 64;
$VS = "#version 450\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$FS = "#version 450\nlayout(location=0) out vec4 o;\n"
    . "void main(){ o = vec4(floor(gl_FragCoord.x) / 64.0, floor(gl_FragCoord.y) / 64.0, 0.0, 1.0); }";

$opts = ["width" => $W, "height" => $W, "headless" => true, "vsync" => false, "shader_model" => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_SHADING_RATE_IMAGE') ?: ''));

function px(string $p, int $x, int $y, int $w): array { $o = ($y * $w + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }
/* 1 when pixels x and x+1 of row y share a shading invocation. */
function coarse(string $p, int $x, int $y, int $w): int { return px($p, $x, $y, $w) === px($p, $x + 1, $y, $w) ? 1 : 0; }

function run_backend(string $name): string {
    global $W, $VS, $FS, $opts, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    $tile = vio_shading_rate_tile_size($ctx);
    if (!vio_supports_feature($ctx, VIO_FEATURE_SHADING_RATE_IMAGE)) {
        $fail = [];
        if ($tile !== 0) $fail[] = "tile size $tile without the feature";
        if (@vio_set_shading_rate_image($ctx, "\0", 1, 1) !== false) $fail[] = "image accepted without the feature";
        vio_destroy($ctx);
        if ($fail) return "FAIL\n  " . implode("\n  ", $fail);
        return $req ? "FAIL\n  required but VIO_FEATURE_SHADING_RATE_IMAGE is 0" : "skip (no shading-rate image)";
    }
    $fail = [];
    if ($tile < 1 || $tile > $W / 2) { vio_destroy($ctx); return "FAIL\n  tile size $tile (want 1.." . ($W / 2) . ")"; }
    $tx = intdiv($W + $tile - 1, $tile);
    $ty = intdiv($W + $tile - 1, $tile);
    $rates = '';
    for ($y = 0; $y < $ty; $y++) for ($x = 0; $x < $tx; $x++) $rates .= chr($x < $tx / 2 ? VIO_SHADING_RATE_2X2 : VIO_SHADING_RATE_1X1);

    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $draw = function () use ($ctx, $pipe, $quad): string {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_set_shading_rate($ctx, VIO_SHADING_RATE_1X1);
        vio_bind_pipeline($ctx, $pipe);
        vio_draw($ctx, $quad);
        vio_end($ctx);
        return vio_read_pixels($ctx);
    };
    /* Sample pairs aligned to 2x2 blocks, in the middle row of each half. */
    $lx = 4; $rx = $W - 12; $row = $W >> 1;

    /* A */
    if (!vio_set_shading_rate_image($ctx, $rates, $tx, $ty)) $fail[] = "A: vio_set_shading_rate_image returned false";
    $p = $draw();
    if (coarse($p, $lx, $row, $W) !== 1) $fail[] = "A: left half (2X2 tiles) not coarse";
    if (coarse($p, $rx, $row, $W) !== 0) $fail[] = "A: right half (1X1 tiles) not fine";
    /* Sticky: a second frame keeps the image. */
    $p = $draw();
    if (coarse($p, $lx, $row, $W) !== 1) $fail[] = "A: the image did not stick to the next frame";

    /* B */
    if (!vio_set_shading_rate_image($ctx, null)) $fail[] = "B: clearing returned false";
    $p = $draw();
    if (coarse($p, $lx, $row, $W) !== 0 || coarse($p, $rx, $row, $W) !== 0) $fail[] = "B: still coarse after clearing";

    /* C */
    if (@vio_set_shading_rate_image($ctx, substr($rates, 1), $tx, $ty) !== false) $fail[] = "C: a short rate string was accepted";
    if (@vio_set_shading_rate_image($ctx, str_repeat(chr(99), $tx * $ty), $tx, $ty) !== false) $fail[] = "C: rate 99 was accepted";

    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) echo "$b: ", run_backend($b), "\n";
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE
