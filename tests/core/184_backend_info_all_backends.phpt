--TEST--
vio_backend_info on every backend: API, device, shading language and its levels, vendor (PCI id and name), driver, device type and VRAM, families and capabilities; D3D12 / Vulkan / GL / D3D11 describe themselves like Metal does
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A4 (BACKEND-SELECTION-PLAN 0a). WARP / llvmpipe / lavapipe
 * report device_type 'software'; on hardware the vendor id is the PCI vendor. */
$vendors = [0x10DE => 'NVIDIA', 0x1002 => 'AMD', 0x8086 => 'Intel', 0x106B => 'Apple', 0x1414 => 'Microsoft',
            0x13B5 => 'ARM', 0x5143 => 'Qualcomm', 0x10005 => 'Mesa', 0 => 'unknown'];
$keys = ['backend', 'api', 'device', 'shading_language', 'shading_language_version', 'shading_language_max',
         'families', 'caps', 'vendor_id', 'vendor', 'driver', 'device_type', 'vram_bytes', 'selected_by', 'candidates'];

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    $ctx = @vio_create($b, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    $i = vio_backend_info($ctx);
    $err = [];
    if (!is_array($i)) $err[] = "no info: " . json_encode($i);
    else {
        if (array_keys($i) !== $keys) $err[] = "keys " . json_encode(array_keys($i));
        if ($i['backend'] !== $b) $err[] = "backend " . $i['backend'];
        foreach (['api', 'device', 'shading_language'] as $k) if (!is_string($i[$k]) || $i[$k] === '') $err[] = "$k empty";
        if (!is_int($i['shading_language_version']) || $i['shading_language_version'] <= 0) $err[] = "shading_language_version " . json_encode($i['shading_language_version']);
        if ($i['shading_language_max'] < $i['shading_language_version']) $err[] = "shading_language_max below the version in use";
        if (!array_key_exists($i['vendor_id'], $vendors)) $err[] = sprintf("vendor_id 0x%X not in the table", $i['vendor_id']);
        elseif ($i['vendor'] !== $vendors[$i['vendor_id']]) $err[] = "vendor {$i['vendor']} for " . sprintf('0x%X', $i['vendor_id']);
        if (!in_array($i['device_type'], ['discrete', 'integrated', 'software', 'unknown'], true)) $err[] = "device_type {$i['device_type']}";
        if (!is_string($i['driver'])) $err[] = "driver not a string";
        if (!is_int($i['vram_bytes']) || $i['vram_bytes'] < 0) $err[] = "vram_bytes " . json_encode($i['vram_bytes']);
        if (!is_array($i['caps']) || !$i['caps']) $err[] = "caps empty";
        else foreach ($i['caps'] as $n => $v) if (!is_string($n) || !is_bool($v)) { $err[] = "cap $n not bool"; break; }
        if ($i['device_type'] === 'software' && $i['vendor_id'] !== 0x1414 && $i['vendor_id'] !== 0x10005 && $i['vendor_id'] !== 0)
            $err[] = "software device with a hardware vendor";
    }
    echo "$b: ", $err ? "FAIL\n  " . implode("\n  ", $err) : "OK", "\n";
    vio_destroy($ctx);
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
