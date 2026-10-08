--TEST--
Multisampled depth-only targets (['depth_only' => true, 'samples' => 4]): the depth renders multisampled and resolves for sampling with the target's 'depth_reduction' - MIN keeps an edge texel near when any sample is covered, MAX far unless all are; interior and exterior exact on every backend
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A24 (depth-only part). */
$W = 32;
foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    $ctx = @vio_create($b, ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    $fail = [];
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $fsRead = "#version 330 core\nuniform sampler2D u_d;\nlayout(location=0) out vec4 o;\nvoid main(){ float d = texelFetch(u_d, ivec2(gl_FragCoord.xy), 0).r; o = vec4(d, d, d, 1.0); }";
    $pDepth = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => "#version 330 core\nvoid main(){ }"]), 'depth_test' => true, 'cull_mode' => VIO_CULL_NONE]);
    $pRead = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsRead]), 'depth_test' => false]);
    $tri = vio_mesh($ctx, ['vertices' => [-1, -1, 0.25, 1, -1, 0.25, -1, 1, 0.25], 'layout' => [VIO_FLOAT3]]);
    $full = vio_mesh($ctx, ['vertices' => [-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0], 'indices' => [0, 1, 2, 0, 2, 3], 'layout' => [VIO_FLOAT3]]);
    $near = [];
    foreach (['max' => VIO_DEPTH_REDUCE_MAX, 'min' => VIO_DEPTH_REDUCE_MIN] as $name => $mode) {
        $rt = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'depth_only' => true, 'samples' => 4, 'depth_reduction' => $mode]);
        if (!$rt) { $fail[] = "$name refused"; continue; }
        vio_begin($ctx);
        vio_bind_render_target($ctx, $rt);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $pDepth);
        vio_draw($ctx, $tri);
        vio_unbind_render_target($ctx);
        vio_bind_pipeline($ctx, $pRead);
        vio_set_uniform($ctx, 'u_d', 0);
        vio_bind_texture($ctx, vio_render_target_texture($rt), 0);
        vio_draw($ctx, $full);
        vio_end($ctx);
        $p = vio_read_pixels($ctx);
        $vals = [];
        for ($i = 0; $i < $W * $W; $i++) $vals[] = ord($p[$i * 4]);
        $lo = min($vals); $hi = max($vals);
        if ($hi < 250 || $lo > 200) { $fail[] = "$name range $lo..$hi"; continue; }
        $near[$name] = count(array_filter($vals, fn($v) => $v < ($lo + $hi) / 2));
        $mid = count(array_filter($vals, fn($v) => $v > $lo + 3 && $v < $hi - 3));
        if ($mid > 0) $fail[] = "$name has $mid in-between texels (depth must not blend)";
    }
    if (count($near) === 2 && !($near['min'] > $near['max'] + 4)) $fail[] = "min / max resolve do not differ at the edge " . json_encode($near);
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
