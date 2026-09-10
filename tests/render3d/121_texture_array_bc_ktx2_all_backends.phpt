--TEST--
Texture arrays, BC-compressed data and KTX2 containers on every backend (vio_texture 'layers' / 'format' / 'mip_levels', vio_texture_ktx2)
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null' || $b === 'vulkan') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE) && vio_supports_feature($c, VIO_FEATURE_TEXTURE_ARRAY)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with texture arrays");
?>
--FILE--
<?php
/* GAP-PHASE5-PLAN Block 9. A two-layer RGBA8 array with an explicit two-level
 * chain (layer 0: red / blue, layer 1: green / white) sampled per layer and
 * LOD; a hand-encoded BC1 block (solid green); the same as KTX2 containers -
 * BC1 4x4 and RGBA8 with two levels loaded with 'mip_offset' => 1 (the 1x1
 * blue level becomes the base); garbage bytes are refused with false. */
$W = 16;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $c, array $want, int $tol = 4): bool { return abs($c[0]-$want[0]) <= $tol && abs($c[1]-$want[1]) <= $tol && abs($c[2]-$want[2]) <= $tol; }
function rgba(int $r, int $g, int $b, int $n): string { return str_repeat(chr($r) . chr($g) . chr($b) . "\xFF", $n); }

/* Minimal KTX2 writer: header, index, level index, a 4-byte DFD stub, no KVD,
 * levels stored smallest-first with 8-byte alignment (as ktx tools do). */
function ktx2(int $vkFormat, int $w, int $h, int $layers, array $levels): string {
    $n = count($levels);
    $hdr = "\xABKTX 20\xBB\r\n\x1A\n" . pack('V9', $vkFormat, 1, $w, $h, 0, $layers > 1 ? $layers : 0, 1, $n, 0);
    $levelIndexLen = $n * 24;
    $dfdOff = 80 + $levelIndexLen;
    $dfd = pack('V', 4);
    $dataStart = ($dfdOff + strlen($dfd) + 7) & ~7;
    $blob = '';
    $offsets = [];
    for ($l = $n - 1; $l >= 0; $l--) {
        while ((($dataStart + strlen($blob)) % 8) !== 0) $blob .= "\0";
        $offsets[$l] = $dataStart + strlen($blob);
        $blob .= $levels[$l];
    }
    $index = pack('V4', $dfdOff, strlen($dfd), 0, 0) . pack('P2', 0, 0);
    $lvl = '';
    for ($l = 0; $l < $n; $l++) $lvl .= pack('P3', $offsets[$l], strlen($levels[$l]), strlen($levels[$l]));
    return str_pad($hdr . $index . $lvl . $dfd, $dataStart, "\0") . $blob;
}

