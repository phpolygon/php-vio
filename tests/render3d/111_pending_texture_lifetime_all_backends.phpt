--TEST--
Draw-time bind resolution keeps a bound temporary texture alive until the draw (shadow cascade regression)
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null' || $b === 'vulkan') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE) && vio_supports_feature($c, VIO_FEATURE_READ_PIXELS)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with a 3D pipeline");
?>
--FILE--
<?php
/* Typed-register backends (D3D11 / D3D12 / Metal) resolve a GL texture unit to a shader register at
 * DRAW time through a pending-bind table (see vio_flush_pending_textures). Engines bind TEMPORARIES:
 *
 *     vio_bind_texture($ctx, vio_render_target_texture($shadowRt), 6);
 *
 * The VioTexture object dies as soon as the statement ends. When the table only remembered the raw
 * pointer, the allocator reused that memory for the next VioTexture created before the draw, and the
 * flush bound THAT texture instead — in a real renderer the half-res AO map landed on the shadow
 * cascade register and cast phantom shadows. The table must own a reference to what it points at.
 *
 * Scenario: a depth-only RT holds a caster in its LEFT half. Its texture is bound to unit 6 as a
 * temporary, then several other temporaries are created and bound to other units (recycling the
 * freed memory), then a fullscreen pass compares against unit 6. Left must be shadowed, right lit;
 * a recycled pointer yields a colour texture on the comparison register (everything lit or dark). */
$W = 32; $H = 32; $S = 64;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }

function run_backend(string $name): string {
    global $W, $H, $S;
    $ctx = @vio_create($name, ["width" => $W, "height" => $H, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE) || !vio_supports_feature($ctx, VIO_FEATURE_READ_PIXELS)) {
        vio_destroy($ctx);
        return "skip (no 3D pipeline)";
    }
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nlayout(location=1) in vec2 aUv;\nout vec2 vUv;\nvoid main(){ vUv = aUv; gl_Position = vec4(aPos, 1.0); }";
    $fs_fill = "#version 330 core\nin vec2 vUv;\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0.0, 1.0, 0.0, 1.0); }";
    /* Two regular samplers declared BEFORE the depth sampler, like a real material shader. The depth
     * compare is done in the shader (plain sampler2D reading .r) so the test does not depend on a
     * backend's comparison-sampler support; the lifetime defect is independent of that. */
    $fs_cmp  = "#version 330 core\nin vec2 vUv;\nuniform sampler2D u_a; uniform sampler2D u_b; uniform sampler2D u_shadow;\nlayout(location=0) out vec4 o;\n"
             . "void main(){ float lit = texture(u_shadow, vUv).r > 0.75 ? 1.0 : 0.0; float ab = texture(u_a, vUv).g * texture(u_b, vUv).g; o = vec4(lit, ab, 0.0, 1.0); }";
    $p_fill = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs_fill, 'format' => $fmt]), 'depth_test' => true, 'cull_mode' => VIO_CULL_NONE]);
    $p_cmp  = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs_cmp, 'format' => $fmt]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $half = vio_mesh($ctx, ['vertices' => [-1,-1,0,0,1, 0,-1,0,1,1, 0,1,0,1,0, -1,1,0,0,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0,0,1, 1,-1,0,1,1, 1,1,0,1,0, -1,1,0,0,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);

    $depthRt = vio_render_target($ctx, ['width' => $S, 'height' => $S, 'depth_only' => true]);
    $colorA  = vio_render_target($ctx, ['width' => $S, 'height' => $S]);
    $colorB  = vio_render_target($ctx, ['width' => $S, 'height' => $S]);
    if (!($depthRt instanceof VioRenderTarget) || !($colorA instanceof VioRenderTarget) || !($colorB instanceof VioRenderTarget)) {
        vio_destroy($ctx);
        return "FAIL\n  render targets not created";
    }

    $fail = [];
    for ($frame = 0; $frame < 3; $frame++) {
        vio_begin($ctx);
        /* Caster depth 0.5 in the left half; cleared depth 1.0 elsewhere. */
        vio_bind_render_target($ctx, $depthRt);
        vio_viewport($ctx, 0, 0, $S, $S);
        vio_clear($ctx, 1.0, 1.0, 1.0, 1.0);
        vio_bind_pipeline($ctx, $p_fill);
        vio_draw($ctx, $half);
        /* Two colour targets filled green (the regular samplers must read them, not the depth). */
        foreach ([$colorA, $colorB] as $rt) {
            vio_bind_render_target($ctx, $rt);
            vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
            vio_draw($ctx, $quad);
        }
        vio_unbind_render_target($ctx);

        vio_viewport($ctx, 0, 0, $W, $H);
        vio_bind_pipeline($ctx, $p_cmp);
        /* The shadow texture is bound as a TEMPORARY first ... */
        vio_bind_texture($ctx, vio_render_target_texture($depthRt), 6);
        vio_set_uniform($ctx, 'u_shadow', 6);
        /* ... then more temporaries are created and bound (each one may reuse the freed object memory). */
        for ($i = 0; $i < 4; $i++) {
            vio_bind_texture($ctx, vio_render_target_texture($colorA), 0);
            vio_bind_texture($ctx, vio_render_target_texture($colorB), 1);
        }
        vio_set_uniform($ctx, 'u_a', 0);
        vio_set_uniform($ctx, 'u_b', 1);
        vio_draw($ctx, $quad);
        vio_end($ctx);

        $p = vio_read_pixels($ctx);
        $l = px($p, 4, $H >> 1, $W);
        $r = px($p, $W - 4, $H >> 1, $W);
        if ($l[0] > 64)  $fail[] = "frame $frame: left should be shadowed, lit=" . $l[0];
        if ($r[0] < 192) $fail[] = "frame $frame: right should be lit, lit=" . $r[0];
        if ($l[1] < 192 || $r[1] < 192) $fail[] = "frame $frame: colour samplers lost their textures g=" . $l[1] . '/' . $r[1];
    }

    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'metal'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %s
d3d11: %s
d3d12: %s
metal: %s
DONE
