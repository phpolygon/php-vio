--TEST--
Sampler units follow OpenGL on every backend: a sampler without vio_set_uniform reads unit 0 (or its layout(binding)), several samplers may share a unit, a texture on a unit no sampler reads goes nowhere, and a sampler moved to another unit leaves the old one
--EXTENSIONS--
vio
--FILE--
<?php
/* D3D11, D3D12 and Vulkan resolved units instead of samplers: a unit no
 * vio_set_uniform had named went to the sampler with that INDEX (a texture on
 * unit 0 landed in the first declared sampler), a sampler nobody set read
 * whatever its slot held, and a sampler moved to another unit kept receiving
 * the old one (Code Rescue: FXAA without bloom read the scene as bloom). */
$VS = "#version 450\nlayout(location=0) in vec3 a_pos;\nvoid main(){ gl_Position = vec4(a_pos, 1.0); }";
$FS = "#version 450\nlayout(location=0) out vec4 o;\nuniform sampler2D u_a;\nuniform sampler2D u_b;\nlayout(binding = 4) uniform sampler2D u_c;\n"
    . "void main(){ o = vec4(texture(u_a, vec2(0.5)).r, texture(u_b, vec2(0.5)).g, texture(u_c, vec2(0.5)).b, 1.0); }";
$W = 8;
const WANT = [200, 150, 100];
function close_to(array $a, array $b): bool { foreach ($b as $i => $v) if (abs($a[$i] - $v) > 4) return false; return true; }

/* Each case: [description, [unit => [r, g, b]], [uniform => unit]]. */
$cases = [
    ['u_a on unit 3, u_b unset reads unit 0, u_c its binding 4',
        [3 => [200, 0, 0], 0 => [0, 150, 0], 4 => [0, 0, 100]], ['u_a' => 3]],
    ['all three on unit 5',
        [5 => [200, 150, 100]], ['u_a' => 5, 'u_b' => 5, 'u_c' => 5]],
    ['a texture on unit 0 that no sampler reads stays out',
        [0 => [255, 255, 255], 1 => [200, 0, 0], 2 => [0, 150, 0], 4 => [0, 0, 100]], ['u_a' => 1, 'u_b' => 2]],
    ['u_a moved from unit 1 to 6',
        [1 => [255, 255, 255], 6 => [200, 0, 0], 2 => [0, 150, 0], 3 => [0, 0, 100]], ['u_a' => 1, 'u_b' => 2, 'u_c' => 3, 'u_a ' => 6]],
];

function run_case(string $name, array $units, array $uniforms): array|string {
    global $VS, $FS, $W;
    $ctx = @vio_create($name, ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D pipeline)"; }
    $tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
    $p = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $tex = [];
    foreach ($units as $u => $c) $tex[$u] = vio_texture($ctx, ['data' => str_repeat(chr($c[0]) . chr($c[1]) . chr($c[2]) . "\xFF", 4), 'width' => 2, 'height' => 2]);
    $got = [];
    foreach (['bind first', 'uniform first'] as $order) {
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $p);
        if ($order === 'bind first') foreach ($tex as $u => $t) vio_bind_texture($ctx, $t, $u);
        foreach ($uniforms as $n => $u) vio_set_uniform($ctx, trim($n), $u);
        if ($order === 'uniform first') foreach ($tex as $u => $t) vio_bind_texture($ctx, $t, $u);
        vio_draw($ctx, $tri);
        vio_end($ctx);
        $px = vio_read_pixels($ctx);
        $o = (intdiv($W, 2) * $W + intdiv($W, 2)) * 4;
        $got[$order] = [ord($px[$o]), ord($px[$o + 1]), ord($px[$o + 2])];
    }
    vio_destroy($ctx);
    return $got;
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    $fail = [];
    $skip = null;
    foreach ($cases as [$what, $units, $uniforms]) {
        $r = run_case($b, $units, $uniforms);
        if (is_string($r)) { $skip = $r; break; }
        foreach ($r as $order => $c) if (!close_to($c, WANT)) $fail[] = "$what ($order): " . json_encode($c);
    }
    echo "$b: ", $skip ?? ($fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK"), "\n";
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
