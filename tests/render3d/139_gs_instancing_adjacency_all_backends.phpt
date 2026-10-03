--TEST--
Geometry-shader instancing (layout(invocations = N), VIO_FEATURE_GEOMETRY_INSTANCING) and adjacency topologies with vio_mesh(['adjacency' => true])
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_GEOMETRY)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with a geometry stage");
?>
--FILE--
<?php
/* GEOMETRY-STAGES-PLAN Phase 2:
 *   A. one point, a GS with invocations = 4: every invocation draws its own
 *      quadrant in its own colour (checked as a set, so the row order of the
 *      backend does not matter)
 *   B. invocations = 6 + gl_Layer = gl_InvocationID fills a cube target in one
 *      draw (VIO_FEATURE_LAYERED_RENDER)
 *   C. VIO_TRIANGLES_ADJACENCY with vio_mesh(['adjacency' => true]): a quad of
 *      two triangles whose vertices are duplicated per triangle - the helper
 *      matches the shared edge by position - and a lone triangle; the GS
 *      colours each triangle by its number of real neighbours (an open edge
 *      repeats the triangle's own opposite vertex)
 *   D. VIO_LINES_ADJACENCY delivers p0..p3 in order
 *   E. contract: adjacency without a geometry stage, a non-triangle list */
$W = 32;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b, int $tol = 3): bool { return abs($a[0]-$b[0]) <= $tol && abs($a[1]-$b[1]) <= $tol && abs($a[2]-$b[2]) <= $tol; }

