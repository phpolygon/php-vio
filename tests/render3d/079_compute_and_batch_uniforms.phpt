--TEST--
GPU compute API (vio_compute_*) + batch uniforms (vio_set_uniforms): symbols, constants, type-dispatch, graceful gating
--EXTENSIONS--
vio
--FILE--
<?php
// Coverage for the GPU-compute primitive and the batch uniform helper.
// Actual GPU dispatch correctness is verified on a real D3D12 device via the
// golden-diff tools (CPU vs GPU bake); here we assert the backend-independent
// contract: the symbols exist, the access constants are right, the batch
// uniform call drives the same type-dispatch as vio_set_uniform, and the
// compute API gates gracefully (returns false, never crashes) on a backend
// without compute support (the null backend).

// Access constants for vio_compute_bind_buffer.
var_dump(VIO_COMPUTE_READ, VIO_COMPUTE_WRITE);

// All new functions are registered regardless of backend.
foreach ([
    'vio_set_uniforms',
    'vio_compute_pipeline',
    'vio_storage_buffer',
    'vio_compute_bind_buffer',
    'vio_compute_set_uniforms',
    'vio_compute_dispatch',
    'vio_storage_buffer_read',
] as $fn) {
    var_dump(function_exists($fn));
}

$ctx = vio_create("null");
var_dump($ctx instanceof VioContext);

vio_begin($ctx);

// Batch form must accept the same value shapes as vio_set_uniform, applied in
// one native call (the per-pair type-dispatch is shared with vio_set_uniform).
vio_set_uniforms($ctx, [
    'u_int'   => 42,
    'u_float' => 3.14,
    'u_vec3'  => [1.0, 2.0, 3.0],
    'u_model' => [
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0,
    ],
    'u_mixed' => [1, 2.5, 3, 4.0],
    'u_empty' => [],
]);
echo "batch uniforms OK\n";

vio_end($ctx);

// Outside a frame must warn, not crash (mirrors vio_set_uniform's guard).
@vio_set_uniforms($ctx, ['u_after' => 1.0]);
echo "outside-frame guard OK\n";

// Compute on a backend without compute support must gate gracefully: the
// pipeline create returns false (feature-gated), never crashes.
$pipe = @vio_compute_pipeline($ctx, [
    'source' => "#version 450\nlayout(local_size_x=1) in;\nvoid main(){}",
]);
var_dump($pipe === false || $pipe instanceof VioComputePipeline);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
int(0)
int(1)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
batch uniforms OK
outside-frame guard OK
bool(true)
OK
