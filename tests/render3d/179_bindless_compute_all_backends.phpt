--TEST--
Bindless textures in compute kernels: a kernel reads vio_textures[] and vio_cubes[] through slots from a storage buffer, synchronously and as an async dispatch inside a frame, on every backend with bindless textures and compute
--EXTENSIONS--
vio
--FILE--
<?php
/* BINDLESS-PLAN Phase 4b / OPEN-ITEMS-PLAN A12. Four invocations: 0..2 fetch the
 * centre of three 2D slots, 3 samples a cube slot towards +X. */
$CS = "#version 450\n#extension GL_EXT_nonuniform_qualifier : require\nlayout(local_size_x = 4) in;\n"
    . "layout(set = 1, binding = 0) uniform texture2D vio_textures[];\n"
    . "layout(set = 1, binding = 1) uniform sampler vio_sampler;\n"
    . "layout(set = 1, binding = 5) uniform textureCube vio_cubes[];\n"
    . "layout(std430, binding = 0) readonly buffer Slots { int s[]; } slots;\n"
    . "layout(std430, binding = 1) buffer Out { vec4 c[]; } outb;\n"
    . "void main(){\n"
    . "  uint i = gl_GlobalInvocationID.x;\n"
    . "  int s = slots.s[i];\n"
    . "  if (i < 3u) outb.c[i] = textureLod(sampler2D(vio_textures[nonuniformEXT(s)], vio_sampler), vec2(0.5), 0.0);\n"
    . "  else        outb.c[i] = textureLod(samplerCube(vio_cubes[nonuniformEXT(s)], vio_sampler), vec3(1.0, 0.0, 0.0), 0.0);\n"
    . "}";
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_BINDLESS') ?: ''));

function rgba(array $c, int $n): string { return str_repeat(chr($c[0]) . chr($c[1]) . chr($c[2]) . "\xFF", $n); }

function run_backend(string $name): string {
    global $CS, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_BINDLESS) || !vio_supports_feature($ctx, VIO_FEATURE_COMPUTE)) {
        vio_destroy($ctx);
        return $req ? "FAIL\n  required but bindless / compute missing" : "skip (no bindless compute)";
    }
    $cp = vio_compute_pipeline($ctx, ['source' => $CS]);
    if (!$cp) { vio_destroy($ctx); return "FAIL\n  bindless kernel not created"; }

    $colours = [[255, 0, 0], [0, 255, 0], [0, 0, 255]];
    $tex = []; $slots = [];
    foreach ($colours as $i => $c) {
        $tex[$i] = vio_texture($ctx, ['data' => rgba($c, 4), 'width' => 2, 'height' => 2]);
        $slots[$i] = vio_texture_index($ctx, $tex[$i]);
    }
    $faces = [[255, 255, 0], [0, 0, 0], [0, 0, 0], [0, 0, 0], [0, 0, 0], [0, 0, 0]];
    $cube = vio_cubemap($ctx, ['pixels' => array_map(fn($c) => array_merge(...array_fill(0, 16, [$c[0], $c[1], $c[2], 255])), $faces),
                               'width' => 4, 'height' => 4]);
    $slots[3] = vio_texture_index($ctx, $cube);
    $want = [[1, 0, 0, 1], [0, 1, 0, 1], [0, 0, 1, 1], [1, 1, 0, 1]];

    /* Slots in reverse order, so a kernel that ignored them would fail. */
    $order = [2, 0, 1, 3];
    $slotBuf = vio_storage_buffer($ctx, ['data' => pack('l*', ...array_map(fn($k) => $slots[$k], $order))]);
    $err = [];
    foreach (['sync', 'async'] as $mode) {
        $out = vio_storage_buffer($ctx, ['size' => 4 * 16]);
        vio_compute_bind_buffer($ctx, $cp, $slotBuf, 0, VIO_COMPUTE_READ);
        vio_compute_bind_buffer($ctx, $cp, $out, 1, VIO_COMPUTE_WRITE);
        if ($mode === 'sync') {
            vio_compute_dispatch($ctx, $cp, 1, 1, 1);
        } else {
            vio_begin($ctx);
            vio_compute_dispatch($ctx, $cp, 1, 1, 1, ['async' => true]);
            vio_end($ctx);
        }
        $f = array_values(unpack('f*', vio_storage_buffer_read($ctx, $out)));
        foreach ($order as $i => $k) {
            $got = array_map(fn($v) => round($v, 2), array_slice($f, $i * 4, 4));
            if ($got != $want[$k]) $err[] = "$mode invocation $i (slot {$slots[$k]}): " . json_encode($got) . ", want " . json_encode($want[$k]);
        }
    }
    vio_destroy($ctx);
    return $err ? "FAIL\n  " . implode("\n  ", $err) : "OK";
}

foreach (['d3d12', 'vulkan', 'metal'] as $b) echo "$b: ", run_backend($b), "\n";
echo "DONE\n";
?>
--EXPECTF--
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE
