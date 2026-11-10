--TEST--
A fragment stage with a uniform block, three storage buffers and 13 samplers: compiles everywhere and the last sampler sees its texture (Metal counts textures apart from buffers)
--EXTENSIONS--
vio
--FILE--
<?php
/* Metal has one argument table for buffers and one for textures and samplers
 * (16 samplers). The MSL indices used to share one count: the uniform block and
 * three storage buffers took 0..3, and 13 samplers then needed [[sampler(16)]] -
 * the program did not compile. A 4 x 4 target, a full-screen quad: units 0..11
 * hold black, unit 12 red; the shader adds all 13 samples, the storage values
 * (zero) and u_tint (zero) - the pixel must be red. */
$W = 4;
$VS = "#version 450\nlayout(location = 0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$FS = "#version 450\n"
    . "uniform vec4 u_tint;\n"
    . "layout(std430, binding = 1) readonly buffer A { float a[]; } ba;\n"
    . "layout(std430, binding = 2) readonly buffer B { float b[]; } bb;\n"
    . "layout(std430, binding = 3) readonly buffer C { float c[]; } bc;\n";
for ($i = 0; $i < 13; $i++) $FS .= "uniform sampler2D u_t$i;\n";
$FS .= "layout(location = 0) out vec4 o;\nvoid main(){ vec4 s = u_tint + vec4(ba.a[0] + bb.b[0] + bc.c[0]);\n";
for ($i = 0; $i < 13; $i++) $FS .= "  s += texture(u_t$i, vec2(0.5));\n";
$FS .= "  o = vec4(s.rgb, 1.0); }";

function run_backend(string $name): string {
    global $W, $VS, $FS;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_FRAGMENT_STORAGE)) { vio_destroy($ctx); return "skip (no fragment storage)"; }
    $sh = @vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    if (!$sh) { vio_destroy($ctx); return "FAIL\n  shader with 4 buffers and 13 samplers did not compile"; }
    $pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $black = vio_texture($ctx, ['data' => "\x00\x00\x00\xff", 'width' => 1, 'height' => 1]);
    $red = vio_texture($ctx, ['data' => "\xff\x00\x00\xff", 'width' => 1, 'height' => 1]);
    $zero = vio_storage_buffer($ctx, ['size' => 16]);
    vio_clear($ctx, 0, 0, 1, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pipe);
    vio_set_uniform($ctx, 'u_tint', [0.0, 0.0, 0.0, 0.0]);
    for ($i = 0; $i < 13; $i++) {
        vio_set_uniform($ctx, "u_t$i", $i);
        vio_bind_texture($ctx, $i === 12 ? $red : $black, $i);
    }
    for ($b = 1; $b <= 3; $b++) vio_bind_fragment_storage_buffer($ctx, $zero, $b);
    vio_draw($ctx, $quad);
    vio_end($ctx);
    $p = vio_read_pixels($ctx);
    vio_destroy($ctx);
    $c = [ord($p[0]), ord($p[1]), ord($p[2])];
    return ($c[0] > 240 && $c[1] < 16 && $c[2] < 16) ? "OK" : "FAIL\n  pixel " . json_encode($c) . ", want red";
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
