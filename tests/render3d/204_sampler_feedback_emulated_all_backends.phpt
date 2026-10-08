--TEST--
Sampler feedback from GLSL (VIO_FEATURE_SAMPLER_FEEDBACK_GLSL): a fragment shader that calls vio_write_feedback() from VIO_SAMPLER_FEEDBACK_GLSL records the lowest mip it sampled per region; vio_sampler_feedback_read / _clear work on every backend with fragment storage
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A15 (part 2). The fragment shader prepends the helper
 * constant and calls vio_write_feedback(u_tex, uv) next to its sample; vio binds
 * the texture's map (fragment storage binding 3) with vio_sampler_feedback_bind.
 *   A. a 256 x 256 texture with a full mip chain on a 32 x 32 target samples
 *      mip 3 (8 texels per pixel): every region reports 3 (+-1); the map has
 *      the hardware path's shape (region = 128 -> 2 x 2 regions)
 *   B. after vio_sampler_feedback_clear nothing is recorded
 *   C. only the first quarter of the texture (uv in [0, 0.5)^2) over the whole
 *      target, 4 texels per pixel -> mip 2: region 0 reports 2 (+-1), the
 *      other three null
 *   D. unbound (null): draws record nothing
 * D3D12 with hardware feedback merges both maps (min per region). */
$W = 32;
$VS = "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=0) out vec2 uv;\nuniform vec2 u_scale;\n"
    . "void main(){ uv = (aPos.xy * 0.5 + 0.5) * u_scale; gl_Position = vec4(aPos, 1.0); }";
$FS = "#version 450\n" . VIO_SAMPLER_FEEDBACK_GLSL
    . "layout(location=0) in vec2 uv;\nlayout(location=0) out vec4 o;\nuniform sampler2D u_tex;\n"
    . "void main(){ vio_write_feedback(u_tex, uv); o = texture(u_tex, uv); }";

function near_all(array $m, ?int $want): bool {
    foreach ($m as $v) {
        if ($want === null) { if ($v !== null) return false; }
        elseif ($v === null || abs($v - $want) > 1) return false;
    }
    return true;
}

function run_backend(string $name): string {
    global $W, $VS, $FS;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    $tex = vio_texture($ctx, ['data' => str_repeat("\x80\x40\x20\xFF", 256 * 256), 'width' => 256, 'height' => 256, 'mipmaps' => true]);
    if (!vio_supports_feature($ctx, VIO_FEATURE_SAMPLER_FEEDBACK_GLSL)) {
        $hw = vio_supports_feature($ctx, VIO_FEATURE_SAMPLER_FEEDBACK);
        $r = @vio_sampler_feedback_bind($ctx, $tex);
        vio_destroy($ctx);
        if (!$hw && $r !== false) return "FAIL\n  bind succeeded without either feedback flag";
        return "skip (no fragment storage)";
    }
    $fail = [];
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    $pipe = $sh ? vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]) : false;
    if (!$pipe) { vio_destroy($ctx); return "FAIL\n  pipeline not created"; }
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $draw = function (float $s) use ($ctx, $pipe, $quad, $tex) {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_bind_texture($ctx, $tex, 0);
        vio_set_uniform($ctx, 'u_tex', 0);
        vio_set_uniform($ctx, 'u_scale', [$s, $s]);
        vio_draw($ctx, $quad);
        vio_end($ctx);
    };

    /* A */
    if (!vio_sampler_feedback_clear($ctx, $tex)) $fail[] = "A: clear returned false";
    if (!vio_sampler_feedback_bind($ctx, $tex)) $fail[] = "A: bind returned false";
    $draw(1.0);
    $r = vio_sampler_feedback_read($ctx, $tex);
    if (!is_array($r) || $r['regions_x'] !== 2 || $r['regions_y'] !== 2 || $r['region'] !== 128) $fail[] = "A: map shape " . json_encode($r ? array_slice($r, 0, 3) : $r);
    elseif (!near_all($r['min_mip'], 3)) $fail[] = "A: min_mip " . json_encode($r['min_mip']) . ", want 3 (+-1)";

    /* B */
    vio_sampler_feedback_clear($ctx, $tex);
    $r = vio_sampler_feedback_read($ctx, $tex);
    if (!is_array($r) || !near_all($r['min_mip'], null)) $fail[] = "B: after clear " . json_encode($r['min_mip'] ?? $r);

    /* C */
    $draw(0.5);
    $r = vio_sampler_feedback_read($ctx, $tex);
    if (is_array($r)) {
        $m = $r['min_mip'];   /* row by row: [y0x0, y0x1, y1x0, y1x1] */
        if (!near_all([$m[0]], 2) || !near_all([$m[1], $m[2], $m[3]], null)) $fail[] = "C: min_mip " . json_encode($m) . ", want region 0 at 2, the rest null";
    } else $fail[] = "C: read failed";

    /* D */
    vio_sampler_feedback_clear($ctx, $tex);
    vio_sampler_feedback_bind($ctx, null);
    $draw(1.0);
    $r = vio_sampler_feedback_read($ctx, $tex);
    if (!is_array($r) || !near_all($r['min_mip'], null)) $fail[] = "D: unbound draw recorded " . json_encode($r['min_mip'] ?? $r);

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
