--TEST--
Tessellation varyings on every backend: interface blocks VS -> TCS -> TES, struct / matrix / patch-block varyings, control points read after barrier(), gl_ClipDistance through the tessellation stages
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A29. One quad patch (4 control points) covers the 16x16 target;
 * every case colours the evaluation stage's output and checks pixels.
 *   A. interface blocks: the vertex stage writes `out V { vec3 col; float k; }`,
 *      the control stage copies it into an output block, the evaluation stage
 *      reads it -> (1, 0.5, 0.25) * 2k = (255, 128, 64)
 *   B. a struct varying, a mat2 varying and a patch-out block between the
 *      tessellation stages -> (51, 128, 64)
 *   C. every invocation writes v[id], barrier(), then w[id] = v[(id + 1) % 4]:
 *      the evaluation stage sees w = (0.25, 0.5, 0.75, 0) -> (64, 128, 0)
 *   D. gl_ClipDistance[0] = x from the vertex stage, carried through gl_out /
 *      gl_in and interpolated in the evaluation stage: the left half is clipped
 * Before, D3D refused A and B ("interface blocks and struct varyings are not
 * supported"), ran C's control points without the other invocations' writes,
 * and dropped D. */
$W = 16;
$VS_PLAIN = "#version 450\nlayout(location = 0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$LEVELS = "  if (gl_InvocationID == 0) { gl_TessLevelOuter[0] = 1.0; gl_TessLevelOuter[1] = 1.0; gl_TessLevelOuter[2] = 1.0;\n"
        . "    gl_TessLevelOuter[3] = 1.0; gl_TessLevelInner[0] = 1.0; gl_TessLevelInner[1] = 1.0; }\n";
$POS_TCS = "  gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n";
$TES_HEAD = "#version 450\nlayout(quads, equal_spacing, ccw) in;\nlayout(location = 0) out vec4 vcol;\n"
          . "vec4 quad_pos(){ vec2 t = gl_TessCoord.xy;\n"
          . "  return mix(mix(gl_in[0].gl_Position, gl_in[1].gl_Position, t.x), mix(gl_in[3].gl_Position, gl_in[2].gl_Position, t.x), t.y); }\n";
$FS = "#version 450\nlayout(location = 0) in vec4 vcol;\nlayout(location = 0) out vec4 o;\nvoid main(){ o = vcol; }";

$CASES = [];
/* A: interface blocks. */
$CASES['A'] = [
    'vertex' => "#version 450\nlayout(location = 0) in vec3 aPos;\nlayout(location = 0) out V { vec3 col; float k; } vOut;\n"
              . "void main(){ gl_Position = vec4(aPos, 1.0); vOut.col = vec3(1.0, 0.5, 0.25); vOut.k = 0.5; }",
    'tess_control' => "#version 450\nlayout(vertices = 4) out;\n"
              . "layout(location = 0) in V { vec3 col; float k; } vIn[];\nlayout(location = 0) out V { vec3 col; float k; } tOut[];\n"
              . "void main(){\n" . $POS_TCS
              . "  tOut[gl_InvocationID].col = vIn[gl_InvocationID].col; tOut[gl_InvocationID].k = vIn[gl_InvocationID].k;\n" . $LEVELS . "}",
    'tess_eval' => $TES_HEAD . "layout(location = 0) in V { vec3 col; float k; } tIn[];\n"
              . "void main(){ gl_Position = quad_pos(); vcol = vec4(tIn[2].col * tIn[2].k * 2.0, 1.0); }",
    'want' => [[8, 8, [255, 128, 64]]],
];
/* B: struct, matrix and patch-block varyings. */
$CASES['B'] = [
    'vertex' => $VS_PLAIN,
    'tess_control' => "#version 450\nlayout(vertices = 4) out;\nstruct S { vec4 a; vec2 b; };\n"
              . "layout(location = 0) out S sOut[];\nlayout(location = 2) out mat2 mOut[];\nlayout(location = 4) patch out P { vec4 c; } pOut;\n"
              . "void main(){\n" . $POS_TCS
              . "  sOut[gl_InvocationID].a = vec4(0.2, 0.4, 0.6, 1.0); sOut[gl_InvocationID].b = vec2(0.5, 1.0);\n"
              . "  mOut[gl_InvocationID] = mat2(1.0, 0.0, 0.0, 0.5);\n"
              . "  if (gl_InvocationID == 0) pOut.c = vec4(0.0, 0.0, 1.0, 1.0);\n" . $LEVELS . "}",
    'tess_eval' => $TES_HEAD . "struct S { vec4 a; vec2 b; };\n"
              . "layout(location = 0) in S sIn[];\nlayout(location = 2) in mat2 mIn[];\nlayout(location = 4) patch in P { vec4 c; } pIn;\n"
              . "void main(){ gl_Position = quad_pos();\n"
              . "  vcol = vec4(sIn[1].a.x * sIn[3].b.y, (mIn[2] * vec2(0.0, 1.0)).y, pIn.c.z * 0.25, 1.0); }",
    'want' => [[8, 8, [51, 128, 64]]],
];
/* C: foreign control points after barrier(). */
$CASES['C'] = [
    'vertex' => $VS_PLAIN,
    'tess_control' => "#version 450\nlayout(vertices = 4) out;\nlayout(location = 0) out float v[];\nlayout(location = 1) out float w[];\n"
              . "void main(){\n" . $POS_TCS
              . "  v[gl_InvocationID] = float(gl_InvocationID) * 0.25;\n  barrier();\n"
              . "  w[gl_InvocationID] = v[(gl_InvocationID + 1) % 4];\n" . $LEVELS . "}",
    'tess_eval' => $TES_HEAD . "layout(location = 0) in float v[];\nlayout(location = 1) in float w[];\n"
              . "void main(){ gl_Position = quad_pos(); vcol = vec4(w[0], w[1], w[3], 1.0); }",
    'want' => [[8, 8, [64, 128, 0]]],
];
/* D: gl_ClipDistance through the tessellation stages. */
$CASES['D'] = [
    'vertex' => "#version 450\nlayout(location = 0) in vec3 aPos;\n"
              . "void main(){ gl_Position = vec4(aPos, 1.0); gl_ClipDistance[0] = aPos.x; }",
    'tess_control' => "#version 450\nlayout(vertices = 4) out;\n"
              . "void main(){\n" . $POS_TCS
              . "  gl_out[gl_InvocationID].gl_ClipDistance[0] = gl_in[gl_InvocationID].gl_ClipDistance[0];\n" . $LEVELS . "}",
    'tess_eval' => $TES_HEAD
              . "void main(){ gl_Position = quad_pos(); vec2 t = gl_TessCoord.xy;\n"
              . "  gl_ClipDistance[0] = mix(mix(gl_in[0].gl_ClipDistance[0], gl_in[1].gl_ClipDistance[0], t.x),\n"
              . "                           mix(gl_in[3].gl_ClipDistance[0], gl_in[2].gl_ClipDistance[0], t.x), t.y);\n"
              . "  vcol = vec4(0.0, 1.0, 0.0, 1.0); }",
    'want' => [[3, 8, [0, 0, 0]], [12, 8, [0, 255, 0]]],
];

function px(string $p, int $x, int $y, int $w): array { $o = ($y * $w + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }

function run_backend(string $name): string {
    global $W, $CASES, $FS;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_TESSELLATION)) { vio_destroy($ctx); return "skip (no tessellation)"; }
    $quad = vio_mesh($ctx, ['vertices' => [-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0], 'layout' => [VIO_FLOAT3]]);
    $fail = [];
    foreach ($CASES as $id => $c) {
        $sh = @vio_shader($ctx, ['vertex' => $c['vertex'], 'tess_control' => $c['tess_control'], 'tess_eval' => $c['tess_eval'], 'fragment' => $FS]);
        if (!$sh) { $fail[] = "$id: shader not created" . (($e = error_get_last()) ? ": " . $e['message'] : ''); error_clear_last(); continue; }
        $pipe = vio_pipeline($ctx, ['shader' => $sh, 'patch_vertices' => 4, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
        if (!$pipe) { $fail[] = "$id: pipeline not created"; continue; }
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_draw($ctx, $quad);
        vio_end($ctx);
        $p = vio_read_pixels($ctx);
        foreach ($c['want'] as [$x, $y, $rgb]) {
            /* Clip-space x is the same on every backend; y does not matter here. */
            $got = px($p, $x, $y, $W);
            for ($k = 0; $k < 3; $k++) if (abs($got[$k] - $rgb[$k]) > 3) { $fail[] = "$id: pixel ($x, $y) " . json_encode($got) . ", want " . json_encode($rgb); break; }
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
