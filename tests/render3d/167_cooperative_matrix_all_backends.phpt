--TEST--
Cooperative matrices (VIO_FEATURE_COOPERATIVE_MATRIX): a compute kernel multiplies tiles with coopMatMulAdd (GL_KHR_cooperative_matrix) in a shape vio_cooperative_matrix_shapes() lists, and the product matches the CPU exactly on every backend that reports the flag
--EXTENSIONS--
vio
--FILE--
<?php
/* GL_KHR_cooperative_matrix: one subgroup-scope matrix multiply-add per tile on
 * the hardware matrix units (Vulkan VK_KHR_cooperative_matrix, Metal
 * simdgroup_matrix through SPIRV-Cross - 8x8 only, MSL 3.1, Apple7+). The test
 * picks a listed shape (A/B float16 with a float32 or float16 accumulator, or
 * all float32) and computes D = A * B + C for a 2x2 grid of output tiles with
 * K = 2 tiles: one workgroup per output tile, a K loop of two coopMatMulAdd.
 * All values are small integers, so every product and sum is exact in binary16
 * and the result must equal the CPU bit for bit. The workgroup has 64
 * invocations - one or two whole subgroups, each computing the same tile.
 * A shape list without the flag (or the flag without shapes) fails; Metal below
 * the MSL 3.1 rung must report neither.
 * VIO_REQUIRE_COOPERATIVE_MATRIX=vulkan,metal makes the listed backends mandatory. */
$opts = ["width" => 16, "height" => 16, "headless" => true, "vsync" => false];

function f16(int $v): string {
    /* binary16 bits of a small integer (|v| < 2048), little-endian. */
    if ($v === 0) return "\0\0";
    $s = $v < 0 ? 0x8000 : 0; $a = abs($v);
    $e = (int)floor(log($a, 2));
    $m = (int)(($a / (1 << $e) - 1.0) * 1024);
    return pack('v', $s | (($e + 15) << 10) | $m);
}
function f16_dec(int $h): float {
    $s = ($h & 0x8000) ? -1.0 : 1.0; $e = ($h >> 10) & 0x1F; $m = $h & 0x3FF;
    if ($e === 0) return $s * $m * 2 ** -24;
    return $s * (1 + $m / 1024) * 2 ** ($e - 15);
}
function glsl_type(string $t): string { return $t === 'float16' ? 'float16_t' : 'float'; }
function pack_vals(array $v, string $t): string {
    return $t === 'float16' ? implode('', array_map('f16', $v)) : pack('g*', ...$v);
}
function unpack_vals(string $bytes, string $t, int $n): array {
    if ($t === 'float16') return array_map('f16_dec', array_values(unpack("v$n", $bytes)));
    return array_map('floatval', array_values(unpack("g$n", $bytes)));
}

function pick_shape(array $shapes): ?array {
    foreach ([['float16', 'float32'], ['float16', 'float16'], ['float32', 'float32']] as [$ab, $cr]) {
        foreach ($shapes as $s) {
            if ($s['a'] === $ab && $s['b'] === $ab && $s['c'] === $cr && $s['result'] === $cr) return $s;
        }
    }
    return null;
}

function kernel(array $s): string {
    [$m, $n, $k] = [$s['m'], $s['n'], $s['k']];
    $ab = glsl_type($s['a']); $cr = glsl_type($s['c']);
    $K = 2 * $k; $N = 2 * $n;
    $A = "coopmat<$ab, gl_ScopeSubgroup, $m, $k, gl_MatrixUseA>";
    $B = "coopmat<$ab, gl_ScopeSubgroup, $k, $n, gl_MatrixUseB>";
    $C = "coopmat<$cr, gl_ScopeSubgroup, $m, $n, gl_MatrixUseAccumulator>";
    return "#version 450\n"
        . "#extension GL_KHR_cooperative_matrix : require\n"
        . "#extension GL_KHR_memory_scope_semantics : require\n"
        . "#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require\n"
        . "layout(local_size_x = 64) in;\n"
        . "layout(std430, binding = 0) readonly buffer BA { $ab a[]; };\n"
        . "layout(std430, binding = 1) readonly buffer BB { $ab b[]; };\n"
        . "layout(std430, binding = 2) readonly buffer BC { $cr c[]; };\n"
        . "layout(std430, binding = 3) writeonly buffer BD { $cr d[]; };\n"
        . "void main(){\n"
        . "  uint ti = gl_WorkGroupID.y, tj = gl_WorkGroupID.x;\n"
        . "  $C acc;\n"
        . "  coopMatLoad(acc, c, ti * $m * $N + tj * $n, $N, gl_CooperativeMatrixLayoutRowMajor);\n"
        . "  for (uint t = 0; t < 2; t++) {\n"
        . "    $A ma; $B mb;\n"
        . "    coopMatLoad(ma, a, ti * $m * $K + t * $k, $K, gl_CooperativeMatrixLayoutRowMajor);\n"
        . "    coopMatLoad(mb, b, t * $k * $N + tj * $n, $N, gl_CooperativeMatrixLayoutRowMajor);\n"
        . "    acc = coopMatMulAdd(ma, mb, acc);\n"
        . "  }\n"
        . "  coopMatStore(acc, d, ti * $m * $N + tj * $n, $N, gl_CooperativeMatrixLayoutRowMajor);\n"
        . "}\n";
}

