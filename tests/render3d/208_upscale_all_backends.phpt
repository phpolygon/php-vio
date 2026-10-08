--TEST--
vio_upscale (OPEN-ITEMS A21, UPSCALE-PLAN) on every backend: spatial is sharper than bilinear and keeps flat areas exact, temporal with jittered frames converges closer to the full-resolution picture than spatial, reset drops the history, the swapchain target matches a render target, argument contract
--EXTENSIONS--
vio
--FILE--
<?php
/* An analytic scene (a hard-edged disc over diagonal stripes, from NDC, so
 * every resolution samples the same picture) rendered at 32x32 and upscaled to
 * 64x64, against the same scene rendered at 64x64. */
$LO = 32; $HI = 64;
$VS = "#version 450\nlayout(location=0) in vec2 aPos;\nlayout(location=0) out vec2 ndc;\nuniform vec2 u_jitter;\nuniform vec2 u_pan;\n"
    . "void main(){ ndc = aPos - u_pan; gl_Position = vec4(aPos + u_jitter, 0.0, 1.0); }";
$FS = "#version 450\nlayout(location=0) in vec2 ndc;\nlayout(location=0) out vec4 o;\nuniform vec4 u_flat;\n"
    . "void main(){\n"
    . "  if (u_flat.a > 0.0) { o = vec4(u_flat.rgb, 1.0); return; }\n"
    . "  float s = step(0.5, fract((ndc.x + ndc.y) * 3.0));\n"
    . "  vec3 c = mix(vec3(0.15, 0.2, 0.6), vec3(0.9, 0.85, 0.3), s);\n"
    . "  float d = step(length(ndc - vec2(0.1, -0.05)), 0.45);\n"
    . "  o = vec4(mix(c, vec3(0.95, 0.2, 0.15), d), 1.0);\n"
    . "}";
/* bilinear baseline, addressed like vio_upscale (storage row r <- row r) */
$FS_BI = "#version 450\nlayout(location=0) in vec2 ndc;\nlayout(location=0) out vec4 o;\nuniform sampler2D u_tex;\nuniform vec2 u_dst;\n"
       . "void main(){ o = texture(u_tex, gl_FragCoord.xy / u_dst); }";

function mae(string $a, string $b): float {
    $s = 0; $n = strlen($a);
    for ($i = 0; $i < $n; $i += 4) $s += abs(ord($a[$i]) - ord($b[$i])) + abs(ord($a[$i+1]) - ord($b[$i+1])) + abs(ord($a[$i+2]) - ord($b[$i+2]));
    return $s / ($n / 4 * 3);
}
/* gradient energy: sum of squared horizontal + vertical luma steps */
function sharp(string $p, int $w, int $h): float {
    $l = fn($x, $y) => 0.3 * ord($p[($y * $w + $x) * 4]) + 0.59 * ord($p[($y * $w + $x) * 4 + 1]) + 0.11 * ord($p[($y * $w + $x) * 4 + 2]);
    $e = 0;
    for ($y = 0; $y < $h - 1; $y++) for ($x = 0; $x < $w - 1; $x++) { $c = $l($x, $y); $e += ($l($x + 1, $y) - $c) ** 2 + ($l($x, $y + 1) - $c) ** 2; }
    return $e;
}

function run_backend(string $name): string {
    global $LO, $HI, $VS, $FS, $FS_BI;
    $ctx = @vio_create($name, ['width' => $HI, 'height' => $HI, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE) || !vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_HDR)) {
        vio_destroy($ctx); return "skip (no 3D pipeline)";
    }
    $fail = [];
    $base = ['depth_test' => false, 'cull_mode' => VIO_CULL_NONE];
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1, 1,-1, 1,1, -1,1], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT2]]);
    $scene = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS])] + $base);
    $bi = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS_BI])] + $base);
    $lo = vio_render_target($ctx, ['width' => $LO, 'height' => $LO]);
    $ref = vio_render_target($ctx, ['width' => $HI, 'height' => $HI]);
    $out = vio_render_target($ctx, ['width' => $HI, 'height' => $HI]);

    $draw = function ($rt, int $size, array $jitter = [0.0, 0.0], array $flat = [0, 0, 0, 0], array $pan = [0.0, 0.0]) use ($ctx, $scene, $quad) {
        vio_bind_render_target($ctx, $rt);
        vio_viewport($ctx, 0, 0, $size, $size);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $scene);
        vio_set_uniforms($ctx, ['u_jitter' => [2.0 * $jitter[0] / $size, 2.0 * $jitter[1] / $size], 'u_flat' => $flat, 'u_pan' => $pan]);
        vio_draw($ctx, $quad);
    };
    $frame = function (callable $body) use ($ctx) { vio_begin($ctx); $body(); vio_unbind_render_target($ctx); vio_end($ctx); };

    $frame(fn() => $draw($ref, $HI));
    $refPx = vio_read_render_target($ref);

    /* bilinear baseline */
    $frame(function () use ($ctx, $draw, $lo, $LO, $HI, $bi, $quad, $out) {
        $draw($lo, $LO);
        vio_bind_render_target($ctx, $out);
        vio_viewport($ctx, 0, 0, $HI, $HI);
        vio_bind_pipeline($ctx, $bi);
        vio_bind_texture($ctx, vio_render_target_texture($lo), 0);
        vio_set_uniforms($ctx, ['u_tex' => 0, 'u_dst' => [(float)$HI, (float)$HI]]);
        vio_draw($ctx, $quad);
    });
    $biPx = vio_read_render_target($out);

    /* spatial, with and without sharpening */
    $frame(function () use ($ctx, $draw, $lo, $LO, $out) { $draw($lo, $LO); if (!vio_upscale($ctx, $lo, $out, ['sharpness' => 0.0, 'native' => false])) throw new Exception('upscale'); });
    $sp0 = vio_read_render_target($out);
    $frame(function () use ($ctx, $draw, $lo, $LO, $out) { $draw($lo, $LO); vio_upscale($ctx, $lo, $out, ['native' => false]); });
    $sp = vio_read_render_target($out);
    $eBi = mae($biPx, $refPx); $eSp0 = mae($sp0, $refPx); $eSp = mae($sp, $refPx);
    $gRef = sharp($refPx, $HI, $HI); $gBi = sharp($biPx, $HI, $HI); $gSp0 = sharp($sp0, $HI, $HI); $gSp = sharp($sp, $HI, $HI);
    if (getenv('VIO_UPSCALE_DEBUG')) fprintf(STDERR, "$name: mae bi %.2f sp0 %.2f sp %.2f | grad ref %.0f bi %.0f sp0 %.0f sp %.0f\n", $eBi, $eSp0, $eSp, $gRef, $gBi, $gSp0, $gSp);
    if (!($gSp0 > $gBi * 1.05)) $fail[] = sprintf("spatial not sharper than bilinear (gradient %.0f vs %.0f)", $gSp0, $gBi);
    if (!($gSp > $gSp0)) $fail[] = sprintf("sharpening did not sharpen (gradient %.0f vs %.0f)", $gSp, $gSp0);
    if (!($eSp0 <= $eBi * 1.05)) $fail[] = sprintf("spatial error %.2f above bilinear %.2f", $eSp0, $eBi);

    /* flat input stays exact */
    $frame(function () use ($ctx, $draw, $lo, $LO, $out) { $draw($lo, $LO, [0, 0], [0.4, 0.6, 0.2, 1]); vio_upscale($ctx, $lo, $out, ['native' => false]); });
    $flat = vio_read_render_target($out);
    $px = substr($flat, 0, 4);
    if (abs(ord($px[0]) - 102) > 2 || abs(ord($px[1]) - 153) > 2 || abs(ord($px[2]) - 51) > 2 || $flat !== str_repeat($px, $HI * $HI))
        $fail[] = "flat input not flat: " . bin2hex($px);

    /* temporal: 16 jittered frames of the static scene */
    for ($f = 0; $f < 16; $f++) {
        $j = vio_upscale_jitter($f, 8);
        $frame(function () use ($ctx, $draw, $lo, $LO, $out, $j, $f) {
            $draw($lo, $LO, $j);
            vio_upscale($ctx, $lo, $out, ['mode' => VIO_UPSCALE_TEMPORAL, 'jitter' => $j, 'sharpness' => 0.0, 'reset' => $f === 0]);
        });
    }
    $tp = vio_read_render_target($out);
    $eTp = mae($tp, $refPx);
    if (getenv('VIO_UPSCALE_DEBUG')) fprintf(STDERR, "$name: temporal mae %.2f grad %.0f\n", $eTp, sharp($tp, $HI, $HI));
    if (!($eTp < $eSp0 * 0.5)) $fail[] = sprintf("temporal error %.2f not below spatial %.2f", $eTp, $eSp0);
    /* reset drops the history at once */
    $frame(function () use ($ctx, $draw, $lo, $LO, $out) {
        $draw($lo, $LO, [0, 0], [0.4, 0.6, 0.2, 1]);
        vio_upscale($ctx, $lo, $out, ['mode' => VIO_UPSCALE_TEMPORAL, 'reset' => true, 'sharpness' => 0.0]);
    });
    $rs = vio_read_render_target($out);
    if (mae($rs, $flat) > 0.5) $fail[] = sprintf("temporal reset kept history (mae %.2f to the flat picture)", mae($rs, $flat));

    /* A panning camera (1.37 / 0.61 source pixels per frame, not in step with
     * the jitter sequence): with motion vectors the history follows the
     * picture; without them the stripes smear (their 3x3 neighbourhood holds
     * both colours, the clamp cannot stop it). Under sub-pixel motion the
     * history is resampled every frame and softens - temporal stays within
     * reach of spatial there, its gain is the still / slow picture above. */
    $motionRt = vio_render_target($ctx, ['width' => $LO, 'height' => $LO, 'hdr' => true]);
    $stepX = 1.37 / $LO * 2.0; $stepY = 0.61 / $LO * 2.0;   /* NDC per frame */
    $pan = function (?bool $withMotion) use ($ctx, $draw, $lo, $LO, $HI, $out, $motionRt, $stepX, $stepY, $ref): float {
        for ($f = 0; $f < 24; $f++) {
            if ($withMotion === null && $f < 23) continue;   /* spatial at the final position */
            $j = vio_upscale_jitter($f, 8);
            $p = [$stepX * $f, $stepY * $f];
            vio_begin($ctx);
            vio_bind_render_target($ctx, $motionRt);
            vio_clear($ctx, $stepX * 0.5, $stepY * 0.5, 0, 0);   /* UV delta, y up */
            $draw($lo, $LO, $j, [0, 0, 0, 0], $p);
            if ($withMotion === null) {
                $draw($lo, $LO, [0, 0], [0, 0, 0, 0], $p);
                vio_upscale($ctx, $lo, $out, ['sharpness' => 0.0, 'native' => false]);
            } else {
                vio_upscale($ctx, $lo, $out, ['mode' => VIO_UPSCALE_TEMPORAL, 'jitter' => $j, 'sharpness' => 0.0, 'reset' => $f === 0]
                                            + ($withMotion ? ['motion' => $motionRt] : []));
            }
            vio_unbind_render_target($ctx);
            vio_end($ctx);
        }
        vio_begin($ctx); $draw($ref, $HI, [0, 0], [0, 0, 0, 0], $p); vio_unbind_render_target($ctx); vio_end($ctx);
        return mae(vio_read_render_target($out), vio_read_render_target($ref));
    };
    $eMo = $pan(true);
    $eNo = $pan(false);
    $ePs = $pan(null);
    if (getenv('VIO_UPSCALE_DEBUG')) fprintf(STDERR, "$name: pan with motion %.2f, without %.2f, spatial %.2f\n", $eMo, $eNo, $ePs);
    if (!($eMo < $ePs * 1.75)) $fail[] = sprintf("panning with motion vectors: error %.2f far above spatial %.2f", $eMo, $ePs);
    if (!($eMo < $eNo * 0.5)) $fail[] = sprintf("motion vectors did not help (%.2f vs %.2f without)", $eMo, $eNo);

    /* the swapchain as target: the same picture as a render target */
    vio_begin($ctx);
    $draw($lo, $LO);
    vio_upscale($ctx, $lo, null);
    vio_end($ctx);
    $sc = vio_read_pixels($ctx);
    $rows = fn(string $p): array => str_split($p, $HI * 4);
    if ($rows($sc) !== $rows($sp) && array_reverse($rows($sc)) !== $rows($sp)) $fail[] = sprintf("swapchain differs from the render target (mae %.2f)", mae($sc, $sp));

    $info = vio_upscale_info($ctx);
    if (($info['spatial'] ?? null) === null || ($info['temporal'] ?? null) === null) $fail[] = "info " . json_encode($info);
    /* The platform scaler (MetalFX on Metal, macOS 13+): a render-target
     * target goes through it by default; it must beat bilinear as well. */
    if ($info['spatial'] !== 'portable') {
        $frame(function () use ($ctx, $draw, $lo, $LO, $out) { $draw($lo, $LO); if (!vio_upscale($ctx, $lo, $out)) throw new Exception('native upscale'); });
        $nat = vio_read_render_target($out);
        $eNat = mae($nat, $refPx);
        if (getenv('VIO_UPSCALE_DEBUG')) fprintf(STDERR, "$name: {$info['spatial']} mae %.2f\n", $eNat);
        if (!($eNat < $eBi)) $fail[] = sprintf("%s: error %.2f not below bilinear %.2f", $info['spatial'], $eNat, $eBi);
        if ($nat === $sp) $fail[] = "{$info['spatial']}: same picture as the portable path (not used?)";
    }

    /* contract */
    if (@vio_upscale($ctx, $lo, $out) !== false) $fail[] = "outside a frame accepted";
    foreach ([['mode' => 7], ['sharpness' => 2.0], ['jitter' => 1.0], ['motion' => 'x']] as $k => $bad) {
        try { vio_upscale($ctx, $lo, $out, $bad); $fail[] = "bad option set $k accepted"; } catch (ValueError $e) {}
    }
    try { vio_upscale($ctx, $quad, $out); $fail[] = "a mesh as source accepted"; } catch (TypeError $e) {}
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

$j = vio_upscale_jitter(0, 8);
if (count($j) !== 2 || abs($j[0]) > 0.5 || abs($j[1]) > 0.5 || vio_upscale_jitter(8, 8) !== $j) echo "jitter sequence wrong\n";
foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) echo "$b: ", run_backend($b), "\n";
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE
