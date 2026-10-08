--TEST--
Geometry stage gaps on every backend with VIO_FEATURE_GEOMETRY: triangle strips with adjacency, gl_PrimitiveID written by the geometry stage and read by the fragment stage, textures sampled in the vertex and geometry stages, interface blocks VS -> GS -> FS, a geometry stage behind tessellation
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A28 (the Metal emulation lacked all five; the other backends
 * run them natively, so this test also pins the expected values). 8 x 4 target.
 *   A. VIO_TRIANGLE_STRIP_ADJACENCY, 8 vertices = 2 triangles. Each vertex carries
 *      its index; the GS draws, per primitive p (columns 4p..4p+3), the indices of
 *      gl_in[0..2] in the upper rows and gl_in[3..5] in the lower rows (20 per
 *      unit). GL 4.6 table 10.1: p0 = [0,1,2,6,4,3], p1 = [4,0,2,5,6,7].
 *   B. the GS writes gl_PrimitiveID = 3 + gl_PrimitiveIDIn, the FS paints it.
 *   C. the VS reads texel 1 of u_tex into a varying, the GS adds texel 2 of the
 *      same texture -> red 40 + 80 = 120.
 *   D. VS -> GS -> FS through interface blocks (vec3 + float) -> (255, 128, 64).
 *   E. VS -> TCS -> TES -> GS -> FS: one quad patch, the GS passes the triangles
 *      through and paints green. Metal refuses it (the emulated geometry stage
 *      would need the tessellator's triangles, which Metal does not expose). */
$W = 8; $H = 4;
$FS_COL = "#version 450\nlayout(location = 0) in vec4 vcol;\nlayout(location = 0) out vec4 o;\nvoid main(){ o = vcol; }";

$CASES = [];
$CASES['A'] = [
    'vertex' => "#version 450\nlayout(location = 0) in vec3 aPos;\nlayout(location = 0) out float vid;\n"
              . "void main(){ gl_Position = vec4(aPos, 1.0); vid = float(gl_VertexIndex); }",
    'geometry' => "#version 450\nlayout(triangles_adjacency) in;\nlayout(triangle_strip, max_vertices = 8) out;\n"
              . "layout(location = 0) in float vid[];\nlayout(location = 0) out vec4 vcol;\n"
              . "void quad(float x0, float y0, float y1, vec4 c){\n"
              . "  /* outputs are undefined after EmitVertex(): write vcol before every one */\n"
              . "  vcol = c; gl_Position = vec4(x0, y0, 0, 1); EmitVertex(); vcol = c; gl_Position = vec4(x0 + 1.0, y0, 0, 1); EmitVertex();\n"
              . "  vcol = c; gl_Position = vec4(x0, y1, 0, 1); EmitVertex(); vcol = c; gl_Position = vec4(x0 + 1.0, y1, 0, 1); EmitVertex(); EndPrimitive(); }\n"
              . "void main(){ float x0 = -1.0 + float(gl_PrimitiveIDIn);\n"
              . "  quad(x0, 0.0, 1.0, vec4(vid[0], vid[1], vid[2], 255.0) * (20.0 / 255.0));\n"
              . "  quad(x0, -1.0, 0.0, vec4(vid[3], vid[4], vid[5], 255.0) * (20.0 / 255.0)); }",
    'fragment' => $FS_COL,
    'topology' => VIO_TRIANGLE_STRIP_ADJACENCY,
    'mesh' => ['vertices' => array_merge(...array_fill(0, 8, [0, 0, 0])), 'layout' => [VIO_FLOAT3]],
];
$CASES['B'] = [
    'vertex' => "#version 450\nlayout(location = 0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }",
    'geometry' => "#version 450\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 3) out;\n"
              . "void main(){ for (int i = 0; i < 3; i++) { gl_Position = gl_in[i].gl_Position; gl_PrimitiveID = 3 + gl_PrimitiveIDIn; EmitVertex(); } EndPrimitive(); }",
    'fragment' => "#version 450\nlayout(location = 0) out vec4 o;\nvoid main(){ o = vec4(float(gl_PrimitiveID) * 20.0 / 255.0, 0.0, 0.0, 1.0); }",
    'topology' => VIO_TRIANGLES,
];
$CASES['C'] = [
    'vertex' => "#version 450\nlayout(location = 0) in vec3 aPos;\nlayout(binding = 0) uniform sampler2D u_tex;\nlayout(location = 0) out float vr;\n"
              . "void main(){ gl_Position = vec4(aPos, 1.0); vr = texelFetch(u_tex, ivec2(1, 0), 0).r; }",
    'geometry' => "#version 450\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 3) out;\n"
              . "layout(binding = 0) uniform sampler2D u_tex;\nlayout(location = 0) in float vr[];\nlayout(location = 0) out vec4 vcol;\n"
              . "void main(){ float g = texelFetch(u_tex, ivec2(2, 0), 0).r;\n"
              . "  for (int i = 0; i < 3; i++) { gl_Position = gl_in[i].gl_Position; vcol = vec4(vr[i] + g, 0.0, 0.0, 1.0); EmitVertex(); } EndPrimitive(); }",
    'fragment' => $FS_COL,
    'topology' => VIO_TRIANGLES,
    'texture' => true,
];
$CASES['D'] = [
    'vertex' => "#version 450\nlayout(location = 0) in vec3 aPos;\nlayout(location = 0) out V { vec3 col; float k; } vOut;\n"
              . "void main(){ gl_Position = vec4(aPos, 1.0); vOut.col = vec3(1.0, 0.5, 0.25); vOut.k = 0.5; }",
    'geometry' => "#version 450\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 3) out;\n"
              . "layout(location = 0) in V { vec3 col; float k; } vIn[];\nlayout(location = 0) out G { vec4 c; } gOut;\n"
              . "void main(){ for (int i = 0; i < 3; i++) { gl_Position = gl_in[i].gl_Position; gOut.c = vec4(vIn[i].col * vIn[i].k * 2.0, 1.0); EmitVertex(); } EndPrimitive(); }",
    'fragment' => "#version 450\nlayout(location = 0) in G { vec4 c; } gIn;\nlayout(location = 0) out vec4 o;\nvoid main(){ o = gIn.c; }",
    'topology' => VIO_TRIANGLES,
];
$CASES['E'] = [
    'vertex' => "#version 450\nlayout(location = 0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }",
    'tess_control' => "#version 450\nlayout(vertices = 4) out;\nvoid main(){\n"
              . "  gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n"
              . "  if (gl_InvocationID == 0) { gl_TessLevelOuter[0] = 2.0; gl_TessLevelOuter[1] = 2.0; gl_TessLevelOuter[2] = 2.0;\n"
              . "    gl_TessLevelOuter[3] = 2.0; gl_TessLevelInner[0] = 2.0; gl_TessLevelInner[1] = 2.0; } }",
    'tess_eval' => "#version 450\nlayout(quads, equal_spacing, ccw) in;\n"
              . "void main(){ vec2 t = gl_TessCoord.xy;\n"
              . "  gl_Position = mix(mix(gl_in[0].gl_Position, gl_in[1].gl_Position, t.x), mix(gl_in[3].gl_Position, gl_in[2].gl_Position, t.x), t.y); }",
    'geometry' => "#version 450\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 3) out;\nlayout(location = 0) out vec4 vcol;\n"
              . "void main(){ for (int i = 0; i < 3; i++) { gl_Position = gl_in[i].gl_Position; vcol = vec4(0.0, 1.0, 0.0, 1.0); EmitVertex(); } EndPrimitive(); }",
    'fragment' => $FS_COL,
    'patch_vertices' => 4,
];
$FULL = [-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, -1, 0, 1, 1, 0, -1, 1, 0];   /* two triangles over the target */

function px(string $p, int $x, int $y, int $w): array { $o = ($y * $w + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }
function near3(array $a, array $b, int $tol = 3): bool { for ($k = 0; $k < 3; $k++) if (abs($a[$k] - $b[$k]) > $tol) return false; return true; }

function run_backend(string $name): string {
    global $W, $H, $CASES, $FULL;
    $ctx = @vio_create($name, ["width" => $W, "height" => $H, "headless" => true, "vsync" => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_GEOMETRY)) { vio_destroy($ctx); return "skip (no geometry stage)"; }
    error_clear_last();   /* an earlier backend's failed vio_create */
    $tess = vio_supports_feature($ctx, VIO_FEATURE_TESSELLATION);
    $tex = vio_texture($ctx, ['data' => pack('C*', 0, 0, 0, 255, 40, 0, 0, 255, 80, 0, 0, 255, 0, 0, 0, 255), 'width' => 4, 'height' => 1, 'filter' => VIO_FILTER_NEAREST]);
    $full = vio_mesh($ctx, ['vertices' => $FULL, 'layout' => [VIO_FLOAT3]]);
    $quadPatch = vio_mesh($ctx, ['vertices' => [-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0], 'layout' => [VIO_FLOAT3]]);
    $fail = [];
    foreach ($CASES as $id => $c) {
        if ($id === 'E' && !$tess) continue;
        $stages = ['vertex' => $c['vertex'], 'geometry' => $c['geometry'], 'fragment' => $c['fragment']];
        if (isset($c['tess_control'])) { $stages['tess_control'] = $c['tess_control']; $stages['tess_eval'] = $c['tess_eval']; }
        if ($id === 'E' && $name === 'metal') {
            /* Metal's tessellator does not hand out the triangles it generates,
             * which the emulated geometry stage would need: refused with a warning. */
            if (@vio_shader($ctx, $stages)) $fail[] = "E: Metal accepted a geometry stage behind tessellation";
            error_clear_last();
            continue;
        }
        $GLOBALS['WARN'] = [];
        set_error_handler(function ($no, $msg) { $GLOBALS['WARN'][] = $msg; return true; });
        $sh = vio_shader($ctx, $stages);
        restore_error_handler();
        if (!$sh) { $fail[] = "$id: shader not created: " . implode(' | ', $GLOBALS['WARN']); $GLOBALS['WARN'] = []; continue; }
        $po = ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE];
        if (isset($c['topology'])) $po['topology'] = $c['topology'];
        if (isset($c['patch_vertices'])) $po['patch_vertices'] = $c['patch_vertices'];
        $pipe = @vio_pipeline($ctx, $po);
        if (!$pipe) { $fail[] = "$id: pipeline not created"; continue; }
        $mesh = isset($c['mesh']) ? vio_mesh($ctx, $c['mesh']) : ($id === 'E' ? $quadPatch : $full);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        if (!empty($c['texture'])) { vio_bind_texture($ctx, $tex, 0); vio_set_uniform($ctx, 'u_tex', 0); }
        vio_draw($ctx, $mesh);
        vio_end($ctx);
        $p = vio_read_pixels($ctx);
        $err = error_get_last(); error_clear_last();
        if ($err && str_contains($err['message'], 'vio_')) $fail[] = "$id: " . $err['message'];
        switch ($id) {
            case 'A':
                /* gl_in[0..5] = v1, adj(v1,v2), v2, adj(v2,v3), v3, adj(v3,v1). The
                 * APIs agree on the triangle, its winding and each edge's adjacent
                 * vertex, but not on which vertex comes first (Vulkan rotates odd
                 * strip triangles), so the (vertex, adjacent) pairs are compared up
                 * to rotation. Either row may be the upper one (readback flips). */
                $want = [[0, 1, 2, 6, 4, 3], [4, 0, 2, 5, 6, 7]];
                $pairs = fn(array $s) => [[$s[0], $s[1]], [$s[2], $s[3]], [$s[4], $s[5]]];
                $same = function (array $got, array $exp) use ($pairs): bool {
                    $g = $pairs($got); $e = $pairs($exp);
                    for ($r = 0; $r < 3; $r++) if ([$g[$r], $g[($r + 1) % 3], $g[($r + 2) % 3]] === $e) return true;
                    return false;
                };
                foreach ([0, 1] as $prim) {
                    $r0 = array_map(fn($v) => (int)round($v / 20), px($p, $prim * 4 + 1, 0, $W));
                    $r1 = array_map(fn($v) => (int)round($v / 20), px($p, $prim * 4 + 1, $H - 1, $W));
                    if (!$same(array_merge($r0, $r1), $want[$prim]) && !$same(array_merge($r1, $r0), $want[$prim]))
                        $fail[] = "A: primitive $prim reads " . json_encode([$r0, $r1]) . ", want " . json_encode($want[$prim]) . " (up to rotation)";
                }
                break;
            case 'B':
                $a = px($p, 1, 2, $W)[0]; $b = px($p, 6, 1, $W)[0];
                $vals = [(int)round($a / 20), (int)round($b / 20)]; sort($vals);
                if ($vals !== [3, 4]) $fail[] = "B: primitive ids " . json_encode($vals) . ", want [3,4]";
                break;
            case 'C':
                if (!near3(px($p, 4, 2, $W), [120, 0, 0])) $fail[] = "C: " . json_encode(px($p, 4, 2, $W)) . ", want [120,0,0]";
                break;
            case 'D':
                if (!near3(px($p, 4, 2, $W), [255, 128, 64])) $fail[] = "D: " . json_encode(px($p, 4, 2, $W)) . ", want [255,128,64]";
                break;
            case 'E':
                if (!near3(px($p, 4, 2, $W), [0, 255, 0])) $fail[] = "E: " . json_encode(px($p, 4, 2, $W)) . ", want [0,255,0]";
                break;
        }
    }
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

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
