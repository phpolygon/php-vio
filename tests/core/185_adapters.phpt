--TEST--
vio_adapters(): adapters per backend without a context (vendor, driver, device type, VRAM, device features); the adapter a context opens is among them; OpenGL reports its live context only
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A6 (BACKEND-SELECTION-PLAN 0b). */
$keys = ['index', 'name', 'vendor_id', 'vendor', 'device_id', 'driver', 'device_type', 'vram_bytes', 'features'];
$all = vio_adapters();
$err = [];
if (!is_array($all)) $err[] = "not an array";
foreach ($all as $b => $list) {
    if (!in_array($b, vio_backends(), true)) $err[] = "unknown backend key $b";
    foreach ($list as $i => $a) {
        if (array_keys($a) !== $keys) { $err[] = "$b#$i keys " . json_encode(array_keys($a)); continue; }
        if ($a['index'] !== $i) $err[] = "$b#$i index {$a['index']}";
        if (!is_string($a['name']) || $a['name'] === '') $err[] = "$b#$i name empty";
        if (!in_array($a['device_type'], ['discrete', 'integrated', 'software', 'unknown'], true)) $err[] = "$b#$i device_type {$a['device_type']}";
        if (!is_int($a['vram_bytes']) || $a['vram_bytes'] < 0) $err[] = "$b#$i vram";
        if (!in_array(VIO_FEATURE_COMPUTE, $a['features'], true)) $err[] = "$b#$i without compute";
        foreach ($a['features'] as $f) if (!is_int($f) || $f < 0 || $f > 63) { $err[] = "$b#$i feature $f"; break; }
    }
    /* Discrete adapters lead (DXGI high-performance order, Vulkan device types). */
    $types = array_column($list, 'device_type');
    $firstSw = array_search('software', $types, true);
    if ($firstSw !== false) foreach (array_slice($types, $firstSw) as $t) if ($t === 'discrete') { $err[] = "$b: discrete after software"; break; }
}
if (PHP_OS_FAMILY === 'Windows' && isset($all['d3d12']) && !$all['d3d12']) $err[] = "d3d12 lists no adapter";
if (isset($all['opengl']) && $all['opengl']) $err[] = "opengl lists adapters without a context";
echo $err ? "FAIL\n  " . implode("\n  ", $err) : "OK", "\n";

/* The adapter a context opens is listed for its backend. */
foreach (['d3d11', 'd3d12', 'vulkan', 'metal', 'opengl'] as $b) {
    $ctx = @vio_create($b, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    $info = vio_backend_info($ctx);
    $list = vio_adapters($b)[$b] ?? [];
    $hit = array_filter($list, fn($a) => $a['vendor_id'] === $info['vendor_id'] && $a['name'] === $info['device']);
    echo "$b: ", $hit ? "OK" : "FAIL (" . $info['device'] . " not in " . json_encode(array_column($list, 'name')) . ")", "\n";
    vio_destroy($ctx);
}

try { vio_adapters('nope'); echo "no error\n"; }
catch (ValueError $e) { echo "ValueError\n"; }
echo "DONE\n";
?>
--EXPECTF--
OK
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
opengl: %r(OK|skip \(.*\))%r
ValueError
DONE
