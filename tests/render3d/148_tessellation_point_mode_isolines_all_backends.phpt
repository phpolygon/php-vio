--TEST--
Tessellation point_mode (triangle and quad domains) emits every domain point exactly once, and isolines honour fractional spacing, on every backend with tessellation (Metal: emulated, its tessellator has neither)
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_TESSELLATION)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with tessellation");
?>
--FILE--
<?php
/* A. triangles, point_mode, outer (2, 3, 4), inner 4: 3 corners + 1 + 2 + 3
 *    edge points + inner ring (6) + centre = 16 points
 * B. quads, point_mode, outer (2, 3, 4, 1), inner (3, 2): 4 corners + 1 + 2 +
 *    3 + 0 edge points + 2 x 1 interior = 12 points
 * Each point lands on its own pixel centre (domain coordinates are multiples
 * of 1/12, scaled to 4-pixel steps) and adds 0.25 to red: a pixel brighter
 * than 64 is a point emitted twice.
 * C. isolines, fractional_odd_spacing, outer (2, 2.5): 2 lines (v = 0, 1/2),
 *    3 segments each, spanning the full u range. Not on D3D: WARP (headless
 *    D3D, the Windows CI) loses the line primitives of isolines (test 144). */
$W = 64;
$VS = "#version 450\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$FS = "#version 450\nlayout(location=0) in vec3 c;\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(c, 1.0); }";
function tcs(int $n, string $levels): string {
    return "#version 450\nlayout(vertices = $n) out;\n"
         . "void main(){ gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n"
         . "  if (gl_InvocationID == 0) { $levels } }";
}
/* pixel centre of domain position (s, t): -0.75 + 1.5 * s NDC, plus half a pixel */
$POS = "vec4(-0.75 + 1.5 * s + 1.0 / 64.0, -0.75 + 1.5 * t + 1.0 / 64.0, 0.0, 1.0)";
$TRI_TES = "#version 450\nlayout(triangles, equal_spacing, ccw, point_mode) in;\nlayout(location=0) out vec3 c;\n"
         . "void main(){ float s = gl_TessCoord.x, t = gl_TessCoord.y; c = vec3(0.25, 0.0, 0.0); gl_Position = $POS; }";
$QUAD_TES = "#version 450\nlayout(quads, equal_spacing, ccw, point_mode) in;\nlayout(location=0) out vec3 c;\n"
          . "void main(){ float s = gl_TessCoord.x, t = gl_TessCoord.y; c = vec3(0.25, 0.0, 0.0); gl_Position = $POS; }";
$ISO_TES = "#version 450\nlayout(isolines, fractional_odd_spacing) in;\nlayout(location=0) out vec3 c;\n"
         . "void main(){ c = vec3(1.0, gl_TessCoord.y, 0.0); gl_Position = vec4(gl_TessCoord.x * 1.8 - 0.9, gl_TessCoord.y * 1.8 - 0.9, 0.0, 1.0); }";

function run_backend(string $name): string {
    global $W, $VS, $FS, $TRI_TES, $QUAD_TES, $ISO_TES;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_TESSELLATION)) { vio_destroy($ctx); return "skip (no tessellation)"; }
    $fail = [];
    $tri  = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 0,1,0], 'layout' => [VIO_FLOAT3]]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'layout' => [VIO_FLOAT3]]);
    $line = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0], 'layout' => [VIO_FLOAT3]]);
    $draw = function (string $tcs, string $tes, int $n, $mesh, int $blend) use ($ctx, $VS, $FS): ?string {
        $sh = @vio_shader($ctx, ['vertex' => $VS, 'tess_control' => $tcs, 'tess_eval' => $tes, 'fragment' => $FS]);
        if (!$sh) return null;
        $pipe = vio_pipeline($ctx, ['shader' => $sh, 'patch_vertices' => $n, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE, 'blend' => $blend]);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx); vio_bind_pipeline($ctx, $pipe); vio_draw($ctx, $mesh); vio_end($ctx);
        return vio_read_pixels($ctx);
    };
    $points = function (?string $p, string $label, int $want) use ($W, &$fail) {
        if ($p === null) { $fail[] = "$label: shader not created"; return; }
        $lit = 0; $dup = 0;
        for ($i = 0; $i < $W * $W; $i++) {
            $r = ord($p[$i * 4]);
            if ($r) { $lit++; if ($r > 72) $dup++; }
        }
        if ($lit !== $want) $fail[] = "$label: $lit points, want $want";
        if ($dup) $fail[] = "$label: $dup points drawn more than once";
    };
    $points($draw(tcs(3, "gl_TessLevelOuter[0] = 2.0; gl_TessLevelOuter[1] = 3.0; gl_TessLevelOuter[2] = 4.0; gl_TessLevelInner[0] = 4.0;"),
                  $TRI_TES, 3, $tri, VIO_BLEND_ADDITIVE), "A triangles", 16);
    $points($draw(tcs(4, "gl_TessLevelOuter[0] = 2.0; gl_TessLevelOuter[1] = 3.0; gl_TessLevelOuter[2] = 4.0; gl_TessLevelOuter[3] = 1.0;"
                       . " gl_TessLevelInner[0] = 3.0; gl_TessLevelInner[1] = 2.0;"),
                  $QUAD_TES, 4, $quad, VIO_BLEND_ADDITIVE), "B quads", 12);

    $d3d = in_array(vio_backend_name($ctx), ['d3d11', 'd3d12'], true);
    $p = $d3d ? false : $draw(tcs(2, "gl_TessLevelOuter[0] = 2.0; gl_TessLevelOuter[1] = 2.5;"), $ISO_TES, 2, $line, VIO_BLEND_NONE);
    if ($p === false) { /* skipped */ }
    elseif ($p === null) $fail[] = "C: shader not created";
    else {
        /* lit rows by green (v), and the x extent of each */
        $rows = [];
        for ($y = 0; $y < $W; $y++) for ($x = 0; $x < $W; $x++) {
            $o = ($y * $W + $x) * 4;
            if (!ord($p[$o])) continue;
            $g = ord($p[$o + 1]);
            $rows[$g] = [min($rows[$g][0] ?? $x, $x), max($rows[$g][1] ?? $x, $x)];
        }
        ksort($rows);
        $gs = array_keys($rows);
        if (count($gs) !== 2 || $gs[0] > 1 || abs($gs[1] - 128) > 2) $fail[] = "C: isolines at green " . json_encode($gs) . ", want ~[0,128]";
        foreach ($rows as $g => [$x0, $x1]) if ($x0 > 4 || $x1 < $W - 5) $fail[] = "C: line at green $g spans x $x0..$x1, want ~3..60";
    }
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %s
d3d11: %s
d3d12: %s
vulkan: %s
metal: %s
DONE