function run_shape($ctx, array $s): array {
    [$m, $n, $k] = [$s['m'], $s['n'], $s['k']];
    $M = 2 * $m; $N = 2 * $n; $K = 2 * $k;
    $a = $b = $c = [];
    for ($i = 0; $i < $M; $i++) for ($j = 0; $j < $K; $j++) $a[] = ($i * 3 + $j * 5) % 5 - 2;
    for ($i = 0; $i < $K; $i++) for ($j = 0; $j < $N; $j++) $b[] = ($i * 7 + $j) % 4 - 1;
    for ($i = 0; $i < $M; $i++) for ($j = 0; $j < $N; $j++) $c[] = ($i + 2 * $j) % 7 - 3;
    $want = [];
    for ($i = 0; $i < $M; $i++) for ($j = 0; $j < $N; $j++) {
        $sum = $c[$i * $N + $j];
        for ($t = 0; $t < $K; $t++) $sum += $a[$i * $K + $t] * $b[$t * $N + $j];
        $want[] = (float)$sum;
    }
    $cp = vio_compute_pipeline($ctx, ['source' => kernel($s)]);
    if (!$cp) return ["compute pipeline not created ({$m}x{$n}x{$k} {$s['a']}/{$s['c']})"];
    $ba = vio_storage_buffer($ctx, ['data' => pack_vals($a, $s['a'])]);
    $bb = vio_storage_buffer($ctx, ['data' => pack_vals($b, $s['b'])]);
    $bc = vio_storage_buffer($ctx, ['data' => pack_vals($c, $s['c'])]);
    $bd = vio_storage_buffer($ctx, ['size' => strlen(pack_vals($c, $s['result']))]);
    vio_compute_bind_buffer($ctx, $cp, $ba, 0, VIO_COMPUTE_READ);
    vio_compute_bind_buffer($ctx, $cp, $bb, 1, VIO_COMPUTE_READ);
    vio_compute_bind_buffer($ctx, $cp, $bc, 2, VIO_COMPUTE_READ);
    vio_compute_bind_buffer($ctx, $cp, $bd, 3, VIO_COMPUTE_WRITE);
    vio_compute_dispatch($ctx, $cp, 2, 2, 1);
    $got = unpack_vals(vio_storage_buffer_read($ctx, $bd), $s['result'], $M * $N);
    $bad = 0; $first = '';
    foreach ($want as $i => $w) if ($got[$i] !== $w) { if (!$bad) $first = "d[$i] = {$got[$i]}, want $w"; $bad++; }
    return $bad ? ["$bad of " . ($M * $N) . " results wrong ({$m}x{$n}x{$k} {$s['a']}/{$s['c']}), first: $first"] : [];
}

function run_backend(string $name): string {
    global $opts;
    $req = in_array($name, array_map('trim', explode(',', getenv('VIO_REQUIRE_COOPERATIVE_MATRIX') ?: '')), true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    $flag = vio_supports_feature($ctx, VIO_FEATURE_COOPERATIVE_MATRIX);
    $shapes = vio_cooperative_matrix_shapes($ctx);
    if (!$flag) {
        vio_destroy($ctx);
        if ($shapes) return "FAIL\n  shapes listed although VIO_FEATURE_COOPERATIVE_MATRIX is 0";
        return $req ? "FAIL\n  required but VIO_FEATURE_COOPERATIVE_MATRIX is 0" : "skip (no cooperative matrices)";
    }
    $fail = [];
    if (!$shapes) $fail[] = "flag set but no shapes listed";
    foreach ($shapes as $s) {
        foreach (['m', 'n', 'k'] as $key) if (!is_int($s[$key] ?? null) || $s[$key] <= 0) $fail[] = "shape without a positive '$key'";
        foreach (['a', 'b', 'c', 'result'] as $key) if (!is_string($s[$key] ?? null)) $fail[] = "shape without a component type '$key'";
    }
    $s = pick_shape($shapes);
    if (!$s) $fail[] = "no float16 / float32 shape among " . json_encode($shapes);
    else $fail = array_merge($fail, run_shape($ctx, $s));
    vio_destroy($ctx);
    /* Metal: below MSL 3.1 SPIRV-Cross emits no simdgroup_matrix. */
    if ($name === 'metal') {
        $low = @vio_create('metal', $opts + ['msl_version' => 30]);
        if ($low) {
            if (vio_supports_feature($low, VIO_FEATURE_COOPERATIVE_MATRIX) || vio_cooperative_matrix_shapes($low))
                $fail[] = "MSL 3.0 rung still reports cooperative matrices";
            vio_destroy($low);
        }
    }
    return $fail ? "FAIL\n  " . implode("\n  ", array_unique($fail)) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) echo "$b: ", run_backend($b), "\n";
$null = vio_create('null', ['width' => 4, 'height' => 4]);
echo "null: ", vio_cooperative_matrix_shapes($null) === [] ? "[]" : "FAIL", "\n";
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
null: []
DONE
