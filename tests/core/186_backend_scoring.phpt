--TEST--
'auto' with prefer / require: vendor profile, device type and feature points rank the candidates (simulated adapters via VIO_TEST_ADAPTERS), require filters, vio_backend_info reports selected_by and the candidates
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A7 (BACKEND-SELECTION-PLAN Phase 1). */
function sim(string $platform, array $adapters): void {
    putenv('VIO_TEST_ADAPTERS=' . json_encode(['platform' => $platform, 'adapters' => $adapters]));
}
function order(array $opts = []): string {
    $r = vio_rank_backends($opts);
    return implode(' > ', array_map(fn($c) => $c['backend'] . ($c['eligible'] ? '' : ' (x)'), $r));
}
function gpu(int $vendor, string $type, array $features, string $name = 'GPU'): array {
    return ['name' => $name, 'vendor_id' => $vendor, 'device_type' => $type, 'vram_bytes' => 0, 'features' => $features];
}
$base = [VIO_FEATURE_COMPUTE, VIO_FEATURE_3D_PIPELINE, VIO_FEATURE_INDIRECT_DRAW];
$rich = array_merge($base, [VIO_FEATURE_SUBGROUP, VIO_FEATURE_BINDLESS, VIO_FEATURE_MESH_SHADER, VIO_FEATURE_RAY_QUERY]);
$nv = gpu(0x10DE, 'discrete', $rich);
$nvNoRq = gpu(0x10DE, 'discrete', array_diff($rich, [VIO_FEATURE_RAY_QUERY]));
$amd = gpu(0x1002, 'discrete', $rich);
$intelOld = gpu(0x8086, 'integrated', $base);
$warp = gpu(0x1414, 'software', $base);
$lavapipe = gpu(0x10005, 'software', $base);

sim('windows', ['d3d12' => [$nv], 'vulkan' => [$nv], 'd3d11' => [$nv], 'opengl' => []]);
echo "nvidia:      ", order(['prefer' => 'performance']), "\n";
echo "compat:      ", order(['prefer' => 'compat']), "\n";
sim('windows', ['d3d12' => [$amd], 'vulkan' => [$amd], 'd3d11' => [$amd], 'opengl' => []]);
echo "amd:         ", order(['prefer' => 'quality']), "\n";
sim('windows', ['d3d12' => [$intelOld], 'vulkan' => [$intelOld], 'd3d11' => [$intelOld], 'opengl' => []]);
echo "intel old:   ", order(['prefer' => 'performance']), "\n";
sim('windows', ['d3d12' => [$nvNoRq], 'vulkan' => [$nv], 'd3d11' => [$nvNoRq]]);
echo "require rq:  ", order(['require' => [VIO_FEATURE_RAY_QUERY]]), "\n";
sim('windows', ['d3d12' => [$warp], 'd3d11' => [$warp], 'vulkan' => [$nv]]);
echo "warp last:   ", order(['prefer' => 'performance']), "\n";
sim('linux', ['vulkan' => [$lavapipe], 'opengl' => []]);
echo "lavapipe:    ", order(['prefer' => 'performance']), "\n";
sim('macos', ['metal' => [gpu(0x106B, 'integrated', $rich)], 'opengl' => []]);
echo "apple:       ", order(['prefer' => 'quality']), "\n";
/* flags past 63 (the feature set has two words): require and adapter lists carry them */
sim('macos', ['metal' => [gpu(0x106B, 'integrated', array_merge($base, [VIO_FEATURE_RASTER_RATE_MAP]))], 'opengl' => [gpu(0x106B, 'integrated', $base)]]);
echo "require 64+: ", order(['require' => [VIO_FEATURE_RASTER_RATE_MAP]]), "\n";
$r = vio_rank_backends(['require' => [VIO_FEATURE_RAY_QUERY]]);
echo "keys:        ", implode(',', array_keys($r[0])), "\n";

/* require nobody meets: vio_create fails with a warning. */
sim('windows', ['d3d12' => [$nvNoRq]]);
var_dump(@vio_create('auto', ['require' => [VIO_FEATURE_RAY_QUERY], 'headless' => true]));
echo error_get_last()['message'] ?? '', "\n";
putenv('VIO_TEST_ADAPTERS');

/* Real machine: selected_by and the candidates. */
$c = vio_create('auto', ['width' => 16, 'height' => 16, 'headless' => true, 'prefer' => 'performance']);
if (!$c) echo "score: skip (no backend)\n";
else {
    $i = vio_backend_info($c);
    $first = array_values(array_filter($i['candidates'], fn($x) => $x['eligible']))[0]['backend'] ?? null;
    echo "score: ", $i['selected_by'], ' ', $i['candidates'] && in_array($i['backend'], array_column($i['candidates'], 'backend'), true) ? 'listed' : 'missing', "\n";
    vio_destroy($c);
}
$c = vio_create('auto', ['width' => 16, 'height' => 16, 'headless' => true]);
if ($c) { $i = vio_backend_info($c); echo "plain: ", $i['selected_by'], ' ', count($i['candidates']), "\n"; vio_destroy($c); }
var_dump(@vio_create('auto', ['prefer' => 'fastest']));
echo error_get_last()['message'] ?? '', "\n";
echo "DONE\n";
?>
--EXPECTF--
nvidia:      d3d12 > vulkan > d3d11 > opengl
compat:      d3d11 > opengl > d3d12 > vulkan
amd:         vulkan > d3d12 > d3d11 > opengl
intel old:   d3d11 > d3d12 > vulkan > opengl
require rq:  vulkan > d3d12 (x) > d3d11 (x)
warp last:   vulkan > d3d12 > d3d11
lavapipe:    opengl > vulkan
apple:       metal > opengl
require 64+: metal > opengl (x)
keys:        backend,adapter,vendor,device_type,score,eligible,reason,benchmark_ms
bool(false)
%Sno backend provides the required features%S
score: score listed
plain: priority 0
bool(false)
%S'prefer' must be one of performance, quality, compat%S
DONE
