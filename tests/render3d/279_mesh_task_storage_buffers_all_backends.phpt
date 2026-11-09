--TEST--
Storage buffers in mesh and task stages (VIO_FEATURE_MESH_SHADER): the task stage reads binding 1, the mesh stage binding 0, both declared in both stages
--EXTENSIONS--
vio
--FILE--
<?php
/* Meshlet culling needs the task stage to read the meshlet bounds and the mesh
 * stage to read the vertices - two storage buffers, one of them in both stages.
 * The target is cut into four 4-pixel columns.
 *   A. task (4 invocations) reads mask b[i] from binding 1 and collects the lit
 *      columns through a shared atomic counter into the payload; mesh group k
 *      emits column ids[k] coloured from a[] at binding 0 (red, green, blue,
 *      white) -> mask 1 0 1 1 gives red, black, blue, white
 *   B. the same draw after swapping in another mask buffer (0 1 0 0) -> only
 *      the green column
 * D3D12: graphics storage buffers 0..3 are the root SRVs t0..t3 in space 2,
 * visible to the task (amplification) stage too. D3D12 is also tried on the
 * hardware adapter (WARP has no mesh shaders). VIO_REQUIRE_MESH_SHADER makes
 * the listed backends mandatory. */
$W = 16;
$HDR = "#version 450\n#extension GL_EXT_mesh_shader : require\n";
$BUF = "layout(std430, binding = 0) readonly buffer Colors { float a[]; };\n"
     . "layout(std430, binding = 1) readonly buffer Mask { float b[]; };\n";
$TASK = $HDR . $BUF . "layout(local_size_x = 4) in;\nstruct Payload { uint ids[4]; };\ntaskPayloadSharedEXT Payload pl;\nshared uint cnt;\n"
      . "void main(){ if (gl_LocalInvocationIndex == 0u) cnt = 0u; barrier();\n"
      . "  uint i = gl_LocalInvocationIndex; if (b[i] > 0.5) { uint s = atomicAdd(cnt, 1u); pl.ids[s] = i; }\n"
      . "  barrier(); EmitMeshTasksEXT(cnt, 1u, 1u); }";
$MESH = $HDR . $BUF . "layout(local_size_x = 1) in;\nlayout(triangles, max_vertices = 4, max_primitives = 2) out;\n"
      . "layout(location = 0) out vec4 vcol[];\nstruct Payload { uint ids[4]; };\ntaskPayloadSharedEXT Payload pl;\n"
      . "void main(){ uint c = pl.ids[gl_WorkGroupID.x]; float x0 = -1.0 + 0.5 * float(c), x1 = x0 + 0.5;\n"
      . "  SetMeshOutputsEXT(4, 2);\n"
      . "  gl_MeshVerticesEXT[0].gl_Position = vec4(x0, -1.0, 0.0, 1.0); gl_MeshVerticesEXT[1].gl_Position = vec4(x1, -1.0, 0.0, 1.0);\n"
      . "  gl_MeshVerticesEXT[2].gl_Position = vec4(x1, 1.0, 0.0, 1.0); gl_MeshVerticesEXT[3].gl_Position = vec4(x0, 1.0, 0.0, 1.0);\n"
      . "  vec4 col = vec4(a[c * 3u], a[c * 3u + 1u], a[c * 3u + 2u], 1.0); for (int i = 0; i < 4; i++) vcol[i] = col;\n"
      . "  gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2); gl_PrimitiveTriangleIndicesEXT[1] = uvec3(0, 2, 3); }";
$FS = "#version 450\nlayout(location = 0) in vec4 vcol;\nlayout(location = 0) out vec4 o;\nvoid main(){ o = vcol; }";

$opts = ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false, 'shader_model' => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_MESH_SHADER') ?: ''));

function columns(string $p, int $w): array {
    $out = [];
    for ($c = 0; $c < 4; $c++) {
        $o = (($w >> 1) * $w + $c * 4 + 1) * 4;
        $out[] = [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])];
    }
    return $out;
}
function near(array $got, array $want): bool {
    foreach ($want as $c => $rgb) for ($k = 0; $k < 3; $k++) if (abs($got[$c][$k] - $rgb[$k]) > 6) return false;
    return true;
}

function run_backend(string $name, array $extra, string $label): string {
    global $W, $TASK, $MESH, $FS, $opts, $require;
    $req = in_array($label, $require, true);
    $ctx = @vio_create($name, $extra + $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_MESH_SHADER)) {
        vio_destroy($ctx);
        return $req ? "FAIL\n  required but VIO_FEATURE_MESH_SHADER is 0" : "skip (no mesh shaders)";
    }
    $fail = [];
    $sh = vio_shader($ctx, ['task' => $TASK, 'mesh' => $MESH, 'fragment' => $FS]);
    $pipe = $sh ? vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]) : false;
    if (!$pipe) { vio_destroy($ctx); return "FAIL\n  shader / pipeline not created"; }
    $colors = vio_storage_buffer($ctx, ['data' => pack('g*', 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1)]);
    $masks = [
        'A' => [[1, 0, 1, 1], [[255, 0, 0], [0, 0, 0], [0, 0, 255], [255, 255, 255]]],
        'B' => [[0, 1, 0, 0], [[0, 0, 0], [0, 255, 0], [0, 0, 0], [0, 0, 0]]],
    ];
    foreach ($masks as $case => [$mask, $want]) {
        $mb = vio_storage_buffer($ctx, ['data' => pack('g*', ...$mask)]);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_bind_storage_buffer($ctx, $colors, 0, VIO_COMPUTE_READ);
        vio_bind_storage_buffer($ctx, $mb, 1, VIO_COMPUTE_READ);
        vio_draw_mesh_tasks($ctx, 1);
        vio_end($ctx);
        $got = columns(vio_read_pixels($ctx), $W);
        if (!near($got, $want)) $fail[] = "$case: columns " . json_encode($got) . ", want " . json_encode($want);
    }
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

$runs = [['opengl', [], 'opengl'], ['d3d12', [], 'd3d12'], ['d3d12', ['headless_hardware' => true], 'd3d12_hw'],
         ['vulkan', [], 'vulkan'], ['metal', [], 'metal']];
foreach ($runs as [$b, $extra, $label]) echo "$label: ", run_backend($b, $extra, $label), "\n";
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
d3d12_hw: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE
