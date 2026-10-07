--TEST--
Metal version ladder: vio_backend_info() reports the MSL version, GPU families and capabilities; 'msl_version' / VIO_METAL_MSL_VERSION pin any rung and the version-gated capabilities follow it
--EXTENSIONS--
vio
--SKIPIF--
<?php
$c = @vio_create('metal', ["width" => 8, "height" => 8, "headless" => true]);
if (!$c) die("skip metal unavailable");
vio_destroy($c);
?>
--FILE--
<?php
/* Like the OpenGL context ladder (4.6 -> 3.0), the Metal backend walks the
 * Metal Shading Language versions from the newest the SDK knows down to the
 * floor (MSL 2.0) and takes the first one the OS compiles. Every shader of the
 * context (graphics, compute, tessellation, emulated geometry, 2D) is
 * transpiled and compiled for that version; capabilities are the device
 * support AND the minimum MSL version, so pinning a lower rung switches the
 * version-gated ones off. Versions are major * 10 + minor (21 = MSL 2.1). */
const LADDER = [20, 21, 22, 23, 24, 30, 31, 32, 40, 41];
/* Minimum MSL version of each version-gated capability (0 = API / device only). */
const CAP_MIN = [
    'tessellation' => 21, 'layered_vertex' => 0, 'quad_group' => 21, 'simd_group' => 22,
    'barycentrics' => 22, 'vertex_amplification' => 22, 'argument_buffers_tier2' => 0,
    'raytracing' => 23, 'function_pointers' => 23, 'raytracing_from_render' => 24,
    'mesh_shaders' => 30, 'atomic64' => 31, 'tensors' => 40, 'bindless' => 30,
    'rasterization_rate_map' => 0, 'bc_texture_compression' => 0, 'unified_memory' => 0,
];
$fail = [];
$opts = ["width" => 16, "height" => 16, "headless" => true];

/* Auto: the highest rung the OS accepts. */
putenv('VIO_METAL_MSL_VERSION');
$ctx = vio_create('metal', $opts);
$info = vio_backend_info($ctx);
vio_destroy($ctx);
if (!is_array($info)) { echo "FAIL: vio_backend_info is not an array\n"; exit; }
foreach (['backend', 'api', 'device', 'shading_language', 'shading_language_version', 'shading_language_max', 'families', 'caps'] as $k)
    if (!array_key_exists($k, $info)) $fail[] = "missing key $k";
$max = $info['shading_language_max'] ?? 0;
if (($info['backend'] ?? '') !== 'metal') $fail[] = "backend " . json_encode($info['backend'] ?? null);
if (($info['shading_language'] ?? '') !== 'MSL') $fail[] = "shading_language " . json_encode($info['shading_language'] ?? null);
if (!in_array($max, LADDER, true)) $fail[] = "shading_language_max $max is no ladder rung";
if (($info['shading_language_version'] ?? 0) !== $max) $fail[] = "auto picked {$info['shading_language_version']}, max is $max";
if (!preg_match('/^Metal [1-9]$/', $info['api'] ?? '')) $fail[] = "api " . json_encode($info['api'] ?? null);
$fam = $info['families'] ?? [];
if (!is_array($fam) || !$fam) $fail[] = "no families";
if (in_array('metal4', $fam, true) && ($info['api'] ?? '') !== 'Metal 4') $fail[] = "metal4 family but api {$info['api']}";
if (in_array('metal3', $fam, true) && !in_array($info['api'] ?? '', ['Metal 3', 'Metal 4'], true)) $fail[] = "metal3 family but api {$info['api']}";
$caps = $info['caps'] ?? [];
$diff = array_diff(array_keys(CAP_MIN), array_keys($caps));
if ($diff) $fail[] = "missing caps " . implode(',', $diff);
foreach ($caps as $k => $v) if (!is_bool($v)) $fail[] = "cap $k is not bool";
$top = $caps;

/* Every rung up to the maximum can be pinned; the gated caps follow it. */
foreach (LADDER as $r) {
    if ($r > $max) break;
    $ctx = vio_create('metal', $opts + ['msl_version' => $r]);
    if (!$ctx) { $fail[] = "msl_version $r: create failed"; continue; }
    $i = vio_backend_info($ctx);
    if ($i['shading_language_version'] !== $r) $fail[] = "msl_version $r: got {$i['shading_language_version']}";
    if ($i['shading_language_max'] !== $max) $fail[] = "msl_version $r: max changed to {$i['shading_language_max']}";
    foreach (CAP_MIN as $k => $min) {
        $want = $top[$k] && $r >= $min;   /* device support is fixed, the version gate is not */
        if (($i['caps'][$k] ?? null) !== $want) $fail[] = "msl_version $r: cap $k " . json_encode($i['caps'][$k] ?? null) . ", want " . json_encode($want);
    }
    if (vio_supports_feature($ctx, VIO_FEATURE_TESSELLATION) !== $i['caps']['tessellation'])
        $fail[] = "msl_version $r: VIO_FEATURE_TESSELLATION disagrees with caps";
    vio_destroy($ctx);
}

/* Out-of-range requests clamp: below the floor -> 2.0, above the OS -> max,
 * between rungs -> the rung below (2.5 is no MSL version). */
foreach ([[10, 20], [99, $max], [25, 24]] as [$req, $want]) {
    $ctx = vio_create('metal', $opts + ['msl_version' => $req]);
    $got = vio_backend_info($ctx)['shading_language_version'];
    vio_destroy($ctx);
    if ($got !== min($want, $max)) $fail[] = "msl_version $req: got $got, want " . min($want, $max);
}

/* VIO_METAL_MSL_VERSION pins the rung when the option is absent (runs the
 * whole suite on an older rung); the option wins over it. */
putenv('VIO_METAL_MSL_VERSION=21');
$ctx = vio_create('metal', $opts);
if (vio_backend_info($ctx)['shading_language_version'] !== 21) $fail[] = "VIO_METAL_MSL_VERSION=21 ignored";
vio_destroy($ctx);
$ctx = vio_create('metal', $opts + ['msl_version' => 20]);
if (vio_backend_info($ctx)['shading_language_version'] !== 20) $fail[] = "option does not win over VIO_METAL_MSL_VERSION";
vio_destroy($ctx);
putenv('VIO_METAL_MSL_VERSION');

/* Backends without a describe slot report false. */
$null = vio_create('null', $opts);
if (vio_backend_info($null) !== false) $fail[] = "null backend: not false";
vio_destroy($null);

echo $fail ? "FAIL\n  " . implode("\n  ", $fail) . "\n" : "OK\n";
?>
--EXPECT--
OK
