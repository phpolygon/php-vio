--TEST--
Subgroup operations (VIO_FEATURE_SUBGROUP): GL_KHR_shader_subgroup compute shaders run where the flag is 1 - on D3D12 as wave intrinsics under Shader Model 6
--EXTENSIONS--
vio
--FILE--
<?php
/* Where VIO_FEATURE_SUBGROUP is 1, a compute shader with subgroupAdd /
 * subgroupBroadcastFirst / gl_SubgroupSize runs, and the results agree with
 * each other whatever the wave size is:
 *   - every thread sees count = subgroupAdd(1) and first = subgroupBroadcastFirst(id);
 *   - the threads sharing one `first` value are one subgroup: exactly `count`
 *     of them, all reporting the same count, and their first lane is `first`;
 *   - count <= gl_SubgroupSize, and at least one subgroup has more than one lane.
 * The fragment stage runs subgroup operations too (subgroupAllEqual on a
 * uniform, subgroupAdd within gl_SubgroupSize) and writes green where they hold.
 * Metal reports the flag wherever vio_backend_info() lists the simd_group
 * capability (Mac2 / Apple7 at MSL 2.2+); a mismatch fails.
 * VIO_REQUIRE_SUBGROUP=vulkan,opengl (Linux CI with lavapipe + llvmpipe) turns
 * an unavailable backend or a missing flag on the listed backends into a
 * failure, so a green run proves those paths executed.
 * D3D12 needs Shader Model 6 (DXC) for wave intrinsics, so the context asks for
 * it (other backends ignore the option). VIO_REQUIRE_SM6=1 (Windows CI) turns
 * a missing D3D12 flag into a failure. */
$N = 64;
$CS = "#version 450\n"
    . "#extension GL_KHR_shader_subgroup_basic : require\n"
    . "#extension GL_KHR_shader_subgroup_ballot : require\n"
    . "#extension GL_KHR_shader_subgroup_arithmetic : require\n"
    . "layout(local_size_x = $N) in;\n"
    . "layout(std430, binding = 0) buffer Out { uint v[]; } dst;\n"
    . "void main(){\n"
    . "  uint i = gl_GlobalInvocationID.x;\n"
    . "  dst.v[i] = subgroupAdd(1u);\n"
    . "  dst.v[$N + i] = subgroupBroadcastFirst(i);\n"
    . "  dst.v[2 * $N + i] = gl_SubgroupSize;\n"
    . "}\n";

$opts = ["width" => 8, "height" => 8, "headless" => true, "vsync" => false, "shader_model" => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;

$VS = "#version 450\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$FS = "#version 450\n"
    . "#extension GL_KHR_shader_subgroup_basic : require\n"
    . "#extension GL_KHR_shader_subgroup_vote : require\n"
    . "#extension GL_KHR_shader_subgroup_arithmetic : require\n"
    . "layout(location=0) out vec4 o;\nuniform uint u_k;\n"
    . "void main(){ uint c = subgroupAdd(1u);\n"
    . "  bool ok = subgroupAllEqual(u_k) && c >= 1u && c <= gl_SubgroupSize;\n"
    . "  o = ok ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0); }";

function run_backend(string $name, array $opts, string $cs, int $n): string {
    global $VS, $FS;
    $required = in_array($name, array_map('trim', explode(',', getenv('VIO_REQUIRE_SUBGROUP') ?: '')), true);
    $ctx = @vio_create($name, $opts);
    if (!$ctx) return $required ? "FAIL\n  VIO_REQUIRE_SUBGROUP lists $name but it is unavailable" : "skip (unavailable)";
    if (vio_backend_name($ctx) !== $name) {
        vio_destroy($ctx);
        return $required ? "FAIL\n  VIO_REQUIRE_SUBGROUP lists $name but it is unavailable" : "skip (unavailable)";
    }
    $info = vio_backend_info($ctx);
    $flag = vio_supports_feature($ctx, VIO_FEATURE_SUBGROUP);
    if ($name === 'metal' && is_array($info) && $flag !== $info['caps']['simd_group']) {
        vio_destroy($ctx);
        return "FAIL\n  metal: VIO_FEATURE_SUBGROUP " . json_encode($flag) . " but caps simd_group " . json_encode($info['caps']['simd_group']);
    }
    if (!$flag) {
        $sm = vio_swapchain_info($ctx)['shader_model'] ?? 0;
        vio_destroy($ctx);
        if ($name === 'd3d12' && getenv('VIO_REQUIRE_SM6')) return "FAIL\n  VIO_REQUIRE_SM6 set but no subgroups (shader_model $sm)";
        if ($required) return "FAIL\n  VIO_REQUIRE_SUBGROUP lists $name but VIO_FEATURE_SUBGROUP is 0";
        return "skip (no subgroups)";
    }
    $cp = vio_compute_pipeline($ctx, ['source' => $cs]);
    if (!$cp) { vio_destroy($ctx); return "FAIL\n  compute pipeline not created"; }
    $buf = vio_storage_buffer($ctx, ['size' => 3 * $n * 4]);
    vio_compute_bind_buffer($ctx, $cp, $buf, 0, VIO_COMPUTE_WRITE);
    vio_compute_dispatch($ctx, $cp, 1, 1, 1);
    $v = array_values(unpack('V*', vio_storage_buffer_read($ctx, $buf)));

    /* Fragment stage. */
    $fail = [];
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    if (!$sh) $fail[] = "fragment shader with subgroup ops not created";
    else {
        $pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
        $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniform($ctx, 'u_k', 7);
        vio_draw($ctx, $quad);
        vio_end($ctx);
        $px = vio_read_pixels($ctx);
        $W = (int)sqrt(strlen($px) / 4);
        foreach ([[1, 1], [$W >> 1, $W >> 1], [$W - 2, $W - 2]] as [$x, $y]) {
            $o = ($y * $W + $x) * 4;
            $c = [ord($px[$o]), ord($px[$o + 1]), ord($px[$o + 2])];
            if ($c[1] < 250 || $c[0] > 5) { $fail[] = "fragment ($x,$y) " . json_encode($c); break; }
        }
    }
    vio_destroy($ctx);

    $groups = [];
    for ($i = 0; $i < $n; $i++) $groups[$v[$n + $i]][] = $i;
    $multi = false;
    foreach ($groups as $first => $members) {
        $count = $v[$members[0]];
        if (count($members) !== $count) $fail[] = "subgroup at $first: " . count($members) . " threads, subgroupAdd says $count";
        if (min($members) !== $first) $fail[] = "subgroup at $first: first lane is " . min($members);
        foreach ($members as $m) {
            if ($v[$m] !== $count) { $fail[] = "thread $m: count {$v[$m]}, its subgroup $count"; break; }
            if ($count > $v[2 * $n + $m]) { $fail[] = "thread $m: count $count > gl_SubgroupSize {$v[2 * $n + $m]}"; break; }
        }
        if ($count > 1) $multi = true;
    }
    if (!$multi) $fail[] = "every subgroup has one lane";
    return $fail ? "FAIL\n  " . implode("\n  ", array_slice($fail, 0, 5)) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    echo "$b: ", run_backend($b, $opts, $CS, $N), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE
