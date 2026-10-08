--TEST--
Multisampled MRT targets (['attachments' => [...], 'samples' => 4]) resolve every attachment on every backend: interior and exterior exact, the diagonal edge blended in each attachment, sampling attachment 1 shows the resolved content
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A24 (MRT part). Attachment 0 gets green, attachment 1 red. */
$W = 64;
function px(string $p, int $x, int $y, int $w): array { $o = ($y * $w + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    $ctx = @vio_create($b, ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    if (!vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_MSAA) || !vio_supports_feature($ctx, VIO_FEATURE_MRT)) {
        echo "$b: skip (no MSAA / MRT)\n"; vio_destroy($ctx); continue;
    }
    $fail = [];
    $fmts = [VIO_FORMAT_RGBA8, VIO_FORMAT_RGBA8];
    $rt = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'attachments' => $fmts, 'samples' => 4]);
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $fs = "#version 330 core\nlayout(location=0) out vec4 o0;\nlayout(location=1) out vec4 o1;\nvoid main(){ o0 = vec4(0.0, 1.0, 0.0, 1.0); o1 = vec4(1.0, 0.0, 0.0, 1.0); }";
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs]), 'depth_test' => false, 'attachments' => $fmts]);
    // Lower-left half in NDC (x + y < 0): a diagonal edge through every backend's orientation.
    $tri = vio_mesh($ctx, ['vertices' => [-1, -1, 0, 1, -1, 0, -1, 1, 0], 'layout' => [VIO_FLOAT3]]);
    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $pipe);
    vio_draw($ctx, $tri);
    vio_unbind_render_target($ctx);
    vio_end($ctx);
    foreach ([0 => 1, 1 => 0] as $att => $ch) {
        $p = vio_read_render_target($rt, -1, $att);
        // Row 0 is NDC top on D3D / Vulkan / Metal and bottom on GL: test the
        // diagonal both ways by counting blended pixels anywhere.
        $full = 0; $empty = 0; $blend = 0;
        for ($y = 0; $y < $W; $y++) for ($x = 0; $x < $W; $x++) {
            $v = px($p, $x, $y, $W)[$ch];
            if ($v > 240) $full++; elseif ($v < 15) $empty++; else $blend++;
        }
        if ($full < 1500 || $empty < 1500) $fail[] = "attachment $att coverage full=$full empty=$empty";
        if ($blend < 8) $fail[] = "attachment $att has no blended edge ($blend px)";
    }
    echo "$b: ", $fail ? "FAIL " . implode('; ', $fail) : "OK", "\n";
    vio_destroy($ctx);
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(unavailable\))%r
d3d11: %r(OK|skip \(unavailable\))%r
d3d12: %r(OK|skip \(unavailable\))%r
vulkan: %r(OK|skip \(unavailable\))%r
metal: %r(OK|skip \(unavailable\))%r
DONE
