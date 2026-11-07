--TEST--
vio_create 'adapter' option: a position in vio_adapters()[backend] or part of an adapter's name opens exactly that adapter (D3D11, D3D12, Vulkan); an unknown choice fails
--EXTENSIONS--
vio
--FILE--
<?php
foreach (['d3d11', 'd3d12', 'vulkan'] as $b) {
    $list = vio_adapters($b)[$b] ?? [];
    if (!$list || !in_array($b, vio_backends(), true)) { echo "$b: skip (unavailable)\n"; continue; }
    $err = [];
    $opened = 0;
    foreach ($list as $i => $a) {
        foreach (['index' => $i, 'name' => strtoupper($a['name'])] as $how => $choice) {
            $ctx = @vio_create($b, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false, 'adapter' => $choice]);
            if (!$ctx) {
                /* Vulkan needs 1.3 (or 1.2 + extensions) and a present queue; an old adapter may not qualify. */
                if ($b !== 'vulkan') $err[] = "#$i by $how: no context";
                continue;
            }
            $dev = vio_backend_info($ctx)['device'];
            if ($dev !== $a['name']) $err[] = "#$i by $how opened '$dev' instead of '{$a['name']}'";
            else $opened++;
            vio_destroy($ctx);
        }
    }
    $bad = @vio_create($b, ['width' => 16, 'height' => 16, 'headless' => true, 'adapter' => 'no such adapter 123']);
    if ($bad) { $err[] = "an unknown name opened " . vio_backend_info($bad)['device']; vio_destroy($bad); }
    $bad = @vio_create($b, ['width' => 16, 'height' => 16, 'headless' => true, 'adapter' => count($list)]);
    if ($bad) { $err[] = "an index past the list opened a context"; vio_destroy($bad); }
    if ($opened === 0) $err[] = "no adapter opened";
    echo "$b: ", $err ? "FAIL\n  " . implode("\n  ", $err) : "OK", "\n";
}
echo "DONE\n";
?>
--EXPECTF--
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
DONE
