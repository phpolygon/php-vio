--TEST--
Depth targets with a mip chain (VIO_FEATURE_DEPTH_MIPMAPS): vio_render_target(['depth_only' => true, 'mipmaps' => true, 'depth_reduction' => VIO_DEPTH_REDUCE_MAX | _MIN]) + vio_generate_mipmaps builds each level as the max / min of the 2x2 texels below it (a Hi-Z pyramid); texelFetch per level reads it on every backend
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A26. The pattern varies along x only (render-target rows are
 * flipped between GL and the rest): columns 0-1 and 3-7 at depth "near", column
 * 2 at "mid", columns 8-15 cleared to 1.0. Values are compared with the
 * backend's own level-0 reads, so GL's [-1, 1] -> [0, 1] depth mapping and the
 * D3D convention both work. */
$W = 16; $H = 4;
function quad(float $x0, float $x1, float $z): array { return [$x0, -1, $z, $x1, -1, $z, $x1, 1, $z, $x0, 1, $z]; }
function col(int $c, int $W): float { return -1 + 2 * $c / $W; }

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    $ctx = @vio_create($b, ['width' => $W, 'height' => $H, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    if (!vio_supports_feature($ctx, VIO_FEATURE_DEPTH_MIPMAPS)) { echo "$b: skip (no depth mipmaps)\n"; vio_destroy($ctx); continue; }
    $fail = [];
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $fsDepth = "#version 330 core\nvoid main(){ }";
    $fsRead = "#version 330 core\nuniform sampler2D u_d;\nuniform float u_lod;\nlayout(location=0) out vec4 o;\n"
            . "void main(){ int lod = int(u_lod); float d = texelFetch(u_d, ivec2(int(gl_FragCoord.x) >> lod, 0), lod).r; o = vec4(d, d, d, 1.0); }";
    $pDepth = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsDepth]), 'depth_test' => true, 'cull_mode' => VIO_CULL_NONE]);
    $pRead = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsRead]), 'depth_test' => false]);
    $near = 0.25; $mid = 0.5;
    $mesh = fn(array $v) => vio_mesh($ctx, ['vertices' => $v, 'indices' => [0, 1, 2, 0, 2, 3], 'layout' => [VIO_FLOAT3]]);
    $qa = $mesh(quad(col(0, $W), col(2, $W), $near));
    $qb = $mesh(quad(col(3, $W), col(8, $W), $near));
    $qs = $mesh(quad(col(2, $W), col(3, $W), $mid));
    $full = $mesh(quad(-1, 1, 0));
    $read = function ($rt, int $lod) use ($ctx, $pRead, $full, $W) {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pRead);
        vio_set_uniform($ctx, 'u_d', 0);
        vio_set_uniform($ctx, 'u_lod', (float)$lod);
        vio_bind_texture($ctx, vio_render_target_texture($rt), 0);
        vio_draw($ctx, $full);
        vio_end($ctx);
        $p = vio_read_pixels($ctx);
        return array_map(fn($x) => ord($p[$x * 4]), range(0, $W - 1));
    };
    foreach (['max' => VIO_DEPTH_REDUCE_MAX, 'min' => VIO_DEPTH_REDUCE_MIN] as $name => $mode) {
        $rt = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'depth_only' => true, 'mipmaps' => true, 'depth_reduction' => $mode]);
        if (!$rt) { $fail[] = "$name: target refused"; continue; }
        vio_begin($ctx);
        vio_bind_render_target($ctx, $rt);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $pDepth);
        foreach ([$qa, $qb, $qs] as $q) vio_draw($ctx, $q);
        vio_unbind_render_target($ctx);
        // 'min' builds the chain inside the frame (recorded on the frame's command stream), 'max' after it.
        $inFrame = $mode === VIO_DEPTH_REDUCE_MIN;
        $gen = $inFrame ? vio_generate_mipmaps($ctx, $rt) : null;
        vio_end($ctx);
        if (!$inFrame) $gen = vio_generate_mipmaps($ctx, $rt);
        if (!$gen) { $fail[] = "$name: generate_mipmaps false"; continue; }
        $l0 = $read($rt, 0);
        [$dl, $ds, $dr] = [$l0[0], $l0[2], $l0[8]];
        if (!($dl < $ds && $ds < $dr)) { $fail[] = "$name level 0 " . json_encode($l0); continue; }
        $l1 = $read($rt, 1);      // texel x covers columns 2x, 2x+1
        $l4 = $read($rt, 4);      // 1x1
        $want1 = $mode === VIO_DEPTH_REDUCE_MAX ? $ds : $dl;
        $want4 = $mode === VIO_DEPTH_REDUCE_MAX ? $dr : $dl;
        if (abs($l1[0] - $dl) > 2) $fail[] = "$name level 1 texel 0 = {$l1[0]}, want $dl";
        if (abs($l1[2] - $want1) > 2) $fail[] = "$name level 1 texel 1 = {$l1[2]}, want $want1";
        if (abs($l1[10] - $dr) > 2) $fail[] = "$name level 1 texel 5 = {$l1[10]}, want $dr";
        if (abs($l4[0] - $want4) > 2) $fail[] = "$name level 4 = {$l4[0]}, want $want4";
    }
    // Plain depth-only targets are untouched, and a depth target without mips refuses mip generation.
    $plain = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'depth_only' => true]);
    if (@vio_generate_mipmaps($ctx, $plain) !== false) $fail[] = "mips on a target without a chain";
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
