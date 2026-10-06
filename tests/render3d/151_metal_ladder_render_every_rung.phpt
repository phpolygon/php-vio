--TEST--
Metal version ladder: every MSL rung up to the OS maximum renders - 3D draw with uniform + texture, render target readback, compute, emulated geometry stage, tessellation (MSL 2.1+), 2D batch
--EXTENSIONS--
vio
--SKIPIF--
<?php
$c = @vio_create('metal', ["width" => 8, "height" => 8, "headless" => true]);
if (!$c) die("skip metal unavailable");
vio_destroy($c);
?>
--FILE--
<?php
/* The same small scene on each rung of the ladder (vio_create(['msl_version'
 * => r])): every shader path of the Metal backend - SPIRV-Cross MSL for the
 * graphics stages, compute kernels, the VS / GS kernels of the geometry
 * emulation, the tessellation kernels, the hand-written 2D shaders - has to
 * compile and run at that MSL version. */
const LADDER = [20, 21, 22, 23, 24, 30, 31, 32, 40, 41];
$W = 32;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b): bool { return abs($a[0]-$b[0]) <= 3 && abs($a[1]-$b[1]) <= 3 && abs($a[2]-$b[2]) <= 3; }

$VS  = "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=0) out vec2 uv;\n"
     . "void main(){ uv = aPos.xy * 0.5 + 0.5; gl_Position = vec4(aPos, 1.0); }";
$FS  = "#version 450\nlayout(location=0) in vec2 uv;\nlayout(location=0) out vec4 o;\n"
     . "uniform sampler2D u_tex;\nuniform vec4 u_tint;\n"
     . "void main(){ o = texture(u_tex, uv) * u_tint; }";
$GVS = "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=0) out vec4 vPos;\n"
     . "void main(){ vPos = vec4(aPos, 1.0); gl_Position = vPos; }";
$GS  = "#version 450\nlayout(points) in;\nlayout(triangle_strip, max_vertices = 4) out;\n"
     . "layout(location=0) in vec4 vPos[];\nuniform float u_half;\n"
     . "void main(){ vec4 c = vPos[0];\n"
     . "  gl_Position = c + vec4(-u_half, -u_half, 0, 0); EmitVertex();\n"
     . "  gl_Position = c + vec4( u_half, -u_half, 0, 0); EmitVertex();\n"
     . "  gl_Position = c + vec4(-u_half,  u_half, 0, 0); EmitVertex();\n"
     . "  gl_Position = c + vec4( u_half,  u_half, 0, 0); EmitVertex(); EndPrimitive(); }";
$GREEN = "#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0, 1, 0, 1); }";
$TVS = "#version 450\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$TCS = "#version 450\nlayout(vertices = 4) out;\n"
     . "void main(){ gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n"
     . "  if (gl_InvocationID == 0) { gl_TessLevelOuter[0] = 2.0; gl_TessLevelOuter[1] = 2.0; gl_TessLevelOuter[2] = 2.0;\n"
     . "    gl_TessLevelOuter[3] = 2.0; gl_TessLevelInner[0] = 2.0; gl_TessLevelInner[1] = 2.0; } }";
$TES = "#version 450\nlayout(quads, equal_spacing, ccw) in;\n"
     . "void main(){ vec4 a = mix(gl_in[0].gl_Position, gl_in[1].gl_Position, gl_TessCoord.x);\n"
     . "  vec4 b = mix(gl_in[3].gl_Position, gl_in[2].gl_Position, gl_TessCoord.x);\n"
     . "  gl_Position = mix(a, b, gl_TessCoord.y) * vec4(0.5, 0.5, 1.0, 1.0); }";
$CS  = "#version 450\nlayout(local_size_x = 8) in;\n"
     . "layout(std430, binding = 0) readonly buffer In { float v[]; } src;\n"
     . "layout(std430, binding = 1) buffer Out { float v[]; } dst;\n"
     . "void main(){ uint i = gl_GlobalInvocationID.x; dst.v[i] = src.v[i] * 2.0; }";

$probe = vio_create('metal', ["width" => 8, "height" => 8, "headless" => true]);
$max = vio_backend_info($probe)['shading_language_max'];
vio_destroy($probe);

