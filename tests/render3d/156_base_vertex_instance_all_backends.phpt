--TEST--
Draw parameters (VIO_FEATURE_BASE_VERTEX): gl_BaseVertex / gl_BaseInstance carry an indirect draw's baseVertex / firstInstance (0 for vio_draw) on every backend that reports the flag
--EXTENSIONS--
vio
--FILE--
<?php
/* An indexed indirect draw with baseVertex 4 and firstInstance 3. Vertices 0..3
 * of the mesh collapse to one point, vertices 4..7 cover the target - if the
 * base vertex were ignored nothing would be drawn. The vertex stage passes
 * gl_BaseVertex / gl_BaseInstance on, the fragment stage writes green when they
 * are (4, 3). A plain vio_draw of a normal quad must see (0, 0).
 * D3D12 needs SM 6.8 (SV_StartVertexLocation / SV_StartInstanceLocation);
 * below that SPIRV-Cross would need a cbuffer vio cannot fill for indirect draws.
 * VIO_REQUIRE_BASE_VERTEX=vulkan makes the listed backends mandatory. */
$VS = "#version 460\nlayout(location=0) in vec3 aPos;\nlayout(location=0) flat out ivec2 bp;\n"
     . "void main(){ bp = ivec2(gl_BaseVertex, gl_BaseInstance); gl_Position = vec4(aPos, 1.0); }";
$FS = "#version 460\nlayout(location=0) flat in ivec2 bp;\nlayout(location=0) out vec4 o;\nuniform ivec2 u_want;\n"
     . "void main(){ o = bp == u_want ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0); }";
$opts = ["width" => 16, "height" => 16, "headless" => true, "vsync" => false, "shader_model" => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
function open_backend(string $name, int $flag, string $env, ?string &$skip): mixed {
    global $opts;
    $req = in_array($name, array_map('trim', explode(',', getenv($env) ?: '')), true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { $skip = $req ? "FAIL\n  required but unavailable" : "skip (unavailable)"; return null; }
    if (!vio_supports_feature($ctx, $flag)) { vio_destroy($ctx); $skip = $req ? "FAIL\n  required but the flag is 0" : "skip (not supported)"; return null; }
    return $ctx;
}

function green(string $p, int $w): bool {
    foreach ([[1, 1], [$w >> 1, $w >> 1], [$w - 2, $w - 2]] as [$x, $y]) {
        $o = ($y * $w + $x) * 4;
        if (ord($p[$o + 1]) < 250 || ord($p[$o]) > 5) return false;
    }
    return true;
}

function run_backend(string $name): string {
    global $VS, $FS;
    $ctx = open_backend($name, VIO_FEATURE_BASE_VERTEX, 'VIO_REQUIRE_BASE_VERTEX', $skip);
    if (!$ctx) return $skip;
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    if (!$sh) { vio_destroy($ctx); return "FAIL\n  shader not created"; }
    $pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $quad = [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0];
    $idx = [0,1,2, 0,2,3];
    $mesh8 = vio_mesh($ctx, ['vertices' => array_merge(array_fill(0, 12, 0.0), $quad), 'indices' => $idx, 'layout' => [VIO_FLOAT3]]);
    $mesh4 = vio_mesh($ctx, ['vertices' => $quad, 'indices' => $idx, 'layout' => [VIO_FLOAT3]]);
    /* indexCount, instanceCount, firstIndex, baseVertex, firstInstance */
    $args = vio_storage_buffer($ctx, ['data' => pack('V*', 6, 1, 0, 4, 3), 'indirect' => true]);
    $fail = [];
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pipe);
    vio_set_uniform($ctx, 'u_want', [4, 3]);
    vio_draw_indirect($ctx, $mesh8, $args, 1, 0);
    vio_end($ctx);
    if (!green(vio_read_pixels($ctx), 16)) $fail[] = "indirect draw: gl_BaseVertex / gl_BaseInstance are not (4, 3) (or the base vertex was ignored)";
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pipe);
    vio_set_uniform($ctx, 'u_want', [0, 0]);
    vio_draw($ctx, $mesh4);
    vio_end($ctx);
    if (!green(vio_read_pixels($ctx), 16)) $fail[] = "vio_draw: gl_BaseVertex / gl_BaseInstance are not (0, 0)";
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
