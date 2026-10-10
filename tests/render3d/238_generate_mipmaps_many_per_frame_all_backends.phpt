--TEST--
vio_generate_mipmaps: 96 textures per frame, across frames without a wait and outside a frame - every texture gets its own averaged top level
--DESCRIPTION--
96 mipmapped 8x8 textures; before each scenario every texture gets new level-0
content (left half (r, 255, 0), right half (r, 0, 255), r distinct per texture
and scenario), then vio_generate_mipmaps builds its chain. Level 3 (1x1) must be
the average (r, 128, 128) of exactly that texture - a level built from another
texture's descriptors, or never built, shows another r. Scenarios: 96 calls in
one frame; 4 frames x 24 calls back to back without a wait; 96 calls outside a
frame. D3D12 wrote every call's descriptors into the next of 64 blocks of a ring
without a fence: from the 65th call per frame (or once a frame still in flight
held its blocks) a recorded call used another call's views. With a hardware
D3D12 adapter the same runs headless on the GPU too ('d3d12_hw').
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
$__ok = false;
foreach (['auto', 'metal', 'opengl'] as $__b) {
    $__c = @vio_create($__b, ['width' => 8, 'height' => 8, 'headless' => true]);
    if ($__c) { vio_destroy($__c); $__ok = true; break; }
}
if (!$__ok) die('skip no headless GPU context available');
?>
--FILE--
<?php
$runs = [];
$seen = [];
foreach (['auto', 'metal', 'opengl', 'd3d11', 'd3d12', 'vulkan'] as $candidate) {
    $probe = @vio_create($candidate, ['width' => 4, 'height' => 4, 'headless' => true]);
    if (!$probe) continue;
    $resolved = vio_backend_name($probe);
    vio_destroy($probe);
    if (in_array($resolved, $seen, true)) continue;
    $seen[] = $resolved;
    $runs[] = [$candidate, $resolved, []];
}
// Hardware D3D12 (headless WARP is the default): only where a GPU adapter exists.
$hw = @vio_create('d3d12', ['width' => 4, 'height' => 4, 'headless' => true, 'headless_hardware' => true]);
if ($hw) {
    $info = vio_gpu_info();
    vio_destroy($hw);
    if (!str_contains((string)($info['name'] ?? ''), 'Basic Render')) {
        $runs[] = ['d3d12', 'd3d12_hw', ['headless_hardware' => true]];
    }
}
if (!$runs) { echo "none: skipped\n"; }

const COUNT = 96;
const SIZE = 8;     // level 3 is 1x1
const TILE = 2;     // one TILE x TILE tile per texture in a single row: x never flips

$VS = "#version 450\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos.xy, 0.0, 1.0); }";
$FS = "#version 450\nlayout(location=0) out vec4 o;\nuniform sampler2D u_tex;\n"
    . "void main(){ o = vec4(textureLod(u_tex, vec2(0.5), 3.0).rgb, 1.0); }";

function red_of(int $k, int $scenario): int { return (20 + $k * 2 + $scenario * 61) % 256; }

function level0(int $r): string
{
    $px = '';
    for ($y = 0; $y < SIZE; $y++) {
        for ($x = 0; $x < SIZE; $x++) {
            $px .= $x < SIZE / 2 ? chr($r) . "\xff\x00\xff" : chr($r) . "\x00\xff\xff";
        }
    }
    return $px;
}

foreach ($runs as [$be, $label, $extra]) {
    $W = COUNT * TILE;
    $ctx = @vio_create($be, ['width' => $W, 'height' => TILE, 'headless' => true, 'vsync' => false] + $extra);
    if (!$ctx) { echo "$label: skipped\n"; continue; }
    $tri = vio_mesh($ctx, ['vertices' => [-1, -1, 0, 3, -1, 0, -1, 3, 0], 'layout' => [VIO_FLOAT3]]);
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]),
                                'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $tex = [];
    $black = str_repeat("\x00\x00\x00\xff", SIZE * SIZE);
    for ($k = 0; $k < COUNT; $k++) {
        $tex[] = vio_texture($ctx, ['data' => $black, 'width' => SIZE, 'height' => SIZE, 'mipmaps' => true,
                                    'filter' => VIO_FILTER_LINEAR]);
    }
    if (!$tri || !$pipe || in_array(false, $tex, true)) {
        echo "$label: setup failed\n";
        vio_destroy($ctx);
        continue;
    }
    $ok = true;

    $upload = function (int $scenario) use ($ctx, $tex): void {
        for ($k = 0; $k < COUNT; $k++) vio_texture_update($ctx, $tex[$k], level0(red_of($k, $scenario)));
    };
    $check = function (string $name, int $scenario) use ($ctx, $tri, $pipe, $tex, $W, $label, &$ok): void {
        vio_begin($ctx);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $pipe);
        for ($k = 0; $k < COUNT; $k++) {
            vio_viewport($ctx, $k * TILE, 0, TILE, TILE);
            vio_bind_texture($ctx, $tex[$k], 0);
            vio_set_uniform($ctx, 'u_tex', 0);
            vio_draw($ctx, $tri);
        }
        vio_end($ctx);
        $p = vio_read_pixels($ctx);
        $bad = 0;
        for ($k = 0; $k < COUNT; $k++) {
            $o = ($k * TILE) * 4;
            $got = [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])];
            $want = [red_of($k, $scenario), 128, 128];
            if (abs($got[0] - $want[0]) > 6 || abs($got[1] - 128) > 6 || abs($got[2] - 128) > 6) {
                if ($bad++ < 3) echo "$label: $name texture $k level 3 = ", json_encode($got), ", want ", json_encode($want), "\n";
            }
        }
        if ($bad) { $ok = false; echo "$label: $name $bad of ", COUNT, " textures wrong\n"; }
    };

    // 1) 96 calls in one frame.
    $upload(1);
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    for ($k = 0; $k < COUNT; $k++) vio_generate_mipmaps($ctx, $tex[$k]);
    vio_end($ctx);
    $check('one-frame', 1);

    // 2) 4 frames x 24 calls back to back, no wait in between.
    $upload(2);
    for ($f = 0; $f < 4; $f++) {
        vio_begin($ctx);
        vio_clear($ctx, 0, 0, 0, 1);
        for ($i = 0; $i < 24; $i++) vio_generate_mipmaps($ctx, $tex[$f * 24 + $i]);
        vio_end($ctx);
    }
    $check('frames', 2);

    // 3) 96 calls outside a frame.
    $upload(3);
    for ($k = 0; $k < COUNT; $k++) vio_generate_mipmaps($ctx, $tex[$k]);
    $check('outside', 3);

    // 4) one frame again (blocks of the earlier frames come back).
    $upload(4);
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    for ($k = 0; $k < COUNT; $k++) vio_generate_mipmaps($ctx, $tex[$k]);
    vio_end($ctx);
    $check('one-frame-again', 4);

    echo "$label: ", $ok ? "OK" : "FAIL", "\n";
    vio_destroy($ctx);
}
?>
--EXPECTREGEX--
(\w+: (OK|skipped))(\n(\w+: (OK|skipped)))*
