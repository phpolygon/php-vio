--TEST--
Compute storage buffers: data-seeded buffers are writable, rebinding a slot replaces the binding, vio_update_buffer honours its offset
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_COMPUTE)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with compute");
?>
--FILE--
<?php
/* Three regressions a GPU particle simulation ran into:
 * 1. A storage buffer created with 'data' could not be bound for writing on
 *    D3D12 (UPLOAD heap without the UAV flag — the write removed the device).
 * 2. vio_compute_bind_buffer appended to a per-pipeline binding list that was
 *    never cleared: rebinding a slot kept feeding the kernel old buffers, and
 *    once the list was full every further bind was silently dropped.
 * 3. vio_update_buffer ignored its offset on every backend and did nothing at
 *    all on OpenGL. */

/** The four floats of a buffer, or null when the read failed. */
function floats($ctx, $buf): ?array
{
    $bytes = @vio_storage_buffer_read($ctx, $buf);
    if (!is_string($bytes) || strlen($bytes) < 16) return null;
    return array_map(static fn ($f) => round($f, 3), array_values(unpack('f4', $bytes)));
}

function run_backend(string $name): string
{
    $ctx = @vio_create($name, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_COMPUTE)) {
        vio_destroy($ctx);
        return "skip (no compute)";
    }
    $fail = [];
    $cs = "#version 450\nlayout(local_size_x = 1) in;\n"
        . "layout(std430, binding = 0) buffer B { float v[]; } b;\n"
        . "void main(){ b.v[gl_GlobalInvocationID.x] += 1.0; }";
    $cp = vio_compute_pipeline($ctx, ['source' => $cs]);
    if (!$cp) {
        vio_destroy($ctx);
        return "FAIL\n  compute pipeline not created";
    }

    /* 1. A data-seeded buffer is a valid kernel output. */
    $seeded = vio_storage_buffer($ctx, ['data' => pack('f4', 1, 2, 3, 4), 'stride' => 4]);
    vio_compute_bind_buffer($ctx, $cp, $seeded, 0, VIO_COMPUTE_WRITE);
    vio_compute_dispatch($ctx, $cp, 4, 1, 1);
    $got = floats($ctx, $seeded);
    if ($got !== [2.0, 3.0, 4.0, 5.0]) $fail[] = "data-seeded buffer as output: " . json_encode($got);

    /* 2. Rebinding slot 0 on the same pipeline replaces the binding — more
     *    rebinds than any backend's binding table holds. */
    for ($i = 1; $i <= 12; $i++) {
        $b = vio_storage_buffer($ctx, ['data' => pack('f4', $i * 10, 0, 0, 0), 'stride' => 4]);
        vio_compute_bind_buffer($ctx, $cp, $b, 0, VIO_COMPUTE_WRITE);
        vio_compute_dispatch($ctx, $cp, 4, 1, 1);
        $got = floats($ctx, $b);
        if ($got === null || $got[0] !== (float) ($i * 10 + 1)) {
            $fail[] = "rebind #$i: the kernel did not write the newly bound buffer " . json_encode($got);
            break;
        }
    }

    /* 3. A partial update at an offset, then one more dispatch. */
    vio_update_buffer($seeded, pack('f', 40), 4);
    vio_compute_bind_buffer($ctx, $cp, $seeded, 0, VIO_COMPUTE_WRITE);
    vio_compute_dispatch($ctx, $cp, 4, 1, 1);
    $got = floats($ctx, $seeded);
    if ($got !== [3.0, 41.0, 5.0, 6.0]) $fail[] = "vio_update_buffer at offset 4: " . json_encode($got);

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
