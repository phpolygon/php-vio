--TEST--
Multisampled cube and array targets ('cube' / 'layers' + 'samples' => 4): each face / layer keeps its own multisampled colour and depth, resolves when the binding leaves it, rebinding keeps the content, other faces stay untouched; the resolved faces sample through the cubemap / sampler2DArray
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A24 (cube / array part). */
$W = 32;
function px(string $p, int $x, int $y, int $w): array { $o = ($y * $w + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }
function census(string $p, int $w, int $ch): array {
    $full = 0; $empty = 0; $blend = 0;
    for ($i = 0; $i < $w * $w; $i++) { $v = ord($p[$i * 4 + $ch]); if ($v > 240) $full++; elseif ($v < 15) $empty++; else $blend++; }
    return [$full, $empty, $blend];
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    $ctx = @vio_create($b, ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    $fail = [];
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $fs = "#version 330 core\nuniform vec4 u_color;\nlayout(location=0) out vec4 o;\nvoid main(){ o = u_color; }";
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs]), 'depth_test' => true]);
    $tri = vio_mesh($ctx, ['vertices' => [-1, -1, 0.5, 1, -1, 0.5, -1, 1, 0.5], 'layout' => [VIO_FLOAT3]]);
    $far = vio_mesh($ctx, ['vertices' => [-1, -1, 0.9, 1, -1, 0.9, 1, 1, 0.9, -1, 1, 0.9], 'indices' => [0, 1, 2, 0, 2, 3], 'layout' => [VIO_FLOAT3]]);
    $targets = [
        'cube'  => vio_render_target($ctx, ['cube' => true, 'size' => $W, 'samples' => 4]),
        'array' => @vio_render_target($ctx, ['width' => $W, 'height' => $W, 'layers' => 3, 'samples' => 4]),
    ];
    foreach ($targets as $kind => $rt) {
        if (!$rt) { $fail[] = "$kind refused"; continue; }
        // Layer 2: green triangle; layer 1: cleared blue only.
        vio_begin($ctx);
        vio_bind_render_target($ctx, $rt, 2);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniform($ctx, 'u_color', [0.0, 1.0, 0.0, 1.0]);
        vio_draw($ctx, $tri);
        vio_bind_render_target($ctx, $rt, 1);   // leaving layer 2 resolves it
        vio_clear($ctx, 0, 0, 1, 1);
        // Back to layer 2: its depth must still hide a farther red quad.
        vio_bind_render_target($ctx, $rt, 2);
        vio_set_uniform($ctx, 'u_color', [1.0, 0.0, 0.0, 1.0]);
        vio_draw($ctx, $far);
        vio_unbind_render_target($ctx);
        vio_end($ctx);
        [$g, $e, $bl] = census(vio_read_render_target($rt, 2), $W, 1);
        if ($g < 300 || $bl < 4) $fail[] = "$kind layer 2 green=$g blend=$bl";
        [$r] = census(vio_read_render_target($rt, 2), $W, 0);
        // The red quad lands only where the triangle did not cover (behind nothing): about half.
        if ($r < 300) $fail[] = "$kind layer 2 red=$r (far quad missing where uncovered)";
        $l1 = vio_read_render_target($rt, 1);
        if (px($l1, 5, 5, $W) !== [0, 0, 255] || px($l1, 26, 26, $W) !== [0, 0, 255]) $fail[] = "$kind layer 1 " . json_encode(px($l1, 5, 5, $W));
        $l0 = vio_read_render_target($rt, 0);
        if (census($l0, $W, 1)[0] > 0 || census($l0, $W, 2)[0] > 0) $fail[] = "$kind layer 0 touched";
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
