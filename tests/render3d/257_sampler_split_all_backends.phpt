--TEST--
Ten regular and six comparison samplers in one fragment shader read their own textures on every backend: any unit may compare (no fixed regular / comparison split since SAMPLER-LIMITS-PLAN; until then D3D12 / Vulkan allowed at most 4 comparison samplers), and vio_backend_info()['samplers'] reports the backend's own limits
--EXTENSIONS--
vio
--FILE--
<?php
$N = 10;           // regular samplers
$S = 6;            // comparison samplers (more than the former 4)
$W = 16;           // one column per sampler: 0..9 regular, 10..15 comparison
$VS = "#version 450\nlayout(location=0) in vec3 a_pos;\nvoid main(){ gl_Position = vec4(a_pos, 1.0); }";
$decl = '';
$body = "    int c = int(gl_FragCoord.x);\n    vec4 v = vec4(0.0);\n";
for ($i = 0; $i < $N; $i++) {
    $decl .= "uniform sampler2D u_t$i;\n";
    $body .= "    if (c == $i) v = texture(u_t$i, vec2(0.5));\n";
}
for ($k = 0; $k < $S; $k++) {
    $decl .= "uniform sampler2DShadow u_s$k;\n";
    $ref = $k % 2 === 0 ? '0.25' : '0.75';   // stored 0.5: 0.25 <= 0.5 -> 1, 0.75 > 0.5 -> 0
    $body .= "    if (c == " . ($N + $k) . ") v = vec4(texture(u_s$k, vec3(0.5, 0.5, $ref)));\n";
}
$FS = "#version 450\n$decl" . "layout(location=0) out vec4 o;\nvoid main(){\n$body    o = vec4(v.rgb, 1.0);\n}";
$DEPTH_FS = "#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(1.0); }";

function run_backend(string $name): string {
    global $N, $S, $W, $VS, $FS, $DEPTH_FS;
    $ctx = @vio_create($name, ['width' => $W, 'height' => 4, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D pipeline)"; }
    $fail = [];

    $s = vio_backend_info($ctx)['samplers'] ?? null;
    if (!is_array($s) || $s['total'] < 16 || $s['regular'] !== $s['total'] || $s['comparison'] !== $s['total']
        || $s['comparison_base'] !== 0 || $s['textures'] < $s['total'] || $s['sampler_states'] < $s['total']) {
        $fail[] = "samplers " . json_encode($s);
    }

    // Depth 0.5 everywhere: a full-screen triangle at NDC z 0.
    $tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
    $dt = vio_render_target($ctx, ['width' => 4, 'height' => 4, 'depth_only' => true]);
    $dp = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $DEPTH_FS]), 'depth_test' => true, 'cull_mode' => VIO_CULL_NONE]);
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    if (!$sh) { vio_destroy($ctx); return "FAIL: shader"; }
    $p = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $tex = [];
    for ($i = 0; $i < $N; $i++) $tex[$i] = vio_texture($ctx, ['data' => chr(20 * ($i + 1)) . chr(255 - 20 * $i) . chr(7 * $i) . "\xFF", 'width' => 1, 'height' => 1]);

    vio_begin($ctx);
    vio_bind_render_target($ctx, $dt);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $dp);
    vio_draw($ctx, $tri);
    vio_unbind_render_target($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $p);
    for ($i = 0; $i < $N; $i++) { vio_set_uniform($ctx, "u_t$i", $i); vio_bind_texture($ctx, $tex[$i], $i); }
    $shadow = vio_render_target_texture($dt);
    for ($k = 0; $k < $S; $k++) { vio_set_uniform($ctx, "u_s$k", $N + $k); vio_bind_texture($ctx, $shadow, $N + $k); }
    vio_draw($ctx, $tri);
    vio_end($ctx);

    $px = vio_read_pixels($ctx);
    for ($c = 0; $c < $N + $S; $c++) {
        $o = (1 * $W + $c) * 4;
        $got = [ord($px[$o]), ord($px[$o + 1]), ord($px[$o + 2])];
        $exp = $c < $N ? [20 * ($c + 1), 255 - 20 * $c, 7 * $c] : ((($c - $N) % 2 === 0) ? [255, 255, 255] : [0, 0, 0]);
        foreach ($exp as $k => $v) if (abs($got[$k] - $v) > 3) { $fail[] = "column $c " . json_encode($got) . " want " . json_encode($exp); break; }
    }
    vio_destroy($ctx);
    return $fail ? 'FAIL: ' . implode('; ', $fail) : 'OK';
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) echo "$b: ", run_backend($b), "\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
