--TEST--
Multiview by instancing on D3D11 and on OpenGL without GL_OVR_multiview2 (forced with VIO_GL_EMULATE_MULTIVIEW=1): views x instances per draw, gl_ViewIndex in both stages, per-instance data stepping per user instance, indirect records, vio_feature_info reports the emulation, SPIR-V input is refused
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A10. vio_shader rewrites the GLSL: the vertex stage runs
 * views x instances, gl_ViewIndex = gl_InstanceIndex % views, the instance index is
 * divided back, gl_Layer selects the layer and the fragment stage reads the view
 * from a flat varying. */
putenv('VIO_GL_EMULATE_MULTIVIEW=1');
$W = 16;
$VS = "#version 450\n#extension GL_EXT_multiview : require\n"
    . "layout(location=0) in vec3 aPos;\nlayout(location=3) in mat4 aModel;\nlayout(location=0) flat out int vinst;\n"
    . "void main(){ vinst = gl_InstanceIndex; gl_Position = aModel * vec4(aPos, 1.0); }";
/* The indirect draw has no instance matrices: a vertex stage without aModel. */
$VS2 = "#version 450\n#extension GL_EXT_multiview : require\n"
    . "layout(location=0) in vec3 aPos;\nlayout(location=0) flat out int vinst;\n"
    . "void main(){ vinst = gl_InstanceIndex; gl_Position = vec4(aPos, 1.0); }";
$FS = "#version 450\n#extension GL_EXT_multiview : require\nlayout(location=0) flat in int vinst;\nlayout(location=0) out vec4 o;\n"
    . "void main(){ o = vec4(gl_ViewIndex == 0 ? 1.0 : 0.0, gl_ViewIndex == 1 ? 1.0 : 0.0, float(vinst) * 0.5, 1.0); }";

function px(string $p, int $x, int $y): array { $o = ($y * 16 + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }
function near(array $a, array $b): bool { foreach ($a as $i => $v) if (abs($v - $b[$i]) > 3) return false; return true; }
function mat(float $tx): array { return [0.5, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  $tx, 0, 0, 1]; }

function run_backend(string $name): string {
    global $W, $VS, $VS2, $FS;
    $ctx = @vio_create($name, ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_MULTIVIEW)) { vio_destroy($ctx); return "skip (no vertex-stage layer)"; }
    $err = [];
    $info = vio_feature_info($ctx, VIO_FEATURE_MULTIVIEW);
    if (!$info['emulated']) $err[] = "vio_feature_info does not report the emulation: " . json_encode($info);
    if (@vio_shader($ctx, ['vertex' => "\x03\x02\x23\x07", 'fragment' => "\x03\x02\x23\x07", 'format' => VIO_SHADER_SPIRV, 'view_count' => 2]) !== false)
        $err[] = "SPIR-V input accepted for multiview by instancing";

    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS, 'view_count' => 2]),
                                'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $rt = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'layers' => 2]);
    $pipe2 = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS2, 'fragment' => $FS, 'view_count' => 2]),
                                 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $frame = function (callable $issue, $p = null) use ($ctx, $rt, $pipe) {
        vio_begin($ctx);
        for ($l = 0; $l < 2; $l++) { vio_bind_render_target($ctx, $rt, $l); vio_clear($ctx, 0, 0, 0, 1); }
        vio_bind_render_target($ctx, $rt, VIO_RT_ALL_LAYERS);
        vio_bind_pipeline($ctx, $p ?? $pipe);
        $issue();
        vio_unbind_render_target($ctx);
        vio_end($ctx);
    };

    /* Two instances: left (instance 0) and right half (instance 1), in both views. */
    $frame(fn() => vio_draw_instanced($ctx, $quad, array_merge(mat(-0.5), mat(0.5)), 2));
    foreach ([0, 1] as $v) {
        $p = vio_read_render_target($rt, $v);
        $want = $v === 0 ? [255, 0] : [0, 255];
        if (!near(px($p, 4, 8), [$want[0], $want[1], 0]))   $err[] = "instanced view $v left: " . json_encode(px($p, 4, 8));
        if (!near(px($p, 12, 8), [$want[0], $want[1], 128])) $err[] = "instanced view $v right: " . json_encode(px($p, 12, 8));
    }

    /* Indirect: one record, two instances, first instance 0. */
    $args = vio_storage_buffer($ctx, ['data' => pack('V*', 6, 2, 0, 0, 0), 'indirect' => true]);
    $frame(fn() => vio_draw_indirect($ctx, $quad, $args, 1, 0), $pipe2);
    foreach ([0, 1] as $v) {
        $p = vio_read_render_target($rt, $v);
        $c = px($p, 8, 8);
        if ($v === 0 ? !($c[0] > 250 && $c[1] < 5) : !($c[1] > 250 && $c[0] < 5)) $err[] = "indirect view $v: " . json_encode($c);
    }
    vio_destroy($ctx);
    return $err ? "FAIL\n  " . implode("\n  ", $err) : "OK";
}

foreach (['opengl', 'd3d11'] as $b) echo "$b: ", run_backend($b), "\n";
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
DONE
