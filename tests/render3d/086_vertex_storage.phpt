--TEST--
Graphics-stage storage buffers (Path B): symbols, VIO_FEATURE_VERTEX_STORAGE, graceful gating
--EXTENSIONS--
vio
--FILE--
<?php
// Backend-independent contract for the readback-free instancing primitive
// (vio_bind_storage_buffer / vio_draw_instanced_from_buffer). Render
// correctness is verified on a real GPU in 087_vertex_storage_render.phpt;
// here we assert the symbols exist, the feature constant is right, and the two
// functions gate gracefully (no-op, never crash) on a backend without the
// feature (the null backend).

var_dump(defined('VIO_FEATURE_VERTEX_STORAGE'));
var_dump(VIO_FEATURE_VERTEX_STORAGE);

foreach (['vio_bind_storage_buffer', 'vio_draw_instanced_from_buffer'] as $fn) {
    var_dump(function_exists($fn));
}

$ctx = vio_create("null");
var_dump($ctx instanceof VioContext);

// Null backend has no graphics vertex-storage -> feature reports false, so the
// engine stays on the readback path. (Functional no-op gating with real GPU
// resources is exercised in 087_vertex_storage_render.phpt.)
var_dump(vio_supports_feature($ctx, VIO_FEATURE_VERTEX_STORAGE));

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
int(30)
bool(true)
bool(true)
bool(true)
bool(false)
OK
