--TEST--
Indirect draws from a storage buffer (vio_draw_indirect, VIO_FEATURE_INDIRECT_DRAW), including arguments written by a compute pass
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_INDIRECT_DRAW) && vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with indirect draws");
?>
--FILE--
<?php
/* GAP-PHASE5-PLAN Block 8. Argument records {indexCount, instanceCount,
 * firstIndex, baseVertex, firstInstance}: record 0 draws the quad once, record
 * 1 has instanceCount 0 and must draw nothing. Then a compute pass zeroes the
 * first record's instanceCount — the next indirect draw paints nothing, with no
 * CPU readback in between. Unindexed meshes use the 4-uint32 record. */
$W = 16;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function green(array $c): bool { return $c[1] > 250 && $c[0] < 5 && $c[2] < 5; }
function black(array $c): bool { return $c[0] + $c[1] + $c[2] < 10; }

function run_backend(string $name): string {
    global $W;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_INDIRECT_DRAW) || !vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) {
        vio_destroy($ctx);
        return "skip (no indirect draws)";
    }
    $fail = [];
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $fs = "#version 330 core\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0.0, 1.0, 0.0, 1.0); }";
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $verts = [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0];
    $quad  = vio_mesh($ctx, ['vertices' => $verts, 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $plain = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,-1,0, 1,1,0, -1,1,0], 'layout' => [VIO_FLOAT3]]);

    $draw = static function ($mesh, $args, int $count) use ($ctx, $pipe, $W): array {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_draw_indirect($ctx, $mesh, $args, $count);
        vio_end($ctx);
        return px(vio_read_pixels($ctx), $W >> 1, $W >> 1, $W);
    };

    /* Indexed: [draw once][draw nothing]. */
    $args = vio_storage_buffer($ctx, ['data' => pack('V*', 6, 1, 0, 0, 0,  6, 0, 0, 0, 0), 'indirect' => true]);
    if (!$args) { vio_destroy($ctx); return "FAIL\n  indirect argument buffer not created"; }
    if (!green($c = $draw($quad, $args, 2))) $fail[] = "indexed indirect draw not green " . json_encode($c);
    /* Only the second record (instanceCount 0) -> nothing. */
    if (!black($c = $draw($quad, vio_storage_buffer($ctx, ['data' => pack('V*', 6, 0, 0, 0, 0), 'indirect' => true]), 1))) $fail[] = "instanceCount 0 drew something " . json_encode($c);
    /* Unindexed record. */
    $pargs = vio_storage_buffer($ctx, ['data' => pack('V*', 6, 1, 0, 0), 'indirect' => true]);
    if (!green($c = $draw($plain, $pargs, 1))) $fail[] = "unindexed indirect draw not green " . json_encode($c);

    /* Compute writes the arguments: zero instanceCount of record 0, then draw. */
    if (vio_supports_feature($ctx, VIO_FEATURE_COMPUTE)) {
        $cs = "#version 450\nlayout(local_size_x = 1) in;\nlayout(std430, binding = 0) buffer Args { uint v[]; } args;\nvoid main(){ args.v[1] = 0u; }";
        $cp = vio_compute_pipeline($ctx, ['source' => $cs]);
        $gpuArgs = vio_storage_buffer($ctx, ['size' => 20, 'indirect' => true]);
        /* Seed it through a compute pass too (a 'size' buffer starts zeroed): count 6, instances 1. */
        $seed = vio_compute_pipeline($ctx, ['source' => "#version 450\nlayout(local_size_x = 1) in;\nlayout(std430, binding = 0) buffer Args { uint v[]; } args;\nvoid main(){ args.v[0] = 6u; args.v[1] = 1u; args.v[2] = 0u; args.v[3] = 0u; args.v[4] = 0u; }"]);
        vio_compute_bind_buffer($ctx, $seed, $gpuArgs, 0, VIO_COMPUTE_WRITE);
        vio_compute_dispatch($ctx, $seed, 1, 1, 1);
        if (!green($c = $draw($quad, $gpuArgs, 1))) $fail[] = "compute-seeded indirect draw not green " . json_encode($c);
        vio_compute_bind_buffer($ctx, $cp, $gpuArgs, 0, VIO_COMPUTE_WRITE);
        vio_compute_dispatch($ctx, $cp, 1, 1, 1);
        if (!black($c = $draw($quad, $gpuArgs, 1))) $fail[] = "compute-culled indirect draw still drew " . json_encode($c);
        /* Async dispatch inside the frame, right before the indirect draw (the
         * D3D12 UAV -> INDIRECT_ARGUMENT barrier path): re-arm to 1 instance. */
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_compute_bind_buffer($ctx, $seed, $gpuArgs, 0, VIO_COMPUTE_WRITE);
        vio_compute_dispatch($ctx, $seed, 1, 1, 1, ['async' => true]);
        vio_bind_pipeline($ctx, $pipe);
        vio_draw_indirect($ctx, $quad, $gpuArgs, 1);
        vio_end($ctx);
        if (!green($c = px(vio_read_pixels($ctx), $W >> 1, $W >> 1, $W))) $fail[] = "in-frame async compute + indirect draw not green " . json_encode($c);
    }
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'metal', 'vulkan'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %s
d3d11: %s
d3d12: %s
metal: %s
vulkan: %s
DONE
