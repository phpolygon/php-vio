--TEST--
Mesh and task shaders (VIO_FEATURE_MESH_SHADER): vio_shader(['task' => ..., 'mesh' => ..., 'fragment' => ...]) + vio_draw_mesh_tasks / vio_draw_mesh_tasks_indirect on every backend that reports the flag
--EXTENSIONS--
vio
--FILE--
<?php
/* GL_EXT_mesh_shader. The target is cut into four 4-pixel columns.
 *   A. task + mesh: four task groups; the task shader forwards its group id in
 *      the payload and launches a mesh group only for even ids; each mesh group
 *      emits a quad (two triangles) over column id with a per-primitive green
 *      times a per-vertex tint from a mesh-stage uniform -> columns 0 and 2 lit
 *      (green 128 from the tint), 1 and 3 black
 *   B. mesh only: four mesh groups, gl_WorkGroupID picks the column -> all lit
 *   C. the same as A through vio_draw_mesh_tasks_indirect (3 x uint32 record)
 *   D. orientation: a mesh quad over the upper half renders exactly like the
 *      same quad through a vertex pipeline (clip-space conventions per backend)
 *   E. the contract: 'vertex' with 'mesh', 'task' without 'mesh' are refused;
 *      without the flag 'mesh' is refused
 * D3D12 needs SM 6.5 + MeshShaderTier (DXC). VIO_REQUIRE_MESH_SHADER makes the
 * listed backends mandatory. */
$W = 16;
$TASK = "#version 450\n#extension GL_EXT_mesh_shader : require\nlayout(local_size_x = 1) in;\n"
      . "struct Payload { uint id; };\ntaskPayloadSharedEXT Payload pl;\n"
      . "void main(){ pl.id = gl_WorkGroupID.x; EmitMeshTasksEXT((gl_WorkGroupID.x & 1u) == 0u ? 1u : 0u, 1u, 1u); }";
$MESH_BODY = "layout(local_size_x = 1) in;\nlayout(triangles, max_vertices = 4, max_primitives = 2) out;\n"
      . "layout(location = 0) out vec4 vcol[];\nlayout(location = 1) perprimitiveEXT flat out vec4 pcol[];\n"
      . "uniform vec4 u_tint;\n"
      . "void emit_quad(float x0, float x1, float y0, float y1){\n"
      . "  SetMeshOutputsEXT(4, 2);\n"
      . "  gl_MeshVerticesEXT[0].gl_Position = vec4(x0, y0, 0.0, 1.0);\n"
      . "  gl_MeshVerticesEXT[1].gl_Position = vec4(x1, y0, 0.0, 1.0);\n"
      . "  gl_MeshVerticesEXT[2].gl_Position = vec4(x1, y1, 0.0, 1.0);\n"
      . "  gl_MeshVerticesEXT[3].gl_Position = vec4(x0, y1, 0.0, 1.0);\n"
      . "  for (int i = 0; i < 4; i++) vcol[i] = u_tint;\n"
      . "  gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);\n"
      . "  gl_PrimitiveTriangleIndicesEXT[1] = uvec3(0, 2, 3);\n"
      . "  pcol[0] = vec4(0.0, 1.0, 0.0, 1.0); pcol[1] = vec4(0.0, 1.0, 0.0, 1.0); }\n";
$MESH_T = "#version 450\n#extension GL_EXT_mesh_shader : require\n" . $MESH_BODY
        . "struct Payload { uint id; };\ntaskPayloadSharedEXT Payload pl;\n"
        . "void main(){ float x0 = -1.0 + 0.5 * float(pl.id); emit_quad(x0, x0 + 0.5, -1.0, 1.0); }";
$MESH_C = "#version 450\n#extension GL_EXT_mesh_shader : require\n" . $MESH_BODY
        . "void main(){ float x0 = -1.0 + 0.5 * float(gl_WorkGroupID.x); emit_quad(x0, x0 + 0.5, -1.0, 1.0); }";
$MESH_TOP = "#version 450\n#extension GL_EXT_mesh_shader : require\n" . $MESH_BODY
        . "void main(){ emit_quad(-1.0, 1.0, 0.0, 1.0); }";
$FS = "#version 450\n#extension GL_EXT_mesh_shader : require\n"
    . "layout(location = 0) in vec4 vcol;\nlayout(location = 1) perprimitiveEXT flat in vec4 pcol;\n"
    . "layout(location = 0) out vec4 o;\nvoid main(){ o = pcol * vcol; }";
$VS_TOP = "#version 450\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$FS_TOP = "#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0.0, 1.0, 0.0, 1.0); }";

