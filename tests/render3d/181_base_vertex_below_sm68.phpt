--TEST--
Draw parameters on D3D12 below Shader Model 6.8: gl_BaseVertex / gl_BaseInstance reach the vertex stage through a root constant - per record of a multi-draw indirect call, and 0 for vio_draw - under FXC, pinned DXC profiles and SM 6.8, with Vulkan and OpenGL as reference
--EXTENSIONS--
vio
--SKIPIF--
<?php
$c = @vio_create('d3d12', ["width" => 8, "height" => 8, "headless" => true]);
if (!$c) die("skip d3d12 unavailable");
vio_destroy($c);
?>
--FILE--
<?php
/* OPEN-ITEMS-PLAN A11. Mesh: vertices 0..3 collapse to a point, 4..7 cover the
 * left half, 8..11 the right half. One vio_draw_indirect with two records
 * (baseVertex 4 / firstInstance 3, baseVertex 8 / firstInstance 5) must draw both
 * halves, each coloured by the (base vertex, base instance) it saw. */
$VS = "#version 450\n#extension GL_ARB_shader_draw_parameters : require\n"
     . "layout(location=0) in vec3 aPos;\nlayout(location=0) flat out ivec2 bp;\n"
     . "void main(){ bp = ivec2(gl_BaseVertexARB, gl_BaseInstanceARB); gl_Position = vec4(aPos, 1.0); }";
$FS = "#version 450\nlayout(location=0) flat in ivec2 bp;\nlayout(location=0) out vec4 o;\n"
     . "void main(){ o = vec4(float(bp.x) / 16.0, float(bp.y) / 16.0, 1.0, 1.0); }";

$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);

function px(string $p, int $x, int $y): array { $o = ($y * 16 + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }
function near(array $a, array $b): bool { foreach ($a as $i => $v) if (abs($v - $b[$i]) > 2) return false; return true; }

function run(string $backend, array $extra, string $label): string {
    global $VS, $FS, $dxc;
    $o = ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false] + $extra;
    if ($dxc !== '' && isset($extra['shader_model'])) $o['dxc_dir'] = $dxc;
    $ctx = @vio_create($backend, $o);
    if ($ctx && vio_backend_name($ctx) !== $backend) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "$label: skip (unavailable)";
    if (isset($extra['shader_model']) && $extra['shader_model'] >= 60
        && (vio_swapchain_info($ctx)['shader_model_version'] ?? 0) !== $extra['shader_model'] && $extra['shader_model'] !== 69) {
        vio_destroy($ctx);
        return "$label: skip (profile not available)";
    }
    if (!vio_supports_feature($ctx, VIO_FEATURE_BASE_VERTEX)) { vio_destroy($ctx); return "$label: FAIL\n  VIO_FEATURE_BASE_VERTEX is 0"; }
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $left  = [-1,-1,0, 0,-1,0, 0,1,0, -1,1,0];
    $right = [0,-1,0, 1,-1,0, 1,1,0, 0,1,0];
    $mesh = vio_mesh($ctx, ['vertices' => array_merge(array_fill(0, 12, 0.0), $left, $right), 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $full = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    /* indexCount, instanceCount, firstIndex, baseVertex, firstInstance */
    $args = vio_storage_buffer($ctx, ['data' => pack('V*', 6, 1, 0, 4, 3, 6, 1, 0, 8, 5), 'indirect' => true]);
    $err = [];
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pipe);
    vio_draw_indirect($ctx, $mesh, $args, 2, 0);
    vio_end($ctx);
    $p = vio_read_pixels($ctx);
    if (!near(px($p, 4, 8), [64, 48, 255])) $err[] = "record 1 (4, 3): " . json_encode(px($p, 4, 8));
    if (!near(px($p, 12, 8), [128, 80, 255])) $err[] = "record 2 (8, 5): " . json_encode(px($p, 12, 8));
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pipe);
    vio_draw($ctx, $full);
    vio_end($ctx);
    if (!near(px(vio_read_pixels($ctx), 8, 8), [0, 0, 255])) $err[] = "vio_draw (0, 0): " . json_encode(px(vio_read_pixels($ctx), 8, 8));
    vio_destroy($ctx);
    return $err ? "$label: FAIL\n  " . implode("\n  ", $err) : "$label: OK";
}

echo run('d3d12', ['shader_model' => 5], 'd3d12 FXC 5.1'), "\n";
echo run('d3d12', ['shader_model' => 60], 'd3d12 SM 6.0'), "\n";
echo run('d3d12', ['shader_model' => 65], 'd3d12 SM 6.5'), "\n";
echo run('d3d12', ['shader_model' => 6], 'd3d12 highest SM'), "\n";
echo run('vulkan', [], 'vulkan'), "\n";
echo run('opengl', [], 'opengl'), "\n";
echo "DONE\n";
?>
--EXPECTF--
d3d12 FXC 5.1: OK
d3d12 SM 6.0: %r(OK|skip \(.*\))%r
d3d12 SM 6.5: %r(OK|skip \(.*\))%r
d3d12 highest SM: OK
vulkan: %r(OK|skip \(.*\)|FAIL\n  VIO_FEATURE_BASE_VERTEX is 0)%r
opengl: %r(OK|skip \(.*\)|FAIL\n  VIO_FEATURE_BASE_VERTEX is 0)%r
DONE
