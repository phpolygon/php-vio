--TEST--
Textures created or updated INSIDE a frame are complete for that frame's draws on every backend (upload ordering)
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE) && vio_supports_feature($c, VIO_FEATURE_READ_PIXELS)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with a 3D pipeline + readback");
?>
--FILE--
<?php
/* D3D-VULKAN-GAP-PLAN.md 4.1 / 4.6. The D3D12 upload queue no longer waits for
 * the GPU after each texture upload; ordering is guaranteed by the single
 * DIRECT queue (uploads execute before the frame list submitted after them).
 * This pins the observable contract on every backend: a texture created
 * between vio_begin and vio_draw, and one changed by vio_texture_update
 * mid-frame, both render with their final contents in that same frame. Many
 * small uploads in a row must also stay correct (allocator ring reuse). */
$W = 16; $H = 16;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b): bool { return abs($a[0]-$b[0]) <= 3 && abs($a[1]-$b[1]) <= 3 && abs($a[2]-$b[2]) <= 3; }

function run_backend(string $name): string {
    global $W, $H;
    $ctx = @vio_create($name, ["width" => $W, "height" => $H, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE) || !vio_supports_feature($ctx, VIO_FEATURE_READ_PIXELS)) {
        vio_destroy($ctx);
        return "skip (no 3D pipeline)";
    }
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nlayout(location=1) in vec2 aUv;\nout vec2 vUv;\nvoid main(){ vUv = aUv; gl_Position = vec4(aPos, 1.0); }";
    $fs = "#version 330 core\nin vec2 vUv; uniform sampler2D u_tex;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texture(u_tex, vUv); }";
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]), 'depth_test' => false, 'blend' => VIO_BLEND_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0,0,1, 1,-1,0,1,1, 1,1,0,1,0, -1,1,0,0,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);
    $fail = [];
    $solid = fn(int $r, int $g, int $b) => str_repeat(chr($r) . chr($g) . chr($b) . "\xFF", 16);

    /* 1. Texture created inside the frame, drawn immediately. */
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    $tex = vio_texture($ctx, ['data' => $solid(255, 0, 0), 'width' => 4, 'height' => 4, 'filter' => VIO_FILTER_NEAREST]);
    vio_bind_pipeline($ctx, $pipe); vio_set_uniform($ctx, 'u_tex', 0); vio_bind_texture($ctx, $tex, 0); vio_draw($ctx, $quad);
    vio_end($ctx);
    $c = px(vio_read_pixels($ctx), 8, 8, $W);
    if (!near($c, [255, 0, 0])) $fail[] = "in-frame create: " . json_encode($c);

    /* 2. Update inside the frame before the draw. */
    if (@vio_texture_update($ctx, $tex, $solid(0, 255, 0)) !== false) {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_texture_update($ctx, $tex, $solid(0, 0, 255));
        vio_bind_pipeline($ctx, $pipe); vio_set_uniform($ctx, 'u_tex', 0); vio_bind_texture($ctx, $tex, 0); vio_draw($ctx, $quad);
        vio_end($ctx);
        $c = px(vio_read_pixels($ctx), 8, 8, $W);
        if (!near($c, [0, 0, 255])) $fail[] = "in-frame update: " . json_encode($c);
    }

    /* 3. A burst of uploads (more than the upload allocator ring), then the
     *    LAST one drawn — every earlier staging buffer must have been retired
     *    or kept alive correctly. */
    $last = null;
    vio_begin($ctx);
    for ($i = 0; $i < 40; $i++) {
        $last = vio_texture($ctx, ['data' => $solid($i * 6, 255 - $i * 6, 128), 'width' => 4, 'height' => 4, 'filter' => VIO_FILTER_NEAREST]);
    }
    vio_bind_pipeline($ctx, $pipe); vio_set_uniform($ctx, 'u_tex', 0); vio_bind_texture($ctx, $last, 0); vio_draw($ctx, $quad);
    vio_end($ctx);
    $c = px(vio_read_pixels($ctx), 8, 8, $W);
    if (!near($c, [39 * 6, 255 - 39 * 6, 128])) $fail[] = "upload burst: " . json_encode($c);

    /* 4. Cross-frame: texture created in frame N is still intact in frame N+2. */
    for ($f = 0; $f < 2; $f++) { vio_begin($ctx); vio_end($ctx); }
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pipe); vio_set_uniform($ctx, 'u_tex', 0); vio_bind_texture($ctx, $last, 0); vio_draw($ctx, $quad);
    vio_end($ctx);
    $c = px(vio_read_pixels($ctx), 8, 8, $W);
    if (!near($c, [39 * 6, 255 - 39 * 6, 128])) $fail[] = "later frame: " . json_encode($c);

    unset($tex, $last);
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'metal', 'vulkan'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %s
d3d11: %s
d3d12: %s
metal: %s
vulkan: %s
DONE