foreach (LADDER as $r) {
    if ($r > $max) break;
    $fail = [];
    $ctx = vio_create('metal', ["width" => $W, "height" => $W, "headless" => true, "vsync" => false, "msl_version" => $r]);
    if (!$ctx) { echo "$r: FAIL create\n"; continue; }
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $tex = vio_texture($ctx, ['data' => str_repeat("\xFF\xFF\xFF\xFF", 4), 'width' => 2, 'height' => 2]);

    /* 3D: textured quad with a uniform tint into a render target, read back. */
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $rt = vio_render_target($ctx, ['width' => $W, 'height' => $W]);
    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $pipe);
    vio_bind_texture($ctx, $tex, 0);
    vio_set_uniform($ctx, 'u_tex', 0);
    vio_set_uniform($ctx, 'u_tint', [1.0, 0.0, 1.0, 1.0]);
    vio_draw($ctx, $quad);
    vio_unbind_render_target($ctx);
    vio_end($ctx);
    $p = vio_read_render_target($rt);
    if (!$p || !near(px($p, 16, 16, $W), [255, 0, 255])) $fail[] = "3D/RT " . json_encode($p ? px($p, 16, 16, $W) : null);

    /* Compute. */
    $cp = vio_compute_pipeline($ctx, ['source' => $CS]);
    $in = vio_storage_buffer($ctx, ['data' => pack('f*', 1, 2, 3, 4, 5, 6, 7, 8)]);
    $out = vio_storage_buffer($ctx, ['size' => 32]);
    vio_compute_bind_buffer($ctx, $cp, $in, 0, VIO_COMPUTE_READ);
    vio_compute_bind_buffer($ctx, $cp, $out, 1, VIO_COMPUTE_WRITE);
    vio_compute_dispatch($ctx, $cp, 1, 1, 1);
    $v = array_values(unpack('f*', vio_storage_buffer_read($ctx, $out)));
    if ($v !== [2.0, 4.0, 6.0, 8.0, 10.0, 12.0, 14.0, 16.0]) $fail[] = "compute " . json_encode($v);

    /* Emulated geometry stage: point -> half-size quad. */
    $gsh = vio_shader($ctx, ['vertex' => $GVS, 'geometry' => $GS, 'fragment' => $GREEN]);
    if (!$gsh) $fail[] = "GS shader";
    else {
        $gp = vio_pipeline($ctx, ['shader' => $gsh, 'topology' => VIO_POINTS, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $gp);
        vio_set_uniform($ctx, 'u_half', 0.5);
        vio_draw($ctx, vio_mesh($ctx, ['vertices' => [0, 0, 0], 'layout' => [VIO_FLOAT3]]));
        vio_end($ctx);
        $p = vio_read_pixels($ctx);
        if (!near(px($p, 16, 16, $W), [0, 255, 0]) || !near(px($p, 2, 2, $W), [0, 0, 0])) $fail[] = "GS " . json_encode([px($p, 16, 16, $W), px($p, 2, 2, $W)]);
    }

    /* Tessellation: only from MSL 2.1 ([[patch]] / tessellation kernels). */
    $tess = vio_supports_feature($ctx, VIO_FEATURE_TESSELLATION);
    if ($tess !== ($r >= 21)) $fail[] = "VIO_FEATURE_TESSELLATION " . json_encode($tess);
    if ($tess) {
        $tsh = vio_shader($ctx, ['vertex' => $TVS, 'tess_control' => $TCS, 'tess_eval' => $TES, 'fragment' => $GREEN]);
        if (!$tsh) $fail[] = "tess shader";
        else {
            $tp = vio_pipeline($ctx, ['shader' => $tsh, 'patch_vertices' => 4, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
            vio_clear($ctx, 0, 0, 0, 1);
            vio_begin($ctx);
            vio_bind_pipeline($ctx, $tp);
            vio_draw($ctx, vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'layout' => [VIO_FLOAT3]]));
            vio_end($ctx);
            $p = vio_read_pixels($ctx);
            if (!near(px($p, 16, 16, $W), [0, 255, 0]) || !near(px($p, 2, 2, $W), [0, 0, 0])) $fail[] = "tess " . json_encode([px($p, 16, 16, $W), px($p, 2, 2, $W)]);
        }
    } else {
        /* Below 2.1 the stage is refused, not mis-rendered. */
        $tsh = @vio_shader($ctx, ['vertex' => $TVS, 'tess_control' => $TCS, 'tess_eval' => $TES, 'fragment' => $GREEN]);
        if ($tsh) $fail[] = "tess shader accepted without VIO_FEATURE_TESSELLATION";
    }

    /* 2D batch (hand-written MSL). */
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_rect($ctx, 8, 8, 16, 16, ['color' => 0xFFFFFFFF]);
    vio_draw_2d($ctx);
    vio_end($ctx);
    $p = vio_read_pixels($ctx);
    if (!near(px($p, 16, 16, $W), [255, 255, 255]) || !near(px($p, 2, 2, $W), [0, 0, 0])) $fail[] = "2D " . json_encode([px($p, 16, 16, $W), px($p, 2, 2, $W)]);

    vio_destroy($ctx);
    echo "$r: ", $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK", "\n";
}
echo "DONE\n";
?>
--EXPECTF--
%r(\d\d: OK\n)+%rDONE
