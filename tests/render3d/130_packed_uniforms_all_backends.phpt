--TEST--
Packed float32 uniforms (pack('g*')) produce the same shader state as float arrays
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with a 3D pipeline");
?>
--FILE--
<?php
/* vio_set_uniform / vio_set_uniforms / vio_submit_batch accept a float-typed
 * value as packed little-endian float32 instead of a PHP array, so the hot draw
 * path skips the per-element zval_get_double() walk (a mat4 array is 16 of
 * them). The packed form must land in the shader byte-identically — the scene
 * below exercises mat4 (u_model), mat3 (the HLSL row-padding special case),
 * vec4, vec3, vec2 and scalar float, then compares the rendered frames byte for
 * byte against the array form. */
$W = 32;

function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b): bool { return abs($a[0]-$b[0]) <= 3 && abs($a[1]-$b[1]) <= 3 && abs($a[2]-$b[2]) <= 3; }

/* A translation the vertex shader must apply, so a mis-marshalled mat4 moves
 * the quad instead of merely tinting it. */
$MODEL = [1,0,0,0,  0,1,0,0,  0,0,1,0,  0.25,0.0,0.0,1];
$NORMAL = [0,1,0,  0,0,1,  1,0,0];   /* mat3: a channel rotation, not identity */
$TINT   = [0.25, 0.5, 0.75, 1.0];
$BIAS   = [0.1, 0.1, 0.1];
$UVSC   = [1.0, 1.0];
$GAIN   = 0.5;

function packed($v) { return is_array($v) ? pack('g*', ...$v) : pack('g', $v); }

function run_backend(string $name): string {
    global $W, $MODEL, $NORMAL, $TINT, $BIAS, $UVSC, $GAIN;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D)"; }
    $fail = [];
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;

    $vs = "#version 330 core\n"
        . "layout(location=0) in vec3 aPos;\n"
        . "uniform mat4 u_model;\n"
        . "uniform vec2 u_uv_scale;\n"
        . "out vec2 vUv;\n"
        . "void main(){ gl_Position = u_model * vec4(aPos, 1.0); vUv = aPos.xy * u_uv_scale; }";
    $fs = "#version 330 core\n"
        . "in vec2 vUv;\n"
        . "layout(location=0) out vec4 o;\n"
        . "uniform mat3 u_normal;\n"
        . "uniform vec4 u_tint;\n"
        . "uniform vec3 u_bias;\n"
        . "uniform float u_gain;\n"
        . "void main(){\n"
        . "  vec3 c = u_normal * u_tint.rgb;\n"   /* mat3 row padding shows up here */
        . "  c = c * u_gain + u_bias + vec3(vUv, 0.0) * 0.0;\n"
        . "  o = vec4(c, u_tint.a);\n"
        . "}";

    $sh = @vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]);
    if (!$sh) { vio_destroy($ctx); return "skip (shader compile failed)"; }
    $pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $mesh = vio_mesh($ctx, [
        'vertices' => [-0.5,-0.5,0,  0.0,-0.5,0,  0.0,0.5,0,  -0.5,0.5,0],
        'indices'  => [0,1,2, 0,2,3],
        'layout'   => [VIO_FLOAT3],
    ]);

    /* Same uniforms twice: as PHP arrays, then as packed float32 strings. */
    $asArray = [
        'u_model' => $MODEL, 'u_uv_scale' => $UVSC, 'u_normal' => $NORMAL,
        'u_tint' => $TINT, 'u_bias' => $BIAS, 'u_gain' => $GAIN,
    ];
    $asPacked = [];
    foreach ($asArray as $k => $v) { $asPacked[$k] = packed($v); }

    $render = function (array $u) use ($ctx, $pipe, $mesh) {
        vio_begin($ctx);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniforms($ctx, $u);
        vio_draw($ctx, $mesh);
        vio_end($ctx);
        return vio_read_pixels($ctx);
    };

    $ref = $render($asArray);
    $pak = $render($asPacked);

    if ($ref === false || $pak === false) {
        $fail[] = "read_pixels failed";
    } else {
        if ($ref !== $pak) $fail[] = "packed uniforms differ from array uniforms";
        /* u_normal rotates rgb, u_gain halves it, u_bias lifts it. GLSL mat3 is
         * COLUMN-major, so [0,1,0, 0,0,1, 1,0,0] has columns (0,1,0), (0,0,1),
         * (1,0,0) and u_normal * tint(0.25, 0.5, 0.75)
         *   = 0.25*(0,1,0) + 0.5*(0,0,1) + 0.75*(1,0,0) = (0.75, 0.25, 0.5)
         * -> * 0.5 = (0.375, 0.125, 0.25) -> + 0.1 = (0.475, 0.225, 0.35). */
        $want = [(int)round(0.475 * 255), (int)round(0.225 * 255), (int)round(0.35 * 255)];
        /* The quad sits left of centre, shifted right by u_model's 0.25. */
        $probe = px($ref, (int)($W * 0.5) - 2, $W >> 1, $W);
        if (!near($probe, $want)) $fail[] = "array form rendered " . json_encode($probe) . ", expected " . json_encode($want);
    }

    /* Mixed forms in one call, and via a vio_submit_batch record. */
    $mixed = $asArray;
    $mixed['u_model']  = $asPacked['u_model'];
    $mixed['u_normal'] = $asPacked['u_normal'];
    if ($render($mixed) !== $ref) $fail[] = "mixing packed and array values in one call differs";

    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $pipe);
    vio_submit_batch($ctx, [['mesh' => $mesh, 'uniforms' => $asPacked]]);
    vio_end($ctx);
    if (vio_read_pixels($ctx) !== $ref) $fail[] = "packed uniforms in a vio_submit_batch record differ";

    /* A byte length that is not a supported shape must warn, not corrupt state. */
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $pipe);
    vio_set_uniforms($ctx, $asArray);
    $warned = false;
    set_error_handler(function ($no, $msg) use (&$warned) { $warned = str_contains($msg, 'packed data'); return true; });
    vio_set_uniform($ctx, 'u_model', pack('g5', 1, 2, 3, 4, 5));   /* 20 bytes: no such uniform shape */
    restore_error_handler();
    vio_draw($ctx, $mesh);
    vio_end($ctx);
    if (!$warned) $fail[] = "a bad packed length did not warn";
    if (vio_read_pixels($ctx) !== $ref) $fail[] = "a rejected packed value changed shader state";

    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

/* The per-backend lines are %s so a skip stays a skip, but a FAIL must not pass
 * silently — the final line is pinned and flips when any backend failed. */
$failed = 0;
foreach (['opengl', 'd3d11', 'd3d12', 'metal', 'vulkan'] as $b) {
    $r = run_backend($b);
    if (str_starts_with($r, 'FAIL')) $failed++;
    echo "$b: ", $r, "\n";
}
echo $failed ? "FAILED ON $failed BACKEND(S)\n" : "DONE\n";
?>
--EXPECTF--
opengl: %s
d3d11: %s
d3d12: %s
metal: %s
vulkan: %s
DONE
