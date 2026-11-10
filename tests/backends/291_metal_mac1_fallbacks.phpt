--TEST--
Metal on a Mac1-only GPU (VIO_METAL_GPU_FAMILY=mac1, the class of the Apple Paravirtual device in macOS VMs such as the Intel GitHub runners): without texture swizzle, attachment-less passes, 2DMultisampleArray and binary archives the fallbacks render text, cube / array MSAA targets and the tessellation capture pass like the full device; a 4x4 texture's mip chain reaches its 1x1 level; text in a second context of the process renders
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
/* Every scene renders twice: once on the device as it is, once with
 * VIO_METAL_GPU_FAMILY=mac1 (read at vio_create), and the two must agree. On a
 * Mac1-only device both runs take the fallbacks. Each scene opens a second
 * context after the first (texture ids of a destroyed context must not
 * release the textures of the next one). */
$fail = [];
function open_metal(bool $mac1, int $w = 64, int $h = 64) {
    putenv($mac1 ? 'VIO_METAL_GPU_FAMILY=mac1' : 'VIO_METAL_GPU_FAMILY');
    $ctx = vio_create('metal', ['width' => $w, 'height' => $h, 'headless' => true, 'vsync' => false]);
    putenv('VIO_METAL_GPU_FAMILY');
    return $ctx;
}
function diff(string $a, string $b, int $tol = 8): int {
    $n = 0;
    for ($i = 0; $i < strlen($a); $i++) if (abs(ord($a[$i]) - ord($b[$i])) > $tol) $n++;
    return $n;
}

/* 1. The capped device reports the fallbacks. */
$ctx = open_metal(true);
$info = vio_backend_info($ctx);
foreach (['texture_swizzle', 'renderless_pass', 'msaa_array', 'binary_archive'] as $k)
    if (($info['caps'][$k] ?? null) !== false) $fail[] = "mac1: cap $k " . json_encode($info['caps'][$k] ?? null);
foreach ($info['families'] as $f) if ($f !== 'mac1' && !str_starts_with($f, 'common')) $fail[] = "mac1: family $f reported";
if (vio_supports_feature($ctx, VIO_FEATURE_TEXTURE_SWIZZLE)) $fail[] = "mac1: VIO_FEATURE_TEXTURE_SWIZZLE";
if (vio_supports_feature($ctx, VIO_FEATURE_SHADER_FLOAT16) !== $info['caps']['half_arithmetic'])
    $fail[] = "VIO_FEATURE_SHADER_FLOAT16 disagrees with the half_arithmetic probe";
vio_destroy($ctx);

/* 2. Text: R8 coverage atlas, filled on demand (create + sub-region uploads). */
$fontPath = null;
foreach (['/Library/Fonts/Arial Unicode.ttf', '/System/Library/Fonts/Helvetica.ttc', '/System/Library/Fonts/Geneva.ttf'] as $c)
    if (is_file($c)) { $fontPath = $c; break; }
if ($fontPath) {
    $img = [];
    foreach ([false, true] as $mac1) {
        $ctx = open_metal($mac1, 256, 64);
        $font = vio_font($ctx, $fontPath, 32.0);
        foreach (["Hi", "Hi there"] as $text) {   /* the second frame uploads new glyphs into the atlas */
            vio_clear($ctx, 0, 0, 0, 1);
            vio_begin($ctx);
            vio_text($ctx, $font, $text, 4.0, 40.0, ['color' => 0xFFFFFFFF]);
            vio_draw_2d($ctx);
            vio_end($ctx);
        }
        $img[] = vio_read_pixels($ctx);
        vio_destroy($ctx);
    }
    $lit = 0; for ($i = 0; $i < strlen($img[1]); $i += 4) if (ord($img[1][$i]) > 128) $lit++;
    if ($lit < 200) $fail[] = "mac1 text: $lit lit pixels";
    if (($d = diff($img[0], $img[1])) > 0) $fail[] = "text differs from the device path in $d bytes";
}

/* 3. Mip chain of an uploaded texture: a 4x4 checker averages to grey at LOD 2
 *    (the Intel UHD 630 blit leaves that 1x1 level empty: CPU levels). */
$VS = "#version 330 core\nlayout(location=0) in vec3 aPos;\nout vec2 vUv;\nvoid main(){ vUv = aPos.xy * 0.5 + 0.5; gl_Position = vec4(aPos, 1.0); }";
$FS = "#version 330 core\nin vec2 vUv;\nuniform sampler2D u_tex;\nuniform float u_lod;\nlayout(location=0) out vec4 o;\nvoid main(){ o = textureLod(u_tex, vUv, u_lod); }";
$checker = '';
for ($y = 0; $y < 4; $y++) for ($x = 0; $x < 4; $x++) $checker .= (($x + $y) & 1) ? "\xFF\xFF\xFF\xFF" : "\x00\x00\x00\xFF";
$mip = [];
foreach ([false, true] as $mac1) {
    $ctx = open_metal($mac1, 16, 16);
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $tex = vio_texture($ctx, ['data' => $checker, 'width' => 4, 'height' => 4, 'mipmaps' => true, 'filter' => VIO_FILTER_LINEAR]);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pipe);
    vio_set_uniform($ctx, 'u_tex', 0);
    vio_set_uniform($ctx, 'u_lod', 2.0);
    vio_bind_texture($ctx, $tex, 0);
    vio_draw($ctx, $quad);
    vio_end($ctx);
    $p = vio_read_pixels($ctx);
    $o = (8 * 16 + 8) * 4;
    $mip[] = [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])];
    vio_destroy($ctx);
}
foreach ($mip as $k => $px) foreach ($px as $v) if (abs($v - 128) > 12) { $fail[] = ($k ? "mac1" : "device") . " mip lod 2 " . json_encode($px); break; }

