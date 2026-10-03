--TEST--
vio_generate_mipmaps inside a frame: render a cube target, build its mips and sample the smallest level in the same frame, twice with different content (no stale level); on D3D12 without a GPU drain
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_RENDER_TARGET_CUBE)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with cube render targets");
?>
--FILE--
<?php
/* D3D12-MIPGEN-STALL-PLAN Phase 2. A 16x16 cube target with a mip chain:
 * frame 1 renders vertical stripes (half white, half black) into every face,
 * frame 2 solid red. After vio_generate_mipmaps the same frame samples level 4
 * (1x1) of +X: grey ~0.5 in frame 1, red in frame 2 - a level left over from
 * the previous frame or never generated fails. D3D12 used to drain the GPU
 * twice per call (~10 ms CPU); the call now records onto the frame's list,
 * checked as a soft bound on the CPU time it adds. */
$W = 32; $SIZE = 16;
$VS = "#version 450\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos.xy, 0.0, 1.0); }";
$FACE = "#version 450\nlayout(location=0) out vec4 o;\nuniform int u_mode;\n"
      . "void main(){ if (u_mode == 0) { float s = gl_FragCoord.x < 8.0 ? 1.0 : 0.0; o = vec4(s, s, s, 1.0); }\n"
      . "  else o = vec4(1.0, 0.0, 0.0, 1.0); }";
$SAMPLE = "#version 450\nlayout(location=0) out vec4 o;\nuniform samplerCube u_cube;\n"
        . "void main(){ o = vec4(textureLod(u_cube, vec3(1.0, 0.0, 0.0), 4.0).rgb, 1.0); }";

function run_backend(string $name, bool $hardware = false): string {
    global $W, $SIZE, $VS, $FACE, $SAMPLE;
    /* Headless D3D is WARP; $hardware opens a window to get the real adapter
     * (its framebuffer may not be $W wide, so only the timing is checked). */
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => !$hardware, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    $gpu = vio_gpu_info()['name'] ?? '';
    if ($hardware && ($gpu === '' || stripos($gpu, 'Basic Render') !== false)) { vio_destroy($ctx); return "skip (WARP)"; }
    if (!vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_CUBE)) { vio_destroy($ctx); return "skip (no cube targets)"; }
    $fail = [];
    $tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
    $base = ['depth_test' => false, 'cull_mode' => VIO_CULL_NONE];
    $face = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FACE])] + $base);
    $sample = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $SAMPLE])] + $base);
    $rt = vio_render_target($ctx, ['cube' => true, 'size' => $SIZE, 'mipmaps' => true]);
    $cube = vio_render_target_cubemap($rt);

    $frame = function (int $mode, bool $mips) use ($ctx, $tri, $face, $sample, $rt, $cube, $W, $SIZE): array {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        $t = hrtime(true);
        for ($f = 0; $f < 6; $f++) {
            vio_bind_render_target($ctx, $rt, $f);
            vio_clear($ctx, 0, 0, 0, 1);
            vio_viewport($ctx, 0, 0, $SIZE, $SIZE);
            vio_bind_pipeline($ctx, $face);
            vio_set_uniform($ctx, 'u_mode', $mode);
            vio_draw($ctx, $tri);
        }
        vio_unbind_render_target($ctx);
        if ($mips) vio_generate_mipmaps($ctx, $rt);
        $ms = (hrtime(true) - $t) / 1e6;
        vio_viewport($ctx, 0, 0, $W, $W);
        vio_bind_pipeline($ctx, $sample);
        vio_bind_cubemap($ctx, $cube, 0);
        vio_set_uniform($ctx, 'u_cube', 0);
        vio_draw($ctx, $tri);
        vio_end($ctx);
        $p = vio_read_pixels($ctx);
        $o = (($W >> 1) * $W + ($W >> 1)) * 4;
        return [[ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])], $ms];
    };

    if (!$hardware) {
        [$px] = $frame(0, true);
        if (abs($px[0] - 128) > 12 || abs($px[1] - 128) > 12 || abs($px[2] - 128) > 12)
            $fail[] = "stripes: level 4 = " . json_encode($px) . ", want ~[128,128,128]";
        [$px] = $frame(1, true);
        if ($px[0] < 240 || $px[1] > 15 || $px[2] > 15)
            $fail[] = "red: level 4 = " . json_encode($px) . ", want [255,0,0] (stale level?)";
    }

    /* Soft bound: building the mips must not add a GPU drain. D3D12 used to
     * add ~10 ms on hardware but only tenths of a millisecond on WARP, hence
     * the hardware run below. Medians over 9 frames each. */
    $med = function (bool $mips) use ($frame): float {
        $s = [];
        for ($i = 0; $i < 9; $i++) $s[] = $frame(1, $mips)[1];
        sort($s);
        return $s[4];
    };
    $plain = $med(false);
    $with = $med(true);
    $limit = $hardware ? $plain + 1.0 : $plain * 3 + 4.0;   /* v2.29.0 added ~3 ms here on an RTX 2080 */
    if ($with > $limit) $fail[] = sprintf("mips add %.2f ms (render %.2f ms, with mips %.2f ms)", $with - $plain, $plain, $with);

    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "d3d12 hardware: ", run_backend('d3d12', true), "\n";
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
d3d12 hardware: %r(OK|skip \(.*\))%r
DONE
