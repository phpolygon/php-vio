--TEST--
Backend capability matrix: VIO_FEATURE_* returns the documented values per backend
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
?>
--FILE--
<?php
/* CLAUDE.md "OpenGL-Feature-Ladder" + each backend's supports_feature impl
 * documents what should be 1 / 0. This test pins that contract:
 *
 *   - opengl  : full set on 3.3+, plus ARB-fallback caps where applicable
 *   - null    : 0 for everything (it's a no-op backend)
 *
 * Other backends are skipped where unavailable. The intent isn't full
 * cross-backend parity — that's 067 — but a regression gate on what
 * supports_feature reports.
 *
 * vio_supports_feature($ctx, $flag) reads ctx->backend->supports_feature. */

function probe(string $backend_name, array $expected, int $width = 16): void {
    $ctx = vio_create($backend_name, ["width" => $width, "height" => $width, "headless" => true]);
    if (!$ctx) {
        echo "$backend_name: skip (unavailable)\n";
        return;
    }
    foreach ($expected as $flag => $want) {
        $got = vio_supports_feature($ctx, $flag);
        if ((bool)$got !== (bool)$want) {
            echo "$backend_name: $flag expected " . ($want ? "1" : "0") .
                 " got " . ($got ? "1" : "0") . "\n";
        }
    }
    vio_destroy($ctx);
    echo "$backend_name: OK\n";
}

/* OpenGL: guaranteed at the 3.3 floor (per CLAUDE.md table) */
probe("opengl", [
    VIO_FEATURE_3D_PIPELINE        => 1,
    VIO_FEATURE_GEOMETRY           => 1,
    VIO_FEATURE_READ_PIXELS        => 1,
    VIO_FEATURE_INSTANCED_DRAW     => 1,
    VIO_FEATURE_RENDER_TARGET      => 1,
    VIO_FEATURE_RENDER_TARGET_HDR  => 1,
    VIO_FEATURE_RENDER_TARGET_DEPTH=> 1,
    VIO_FEATURE_RENDER_TARGET_MSAA => 1,
    VIO_FEATURE_CUBEMAP            => 1,
    VIO_FEATURE_DEPTH_BIAS         => 1,
    VIO_FEATURE_SCISSOR            => 1,
    VIO_FEATURE_TEXTURE_SWIZZLE    => 1,
    VIO_FEATURE_NATIVE_2D_BATCH    => 1,
    VIO_FEATURE_RAYTRACING         => 0,
    VIO_FEATURE_MULTIVIEW          => 0,
]);

/* Null: always 0 — it's the no-op test backend */
probe("null", [
    VIO_FEATURE_3D_PIPELINE        => 0,
    VIO_FEATURE_COMPUTE            => 0,
    VIO_FEATURE_TESSELLATION       => 0,
    VIO_FEATURE_GEOMETRY           => 0,
    VIO_FEATURE_RENDER_TARGET      => 0,
    VIO_FEATURE_READ_PIXELS        => 0,
    VIO_FEATURE_NATIVE_2D_BATCH    => 0,
]);

/* Metal (macOS) — 3D stub, but RT + 2D-batch + swizzle implemented */
$mtl = @vio_create("metal", ["width" => 16, "height" => 16, "headless" => true]);
if ($mtl) {
    var_dump(vio_supports_feature($mtl, VIO_FEATURE_3D_PIPELINE) === false);
    var_dump(vio_supports_feature($mtl, VIO_FEATURE_NATIVE_2D_BATCH) === true);
    var_dump(vio_supports_feature($mtl, VIO_FEATURE_RENDER_TARGET) === true);
    var_dump(vio_supports_feature($mtl, VIO_FEATURE_TEXTURE_SWIZZLE) === true);
    vio_destroy($mtl);
    echo "metal: OK\n";
} else {
    echo "metal: skip (unavailable)\n";
}

echo "DONE\n";
?>
--EXPECT--
opengl: OK
null: OK
bool(true)
bool(true)
bool(true)
bool(true)
metal: OK
DONE
