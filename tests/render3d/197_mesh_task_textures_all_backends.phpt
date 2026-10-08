--TEST--
Textures in mesh and task stages (VIO_FEATURE_MESH_SHADER): a mesh shader reads a sampler2D / sampler2DArray, a task shader reads a mask texture, the fragment shader shares the table
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A30. The target is cut into four 4-pixel columns.
 *   A. mesh only: mesh group c reads texel c of u_tex (red, green, blue, white)
 *      with texelFetch and passes it as the vertex colour; the fragment shader
 *      multiplies it with texel 3 of the same u_tex (white) -> the four colours
 *   B. task + mesh: the task shader reads u_mask (r = 255, 0, 255, 0) and only
 *      launches the mesh group of a lit texel -> columns 0 and 2 coloured, 1 and
 *      3 black; the mesh shader still reads u_tex
 *   C. mesh shader reads layer 1 of a two-layer sampler2DArray (layer 0 black,
 *      layer 1 the colours) through textureLod with normalised coordinates
 * Samplers are declared in the same order in every stage (the vio contract).
 * VIO_REQUIRE_MESH_SHADER makes the listed backends mandatory. */
$W = 16;
$HDR = "#version 450\n#extension GL_EXT_mesh_shader : require\n";
$SMP = "layout(binding = 0) uniform sampler2D u_tex;\nlayout(binding = 1) uniform sampler2D u_mask;\n";
$QUAD = "layout(local_size_x = 1) in;\nlayout(triangles, max_vertices = 4, max_primitives = 2) out;\n"
      . "layout(location = 0) out vec4 vcol[];\n"
      . "void emit_quad(float x0, float x1, vec4 c){\n"
      . "  SetMeshOutputsEXT(4, 2);\n"
      . "  gl_MeshVerticesEXT[0].gl_Position = vec4(x0, -1.0, 0.0, 1.0);\n"
      . "  gl_MeshVerticesEXT[1].gl_Position = vec4(x1, -1.0, 0.0, 1.0);\n"
      . "  gl_MeshVerticesEXT[2].gl_Position = vec4(x1, 1.0, 0.0, 1.0);\n"
      . "  gl_MeshVerticesEXT[3].gl_Position = vec4(x0, 1.0, 0.0, 1.0);\n"
      . "  for (int i = 0; i < 4; i++) vcol[i] = c;\n"
      . "  gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);\n"
      . "  gl_PrimitiveTriangleIndicesEXT[1] = uvec3(0, 2, 3); }\n";
$MESH_A = $HDR . $SMP . $QUAD
        . "void main(){ uint c = gl_WorkGroupID.x; float x0 = -1.0 + 0.5 * float(c);\n"
        . "  emit_quad(x0, x0 + 0.5, texelFetch(u_tex, ivec2(int(c), 0), 0)); }";
$TASK = $HDR . $SMP . "layout(local_size_x = 1) in;\n"
      . "struct Payload { uint id; };\ntaskPayloadSharedEXT Payload pl;\n"
      . "void main(){ pl.id = gl_WorkGroupID.x;\n"
      . "  bool on = texelFetch(u_mask, ivec2(int(gl_WorkGroupID.x), 0), 0).r > 0.5;\n"
      . "  EmitMeshTasksEXT(on ? 1u : 0u, 1u, 1u); }";
$MESH_B = $HDR . $SMP . $QUAD
        . "struct Payload { uint id; };\ntaskPayloadSharedEXT Payload pl;\n"
        . "void main(){ float x0 = -1.0 + 0.5 * float(pl.id);\n"
        . "  emit_quad(x0, x0 + 0.5, texelFetch(u_tex, ivec2(int(pl.id), 0), 0)); }";
$MESH_C = $HDR . "layout(binding = 0) uniform sampler2DArray u_arr;\n" . $QUAD
        . "void main(){ uint c = gl_WorkGroupID.x; float x0 = -1.0 + 0.5 * float(c);\n"
        . "  emit_quad(x0, x0 + 0.5, textureLod(u_arr, vec3((float(c) + 0.5) / 4.0, 0.5, 1.0), 0.0)); }";
$FS = "#version 450\nlayout(binding = 0) uniform sampler2D u_tex;\n"
    . "layout(location = 0) in vec4 vcol;\nlayout(location = 0) out vec4 o;\n"
    . "void main(){ o = vcol * texelFetch(u_tex, ivec2(3, 0), 0); }";
$FS_PLAIN = "#version 450\nlayout(location = 0) in vec4 vcol;\nlayout(location = 0) out vec4 o;\nvoid main(){ o = vcol; }";

$opts = ["width" => $W, "height" => $W, "headless" => true, "vsync" => false, "shader_model" => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_MESH_SHADER') ?: ''));

$COLORS = [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 255]];
function rgba(array $cols): string { $s = ''; foreach ($cols as $c) $s .= pack('C4', $c[0], $c[1], $c[2], 255); return $s; }
function px(string $p, int $x, int $y, int $w): array { $o = ($y * $w + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }
function columns(string $p, int $w): array { $c = []; for ($i = 0; $i < 4; $i++) $c[] = px($p, $i * 4 + 1, $w >> 1, $w); return $c; }
function same(array $got, array $want): bool {
    foreach ($want as $i => $w) for ($k = 0; $k < 3; $k++) if (abs($got[$i][$k] - $w[$k]) > 6) return false;
    return true;
}

function run_backend(string $name): string {
    global $W, $MESH_A, $TASK, $MESH_B, $MESH_C, $FS, $FS_PLAIN, $opts, $require, $COLORS;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_MESH_SHADER)) {
        vio_destroy($ctx);
        return $req ? "FAIL\n  required but VIO_FEATURE_MESH_SHADER is 0" : "skip (no mesh shaders)";
    }
    $fail = [];
    $base = ['depth_test' => false, 'cull_mode' => VIO_CULL_NONE];
    $tex  = vio_texture($ctx, ['data' => rgba($COLORS), 'width' => 4, 'height' => 1, 'filter' => VIO_FILTER_NEAREST]);
    $mask = vio_texture($ctx, ['data' => rgba([[255, 0, 0], [0, 0, 0], [255, 0, 0], [0, 0, 0]]), 'width' => 4, 'height' => 1, 'filter' => VIO_FILTER_NEAREST]);
    $frame = function ($pipe, callable $issue) use ($ctx, $W): string {
        if (!$pipe) return str_repeat("\x7f", $W * $W * 4);  /* rejected pipeline: grey never matches */
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        $issue();
        vio_end($ctx);
        return vio_read_pixels($ctx);
    };
    $bindBoth = function () use ($ctx, $tex, $mask) {
        vio_bind_texture($ctx, $tex, 0);
        vio_bind_texture($ctx, $mask, 1);
        vio_set_uniform($ctx, 'u_tex', 0);
        vio_set_uniform($ctx, 'u_mask', 1);
    };

    /* A: mesh only, fragment shares u_tex. */
    $shA = vio_shader($ctx, ['mesh' => $MESH_A, 'fragment' => $FS]);
    if (!$shA) $fail[] = "A: shader not created";
    else {
        $g = columns($frame(vio_pipeline($ctx, ['shader' => $shA] + $base), function () use ($ctx, $bindBoth) { $bindBoth(); vio_draw_mesh_tasks($ctx, 4); }), $W);
        if (!same($g, $COLORS)) $fail[] = "A: columns " . json_encode($g) . ", want " . json_encode($COLORS);
    }

    /* B: task reads the mask. */
    $shB = vio_shader($ctx, ['task' => $TASK, 'mesh' => $MESH_B, 'fragment' => $FS]);
    if (!$shB) $fail[] = "B: shader not created";
    else {
        $want = [$COLORS[0], [0, 0, 0], $COLORS[2], [0, 0, 0]];
        $g = columns($frame(vio_pipeline($ctx, ['shader' => $shB] + $base), function () use ($ctx, $bindBoth) { $bindBoth(); vio_draw_mesh_tasks($ctx, 4); }), $W);
        if (!same($g, $want)) $fail[] = "B: columns " . json_encode($g) . ", want " . json_encode($want);
    }

    /* C: texture array in the mesh stage. */
    $arr = vio_texture($ctx, ['data' => rgba(array_fill(0, 4, [0, 0, 0])) . rgba($COLORS), 'width' => 4, 'height' => 1, 'layers' => 2, 'filter' => VIO_FILTER_NEAREST]);
    $shC = vio_shader($ctx, ['mesh' => $MESH_C, 'fragment' => $FS_PLAIN]);
    if (!$arr || !$shC) $fail[] = "C: texture array or shader not created";
    else {
        $g = columns($frame(vio_pipeline($ctx, ['shader' => $shC] + $base), function () use ($ctx, $arr) { vio_bind_texture($ctx, $arr, 0); vio_set_uniform($ctx, 'u_arr', 0); vio_draw_mesh_tasks($ctx, 4); }), $W);
        if (!same($g, $COLORS)) $fail[] = "C: columns " . json_encode($g) . ", want " . json_encode($COLORS);
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