/* 4. Cube / array MSAA targets: one 2DMultisample pair per layer on mac1. */
$VS = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$FS = "#version 330 core\nuniform vec4 u_color;\nlayout(location=0) out vec4 o;\nvoid main(){ o = u_color; }";
$msaa = [];
foreach ([false, true] as $mac1) {
    $ctx = open_metal($mac1, 32, 32);
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]), 'depth_test' => true]);
    $tri = vio_mesh($ctx, ['vertices' => [-1, -1, 0.5, 1, -1, 0.5, -1, 1, 0.5], 'layout' => [VIO_FLOAT3]]);
    $far = vio_mesh($ctx, ['vertices' => [-1, -1, 0.9, 1, -1, 0.9, 1, 1, 0.9, -1, 1, 0.9], 'indices' => [0, 1, 2, 0, 2, 3], 'layout' => [VIO_FLOAT3]]);
    $out = [];
    foreach (['cube' => ['cube' => true, 'size' => 32, 'samples' => 4],
              'array' => ['width' => 32, 'height' => 32, 'layers' => 3, 'samples' => 4]] as $kind => $opt) {
        $rt = vio_render_target($ctx, $opt);
        if (!$rt) { $fail[] = ($mac1 ? "mac1" : "device") . " $kind refused"; continue; }
        vio_begin($ctx);
        vio_bind_render_target($ctx, $rt, 2);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniform($ctx, 'u_color', [0.0, 1.0, 0.0, 1.0]);
        vio_draw($ctx, $tri);
        vio_bind_render_target($ctx, $rt, 1);   /* leaving layer 2 resolves it */
        vio_clear($ctx, 0, 0, 1, 1);
        vio_bind_render_target($ctx, $rt, 2);   /* its depth still hides the far quad */
        vio_set_uniform($ctx, 'u_color', [1.0, 0.0, 0.0, 1.0]);
        vio_draw($ctx, $far);
        vio_unbind_render_target($ctx);
        vio_end($ctx);
        $out[$kind] = [vio_read_render_target($rt, 0), vio_read_render_target($rt, 1), vio_read_render_target($rt, 2)];
        $blend = 0; $l2 = $out[$kind][2];
        for ($i = 0; $i < 32 * 32; $i++) { $g = ord($l2[$i * 4 + 1]); if ($g > 15 && $g < 240) $blend++; }
        if ($blend < 4) $fail[] = ($mac1 ? "mac1" : "device") . " $kind: layer 2 not multisampled ($blend edge pixels)";
    }
    $msaa[] = $out;
    vio_destroy($ctx);
}
foreach (['cube', 'array'] as $kind)
    for ($l = 0; $l < 3; $l++)
        if (isset($msaa[0][$kind], $msaa[1][$kind]) && ($d = diff($msaa[0][$kind][$l], $msaa[1][$kind][$l])) > 0)
            $fail[] = "$kind layer $l differs from the device path in $d bytes";

/* 5. Tessellation capture pass (point_mode, emulated): attachment-less on the
 *    device, a 1x1 dummy colour on mac1. 16 points, each exactly once. */
$ctx = open_metal(false);
$tess = vio_supports_feature($ctx, VIO_FEATURE_TESSELLATION);
vio_destroy($ctx);
if ($tess) {
    $VS = "#version 450\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $FS = "#version 450\nlayout(location=0) in vec3 c;\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(c, 1.0); }";
    $TCS = "#version 450\nlayout(vertices = 3) out;\n"
         . "void main(){ gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n"
         . "  if (gl_InvocationID == 0) { gl_TessLevelOuter[0] = 2.0; gl_TessLevelOuter[1] = 3.0; gl_TessLevelOuter[2] = 4.0; gl_TessLevelInner[0] = 4.0; } }";
    $TES = "#version 450\nlayout(triangles, equal_spacing, ccw, point_mode) in;\nlayout(location=0) out vec3 c;\n"
         . "void main(){ float s = gl_TessCoord.x, t = gl_TessCoord.y; c = vec3(0.25, 0.0, 0.0);"
         . " gl_Position = vec4(-0.75 + 1.5 * s + 1.0 / 64.0, -0.75 + 1.5 * t + 1.0 / 64.0, 0.0, 1.0); }";
    $pts = [];
    foreach ([false, true] as $mac1) {
        $ctx = open_metal($mac1);
        $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'tess_control' => $TCS, 'tess_eval' => $TES, 'fragment' => $FS]),
                                    'patch_vertices' => 3, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE, 'blend' => VIO_BLEND_ADDITIVE]);
        $tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 0,1,0], 'layout' => [VIO_FLOAT3]]);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx); vio_bind_pipeline($ctx, $pipe); vio_draw($ctx, $tri); vio_end($ctx);
        $p = vio_read_pixels($ctx);
        $lit = 0; $dup = 0;
        for ($i = 0; $i < 64 * 64; $i++) { $r = ord($p[$i * 4]); if ($r) { $lit++; if ($r > 72) $dup++; } }
        if ($lit !== 16 || $dup) $fail[] = ($mac1 ? "mac1" : "device") . " point_mode: $lit points ($dup twice), want 16";
        vio_destroy($ctx);
    }
}

echo $fail ? "FAIL\n  " . implode("\n  ", $fail) . "\n" : "OK\n";
?>
--EXPECT--
OK
