--TEST--
Raw render-target readback: vio_read_render_target($rt, -1, $i, ['raw' => true]) returns the attachment's texels bit-exact in its own format (RGBA16F / RG16F / R16F / R32F halves and floats unclamped, R11G11B10F / RGB10A2 packed, RGBA8 in R, G, B, A order), top-down like the RGBA8 readback, on every backend
--EXTENSIONS--
vio
--FILE--
<?php
/* TEMPORAL-UPSCALING S0: motion vectors and history buffers are floats outside
 * [0, 1] - the RGBA8 readback clamps and quantises them, so tests compare raw
 * texels instead. Every value written is exactly representable in its format,
 * so the expected bytes are known without any rounding rule. The full target
 * gets values A, the top half (NDC y > 0) values B afterwards; the RGBA8
 * readback (blue / red marker) tells where the top is. */
$W = 8; $H = 4;
$formats = [VIO_FORMAT_RGBA16F, VIO_FORMAT_RG16F, VIO_FORMAT_R16F, VIO_FORMAT_R32F,
            VIO_FORMAT_RGBA8, VIO_FORMAT_R11G11B10F, VIO_FORMAT_RGB10A2];
$h = fn(int ...$v) => pack('v*', ...$v);   // IEEE half bits, little endian
$A = ['v' => [1.5, -2.0, 0.25, 1024.0], 'f' => 3.14159265358979, 'c' => [0.0, 0.0, 1.0, 1.0],
      'p' => [1.5, 0.25, 3.0], 'u' => [1.0, 0.0, 1.0, 1.0]];
$B = ['v' => [0.0009765625, -0.5, 3.0, 65504.0], 'f' => -123456.789, 'c' => [1.0, 0.0, 0.0, 1.0],
      'p' => [0.125, 64.0, 0.5], 'u' => [0.0, 1.0, 0.0, 0.0]];
$expect = [
    // [A texel, B texel] per attachment
    [$h(0x3E00, 0xC000, 0x3400, 0x6400), $h(0x1400, 0xB800, 0x4200, 0x7BFF)],   // RGBA16F
    [$h(0xC000, 0x3400), $h(0xB800, 0x4200)],                                   // RG16F  (v.y, v.z)
    [$h(0x6400), $h(0x7BFF)],                                                   // R16F   (v.w)
    [pack('g', $A['f']), pack('g', $B['f'])],                                   // R32F
    ["\x00\x00\xFF\xFF", "\xFF\x00\x00\xFF"],                                   // RGBA8
    [pack('V', 0x3E0 | (0x340 << 11) | (0x210 << 22)), pack('V', 0x300 | (0x540 << 11) | (0x1C0 << 22))],   // R11G11B10F
    [pack('V', 1023 | (0 << 10) | (1023 << 20) | (3 << 30)), pack('V', 0 | (1023 << 10) | (0 << 20) | (0 << 30))],   // RGB10A2
];
$bpp = [8, 4, 2, 4, 4, 4, 4];

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    $ctx = @vio_create($b, ['width' => $W, 'height' => $H, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    if (!vio_supports_feature($ctx, VIO_FEATURE_MRT) || !vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) {
        echo "$b: skip (no MRT)\n"; vio_destroy($ctx); continue;
    }
    $fail = [];
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $fs = "#version 330 core\nuniform vec4 u_v;\nuniform float u_f;\nuniform vec4 u_c;\nuniform vec3 u_p;\nuniform vec4 u_u;\n"
        . "layout(location=0) out vec4 o0;\nlayout(location=1) out vec4 o1;\nlayout(location=2) out vec4 o2;\nlayout(location=3) out vec4 o3;\n"
        . "layout(location=4) out vec4 o4;\nlayout(location=5) out vec4 o5;\nlayout(location=6) out vec4 o6;\n"
        . "void main(){ o0 = u_v; o1 = vec4(u_v.y, u_v.z, 0.0, 1.0); o2 = vec4(u_v.w, 0.0, 0.0, 1.0); o3 = vec4(u_f, 0.0, 0.0, 1.0);\n"
        . "  o4 = u_c; o5 = vec4(u_p, 1.0); o6 = u_u; }";
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs]),
                                'depth_test' => false, 'cull_mode' => VIO_CULL_NONE, 'blend' => VIO_BLEND_NONE,
                                'attachments' => $formats]);
    $mesh = fn(float $y0, float $y1) => vio_mesh($ctx, ['vertices' => [-1, $y0, 0, 1, $y0, 0, 1, $y1, 0, -1, $y1, 0],
                                                       'indices' => [0, 1, 2, 0, 2, 3], 'layout' => [VIO_FLOAT3]]);
    $full = $mesh(-1, 1);
    $top = $mesh(0, 1);
    $rt = vio_render_target($ctx, ['width' => $W, 'height' => $H, 'attachments' => $formats]);
    if (!$rt || !$pipe) { echo "$b: FAIL target / pipeline\n"; vio_destroy($ctx); continue; }
    $set = function (array $s) use ($ctx) {
        vio_set_uniform($ctx, 'u_v', $s['v']);
        vio_set_uniform($ctx, 'u_f', $s['f']);
        vio_set_uniform($ctx, 'u_c', $s['c']);
        vio_set_uniform($ctx, 'u_p', $s['p']);
        vio_set_uniform($ctx, 'u_u', $s['u']);
    };
    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt);
    vio_bind_pipeline($ctx, $pipe);
    $set($A); vio_draw($ctx, $full);
    $set($B); vio_draw($ctx, $top);
    vio_unbind_render_target($ctx);
    vio_end($ctx);

    // The RGBA8 readback says which rows are the top (B, red) - rows 0 .. H/2-1, top-down.
    $rgba = vio_read_render_target($rt, -1, 4);
    if (substr($rgba, 0, 4) !== "\xFF\x00\x00\xFF" || substr($rgba, -4) !== "\x00\x00\xFF\xFF") {
        $fail[] = "RGBA8 orientation " . bin2hex(substr($rgba, 0, 4)) . " / " . bin2hex(substr($rgba, -4));
    }
    foreach ($formats as $i => $f) {
        $raw = vio_read_render_target($rt, -1, $i, ['raw' => true]);
        $want = str_repeat($expect[$i][1], $W * $H / 2) . str_repeat($expect[$i][0], $W * $H / 2);
        if (!is_string($raw) || strlen($raw) !== $W * $H * $bpp[$i]) {
            $fail[] = "att$i raw size " . (is_string($raw) ? strlen($raw) : var_export($raw, true)) . ", want " . ($W * $H * $bpp[$i]);
        } elseif ($raw !== $want) {
            $n = $bpp[$i];
            $fail[] = "att$i raw top " . bin2hex(substr($raw, 0, $n)) . " (want " . bin2hex($expect[$i][1]) . "), bottom "
                    . bin2hex(substr($raw, -$n)) . " (want " . bin2hex($expect[$i][0]) . ")";
        }
    }
    // Without the option the readback stays RGBA8 (clamped).
    if (strlen(vio_read_render_target($rt, -1, 0)) !== $W * $H * 4) $fail[] = "default readback not RGBA8";
    if (strlen(vio_read_render_target($rt, -1, 0, ['raw' => false])) !== $W * $H * 4) $fail[] = "'raw' => false not RGBA8";
    // A depth_only target has no colour texels to return raw.
    $d = vio_render_target($ctx, ['width' => $W, 'height' => $H, 'depth_only' => true]);
    if (@vio_read_render_target($d, -1, 0, ['raw' => true]) !== false) $fail[] = "raw depth readback accepted";

    unset($rt, $d);
    echo "$b: ", $fail ? "FAIL " . implode('; ', $fail) : "OK", "\n";
    vio_destroy($ctx);
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE
