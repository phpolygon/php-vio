--TEST--
Depth of a colour render target is samplable (VIO_FEATURE_RENDER_TARGET_DEPTH_SAMPLE): vio_render_target_texture($rt, VIO_RT_DEPTH) reads the same depth a depth_only target gets from the same draws, for plain and MRT targets, inside the frame and after it; re-binding keeps depth-testing against it, a shadow (depth_only) target in the same chain is unaffected, multisampled targets refuse
--EXTENSIONS--
vio
--FILE--
<?php
/* TEMPORAL-UPSCALING S0: TAA / motion-vector dilation reads the scene depth of
 * the G-buffer target. The pattern varies along x only (render-target rows are
 * flipped between GL and the rest): columns 0-7 at "near", 8-11 at "mid",
 * 12-15 cleared to 1.0. Values are compared with a depth_only target drawn with
 * the same geometry in the same frame (so GL's [-1, 1] -> [0, 1] mapping and
 * the D3D convention both hold), and they must order near < mid < 1.0. */
$W = 16; $H = 4;
function quad(float $x0, float $x1, float $z): array { return [$x0, -1, $z, $x1, -1, $z, $x1, 1, $z, $x0, 1, $z]; }
function col(int $c, int $W): float { return -1 + 2 * $c / $W; }

echo "constant: ", VIO_RT_DEPTH, "\n";

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    $ctx = @vio_create($b, ['width' => $W, 'height' => $H, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    if (!vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_DEPTH_SAMPLE)) { echo "$b: skip (no depth sampling)\n"; vio_destroy($ctx); continue; }
    $fail = [];
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $fsCol = "#version 330 core\nuniform vec4 u_col;\nlayout(location=0) out vec4 o;\nvoid main(){ o = u_col; }";
    $fsMrt = "#version 330 core\nuniform vec4 u_col;\nlayout(location=0) out vec4 o0;\nlayout(location=1) out vec4 o1;\nvoid main(){ o0 = u_col; o1 = vec4(0.5, 0.25, 0.0, 1.0); }";
    $fsDepth = "#version 330 core\nvoid main(){ }";
    $fsRead = "#version 330 core\nuniform sampler2D u_d;\nlayout(location=0) out vec4 o;\n"
            . "void main(){ float d = texelFetch(u_d, ivec2(int(gl_FragCoord.x), 0), 0).r; o = vec4(d, d, d, 1.0); }";
    $sh = fn(string $fs) => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs]);
    $pCol = vio_pipeline($ctx, ['shader' => $sh($fsCol), 'depth_test' => true, 'cull_mode' => VIO_CULL_NONE]);
    $pMrt = vio_pipeline($ctx, ['shader' => $sh($fsMrt), 'depth_test' => true, 'cull_mode' => VIO_CULL_NONE,
                                'attachments' => [VIO_FORMAT_RGBA16F, VIO_FORMAT_RG16F]]);
    $pDepth = vio_pipeline($ctx, ['shader' => $sh($fsDepth), 'depth_test' => true, 'cull_mode' => VIO_CULL_NONE]);
    $pRead = vio_pipeline($ctx, ['shader' => $sh($fsRead), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $mesh = fn(array $v) => vio_mesh($ctx, ['vertices' => $v, 'indices' => [0, 1, 2, 0, 2, 3], 'layout' => [VIO_FLOAT3]]);
    $near = $mesh(quad(col(0, $W), col(8, $W), 0.25));
    $mid  = $mesh(quad(col(8, $W), col(12, $W), 0.5));
    $far  = $mesh(quad(-1, 1, 0.75));
    $full = $mesh(quad(-1, 1, 0));

    $rt    = vio_render_target($ctx, ['width' => $W, 'height' => $H]);
    $mrt   = vio_render_target($ctx, ['width' => $W, 'height' => $H, 'attachments' => [VIO_FORMAT_RGBA16F, VIO_FORMAT_RG16F]]);
    $shadow = vio_render_target($ctx, ['width' => $W, 'height' => $H, 'depth_only' => true]);
    $dRt  = vio_render_target_texture($rt, VIO_RT_DEPTH);
    $dMrt = vio_render_target_texture($mrt, VIO_RT_DEPTH);
    $dSh  = vio_render_target_texture($shadow, VIO_RT_DEPTH);
    if (!($dRt instanceof VioTexture) || !($dMrt instanceof VioTexture) || !($dSh instanceof VioTexture)) {
        echo "$b: FAIL depth textures not handed out\n"; vio_destroy($ctx); continue;
    }

    $scene = function ($pipe, bool $colour) use ($ctx, $near, $mid) {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $pipe);
        if ($colour) vio_set_uniform($ctx, 'u_col', [1.0, 0.0, 0.0, 1.0]);
        vio_draw($ctx, $near);
        vio_draw($ctx, $mid);
    };
    $sample = function ($tex) use ($ctx, $pRead, $full) {
        vio_bind_pipeline($ctx, $pRead);
        vio_set_uniform($ctx, 'u_d', 0);
        vio_bind_texture($ctx, $tex, 0);
        vio_draw($ctx, $full);
    };
    $row = function () use ($ctx, $W) { $p = vio_read_pixels($ctx); return array_map(fn($x) => ord($p[$x * 4]), range(0, $W - 1)); };

    // Frame 1: colour target -> MRT target -> shadow target as one bind chain
    // (no unbind in between), then sample the colour target's depth in the same frame.
    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt);     $scene($pCol, true);
    vio_bind_render_target($ctx, $mrt);    $scene($pMrt, true);
    vio_bind_render_target($ctx, $shadow); $scene($pDepth, false);
    vio_unbind_render_target($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    $sample($dRt);
    vio_end($ctx);
    $inFrame = $row();

    $read = function ($tex) use ($ctx, $sample, $row) {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        $sample($tex);
        vio_end($ctx);
        return $row();
    };
    $ref = $read($dSh);
    [$dn, $dm, $df] = [$ref[2], $ref[9], $ref[14]];
    if (!($dn < $dm && $dm < $df && $df >= 254)) $fail[] = "shadow reference " . json_encode($ref);
    $close = fn(array $a, array $b) => max(array_map(fn($x, $y) => abs($x - $y), $a, $b)) <= 1;
    if (!$close($inFrame, $ref)) $fail[] = "colour target depth (in frame) " . json_encode($inFrame) . " vs " . json_encode($ref);
    $after = $read($dRt);
    if (!$close($after, $ref)) $fail[] = "colour target depth (next frame) " . json_encode($after);
    $afterMrt = $read($dMrt);
    if (!$close($afterMrt, $ref)) $fail[] = "MRT target depth " . json_encode($afterMrt);
    // the colour contents are untouched by the depth view
    $c = vio_read_render_target($rt);
    $px = fn(string $p, int $x) => [ord($p[$x * 4]), ord($p[$x * 4 + 1]), ord($p[$x * 4 + 2])];
    if ($px($c, 2) !== [255, 0, 0] || $px($c, 14) !== [0, 0, 0]) $fail[] = "colour " . json_encode([$px($c, 2), $px($c, 14)]);

    // Re-bind without clearing: the kept depth still rejects the far quad where
    // near / mid were drawn; the cleared columns take it and their depth changes.
    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt);
    vio_bind_pipeline($ctx, $pCol);
    vio_set_uniform($ctx, 'u_col', [0.0, 1.0, 0.0, 1.0]);
    vio_draw($ctx, $far);
    vio_unbind_render_target($ctx);
    vio_end($ctx);
    $c = vio_read_render_target($rt);
    if ($px($c, 2) !== [255, 0, 0] || $px($c, 9) !== [255, 0, 0] || $px($c, 14) !== [0, 255, 0]) {
        $fail[] = "re-bind depth test " . json_encode([$px($c, 2), $px($c, 9), $px($c, 14)]);
    }
    $again = $read($dRt);
    if (abs($again[2] - $dn) > 1 || !($again[14] > $dm && $again[14] < $df)) $fail[] = "depth after re-bind " . json_encode($again);
    // the shadow target still samples what it had
    if (!$close($read($dSh), $ref)) $fail[] = "shadow target changed";

    // Multisampled colour targets have no single-sample depth to hand out.
    $ms = vio_render_target($ctx, ['width' => $W, 'height' => $H, 'samples' => 4]);
    if ($ms && @vio_render_target_texture($ms, VIO_RT_DEPTH) !== false) {
        $fail[] = "multisampled target handed out a depth texture";
    }
    // Out-of-range attachments still fail.
    if (@vio_render_target_texture($rt, -5) !== false || @vio_render_target_texture($rt, 1) !== false) $fail[] = "bad attachment accepted";

    unset($dRt, $dMrt, $dSh, $rt, $mrt, $shadow, $ms);
    echo "$b: ", $fail ? "FAIL " . implode('; ', $fail) : "OK", "\n";
    vio_destroy($ctx);
}
echo "DONE\n";
?>
--EXPECTF--
constant: -1
opengl: %r(OK|skip \(unavailable\))%r
d3d11: %r(OK|skip \(unavailable\))%r
d3d12: %r(OK|skip \(unavailable\))%r
vulkan: %r(OK|skip \(unavailable\))%r
metal: %r(OK|skip \(unavailable\))%r
DONE
