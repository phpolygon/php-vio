--TEST--
64-bit atomics (VIO_FEATURE_ATOMIC64): atomicMax / atomicMin / atomicAdd / atomicCompSwap on uint64_t storage-buffer elements in a compute shader on every backend that reports the flag
--EXTENSIONS--
vio
--FILE--
<?php
/* 64 threads pack (depth << 32 | id) the way a visibility buffer does and race
 * on four uint64_t slots: max and min of all keys, a sum whose terms only fit
 * in 64 bits (64 * 2^33 = 2^39), and a compare-and-swap that exactly one
 * thread wins. A 32-bit truncation anywhere (e.g. a non-64 HLSL method on a
 * RWByteAddressBuffer) breaks every slot.
 * Metal has 64-bit min / max only, and SPIRV-Cross refuses 64-bit atomics for
 * MSL - the flag is 0 there by design. D3D12 needs Shader Model 6.6 (DXC).
 * VIO_REQUIRE_ATOMIC64=vulkan makes the listed backends mandatory (Linux CI). */
$N = 64;
$CS = "#version 450\n"
    . "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require\n"
    . "#extension GL_EXT_shader_atomic_int64 : require\n"
    . "layout(local_size_x = $N) in;\n"
    . "layout(std430, binding = 0) buffer B { uint64_t v[]; } b;\n"
    . "void main(){ uint i = gl_GlobalInvocationID.x;\n"
    . "  uint64_t key = (uint64_t((i * 7u) % 64u) << 32) | uint64_t(i);\n"
    . "  atomicMax(b.v[0], key);\n"
    . "  atomicMin(b.v[1], key);\n"
    . "  atomicAdd(b.v[2], uint64_t(1) << 33);\n"
    . "  atomicCompSwap(b.v[3], uint64_t(0), key | (uint64_t(1) << 40)); }\n";

$opts = ["width" => 8, "height" => 8, "headless" => true, "vsync" => false, "shader_model" => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_ATOMIC64') ?: ''));

function key_of(int $i): int { return ((($i * 7) % 64) << 32) | $i; }

function run_backend(string $name): string {
    global $CS, $N, $opts, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    $flag = vio_supports_feature($ctx, VIO_FEATURE_ATOMIC64);
    if ($name === 'metal' && $flag) { vio_destroy($ctx); return "FAIL\n  metal reports VIO_FEATURE_ATOMIC64 (SPIRV-Cross has no 64-bit atomics for MSL)"; }
    if (!$flag) { vio_destroy($ctx); return $req ? "FAIL\n  required but VIO_FEATURE_ATOMIC64 is 0" : "skip (no 64-bit atomics)"; }
    $cp = vio_compute_pipeline($ctx, ['source' => $CS]);
    if (!$cp) { vio_destroy($ctx); return "FAIL\n  compute pipeline not created"; }
    /* v[1] starts at UINT64_MAX (min), the others at 0. */
    $init = pack('P', 0) . str_repeat("\xFF", 8) . pack('P', 0) . pack('P', 0);
    $buf = vio_storage_buffer($ctx, ['data' => $init]);
    vio_compute_bind_buffer($ctx, $cp, $buf, 0, VIO_COMPUTE_WRITE);
    vio_compute_dispatch($ctx, $cp, 1, 1, 1);
    $v = array_values(unpack('P4', vio_storage_buffer_read($ctx, $buf)));
    vio_destroy($ctx);
    $keys = array_map('key_of', range(0, $N - 1));
    $fail = [];
    if ($v[0] !== max($keys)) $fail[] = sprintf("max 0x%x, want 0x%x", $v[0], max($keys));
    if ($v[1] !== min($keys)) $fail[] = sprintf("min 0x%x, want 0x%x", $v[1], min($keys));
    if ($v[2] !== $N << 33)   $fail[] = sprintf("add 0x%x, want 0x%x", $v[2], $N << 33);
    if (!in_array($v[3] ^ (1 << 40), $keys, true) || !($v[3] & (1 << 40))) $fail[] = sprintf("compswap 0x%x is no key | 2^40", $v[3]);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) echo "$b: ", run_backend($b), "\n";
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE
