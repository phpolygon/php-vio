--TEST--
Bindless textures with mesh pipelines: the fragment stage of a mesh pipeline reads vio_textures[] through a slot, direct and indirect
--EXTENSIONS--
vio
--FILE--
<?php
/* A mesh group emits a full-screen triangle, the fragment stage samples the slot of a
 * green 1x1 texture from the bindless table (vio_sampler_aniso). D3D12 points the
 * table root parameter at every mesh dispatch like at every vertex draw. */
$MESH = "#version 450\n#extension GL_EXT_mesh_shader : require\nlayout(local_size_x = 1) in;\n"
      . "layout(triangles, max_vertices = 3, max_primitives = 1) out;\nlayout(location = 0) out vec2 vuv[];\n"
      . "void main(){ SetMeshOutputsEXT(3, 1);\n"
      . "  gl_MeshVerticesEXT[0].gl_Position = vec4(-1.0, -1.0, 0.0, 1.0); gl_MeshVerticesEXT[1].gl_Position = vec4(3.0, -1.0, 0.0, 1.0);\n"
      . "  gl_MeshVerticesEXT[2].gl_Position = vec4(-1.0, 3.0, 0.0, 1.0);\n"
      . "  vuv[0] = vec2(0.0); vuv[1] = vec2(2.0, 0.0); vuv[2] = vec2(0.0, 2.0);\n"
      . "  gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2); }";
$FS = "#version 450\n#extension GL_EXT_nonuniform_qualifier : require\n"
    . "layout(location = 0) in vec2 vuv;\nlayout(location = 0) out vec4 o;\nuniform int u_slot;\n"
    . "layout(set = 1, binding = 0) uniform texture2D vio_textures[];\n"
    . "layout(set = 1, binding = 7) uniform sampler vio_sampler_aniso;\n"
    . "void main(){ o = texture(sampler2D(vio_textures[nonuniformEXT(u_slot)], vio_sampler_aniso), vuv); }";
$opts = ['width' => 8, 'height' => 8, 'headless' => true, 'vsync' => false, 'shader_model' => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;

function run_backend(string $name, array $extra = []): string {
    global $MESH, $FS, $opts;
    $ctx = @vio_create($name, $extra + $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_MESH_SHADER) || !vio_supports_feature($ctx, VIO_FEATURE_BINDLESS)) {
        vio_destroy($ctx);
        return "skip (no mesh shaders or bindless)";
    }
    $sh = vio_shader($ctx, ['mesh' => $MESH, 'fragment' => $FS]);
    $pipe = $sh ? vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]) : false;
    if (!$pipe) { vio_destroy($ctx); return "FAIL\n  mesh pipeline with a bindless fragment stage not created"; }
    vio_texture_index($ctx, vio_texture($ctx, ['data' => "\xFF\x00\x00\xFF", 'width' => 1, 'height' => 1]));
    $slot = vio_texture_index($ctx, vio_texture($ctx, ['data' => "\x00\xFF\x00\xFF", 'width' => 1, 'height' => 1]));
    $args = vio_storage_buffer($ctx, ['data' => pack('V*', 1, 1, 1), 'indirect' => true]);
    $err = [];
    foreach (['direct', 'indirect'] as $mode) {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniform($ctx, 'u_slot', $slot);
        if ($mode === 'direct') vio_draw_mesh_tasks($ctx, 1);
        else vio_draw_mesh_tasks_indirect($ctx, $args, 1, 0);
        vio_end($ctx);
        $p = vio_read_pixels($ctx);
        $o = (4 * 8 + 4) * 4;
        $rgb = [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])];
        if ($rgb[1] < 240 || $rgb[0] > 15) $err[] = "$mode: " . json_encode($rgb) . ", want green";
    }
    vio_destroy($ctx);
    return $err ? "FAIL\n  " . implode("\n  ", $err) : "OK";
}

echo "d3d12_hw: ", run_backend('d3d12', ['headless_hardware' => true]), "\n";
echo "vulkan: ", run_backend('vulkan'), "\n";
echo "metal: ", run_backend('metal'), "\n";
echo "DONE\n";
?>
--EXPECTF--
d3d12_hw: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE
