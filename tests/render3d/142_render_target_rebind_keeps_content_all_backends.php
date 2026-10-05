<?php
/* Vulkan render passes used loadOp CLEAR, so every bind wiped the target to
 * the last clear colour (and the depth of colour targets was never stored),
 * while OpenGL and D3D keep what was drawn. The contract, for a plain, an
 * MSAA, an HDR, an array and a cube target:
 *   frame 1: bind, clear red (depth 1), draw a full-screen quad at z = 0.4
 *            in red, unbind (z values valid for GL-style and Metal 0..1 NDC)
 *   frame 2: bind again WITHOUT clearing, draw a centred green quad at z = 0.8
 *            (behind the stored depth -> rejected) and a smaller blue quad at
 *            z = 0.1 (in front -> drawn)
 * => corners stay red (colour kept), the green quad is invisible (depth kept),
 *    the centre is blue. Also: a second bind in the SAME frame keeps content. */
$W = 16;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b): bool { return abs($a[0]-$b[0]) <= 4 && abs($a[1]-$b[1]) <= 4 && abs($a[2]-$b[2]) <= 4; }

function run_backend(string $name): string {
    global $W;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET) || !vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) {
        vio_destroy($ctx);
        return "skip (no render targets)";
    }
    $fail = [];
    $vs = "#version 450\nlayout(location=0) in vec3 aPos;\nuniform float u_z; uniform float u_scale;\n"
        . "void main(){ gl_Position = vec4(aPos.xy * u_scale, u_z, 1.0); }";
    $fs = "#version 450\nuniform vec4 u_color;\nlayout(location=0) out vec4 o;\nvoid main(){ o = u_color; }";
    $p = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs]),
                             'depth_test' => true, 'depth_write' => true, 'cull_mode' => VIO_CULL_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $draw = function (float $z, float $scale, array $c) use ($ctx, $p, $quad) {
        vio_bind_pipeline($ctx, $p);
        vio_set_uniform($ctx, 'u_z', $z);
        vio_set_uniform($ctx, 'u_scale', $scale);
        vio_set_uniform($ctx, 'u_color', $c);
        vio_draw($ctx, $quad);
    };
    $cases = ['plain' => ['width' => $W, 'height' => $W], 'hdr' => ['width' => $W, 'height' => $W, 'hdr' => true]];
    if (vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_MSAA)) $cases['msaa x4'] = ['width' => $W, 'height' => $W, 'samples' => 4];
    if (vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_LAYERED)) $cases['array layer 1'] = ['width' => $W, 'height' => $W, 'layers' => 2];
    if (vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_CUBE)) $cases['cube face 3'] = ['cube' => true, 'size' => $W];
    foreach ($cases as $label => $opts) {
        $rt = vio_render_target($ctx, $opts);
        if (!$rt) { $fail[] = "$label: target not created"; continue; }
        $layer = isset($opts['layers']) ? 1 : (isset($opts['cube']) ? 3 : -1);
        $bind = function () use ($ctx, $rt, $layer) { if ($layer < 0) vio_bind_render_target($ctx, $rt); else vio_bind_render_target($ctx, $rt, $layer); };
        foreach (['next frame' => true, 'same frame' => false] as $when => $twoFrames) {
            vio_begin($ctx);
            $bind();
            vio_clear($ctx, 1, 0, 0, 1);
            $draw(0.4, 1.0, [1.0, 0.0, 0.0, 1.0]);
            vio_unbind_render_target($ctx);
            if ($twoFrames) { vio_end($ctx); vio_clear($ctx, 0, 0, 0, 1); vio_begin($ctx); }
            $bind();
            $draw(0.8, 0.75, [0.0, 1.0, 0.0, 1.0]);    /* behind: must be rejected by the kept depth */
            $draw(0.1, 0.25, [0.0, 0.0, 1.0, 1.0]);    /* in front: drawn */
            vio_unbind_render_target($ctx);
            vio_end($ctx);
            $img = $layer < 0 ? vio_read_render_target($rt) : vio_read_render_target($rt, $layer);
            if (!near(px($img, 1, 1, $W), [255, 0, 0])) $fail[] = "$label ($when): corner " . json_encode(px($img, 1, 1, $W)) . " want red (colour kept)";
            if (!near(px($img, 4, 4, $W), [255, 0, 0])) $fail[] = "$label ($when): ring " . json_encode(px($img, 4, 4, $W)) . " want red (depth kept)";
            if (!near(px($img, 8, 8, $W), [0, 0, 255])) $fail[] = "$label ($when): centre " . json_encode(px($img, 8, 8, $W)) . " want blue";
        }
    }
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>
