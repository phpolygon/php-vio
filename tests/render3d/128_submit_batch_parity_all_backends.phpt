--TEST--
vio_submit_batch: byte-identical to the per-draw path, including a per-record 'pipeline' switch
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
/* vio_submit_batch() runs records through the same cores as vio_bind_pipeline /
 * vio_bind_texture / vio_set_uniforms / vio_draw, so a batch must produce the
 * SAME framebuffer bytes as issuing those calls one by one. Covered here:
 * material (uniform) change between records, a record without a texture, a
 * record with one, and — the regression — a per-record 'pipeline' switch.
 *
 * That last case was a silent no-op on OpenGL: the change check compared
 * pipe->backend_pipeline, which OpenGL leaves NULL (it binds its state through
 * bind_pipeline_state instead), so NULL != NULL was false and the record's
 * pipeline was never bound. On Metal the inline bind also skipped
 * vio_metal_set_shader_cbuffers(), leaving the new shader's uniforms writing
 * into the previous shader's ring buffers. */
$W = 32;

function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b): bool { return abs($a[0]-$b[0]) <= 3 && abs($a[1]-$b[1]) <= 3 && abs($a[2]-$b[2]) <= 3; }
function ident(): array { return [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]; }

function quad(float $x0, float $x1): array {
    return [
        'vertices' => [$x0,-0.9,0, 0,0,  $x1,-0.9,0, 1,0,  $x1,0.9,0, 1,1,  $x0,0.9,0, 0,1],
        'indices'  => [0,1,2, 0,2,3],
        'layout'   => [VIO_FLOAT3, VIO_FLOAT2],
    ];
}

function run_backend(string $name): string {
    global $W;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D)"; }
    $fail = [];
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;

    $vs = "#version 330 core\n"
        . "layout(location=0) in vec3 aPos;\n"
        . "layout(location=1) in vec2 aUv;\n"
        . "uniform mat4 u_model;\n"
        . "out vec2 vUv;\n"
        . "void main(){ gl_Position = u_model * vec4(aPos, 1.0); vUv = aUv; }";
    /* Tinted, optionally modulated by a texture — the opaque-pass shape. */
    $fsA = "#version 330 core\n"
        . "in vec2 vUv;\n"
        . "layout(location=0) out vec4 o;\n"
        . "uniform vec4 u_tint;\n"
        . "uniform int u_has_tex;\n"
        . "uniform sampler2D u_tex;\n"
        . "void main(){ vec4 c = u_tint; if (u_has_tex != 0) c *= texture(u_tex, vUv); o = c; }";
    /* A second shader whose output cannot be produced by the first one, so a
     * missed pipeline bind shows up as the WRONG colour, not just as a diff. */
    $fsB = "#version 330 core\n"
        . "layout(location=0) out vec4 o;\n"
        . "void main(){ o = vec4(0.0, 1.0, 0.0, 1.0); }";

    $shA = @vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsA, 'format' => $fmt]);
    $shB = @vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsB, 'format' => $fmt]);
    if (!$shA || !$shB) { vio_destroy($ctx); return "skip (shader compile failed)"; }
    $pipeA = vio_pipeline($ctx, ['shader' => $shA, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $pipeB = vio_pipeline($ctx, ['shader' => $shB, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);

    $meshL = vio_mesh($ctx, quad(-0.9, -0.1));
    $meshR = vio_mesh($ctx, quad(0.1, 0.9));
    /* Opaque white: modulating by it must leave the tint untouched. */
    $tex = vio_texture($ctx, ['data' => str_repeat("\xFF\xFF\xFF\xFF", 4), 'width' => 2, 'height' => 2]);

    $red  = [1.0, 0.0, 0.0, 1.0];
    $blue = [0.0, 0.0, 1.0, 1.0];

    /* ---- (1) parity: uniform change between records, one record with a
     *      texture and one without, no per-record pipeline ------------------ */
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $pipeA);
    vio_set_uniforms($ctx, ['u_model' => ident(), 'u_tint' => $red, 'u_has_tex' => 0]);
    vio_draw($ctx, $meshL);
    vio_bind_texture($ctx, $tex, 0);
    vio_set_uniforms($ctx, ['u_tint' => $blue, 'u_has_tex' => 1, 'u_tex' => 0]);
    vio_draw($ctx, $meshR);
    vio_end($ctx);
    $single = vio_read_pixels($ctx);

    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $pipeA);
    vio_submit_batch($ctx, [
        ['mesh' => $meshL, 'uniforms' => ['u_model' => ident(), 'u_tint' => $red, 'u_has_tex' => 0]],
        ['mesh' => $meshR, 'textures' => [0 => $tex], 'uniforms' => ['u_tint' => $blue, 'u_has_tex' => 1, 'u_tex' => 0]],
    ]);
    vio_end($ctx);
    $batch = vio_read_pixels($ctx);

    if ($single === false || $batch === false) {
        $fail[] = "read_pixels failed";
    } else {
        if ($single !== $batch) $fail[] = "batch differs from per-draw submission";
        /* Guard against both paths being equally wrong. */
        if (!near(px($single, 6, $W >> 1, $W), [255, 0, 0]))   $fail[] = "left quad not red " . json_encode(px($single, 6, $W >> 1, $W));
        if (!near(px($single, $W - 7, $W >> 1, $W), [0, 0, 255])) $fail[] = "right quad not blue " . json_encode(px($single, $W - 7, $W >> 1, $W));
    }

    /* ---- (2) regression: a per-record 'pipeline' must bind, on every backend */
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $pipeA);
    vio_set_uniforms($ctx, ['u_model' => ident(), 'u_tint' => $red, 'u_has_tex' => 0]);
    vio_draw($ctx, $meshL);
    vio_bind_pipeline($ctx, $pipeB);
    vio_set_uniforms($ctx, ['u_model' => ident()]);
    vio_draw($ctx, $meshR);
    vio_end($ctx);
    $refSwitch = vio_read_pixels($ctx);

    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $pipeA);
    vio_submit_batch($ctx, [
        ['mesh' => $meshL, 'uniforms' => ['u_model' => ident(), 'u_tint' => $red, 'u_has_tex' => 0]],
        ['mesh' => $meshR, 'pipeline' => $pipeB, 'uniforms' => ['u_model' => ident()]],
    ]);
    vio_end($ctx);
    $batSwitch = vio_read_pixels($ctx);

    if ($refSwitch === false || $batSwitch === false) {
        $fail[] = "read_pixels failed (pipeline switch)";
    } else {
        if ($refSwitch !== $batSwitch) $fail[] = "per-record 'pipeline' differs from explicit vio_bind_pipeline";
        if (!near(px($batSwitch, $W - 7, $W >> 1, $W), [0, 255, 0]))
            $fail[] = "per-record 'pipeline' did not bind: right quad " . json_encode(px($batSwitch, $W - 7, $W >> 1, $W)) . ", expected green";
        if (!near(px($batSwitch, 6, $W >> 1, $W), [255, 0, 0]))
            $fail[] = "record before the switch changed: left quad " . json_encode(px($batSwitch, 6, $W >> 1, $W)) . ", expected red";
    }

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