function run_backend(string $name): string {
    global $W;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE) || !vio_supports_feature($ctx, VIO_FEATURE_TEXTURE_ARRAY)) {
        vio_destroy($ctx);
        return "skip (no texture arrays)";
    }
    $fail = [];
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $fsArr = "#version 330 core\nuniform sampler2DArray u_tex; uniform float u_layer; uniform float u_lod;\n"
           . "layout(location=0) out vec4 o;\nvoid main(){ o = textureLod(u_tex, vec3(0.5, 0.5, u_layer), u_lod); }";
    $fs2d  = "#version 330 core\nuniform sampler2D u_tex; uniform float u_lod;\n"
           . "layout(location=0) out vec4 o;\nvoid main(){ o = textureLod(u_tex, vec2(0.5, 0.5), u_lod); }";
    $pArr = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsArr, 'format' => $fmt]), 'depth_test' => false]);
    $p2d  = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs2d, 'format' => $fmt]), 'depth_test' => false]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);

    $sample = static function ($pipe, $tex, float $lod, ?float $layer = null) use ($ctx, $quad, $W): array {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniform($ctx, 'u_tex', 0);
        vio_bind_texture($ctx, $tex, 0);
        vio_set_uniform($ctx, 'u_lod', $lod);
        if ($layer !== null) vio_set_uniform($ctx, 'u_layer', $layer);
        vio_draw($ctx, $quad);
        vio_end($ctx);
        return px(vio_read_pixels($ctx), $W >> 1, $W >> 1, $W);
    };

    /* Two-layer 2x2 array with an explicit 1x1 second level (level-major, layers inside). */
    $data = rgba(255, 0, 0, 4) . rgba(0, 255, 0, 4)      /* level 0: layer 0 red, layer 1 green */
          . rgba(0, 0, 255, 1) . rgba(255, 255, 255, 1); /* level 1: layer 0 blue, layer 1 white */
    $arr = vio_texture($ctx, ['data' => $data, 'width' => 2, 'height' => 2, 'layers' => 2, 'mip_levels' => 2, 'filter' => VIO_FILTER_NEAREST]);
    if (!($arr instanceof VioTexture)) { $fail[] = "array texture not created"; }
    else {
        if (vio_texture_size($arr) !== [2, 2, 2, VIO_FORMAT_RGBA8]) $fail[] = "array size " . json_encode(vio_texture_size($arr));
        if (!near($c = $sample($pArr, $arr, 0.0, 0.0), [255, 0, 0])) $fail[] = "layer 0 lod 0 " . json_encode($c);
        if (!near($c = $sample($pArr, $arr, 0.0, 1.0), [0, 255, 0])) $fail[] = "layer 1 lod 0 " . json_encode($c);
        if (!near($c = $sample($pArr, $arr, 1.0, 0.0), [0, 0, 255])) $fail[] = "layer 0 lod 1 " . json_encode($c);
        if (!near($c = $sample($pArr, $arr, 1.0, 1.0), [255, 255, 255])) $fail[] = "layer 1 lod 1 " . json_encode($c);
    }
    /* Too little data is refused. */
    if (@vio_texture($ctx, ['data' => rgba(0, 0, 0, 4), 'width' => 2, 'height' => 2, 'layers' => 2]) !== false) $fail[] = "short array data accepted";

    /* BC1: one 4x4 block, colour0 = colour1 = green (RGB565 0x07E0), all indices 0. */
    $bc1 = "\xE0\x07\xE0\x07\x00\x00\x00\x00";
    $hasBc = vio_supports_feature($ctx, VIO_FEATURE_TEXTURE_COMPRESSION_BC);
    if ($hasBc) {
        $tex = vio_texture($ctx, ['data' => $bc1, 'width' => 4, 'height' => 4, 'format' => VIO_FORMAT_BC1]);
        if (!($tex instanceof VioTexture)) $fail[] = "BC1 texture not created";
        elseif (!near($c = $sample($p2d, $tex, 0.0), [0, 255, 0])) $fail[] = "BC1 sample " . json_encode($c);
        elseif (vio_texture_size($tex) !== [4, 4, 1, VIO_FORMAT_BC1]) $fail[] = "BC1 size " . json_encode(vio_texture_size($tex));
        /* Compressed textures cannot be partially updated. */
        if ($tex instanceof VioTexture && @vio_texture_update($ctx, $tex, rgba(0, 0, 0, 1), 0, 0, 1, 1) !== false) $fail[] = "BC1 update accepted";

        $k = vio_texture_ktx2($ctx, ktx2(133 /* BC1_RGBA_UNORM */, 4, 4, 1, [$bc1]));
        if (!($k instanceof VioTexture)) $fail[] = "KTX2 BC1 not created";
        elseif (!near($c = $sample($p2d, $k, 0.0), [0, 255, 0])) $fail[] = "KTX2 BC1 sample " . json_encode($c);
    } else {
        if (@vio_texture($ctx, ['data' => $bc1, 'width' => 4, 'height' => 4, 'format' => VIO_FORMAT_BC1]) !== false) $fail[] = "BC1 accepted without the feature";
    }

    /* KTX2 RGBA8, two levels (2x2 red, 1x1 blue): mip_offset 1 makes the blue level the base. */
    $file = ktx2(37 /* R8G8B8A8_UNORM */, 2, 2, 1, [rgba(255, 0, 0, 4), rgba(0, 0, 255, 1)]);
    $k0 = vio_texture_ktx2($ctx, $file, ['filter' => VIO_FILTER_NEAREST]);
    $k1 = vio_texture_ktx2($ctx, $file, ['filter' => VIO_FILTER_NEAREST, 'mip_offset' => 1]);
    if (!($k0 instanceof VioTexture) || !($k1 instanceof VioTexture)) $fail[] = "KTX2 RGBA8 not created";
    else {
        if (vio_texture_size($k0) !== [2, 2, 1, VIO_FORMAT_RGBA8]) $fail[] = "KTX2 size " . json_encode(vio_texture_size($k0));
        if (vio_texture_size($k1) !== [1, 1, 1, VIO_FORMAT_RGBA8]) $fail[] = "KTX2 mip_offset size " . json_encode(vio_texture_size($k1));
        if (!near($c = $sample($p2d, $k0, 0.0), [255, 0, 0])) $fail[] = "KTX2 lod 0 " . json_encode($c);
        if (!near($c = $sample($p2d, $k0, 1.0), [0, 0, 255])) $fail[] = "KTX2 lod 1 " . json_encode($c);
        if (!near($c = $sample($p2d, $k1, 0.0), [0, 0, 255])) $fail[] = "KTX2 mip_offset base " . json_encode($c);
    }
    /* A two-layer KTX2 array. */
    $ka = vio_texture_ktx2($ctx, ktx2(37, 2, 2, 2, [rgba(255, 0, 0, 4) . rgba(0, 255, 0, 4)]), ['filter' => VIO_FILTER_NEAREST]);
    if (!($ka instanceof VioTexture)) $fail[] = "KTX2 array not created";
    elseif (!near($c = $sample($pArr, $ka, 0.0, 1.0), [0, 255, 0])) $fail[] = "KTX2 array layer 1 " . json_encode($c);

    /* Garbage and supercompressed containers are refused. */
    if (@vio_texture_ktx2($ctx, "not a ktx2 file at all, just bytes that are long enough to pass the length check ......................") !== false) $fail[] = "garbage accepted";
    $sc = ktx2(37, 2, 2, 1, [rgba(0, 0, 0, 4)]);
    $sc = substr_replace($sc, pack('V', 2 /* zstd */), 44, 4);
    if (@vio_texture_ktx2($ctx, $sc) !== false) $fail[] = "supercompressed accepted";

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
