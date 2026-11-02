--TEST--
vio_bind_cubemap on any texture unit on every backend: a samplerCube next to a sampler2D reads its own cube on units 0, 1, 7, 10 and 15 (VioCubemap from face data and the cube of a render target), bind before and after vio_set_uniform
--EXTENSIONS--
vio
--FILE--
<?php
/* Vulkan handed the raw GL unit to its bind slot, which takes the shader's
 * sampler index (as vio_bind_texture already did): a cube read 0 on every unit
 * that was not equal to its sampler index (Code Rescue: environment IBL black on
 * Vulkan, unit 10). */
$VS = "#version 450\nlayout(location=0) in vec3 a_pos;\nvoid main(){ gl_Position = vec4(a_pos, 1.0); }";
$FACE = "#version 450\nlayout(location=0) out vec4 o;\nuniform vec4 u_col;\nvoid main(){ o = u_col; }";
$FS = "#version 450\nlayout(location=0) out vec4 o;\nuniform sampler2D u_tex;\nuniform samplerCube u_env;\n"
    . "void main(){ o = vec4(textureLod(u_env, vec3(1.0, 0.0, 0.0), 0.0).r, texture(u_tex, vec2(0.5)).g, textureLod(u_env, vec3(0.0, 0.0, -1.0), 0.0).b, 1.0); }";
$W = 8;
function close_to(array $a, array $b): bool { foreach ($b as $i => $v) if (abs($a[$i] - $v) > 4) return false; return true; }

function run_backend(string $name): string {
    $fail = [];
    foreach (['data', 'render target'] as $kind)
        foreach ([0, 1, 7, 10, 15] as $unit) {
            $r = run_case($name, $kind, $unit);
            if ($r === null) return "skip (unavailable)";
            if ($r === false) return "skip (no cubemaps)";
            array_push($fail, ...$r);
        }
    return $fail ? "FAIL\n  " . implode("\n  ", array_slice($fail, 0, 8)) : "OK";
}

/* A fresh context per case: a cube still bound from an earlier case would hide a bind that went nowhere. */
function run_case(string $name, string $kind, int $unit): array|false|null {
    global $VS, $FACE, $FS, $W;
    $ctx = @vio_create($name, ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return null;
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE) || !vio_supports_feature($ctx, VIO_FEATURE_CUBEMAP)) { vio_destroy($ctx); return false; }
    $base = ['depth_test' => false, 'cull_mode' => VIO_CULL_NONE];
    $tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
    $faceP = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FACE])] + $base);
    $p = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS])] + $base);
    /* +X red 200, -Z blue 100, the other faces black; the 2D texture green 150. */
    $faces = [];
    for ($f = 0; $f < 6; $f++) $faces[] = str_repeat($f === 0 ? "\xC8\x00\x00\xFF" : ($f === 5 ? "\x00\x00\x64\xFF" : "\x00\x00\x00\xFF"), 4);
    $dataCube = vio_cubemap($ctx, ['faces' => $faces, 'width' => 2, 'height' => 2]);
    $tex = vio_texture($ctx, ['data' => str_repeat("\x00\x96\x00\xFF", 4), 'width' => 2, 'height' => 2]);
    $rt = vio_render_target($ctx, ['cube' => true, 'size' => 4]);
    $rtCube = vio_render_target_cubemap($rt);
    $fail = [];
    set_error_handler(function (int $no, string $msg) use (&$fail) { $fail[] = "warning: $msg"; return true; });

    $cube = $kind === 'data' ? $dataCube : $rtCube;
    {
        {
            $texUnit = $unit === 1 ? 2 : 1;
            foreach (['bind first', 'uniform first'] as $order) {
                vio_begin($ctx);
                if ($kind === 'render target') {
                    for ($f = 0; $f < 6; $f++) {
                        vio_bind_render_target($ctx, $rt, $f);
                        vio_clear($ctx, 0, 0, 0, 1);
                        vio_bind_pipeline($ctx, $faceP);
                        vio_set_uniform($ctx, 'u_col', $f === 0 ? [200 / 255, 0, 0, 1] : ($f === 5 ? [0, 0, 100 / 255, 1] : [0, 0, 0, 1]));
                        vio_draw($ctx, $tri);
                    }
                    vio_unbind_render_target($ctx);
                }
                vio_clear($ctx, 1, 1, 1, 1);
                vio_viewport($ctx, 0, 0, $W, $W);
                vio_bind_pipeline($ctx, $p);
                if ($order === 'bind first') {
                    vio_bind_cubemap($ctx, $cube, $unit);
                    vio_bind_texture($ctx, $tex, $texUnit);
                }
                vio_set_uniform($ctx, 'u_env', $unit);
                vio_set_uniform($ctx, 'u_tex', $texUnit);
                if ($order === 'uniform first') {
                    vio_bind_cubemap($ctx, $cube, $unit);
                    vio_bind_texture($ctx, $tex, $texUnit);
                }
                vio_draw($ctx, $tri);
                vio_end($ctx);
                $px = vio_read_pixels($ctx);
                $o = (intdiv($W, 2) * $W + intdiv($W, 2)) * 4;
                $got = [ord($px[$o]), ord($px[$o + 1]), ord($px[$o + 2])];
                if (!close_to($got, [200, 150, 100])) $fail[] = "$kind cube on unit $unit ($order): " . json_encode($got) . ", want [200,150,100]";
            }
        }
    }

    restore_error_handler();
    vio_destroy($ctx);
    return $fail;
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
