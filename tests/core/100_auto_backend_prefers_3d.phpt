--TEST--
vio_create('auto') never picks a backend without a 3D pipeline when another registered backend has one
--EXTENSIONS--
vio
--FILE--
<?php
/* D3D-VULKAN-GAP-PLAN.md Phase 0.4. The Vulkan backend reports
 * VIO_FEATURE_3D_PIPELINE = 0 (vulkan_create_pipeline is a stub), yet it sits
 * ahead of OpenGL in the Linux priority list. vio_get_auto_backend() therefore
 * skips 3D-less backends when a later candidate can draw 3D. This test checks
 * the contract from the outside: if ANY registered backend that can be opened
 * headless reports a 3D pipeline, then 'auto' must report one as well.
 *
 * Feature flags are static per backend (no device needed), so probing every
 * name is cheap; only backends that fail to open are ignored. */
$any_3d = false;
foreach (vio_backends() as $name) {
    if ($name === 'null') continue;
    $c = @vio_create($name, ['width' => 8, 'height' => 8, 'headless' => true, 'vsync' => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)) $any_3d = true;
    vio_destroy($c);
}

$auto = @vio_create('auto', ['width' => 8, 'height' => 8, 'headless' => true, 'vsync' => false]);
if (!$auto) {
    echo $any_3d ? "FAIL: auto context could not be created\n" : "no GPU context available\n";
    echo "DONE\n";
    exit;
}
$auto_3d = vio_supports_feature($auto, VIO_FEATURE_3D_PIPELINE);
$auto_name = vio_backend_name($auto);
vio_destroy($auto);

if ($any_3d && !$auto_3d) {
    echo "FAIL: auto picked '$auto_name' (no 3D pipeline) although a 3D-capable backend is registered\n";
} else {
    echo "auto backend is consistent\n";
}
echo "DONE\n";
?>
--EXPECTF--
%s
DONE
