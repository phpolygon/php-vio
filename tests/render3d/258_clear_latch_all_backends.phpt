--TEST--
vio_clear inside a frame clears the bound target only and never becomes the swapchain clear: a colour latched before vio_begin clears the screen although render targets are bound and cleared first, also with 'present' => true and a target bound before vio_begin; backends that clear every frame keep the latched colour across frames
--EXTENSIONS--
vio
--FILE--
<?php
/* Vulkan, OpenGL and Metal latched every vio_clear, also the clear of a bound
 * render target inside the frame; the next frame then cleared the screen with
 * that colour (Code Rescue: a shadow pass cleared to white, the screen came up
 * white where nothing was drawn). D3D11/D3D12 latch only outside a frame. */
$VS = "#version 450\nlayout(location=0) in vec3 a_pos;\nvoid main(){ gl_Position = vec4(a_pos, 1.0); }";
$FLAT = "#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(1.0, 0.5, 0.0, 1.0); }";
$W = 16;
const GREEN = [0, 255, 0];
function close_to(array $a, array $b): bool { foreach ($b as $i => $v) if (abs($a[$i] - $v) > 4) return false; return true; }
function px(string $p, int $w, int $x, int $y): array { $o = ($y * $w + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }
function screen(string $p, int $w): array { return [px($p, $w, intdiv($w, 2), 1), px($p, $w, intdiv($w, 2), intdiv($w, 2)), px($p, $w, intdiv($w, 2), $w - 2)]; }

function run_backend(string $name): string {
    global $VS, $FLAT, $W;
    $ctx = @vio_create($name, ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D pipeline)"; }
    /* D3D11/D3D12 clear the screen only in the frame after a vio_clear outside a frame. */
    $everyFrame = !in_array($name, ['d3d11', 'd3d12'], true);
    $flat = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FLAT]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
    $shadow = vio_render_target($ctx, ['width' => 8, 'height' => 8, 'depth_only' => true]);
    $color = vio_render_target($ctx, ['width' => 8, 'height' => 8]);
    $fail = [];
    set_error_handler(function (int $no, string $msg) use (&$fail) { $fail[] = "warning: $msg"; return true; });

    /* The passes before the screen pass: bind, clear white / grey, draw, unbind. */
    $passes = function () use ($ctx, $shadow, $color, $flat, $tri) {
        foreach ([[$shadow, [1, 1, 1, 1]], [$color, [0.5, 0.5, 0.5, 1]]] as [$rt, $c]) {
            vio_bind_render_target($ctx, $rt);
            vio_clear($ctx, ...$c);
            vio_bind_pipeline($ctx, $flat);
            vio_draw($ctx, $tri);
            vio_unbind_render_target($ctx);
        }
    };
    $check = function (string $what, array $want) use ($ctx, $W, &$fail) {
        foreach (screen(vio_read_pixels($ctx), $W) as $i => $got)
            if (!close_to($got, $want)) { $fail[] = "$what: pixel $i " . json_encode($got) . ", want " . json_encode($want); return; }
    };
    $notTarget = function (string $what) use ($ctx, $W, &$fail) {
        foreach (screen(vio_read_pixels($ctx), $W) as $i => $got)
            if (close_to($got, [255, 255, 255]) || close_to($got, [128, 128, 128])) { $fail[] = "$what: pixel $i is a target's clear " . json_encode($got); return; }
    };

    // Latched before vio_begin, targets bound and cleared first: the screen is green.
    for ($f = 0; $f < 3; $f++) {
        vio_clear($ctx, 0, 1, 0, 1);
        vio_begin($ctx);
        $passes();
        vio_end($ctx);
        $check("latched, frame $f", GREEN);
    }

    // No vio_clear before vio_begin: the latch is still green, the targets' clears are not it.
    for ($f = 0; $f < 3; $f++) {
        vio_begin($ctx);
        $passes();
        vio_end($ctx);
        $everyFrame ? $check("no new latch, frame $f", GREEN) : $notTarget("no new latch, frame $f");
    }

    // A clear of the screen inside the frame clears it now and does not latch either.
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 1, 1);
    $passes();
    vio_end($ctx);
    $check("screen cleared in the frame", [0, 0, 255]);
    vio_begin($ctx);
    $passes();
    vio_end($ctx);
    if ($everyFrame) $check("after a screen clear in the frame", GREEN);

    // 'present' => true with a target bound before vio_begin: the latched clear still reaches the screen.
    if (vio_supports_feature($ctx, VIO_FEATURE_EXPLICIT_PRESENT)) {
        for ($f = 0; $f < 2; $f++) {
            vio_clear($ctx, 0, 1, 0, 1);
            vio_bind_render_target($ctx, $color);
            vio_begin($ctx, ['present' => true]);
            vio_clear($ctx, 0.5, 0.5, 0.5, 1);
            vio_bind_pipeline($ctx, $flat);
            vio_draw($ctx, $tri);
            vio_unbind_render_target($ctx);
            $passes();
            vio_end($ctx);
            $check("bound before vio_begin, frame $f", GREEN);
        }
    }

    restore_error_handler();
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", array_slice($fail, 0, 6)) : "OK";
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
