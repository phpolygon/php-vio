--TEST--
ASTC textures (VIO_FORMAT_ASTC_4x4 / 5x5 / 6x6 / 8x8, VIO_FEATURE_TEXTURE_COMPRESSION_ASTC): a hand-encoded void-extent block (constant colour) decodes on every backend with the flag, also from a KTX2 container; without the flag vio_texture refuses with a warning naming the flag; short data is refused
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A20. A void-extent block (ASTC spec 23.24) is a whole block of
 * one UNORM16 colour - the one ASTC block that is easy to write by hand. */
function voidExtent(int $r, int $g, int $b, int $a): string { return pack('V2', 0xFFFFFDFC, 0xFFFFFFFF) . pack('v4', $r, $g, $b, $a); }
function ktx2(int $vk, int $w, int $h, string $level): string {
    $hdr = "\xABKTX 20\xBB\r\n\x1A\n" . pack('V9', $vk, 1, $w, $h, 0, 0, 1, 1, 0);
    $dfdOff = 80 + 24;
    $dfd = pack('V', 4);
    $dataStart = ($dfdOff + strlen($dfd) + 7) & ~7;
    $index = pack('V4', $dfdOff, strlen($dfd), 0, 0) . pack('P2', 0, 0) . pack('P3', $dataStart, strlen($level), strlen($level));
    return str_pad($hdr . $index . $dfd, $dataStart, "\0") . $level;
}
function near(array $a, array $b): bool { for ($i = 0; $i < 3; $i++) if (abs($a[$i] - $b[$i]) > 6) return false; return true; }
$W = 8;
$block = voidExtent(0xFFFF, 0x8080, 0x0000, 0xFFFF);   // (255, 128, 0)
$want = [255, 128, 0];
$cases = [
    'ASTC_4x4' => [VIO_FORMAT_ASTC_4x4, str_repeat($block, 4)],   // 8x8 = 2x2 blocks
    'ASTC_5x5' => [VIO_FORMAT_ASTC_5x5, str_repeat($block, 4)],   // 8x8 = 2x2 blocks (partial)
    'ASTC_6x6' => [VIO_FORMAT_ASTC_6x6, str_repeat($block, 4)],
    'ASTC_8x8' => [VIO_FORMAT_ASTC_8x8, $block],                  // one block
];

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    $ctx = @vio_create($b, ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    $fail = [];
    $has = vio_supports_feature($ctx, VIO_FEATURE_TEXTURE_COMPRESSION_ASTC);
    if ($has) {
        $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nout vec2 v_uv;\nvoid main(){ v_uv = aPos.xy * 0.5 + 0.5; gl_Position = vec4(aPos, 1.0); }";
        $fs = "#version 330 core\nin vec2 v_uv;\nuniform sampler2D u_tex;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texture(u_tex, v_uv); }";
        $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs]), 'depth_test' => false]);
        $full = vio_mesh($ctx, ['vertices' => [-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0], 'indices' => [0, 1, 2, 0, 2, 3], 'layout' => [VIO_FLOAT3]]);
        $sample = function ($tex) use ($ctx, $pipe, $full, $W) {
            vio_clear($ctx, 0, 0, 0, 1);
            vio_begin($ctx);
            vio_bind_pipeline($ctx, $pipe);
            vio_set_uniform($ctx, 'u_tex', 0);
            vio_bind_texture($ctx, $tex, 0);
            vio_draw($ctx, $full);
            vio_end($ctx);
            $p = vio_read_pixels($ctx);
            $o = (4 * $W + 4) * 4;
            return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])];
        };
        foreach ($cases as $name => [$fmt, $data]) {
            $tex = vio_texture($ctx, ['format' => $fmt, 'data' => $data, 'width' => $W, 'height' => $W, 'filter' => VIO_FILTER_NEAREST]);
            if (!$tex) { $fail[] = "$name refused"; continue; }
            if (!near($got = $sample($tex), $want)) $fail[] = "$name " . json_encode($got);
        }
        $k = vio_texture_ktx2($ctx, ktx2(157, $W, $W, str_repeat($block, 4)), ['filter' => VIO_FILTER_NEAREST]);
        if (!$k) $fail[] = "ktx2 refused";
        elseif (!near($got = $sample($k), $want)) $fail[] = "ktx2 " . json_encode($got);
    } else {
        if (@vio_texture($ctx, ['format' => VIO_FORMAT_ASTC_4x4, 'data' => $cases['ASTC_4x4'][1], 'width' => $W, 'height' => $W]) !== false) $fail[] = "accepted without the flag";
        elseif (!str_contains(error_get_last()['message'] ?? '', 'VIO_FEATURE_TEXTURE_COMPRESSION_ASTC')) $fail[] = "message: " . (error_get_last()['message'] ?? '');
    }
    if (@vio_texture($ctx, ['format' => VIO_FORMAT_ASTC_4x4, 'data' => $block, 'width' => $W, 'height' => $W]) !== false) $fail[] = "short data accepted";
    echo "$b: ", $fail ? "FAIL " . implode('; ', $fail) : ($has ? "OK decoded" : "OK refused"), "\n";
    vio_destroy($ctx);
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK (decoded|refused)|skip \(.*\))%r
d3d11: %r(OK refused|skip \(.*\))%r
d3d12: %r(OK refused|skip \(.*\))%r
vulkan: %r(OK (decoded|refused)|skip \(.*\))%r
metal: %r(OK (decoded|refused)|skip \(.*\))%r
DONE