function run_backend(string $name): string {
    global $W;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_GEOMETRY)) {
        vio_destroy($ctx);
        return "skip (no geometry stage)";
    }
    $fail = [];
    $base = ['depth_test' => false, 'cull_mode' => VIO_CULL_NONE];
    $vs = "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=0) out vec4 vPos;\nvoid main(){ vPos = vec4(aPos, 1.0); gl_Position = vPos; }";
    $fs = "#version 450\nlayout(location=0) in vec3 gColor;\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(gColor, 1.0); }";
    $PAL = "const vec3 PAL[6] = vec3[6](vec3(1,0,0), vec3(0,1,0), vec3(0,0,1), vec3(1,1,0), vec3(1,0,1), vec3(0,1,1));\n";
    $frame = function (callable $draw) use ($ctx): string {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx); $draw(); vio_end($ctx);
        return vio_read_pixels($ctx);
    };
    $point = vio_mesh($ctx, ['vertices' => [0, 0, 0], 'layout' => [VIO_FLOAT3]]);

    /* ---- A / B: GS instancing ------------------------------------------- */
    if (vio_supports_feature($ctx, VIO_FEATURE_GEOMETRY_INSTANCING)) {
        $gs = "#version 450\nlayout(points, invocations = 4) in;\nlayout(triangle_strip, max_vertices = 4) out;\n"
            . "layout(location=0) in vec4 vPos[];\nlayout(location=0) out vec3 gColor;\n" . $PAL
            . "void main(){ vec2 o = vec2(gl_InvocationID % 2 == 0 ? -0.5 : 0.5, gl_InvocationID < 2 ? -0.5 : 0.5);\n"
            . "  for (int i = 0; i < 4; i++) { vec2 c = vec2(i % 2 == 0 ? -0.5 : 0.5, i < 2 ? -0.5 : 0.5);\n"
            . "    gColor = PAL[gl_InvocationID]; gl_Position = vec4(vPos[0].xy + o + c * 0.9, 0.0, 1.0); EmitVertex(); }\n"
            . "  EndPrimitive(); }";
        $sh = vio_shader($ctx, ['vertex' => $vs, 'geometry' => $gs, 'fragment' => $fs]);
        if (!$sh) { $fail[] = "A: instanced geometry shader not created"; }
        else {
            $p = vio_pipeline($ctx, ['shader' => $sh, 'topology' => VIO_POINTS] + $base);
            $img = $frame(function () use ($ctx, $p, $point) { vio_bind_pipeline($ctx, $p); vio_draw($ctx, $point); });
            $got = [];
            foreach ([[8, 8], [24, 8], [8, 24], [24, 24]] as [$x, $y]) $got[] = json_encode(px($img, $x, $y, $W));
            sort($got);
            $want = [json_encode([0, 0, 255]), json_encode([0, 255, 0]), json_encode([255, 0, 0]), json_encode([255, 255, 0])];
            sort($want);
            if ($got !== $want) $fail[] = "A: quadrant colours " . implode(' ', $got);
        }
        if (vio_supports_feature($ctx, VIO_FEATURE_LAYERED_RENDER)) {
            $gsCube = "#version 450\nlayout(points, invocations = 6) in;\nlayout(triangle_strip, max_vertices = 3) out;\n"
                    . "layout(location=0) in vec4 vPos[];\nlayout(location=0) out vec3 gColor;\n" . $PAL
                    . "void main(){ const vec2 T[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));\n"
                    . "  for (int i = 0; i < 3; i++) { gl_Layer = gl_InvocationID; gColor = PAL[gl_InvocationID];\n"
                    . "    gl_Position = vec4(T[i], 0.0, 1.0); EmitVertex(); } EndPrimitive(); }";
            $shCube = vio_shader($ctx, ['vertex' => $vs, 'geometry' => $gsCube, 'fragment' => $fs]);
            $cube = vio_render_target($ctx, ['cube' => true, 'size' => 8]);
            if (!$shCube || !$cube) { $fail[] = "B: cube shader / target not created"; }
            else {
                $pc = vio_pipeline($ctx, ['shader' => $shCube, 'topology' => VIO_POINTS] + $base);
                vio_begin($ctx);
                vio_bind_render_target($ctx, $cube, VIO_RT_ALL_LAYERS);
                vio_clear($ctx, 0, 0, 0, 1);
                vio_bind_pipeline($ctx, $pc);
                vio_draw($ctx, $point);
                vio_unbind_render_target($ctx);
                vio_end($ctx);
                $pal = [[255,0,0], [0,255,0], [0,0,255], [255,255,0], [255,0,255], [0,255,255]];
                for ($f = 0; $f < 6; $f++) {
                    $c = px(vio_read_render_target($cube, $f), 4, 4, 8);
                    if (!near($c, $pal[$f])) $fail[] = "B: face $f " . json_encode($c) . " want " . json_encode($pal[$f]);
                }
            }
        }
    }

    /* ---- C: triangles with adjacency ------------------------------------- */
    $gsAdj = "#version 450\nlayout(triangles_adjacency) in;\nlayout(triangle_strip, max_vertices = 3) out;\n"
           . "layout(location=0) in vec4 vPos[];\nlayout(location=0) out vec3 gColor;\n"
           . "void main(){\n"
           . "  float n = (vPos[1] != vPos[4] ? 1.0 : 0.0) + (vPos[3] != vPos[0] ? 1.0 : 0.0) + (vPos[5] != vPos[2] ? 1.0 : 0.0);\n"
           . "  for (int i = 0; i < 6; i += 2) { gColor = vec3(n / 3.0, 0.0, 1.0); gl_Position = vPos[i]; EmitVertex(); }\n"
           . "  EndPrimitive(); }";
    $shAdj = vio_shader($ctx, ['vertex' => $vs, 'geometry' => $gsAdj, 'fragment' => $fs]);
    if (!$shAdj) { $fail[] = "C: adjacency geometry shader not created"; }
    else {
        $pa = vio_pipeline($ctx, ['shader' => $shAdj, 'topology' => VIO_TRIANGLES_ADJACENCY] + $base);
        /* Two triangles of a full-screen quad, every vertex duplicated per triangle. */
        $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0,   -1,-1,0, 1,1,0, -1,1,0],
                                'layout' => [VIO_FLOAT3], 'adjacency' => true]);
        $lone = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, -1,1,0], 'indices' => [0, 1, 2],
                                'layout' => [VIO_FLOAT3], 'adjacency' => true]);
        if (!$quad || !$lone) { $fail[] = "C: adjacency mesh not created"; }
        else {
            $img = $frame(function () use ($ctx, $pa, $quad) { vio_bind_pipeline($ctx, $pa); vio_draw($ctx, $quad); });
            /* (28, 4) and (4, 28) lie in different triangles, whatever the row order. */
            foreach ([[28, 4], [4, 28]] as [$x, $y]) {
                if (!near(px($img, $x, $y, $W), [85, 0, 255])) $fail[] = "C: quad triangle at ($x,$y) " . json_encode(px($img, $x, $y, $W)) . " want one neighbour [85,0,255]";
            }
            $img = $frame(function () use ($ctx, $pa, $lone) { vio_bind_pipeline($ctx, $pa); vio_draw($ctx, $lone); });
            if (!near(px($img, 4, 4, $W), [0, 0, 255]) && !near(px($img, 4, 27, $W), [0, 0, 255])) {
                $fail[] = "C: lone triangle should have no neighbour " . json_encode([px($img, 4, 4, $W), px($img, 4, 27, $W)]);
            }
        }
    }

    /* ---- D: lines with adjacency ---------------------------------------- */
    $gsLine = "#version 450\nlayout(lines_adjacency) in;\nlayout(triangle_strip, max_vertices = 3) out;\n"
            . "layout(location=0) in vec4 vPos[];\nlayout(location=0) out vec3 gColor;\n"
            . "void main(){ const vec2 T[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));\n"
            . "  for (int i = 0; i < 3; i++) { gColor = vec3((vPos[0].x + 1.0) * 0.5, (vPos[3].x + 1.0) * 0.5, (vPos[1].x + vPos[2].x + 2.0) * 0.25);\n"
            . "    gl_Position = vec4(T[i], 0.0, 1.0); EmitVertex(); } EndPrimitive(); }";
    $shLine = vio_shader($ctx, ['vertex' => $vs, 'geometry' => $gsLine, 'fragment' => $fs]);
    if (!$shLine) { $fail[] = "D: line-adjacency geometry shader not created"; }
    else {
        $pl = vio_pipeline($ctx, ['shader' => $shLine, 'topology' => VIO_LINES_ADJACENCY] + $base);
        $line = vio_mesh($ctx, ['vertices' => [-0.5,0,0, 0,0,0, 0,0,0, 0.5,0,0], 'layout' => [VIO_FLOAT3]]);
        $img = $frame(function () use ($ctx, $pl, $line) { vio_bind_pipeline($ctx, $pl); vio_draw($ctx, $line); });
        if (!near(px($img, 16, 16, $W), [64, 191, 128])) $fail[] = "D: lines_adjacency vertex order " . json_encode(px($img, 16, 16, $W)) . " want [64,191,128]";
    }

    /* ---- E: contract ------------------------------------------------------ */
    $plain = vio_shader($ctx, ['vertex' => $vs, 'fragment' => "#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(1.0); }"]);
    if (@vio_pipeline($ctx, ['shader' => $plain, 'topology' => VIO_TRIANGLES_ADJACENCY]) !== false) $fail[] = "E: adjacency without a geometry stage accepted";
    if (@vio_mesh($ctx, ['vertices' => [0,0,0, 1,0,0], 'layout' => [VIO_FLOAT3], 'adjacency' => true]) !== false) $fail[] = "E: adjacency on 2 vertices accepted";

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
