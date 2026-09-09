--TEST--
Geometry stage: vio_shader(['geometry' => ...]) expands a point into a quad on every backend that reports VIO_FEATURE_GEOMETRY; GS uniforms reach the stage; unbinding restores plain draws
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null' || $b === 'vulkan') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_GEOMETRY) && vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)
        && vio_supports_feature($c, VIO_FEATURE_READ_PIXELS)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with a geometry stage");
?>
--FILE--
<?php
/* A one-vertex POINTS mesh at the origin; the geometry shader emits a quad of
 * half-size u_half (a GS-stage uniform) around it. Without the stage nothing
 * but (at most) one pixel would be lit, so a green centre + black corners
 * proves the GS ran and read its constant block. Afterwards a plain VS+FS
 * pipeline draws a full-screen triangle - it must not inherit the GS.
 *
 * The GS reads the input position from a user varying (vPos), not from
 * gl_in[].gl_Position: that is the portable form - SPIRV-Cross's HLSL
 * geometry path only flattens location-qualified inputs. */
$W = 64; $H = 64;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b): bool { return abs($a[0]-$b[0]) <= 3 && abs($a[1]-$b[1]) <= 3 && abs($a[2]-$b[2]) <= 3; }

function run_backend(string $name): string {
    global $W, $H;
    $ctx = @vio_create($name, ["width" => $W, "height" => $H, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_GEOMETRY) || !vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)
        || !vio_supports_feature($ctx, VIO_FEATURE_READ_PIXELS)) {
        vio_destroy($ctx);
        return "skip (no geometry stage)";
    }
    $fail = [];
    /* #version 450: location qualifiers on varyings need GLSL 4.1+; the
     * SPIR-V path re-targets the runtime GL version anyway. */
    $vs = "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=0) out vec4 vPos;\n"
        . "void main(){ vPos = vec4(aPos, 1.0); gl_Position = vPos; }";
    $gs = "#version 450\n"
        . "layout(points) in;\nlayout(triangle_strip, max_vertices = 4) out;\n"
        . "layout(location=0) in vec4 vPos[];\n"
        . "uniform float u_half;\n"
        . "void main(){\n"
        . "  vec4 c = vPos[0];\n"
        . "  gl_Position = c + vec4(-u_half, -u_half, 0.0, 0.0); EmitVertex();\n"
        . "  gl_Position = c + vec4( u_half, -u_half, 0.0, 0.0); EmitVertex();\n"
        . "  gl_Position = c + vec4(-u_half,  u_half, 0.0, 0.0); EmitVertex();\n"
        . "  gl_Position = c + vec4( u_half,  u_half, 0.0, 0.0); EmitVertex();\n"
        . "  EndPrimitive();\n}";
    $fs = "#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0.0, 1.0, 0.0, 1.0); }";
    $fs_red = "#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(1.0, 0.0, 0.0, 1.0); }";

    $sh = vio_shader($ctx, ['vertex' => $vs, 'geometry' => $gs, 'fragment' => $fs]);
    if (!($sh instanceof VioShader)) { vio_destroy($ctx); return "FAIL\n  shader with geometry stage not created"; }
    $refl = vio_shader_reflect($sh);
    if (!is_array($refl) || !isset($refl['geometry'])) $fail[] = "reflection lacks 'geometry' stage";

    $p_gs = vio_pipeline($ctx, ['shader' => $sh, 'topology' => VIO_POINTS, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $point = vio_mesh($ctx, ['vertices' => [0, 0, 0], 'layout' => [VIO_FLOAT3]]);

    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $p_gs);
    vio_set_uniform($ctx, 'u_half', 0.5);
    vio_draw($ctx, $point);
    vio_end($ctx);

    $p = vio_read_pixels($ctx);
    if (!$p || strlen($p) !== $W * $H * 4) { vio_destroy($ctx); return "FAIL\n  readback size"; }
    /* Quad covers NDC [-0.5, 0.5]^2 = pixels 16..47. */
    if (!near(px($p, 32, 32, $W), [0, 255, 0])) $fail[] = "centre not green " . json_encode(px($p, 32, 32, $W));
    if (!near(px($p, 20, 20, $W), [0, 255, 0])) $fail[] = "inside quad not green " . json_encode(px($p, 20, 20, $W));
    if (!near(px($p, 44, 44, $W), [0, 255, 0])) $fail[] = "inside quad (far corner) not green " . json_encode(px($p, 44, 44, $W));
    if (!near(px($p, 4, 4, $W), [0, 0, 0]))     $fail[] = "outside quad not black " . json_encode(px($p, 4, 4, $W));
    if (!near(px($p, 60, 4, $W), [0, 0, 0]))    $fail[] = "outside quad (top right) not black " . json_encode(px($p, 60, 4, $W));

    /* Smaller u_half via the GS constant block: pixel 20,20 must go dark. */
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $p_gs);
    vio_set_uniform($ctx, 'u_half', 0.25);
    vio_draw($ctx, $point);
    vio_end($ctx);
    $p = vio_read_pixels($ctx);
    if (!near(px($p, 32, 32, $W), [0, 255, 0])) $fail[] = "u_half=0.25: centre not green " . json_encode(px($p, 32, 32, $W));
    if (!near(px($p, 20, 20, $W), [0, 0, 0]))   $fail[] = "u_half=0.25: pixel 20,20 not black (GS uniform ignored?) " . json_encode(px($p, 20, 20, $W));

    /* A plain pipeline bound afterwards must NOT still run the GS. */
    $vs_plain = "#version 450\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $sh_plain = vio_shader($ctx, ['vertex' => $vs_plain, 'fragment' => $fs_red]);
    $p_plain = vio_pipeline($ctx, ['shader' => $sh_plain, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $p_gs);
    vio_set_uniform($ctx, 'u_half', 0.5);
    vio_draw($ctx, $point);
    vio_bind_pipeline($ctx, $p_plain);
    vio_draw($ctx, $tri);
    vio_end($ctx);
    $p = vio_read_pixels($ctx);
    if (!near(px($p, 4, 4, $W), [255, 0, 0]))   $fail[] = "plain pipeline after GS pipeline: corner not red " . json_encode(px($p, 4, 4, $W));
    if (!near(px($p, 32, 32, $W), [255, 0, 0])) $fail[] = "plain pipeline after GS pipeline: centre not red " . json_encode(px($p, 32, 32, $W));

    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'metal'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %s
d3d11: %s
d3d12: %s
metal: %s
DONE