$opts = ["width" => $W, "height" => $W, "headless" => true, "vsync" => false, "shader_model" => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_MESH_SHADER') ?: ''));

function px(string $p, int $x, int $y, int $w): array { $o = ($y * $w + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }
/* Green of the four columns at mid-height. */
function columns(string $p, int $w): array { $g = []; for ($c = 0; $c < 4; $c++) $g[] = px($p, $c * 4 + 1, $w >> 1, $w)[1]; return $g; }
function lit(int $g, int $want): bool { return abs($g - $want) <= 6; }

function run_backend(string $name): string {
    global $W, $TASK, $MESH_T, $MESH_C, $MESH_TOP, $FS, $VS_TOP, $FS_TOP, $opts, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_MESH_SHADER)) {
        $sh = @vio_shader($ctx, ['mesh' => $MESH_C, 'fragment' => $FS]);
        vio_destroy($ctx);
        if ($sh) return "FAIL\n  'mesh' accepted without VIO_FEATURE_MESH_SHADER";
        return $req ? "FAIL\n  required but VIO_FEATURE_MESH_SHADER is 0" : "skip (no mesh shaders)";
    }
    $fail = [];
    $base = ['depth_test' => false, 'cull_mode' => VIO_CULL_NONE];
    $frame = function ($pipe, callable $issue) use ($ctx, $W): string {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        $issue();
        vio_end($ctx);
        return vio_read_pixels($ctx);
    };

    /* A + C: task + mesh. */
    $shT = vio_shader($ctx, ['task' => $TASK, 'mesh' => $MESH_T, 'fragment' => $FS]);
    if (!$shT) $fail[] = "A: task + mesh shader not created";
    else {
        $pT = vio_pipeline($ctx, ['shader' => $shT] + $base);
        $want = [128, 0, 128, 0];
        $g = columns($frame($pT, function () use ($ctx) { vio_set_uniform($ctx, 'u_tint', [1.0, 0.5, 1.0, 1.0]); vio_draw_mesh_tasks($ctx, 4); }), $W);
        foreach ($want as $c => $w) if (!lit($g[$c], $w)) { $fail[] = "A: column greens " . json_encode($g) . ", want " . json_encode($want); break; }
        $args = vio_storage_buffer($ctx, ['data' => pack('V*', 4, 1, 1), 'indirect' => true]);
        $g = columns($frame($pT, function () use ($ctx, $args) { vio_set_uniform($ctx, 'u_tint', [1.0, 0.5, 1.0, 1.0]); vio_draw_mesh_tasks_indirect($ctx, $args, 1, 0); }), $W);
        foreach ($want as $c => $w) if (!lit($g[$c], $w)) { $fail[] = "C: indirect column greens " . json_encode($g) . ", want " . json_encode($want); break; }
    }

    /* B: mesh only. */
    $shC = vio_shader($ctx, ['mesh' => $MESH_C, 'fragment' => $FS]);
    if (!$shC) $fail[] = "B: mesh shader not created";
    else {
        $g = columns($frame(vio_pipeline($ctx, ['shader' => $shC] + $base), function () use ($ctx) { vio_set_uniform($ctx, 'u_tint', [1.0, 1.0, 1.0, 1.0]); vio_draw_mesh_tasks($ctx, 4); }), $W);
        foreach ($g as $c => $v) if (!lit($v, 255)) { $fail[] = "B: column greens " . json_encode($g) . ", want all 255"; break; }
    }

    /* D: orientation against the vertex pipeline. */
    $shTop = vio_shader($ctx, ['mesh' => $MESH_TOP, 'fragment' => $FS]);
    $vsTop = vio_shader($ctx, ['vertex' => $VS_TOP, 'fragment' => $FS_TOP]);
    if (!$shTop || !$vsTop) $fail[] = "D: shaders not created";
    else {
        $pm = $frame(vio_pipeline($ctx, ['shader' => $shTop] + $base), function () use ($ctx) { vio_set_uniform($ctx, 'u_tint', [1.0, 1.0, 1.0, 1.0]); vio_draw_mesh_tasks($ctx, 1); });
        $top = vio_mesh($ctx, ['vertices' => [-1,0,0, 1,0,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
        $pv = $frame(vio_pipeline($ctx, ['shader' => $vsTop] + $base), function () use ($ctx, $top) { vio_draw($ctx, $top); });
        $rowsM = []; $rowsV = [];
        for ($y = 0; $y < $W; $y++) { $rowsM[] = px($pm, 8, $y, $W)[1] > 128 ? 1 : 0; $rowsV[] = px($pv, 8, $y, $W)[1] > 128 ? 1 : 0; }
        if ($rowsM !== $rowsV) $fail[] = "D: mesh rows " . implode('', $rowsM) . " vs vertex rows " . implode('', $rowsV);
        if (array_sum($rowsV) !== $W / 2) $fail[] = "D: the vertex quad covers " . array_sum($rowsV) . " rows, want " . ($W / 2);
    }

    /* E: the contract. */
    if (@vio_shader($ctx, ['vertex' => $VS_TOP, 'mesh' => $MESH_C, 'fragment' => $FS])) $fail[] = "E: 'vertex' with 'mesh' accepted";
    if (@vio_shader($ctx, ['task' => $TASK, 'vertex' => $VS_TOP, 'fragment' => $FS_TOP])) $fail[] = "E: 'task' without 'mesh' accepted";

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
