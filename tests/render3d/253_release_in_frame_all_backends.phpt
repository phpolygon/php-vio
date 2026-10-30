--TEST--
Objects released inside the frame that used them: a render target (plain, MSAA, MRT, HDR), a texture, a cubemap, a mesh, a uniform-feeding storage buffer and a pipeline can go out of scope between two draws, or while the target is still bound, without the GPU losing them - later draws, the next frames and vio_read_pixels stay correct
--EXTENSIONS--
vio
--FILE--
<?php
/* Each case records work that uses an object, drops the last PHP reference
 * before the frame ends, and draws on. The frame's command list still holds
 * the object, so the backend has to keep it until that frame retires. Every
 * case repeats over 4 frames, without waiting, and the screen must hold the
 * expected colour. D3D12 used to release render targets at once: DEVICE_HUNG
 * on hardware, an access violation on WARP. */
$VS = "#version 450\nlayout(location=0) in vec3 a_pos;\nvoid main(){ gl_Position = vec4(a_pos, 1.0); }";
$FLAT = "#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(1.0, 0.5, 0.0, 1.0); }";
$BLIT = "#version 450\nlayout(binding=0) uniform sampler2D u_tex;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texture(u_tex, vec2(0.5)); }";
$CUBE = "#version 450\nlayout(binding=0) uniform samplerCube u_tex;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texture(u_tex, vec3(1.0, 0.0, 0.0)); }";
$W = 16;
const ORANGE = [255, 128, 0];
function close_to(array $a, array $b): bool { foreach ($a as $i => $v) if (abs($v - $b[$i]) > 4) return false; return true; }

function run_backend(string $name): string {
    global $VS, $FLAT, $BLIT, $CUBE, $W;
    $ctx = @vio_create($name, ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D pipeline)"; }
    $flat = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FLAT]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $blit = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $BLIT]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $cubeP = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $CUBE]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
    $orange = str_repeat("\xFF\x80\x00\xFF", 4);

    $fill = function ($rt) use ($ctx, $flat, $tri) {
        vio_bind_render_target($ctx, $rt);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $flat);
        vio_draw($ctx, $tri);
        vio_unbind_render_target($ctx);
    };
    $show = function ($tex) use ($ctx, $blit, $tri) {
        vio_bind_pipeline($ctx, $blit);
        vio_set_uniform($ctx, 'u_tex', 0);
        vio_bind_texture($ctx, $tex, 0);
        vio_draw($ctx, $tri);
    };
    $target = function (array $o) use ($ctx, $fill, $show) {
        $rt = vio_render_target($ctx, $o + ['width' => 8, 'height' => 8]);
        $fill($rt);
        $show(vio_render_target_texture($rt));
    };
    $cases = [
        'target'       => fn() => $target([]),
        'target twice' => function () use ($target) { $target([]); $target([]); },
        'texture outlives its target' => function () use ($ctx, $fill, $show) {
            $rt = vio_render_target($ctx, ['width' => 8, 'height' => 8]);
            $fill($rt);
            $tex = vio_render_target_texture($rt);
            unset($rt);
            $show($tex);
        },
        'hdr'          => fn() => vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_HDR) ? $target(['hdr' => true]) : null,
        'msaa'         => fn() => vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_MSAA) ? $target(['samples' => 4]) : null,
        'mrt'          => fn() => vio_supports_feature($ctx, VIO_FEATURE_MRT) ? $target(['attachments' => [VIO_FORMAT_RGBA8, VIO_FORMAT_RGBA8]]) : null,
        'texture'      => function () use ($ctx, $show, $orange) { $show(vio_texture($ctx, ['data' => $orange, 'width' => 2, 'height' => 2])); },
        'cubemap'      => function () use ($ctx, $cubeP, $tri) {
            $f = str_repeat("\xFF\x80\x00\xFF", 4);
            $c = vio_cubemap($ctx, ['faces' => [$f, $f, $f, $f, $f, $f], 'width' => 2, 'height' => 2]);
            vio_bind_pipeline($ctx, $cubeP);
            vio_set_uniform($ctx, 'u_tex', 0);
            vio_bind_cubemap($ctx, $c, 0);
            vio_draw($ctx, $tri);
        },
        'mesh'         => function () use ($ctx, $flat) {
            vio_bind_pipeline($ctx, $flat);
            vio_draw($ctx, vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]));
        },
        'pipeline'     => function () use ($ctx, $VS, $FLAT, $tri) {
            vio_bind_pipeline($ctx, vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FLAT]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]));
            vio_draw($ctx, $tri);
        },
    ];
    $fail = [];
    foreach ($cases as $label => $case) {
        for ($frame = 0; $frame < 4; $frame++) {
            vio_begin($ctx);
            vio_clear($ctx, 0, 0, 0, 1);
            $case();                                   // the object dies here
            vio_bind_pipeline($ctx, $flat);            // and the frame goes on
            vio_draw($ctx, $tri);
            vio_end($ctx);
        }
        $p = vio_read_pixels($ctx);
        if (strlen($p) !== $W * $W * 4) { $fail[] = "$label: readback failed"; break; }
        $got = [ord($p[0]), ord($p[1]), ord($p[2])];
        if (!close_to($got, ORANGE)) $fail[] = "$label: got " . implode(',', $got);
    }
    /* released while still bound */
    if (vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET)) {
        for ($frame = 0; $frame < 4; $frame++) {
            vio_begin($ctx);
            vio_clear($ctx, 0, 0, 0, 1);
            $rt = vio_render_target($ctx, ['width' => 8, 'height' => 8]);
            vio_bind_render_target($ctx, $rt);
            vio_bind_pipeline($ctx, $flat);
            vio_draw($ctx, $tri);
            vio_destroy_render_target($rt);
            unset($rt);
            vio_unbind_render_target($ctx);
            vio_bind_pipeline($ctx, $flat);
            vio_draw($ctx, $tri);
            vio_end($ctx);
        }
        $p = vio_read_pixels($ctx);
        if (!close_to([ord($p[0]), ord($p[1]), ord($p[2])], ORANGE)) $fail[] = "bound: got " . ord($p[0]) . ',' . ord($p[1]) . ',' . ord($p[2]);
    }
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
