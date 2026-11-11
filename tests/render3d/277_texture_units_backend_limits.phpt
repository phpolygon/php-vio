--TEST--
Texture units follow the backend's own limits (SAMPLER-LIMITS-PLAN): vio_backend_info()['samplers'] reports them, a fragment shader with as many samplers as the backend allows (up to 40 here, every fifth comparing) reads its own texture in every column, and a shader with one sampler more than the limit fails with a clear warning instead of losing samplers
--EXTENSIONS--
vio
--FILE--
<?php
$VS = "#version 450\nlayout(location=0) in vec3 a_pos;\nvoid main(){ gl_Position = vec4(a_pos, 1.0); }";

function fs_for(int $n): string {
    $decl = ''; $body = "    int c = int(gl_FragCoord.x);\n    vec4 v = vec4(0.0);\n";
    for ($i = 0; $i < $n; $i++) {
        if ($i % 5 === 4) {
            $decl .= "uniform sampler2DShadow u_t$i;\n";
            $ref = intdiv($i, 5) % 2 ? '0.75' : '0.25';   // stored 0.5
            $body .= "    if (c == $i) v = vec4(texture(u_t$i, vec3(0.5, 0.5, $ref)));\n";
        } else {
            $decl .= "uniform sampler2D u_t$i;\n";
            $body .= "    if (c == $i) v = texture(u_t$i, vec2(0.5));\n";
        }
    }
    return "#version 450\n$decl" . "layout(location=0) out vec4 o;\nvoid main(){\n$body    o = vec4(v.rgb, 1.0);\n}";
}

function run_backend(string $name, array $opts = []): string {
    global $VS;
    $ctx = @vio_create($name, ['width' => 40, 'height' => 2, 'headless' => true, 'vsync' => false] + $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D pipeline)"; }
    $s = vio_backend_info($ctx)['samplers'];
    $units = $s['total'];
    if ($units < 16 || $s['textures'] < $units || $s['sampler_states'] < $units) { vio_destroy($ctx); return "FAIL: limits " . json_encode($s); }
    $tu = vio_texture_units($ctx);
    if ($tu !== ['units' => $units, 'textures' => $s['textures'], 'sampler_states' => $s['sampler_states']]) { vio_destroy($ctx); return "FAIL: vio_texture_units " . json_encode($tu); }
    $n = min(40, $units);
    $fail = [];

    $tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
    $dt = vio_render_target($ctx, ['width' => 4, 'height' => 4, 'depth_only' => true]);
    $dp = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => "#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(1.0); }"]), 'depth_test' => true, 'cull_mode' => VIO_CULL_NONE]);
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => fs_for($n)]);
    if (!$sh) { vio_destroy($ctx); return "FAIL: shader with $n samplers"; }
    $p = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $tex = [];
    for ($i = 0; $i < $n; $i++) $tex[$i] = vio_texture($ctx, ['data' => chr((6 * $i) % 256) . chr(255 - (6 * $i) % 256) . chr(100) . "\xFF", 'width' => 1, 'height' => 1]);

    vio_begin($ctx);
    vio_bind_render_target($ctx, $dt); vio_clear($ctx, 0, 0, 0, 1); vio_bind_pipeline($ctx, $dp); vio_draw($ctx, $tri); vio_unbind_render_target($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $p);
    $shadow = vio_render_target_texture($dt);
    for ($i = 0; $i < $n; $i++) { vio_set_uniform($ctx, "u_t$i", $i); vio_bind_texture($ctx, $i % 5 === 4 ? $shadow : $tex[$i], $i); }
    vio_draw($ctx, $tri);
    vio_end($ctx);

    $px = vio_read_pixels($ctx);
    for ($c = 0; $c < $n; $c++) {
        $o = (40 + $c) * 4;
        $got = [ord($px[$o]), ord($px[$o + 1]), ord($px[$o + 2])];
        $exp = $c % 5 === 4 ? ((intdiv($c, 5) % 2) ? [0, 0, 0] : [255, 255, 255]) : [(6 * $c) % 256, 255 - (6 * $c) % 256, 100];
        foreach ($exp as $k => $v) if (abs($got[$k] - $v) > 3) { $fail[] = "column $c " . json_encode($got); break; }
    }

    // One sampler over the limit: a warning naming the limit, no shader.
    if ($units < 40) {
        $warn = '';
        set_error_handler(function ($no, $msg) use (&$warn) { $warn .= $msg . "\n"; return true; });
        $over = vio_shader($ctx, ['vertex' => $VS, 'fragment' => fs_for($units + 1)]);
        restore_error_handler();
        if ($over !== false) $fail[] = "a shader over the limit was created";
        elseif ($name !== 'opengl' && !str_contains($warn, "binds at most $units per stage")) $fail[] = "warning: " . trim($warn);
    }
    vio_destroy($ctx);
    return $fail ? 'FAIL: ' . implode('; ', $fail) : "OK ($n of $units)";
}

foreach (['opengl', 'd3d11', 'vulkan', 'metal'] as $b) echo "$b: ", run_backend($b), "\n";
echo "d3d12: ", run_backend('d3d12'), "\n";
echo "d3d12 sm6: ", run_backend('d3d12', ['shader_model' => 6]), "\n";
?>
--EXPECTF--
opengl: %r(OK \(\d+ of \d+\)|skip \(.*\))%r
d3d11: %r(OK \(16 of 16\)|skip \(.*\))%r
vulkan: %r(OK \(\d+ of \d+\)|skip \(.*\))%r
metal: %r(OK \(16 of 16\)|skip \(.*\))%r
d3d12: %r(OK \(16 of 16\)|skip \(.*\))%r
d3d12 sm6: %r(OK \(\d+ of \d+\)|skip \(.*\))%r
