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

function run_backend(string $name, array $opts, string $cs, int $n): string {
    $ctx = @vio_create($name, $opts);
    if (!$ctx) return "skip (unavailable)";
    if (vio_backend_name($ctx) !== $name) { vio_destroy($ctx); return "skip (unavailable)"; }
    if (!vio_supports_feature($ctx, VIO_FEATURE_SUBGROUP)) {
        $sm = vio_swapchain_info($ctx)['shader_model'] ?? 0;
        vio_destroy($ctx);
        if ($name === 'd3d12' && getenv('VIO_REQUIRE_SM6')) return "FAIL\n  VIO_REQUIRE_SM6 set but no subgroups (shader_model $sm)";
        return "skip (no subgroups)";
    }
    $cp = vio_compute_pipeline($ctx, ['source' => $cs]);
    if (!$cp) { vio_destroy($ctx); return "FAIL\n  compute pipeline not created"; }
    $buf = vio_storage_buffer($ctx, ['size' => 3 * $n * 4]);
    vio_compute_bind_buffer($ctx, $cp, $buf, 0, VIO_COMPUTE_WRITE);
    vio_compute_dispatch($ctx, $cp, 1, 1, 1);
    $v = array_values(unpack('V*', vio_storage_buffer_read($ctx, $buf)));
    vio_destroy($ctx);

    $fail = [];
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
