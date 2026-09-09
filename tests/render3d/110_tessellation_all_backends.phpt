--TEST--
Tessellation stages: vio_shader(['tess_control', 'tess_eval']) + patch topology tessellate a quad patch into a disc whose edge count follows the TCS uniform u_level on every backend that reports VIO_FEATURE_TESSELLATION
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null' || $b === 'vulkan') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_TESSELLATION) && vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)
        && vio_supports_feature($c, VIO_FEATURE_READ_PIXELS)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with tessellation stages");
?>
--FILE--
<?php
/* One 4-control-point quad patch. The evaluation shader maps (u, v) of the
 * unit square onto a disc of radius R: angle = 2*pi*u, radius = v*R. The TCS
 * sets all tessellation levels from the uniform u_level (its own constant
 * block), so the disc outline is a polygon with u_level edges:
 *   - level 4  -> a diamond touching R on the axes; at 45 deg its edge sits
 *                 at R*cos(45) = 0.707R, so a pixel at 45 deg / 0.9R is BLACK
 *   - level 16 -> a 16-gon whose edge never dips below R*cos(11.25) = 0.98R,
 *                 so the same pixel is GREEN
 * A pixel at 0.5R is green either way and one outside R is always black. The
 * shape is symmetric, so the GL-vs-D3D row order does not matter. */
$W = 64; $H = 64; $R = 0.8;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b): bool { return abs($a[0]-$b[0]) <= 3 && abs($a[1]-$b[1]) <= 3 && abs($a[2]-$b[2]) <= 3; }
/* pixel at polar (r in NDC units, angle deg) around the centre */
function polar(float $r, float $deg): array { global $W, $H; $a = deg2rad($deg); return [(int)round($W/2 + cos($a) * $r * $W/2), (int)round($H/2 - sin($a) * $r * $H/2)]; }

function run_backend(string $name): string {
    global $W, $H, $R;
    $ctx = @vio_create($name, ["width" => $W, "height" => $H, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_TESSELLATION) || !vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)
        || !vio_supports_feature($ctx, VIO_FEATURE_READ_PIXELS)) {
        vio_destroy($ctx);
        return "skip (no tessellation stages)";
    }
    $fail = [];
    $vs  = "#version 400 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $tcs = "#version 400 core\n"
         . "layout(vertices = 4) out;\n"
         . "uniform float u_level;\n"
         . "void main(){\n"
         . "  gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n"
         . "  if (gl_InvocationID == 0) {\n"
         . "    gl_TessLevelOuter[0] = u_level; gl_TessLevelOuter[1] = u_level;\n"
         . "    gl_TessLevelOuter[2] = u_level; gl_TessLevelOuter[3] = u_level;\n"
         . "    gl_TessLevelInner[0] = u_level; gl_TessLevelInner[1] = u_level;\n"
         . "  }\n}";
    $tes = "#version 400 core\n"
         . "layout(quads, equal_spacing, ccw) in;\n"
         . "uniform float u_radius;\n"
         . "void main(){\n"
         . "  float a = gl_TessCoord.x * 6.28318530718;\n"
         . "  float r = gl_TessCoord.y * u_radius;\n"
         . "  gl_Position = vec4(cos(a) * r, sin(a) * r, 0.0, 1.0);\n}";
    $fs  = "#version 400 core\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0.0, 1.0, 0.0, 1.0); }";

    $sh = vio_shader($ctx, ['vertex' => $vs, 'tess_control' => $tcs, 'tess_eval' => $tes, 'fragment' => $fs]);
    if (!($sh instanceof VioShader)) { vio_destroy($ctx); return "FAIL\n  shader with tessellation stages not created"; }
    $refl = vio_shader_reflect($sh);
    if (!is_array($refl) || !isset($refl['tess_control']) || !isset($refl['tess_eval'])) $fail[] = "reflection lacks tessellation stages";

    $pipe = vio_pipeline($ctx, ['shader' => $sh, 'topology' => VIO_PATCHES, 'patch_vertices' => 4,
                                'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    if (!($pipe instanceof VioPipeline)) { vio_destroy($ctx); return "FAIL\n  patch pipeline not created"; }
    /* Control points are only carried through; the TES ignores them. */
    $patch = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'layout' => [VIO_FLOAT3]]);

    foreach ([4 => false, 16 => true] as $level => $lit45) {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniform($ctx, 'u_level', (float)$level);
        vio_set_uniform($ctx, 'u_radius', $R);
        vio_draw($ctx, $patch);
        vio_end($ctx);
        $p = vio_read_pixels($ctx);
        if (!$p || strlen($p) !== $W * $H * 4) { vio_destroy($ctx); return "FAIL\n  readback size"; }

        [$cx, $cy] = polar(0.5 * $R, 45.0);
        if (!near(px($p, $cx, $cy, $W), [0, 255, 0]))
            $fail[] = "level $level: pixel at 0.5R not green " . json_encode(px($p, $cx, $cy, $W));
        [$ox, $oy] = polar(1.15 * $R, 45.0);
        if (!near(px($p, $ox, $oy, $W), [0, 0, 0]))
            $fail[] = "level $level: pixel outside R not black " . json_encode(px($p, $ox, $oy, $W));
        [$ex, $ey] = polar(0.9 * $R, 45.0);
        $want = $lit45 ? [0, 255, 0] : [0, 0, 0];
        if (!near(px($p, $ex, $ey, $W), $want))
            $fail[] = "level $level: pixel at 45deg/0.9R expected " . json_encode($want) . " got " . json_encode(px($p, $ex, $ey, $W));
        /* On the axes the outline always reaches R: 0.9R at 0 deg is green. */
        [$ax, $ay] = polar(0.9 * $R, 0.0);
        if (!near(px($p, $ax, $ay, $W), [0, 255, 0]))
            $fail[] = "level $level: pixel at 0deg/0.9R not green " . json_encode(px($p, $ax, $ay, $W));
    }

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
