--TEST--
vio_texture_ktx2 with six faces returns a VioCubemap, with a depth a 3D VioTexture (RGBA8 and R8, mip_offset picks the base level); block-compressed cubes / volumes are refused; vio_cubemap takes raw RGBA strings in 'faces' with 'width'/'height'; sampled on every backend
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A35. */
$W = 8;
function ktx2(int $vk, int $w, int $h, int $depth, int $faces, array $levels): string {
    $n = count($levels);
    $hdr = "\xABKTX 20\xBB\r\n\x1A\n" . pack('V9', $vk, 1, $w, $h, $depth, 0, $faces, $n, 0);
    $dfdOff = 80 + $n * 24;
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
function rgba(array $c, int $n): string { return str_repeat(pack('C4', ...$c), $n); }
function near(array $a, array $b): bool { for ($i = 0; $i < 3; $i++) if (abs($a[$i] - $b[$i]) > 10) return false; return true; }
function center(string $p, int $w): array { $o = (intdiv($w, 2) * $w + intdiv($w, 2)) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }

$faceColors = [[255, 0, 0, 255], [0, 255, 0, 255], [0, 0, 255, 255], [255, 255, 0, 255], [0, 255, 255, 255], [255, 0, 255, 255]];
$dirs = [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]];
// Level 0 is 4x4 grey (mip_offset 1 must skip it), level 1 2x2 holds the face colours.
$cubeL0 = ''; $cubeL1 = '';
foreach ($faceColors as $c) { $cubeL0 .= rgba([128, 128, 128, 255], 16); $cubeL1 .= rgba($c, 4); }
$cubeRgba = ktx2(37, 4, 4, 0, 6, [$cubeL0, $cubeL1]);
$cubeR8 = ktx2(9, 2, 2, 0, 6, [implode('', array_map(fn($v) => str_repeat(chr($v), 4), [40, 80, 120, 160, 200, 240]))]);
// 2x2x2 volume: slice 0 red, slice 1 blue.
$vol = ktx2(37, 2, 2, 2, 1, [rgba([255, 0, 0, 255], 4) . rgba([0, 0, 255, 255], 4)]);
$bcCube = ktx2(131, 4, 4, 0, 6, [str_repeat("\0", 8 * 6)]);

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    $ctx = @vio_create($b, ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE) || !vio_supports_feature($ctx, VIO_FEATURE_CUBEMAP)) { echo "$b: skip (no cubemaps)\n"; vio_destroy($ctx); continue; }
    $fail = [];
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $fsCube = "#version 330 core\nuniform samplerCube u_env;\nuniform vec3 u_dir;\nlayout(location=0) out vec4 o;\nvoid main(){ o = textureLod(u_env, u_dir, 0.0); }";
    $fs3d = "#version 330 core\nuniform sampler3D u_vol;\nuniform float u_z;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texture(u_vol, vec3(0.5, 0.5, u_z)); }";
    $full = vio_mesh($ctx, ['vertices' => [-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0], 'indices' => [0, 1, 2, 0, 2, 3], 'layout' => [VIO_FLOAT3]]);
    $pCube = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsCube]), 'depth_test' => false]);
    $draw = function ($pipe, array $uniforms, $bind) use ($ctx, $full, $W) {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        foreach ($uniforms as $k => $v) vio_set_uniform($ctx, $k, $v);
        $bind();
        vio_draw($ctx, $full);
        vio_end($ctx);
        return center(vio_read_pixels($ctx), $W);
    };
    $sampleCube = function ($cm, array $want, string $what) use ($draw, $pCube, $dirs, &$fail, $ctx) {
        foreach ($dirs as $f => $d) {
            $got = $draw($pCube, ['u_env' => 0, 'u_dir' => $d], fn() => vio_bind_cubemap($ctx, $cm, 0));
            if (!near($got, $want[$f])) { $fail[] = "$what face $f " . json_encode($got); break; }
        }
    };
    $cm = vio_texture_ktx2($ctx, $cubeRgba, ['mip_offset' => 1]);
    if (!$cm instanceof VioCubemap) $fail[] = "rgba cube: " . get_debug_type($cm);
    else $sampleCube($cm, $faceColors, "rgba");
    $cm8 = vio_texture_ktx2($ctx, $cubeR8);
    if (!$cm8 instanceof VioCubemap) $fail[] = "r8 cube: " . get_debug_type($cm8);
    else $sampleCube($cm8, array_map(fn($v) => [$v, $v, $v], [40, 80, 120, 160, 200, 240]), "r8");
    // vio_cubemap with raw RGBA strings.
    $raw = vio_cubemap($ctx, ['faces' => array_map(fn($c) => rgba($c, 4), $faceColors), 'width' => 2, 'height' => 2]);
    if (!$raw instanceof VioCubemap) $fail[] = "raw faces: " . get_debug_type($raw);
    else $sampleCube($raw, $faceColors, "raw");
    if (vio_supports_feature($ctx, VIO_FEATURE_TEXTURE_3D)) {
        $tex = vio_texture_ktx2($ctx, $vol, ['filter' => VIO_FILTER_NEAREST]);
        if (!$tex instanceof VioTexture) $fail[] = "volume: " . get_debug_type($tex);
        else {
            $p3 = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs3d]), 'depth_test' => false]);
            foreach ([[0.25, [255, 0, 0]], [0.75, [0, 0, 255]]] as [$z, $want]) {
                $got = $draw($p3, ['u_vol' => 0, 'u_z' => $z], fn() => vio_bind_texture($ctx, $tex, 0));
                if (!near($got, $want)) $fail[] = "volume z=$z " . json_encode($got);
            }
        }
    }
    if (@vio_texture_ktx2($ctx, $bcCube) !== false) $fail[] = "BC cube accepted";
    elseif (!str_contains(error_get_last()['message'] ?? '', 'cubemap')) $fail[] = "BC cube message: " . (error_get_last()['message'] ?? '');
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
