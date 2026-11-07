--TEST--
Bindless anisotropic sampler: set 1 binding 7 (vio_sampler_aniso) keeps detail along the short axis of a stretched footprint where the linear sampler blurs it
--EXTENSIONS--
vio
--FILE--
<?php
/* A 64x64 texture of one-texel horizontal stripes (rows black / white) with a mip
 * chain, sampled with textureGrad at a 16:1 footprint (16 texels across u per pixel,
 * one texel along v) - a floor seen at a grazing angle. The linear sampler picks the
 * mip by the long axis: the stripes are averaged away (grey). vio_sampler_aniso
 * picks it by the short axis and averages along u only: the stripes stay. */
$VS = "#version 450\nlayout(location=0) in vec2 aPos;\nvoid main(){ gl_Position = vec4(aPos, 0.0, 1.0); }";
$FS = "#version 450\n#extension GL_EXT_nonuniform_qualifier : require\nlayout(location=0) out vec4 o;\n"
    . "uniform int u_slot;\nuniform int u_mode;\n"
    . "layout(set = 1, binding = 0) uniform texture2D vio_textures[];\n"
    . "layout(set = 1, binding = 1) uniform sampler vio_sampler;\n"
    . "layout(set = 1, binding = 7) uniform sampler vio_sampler_aniso;\n"
    . "void main(){\n"
    . "  vec2 uv = vec2(gl_FragCoord.x / 64.0, (floor(gl_FragCoord.y) + 0.5) / 64.0);\n"
    . "  vec2 dx = vec2(16.0 / 64.0, 0.0), dy = vec2(0.0, 1.0 / 64.0);\n"
    . "  if (u_mode == 0) o = textureGrad(sampler2D(vio_textures[nonuniformEXT(u_slot)], vio_sampler), uv, dx, dy);\n"
    . "  else             o = textureGrad(sampler2D(vio_textures[nonuniformEXT(u_slot)], vio_sampler_aniso), uv, dx, dy);\n"
    . "}";
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_BINDLESS') ?: ''));

/* Mean difference between neighbouring rows in the middle column. */
function stripe_contrast(string $p): float {
    $sum = 0.0;
    for ($y = 8; $y < 56; $y++) {
        $a = ord($p[($y * 64 + 32) * 4]);
        $b = ord($p[(($y + 1) * 64 + 32) * 4]);
        $sum += abs($a - $b);
    }
    return $sum / 48.0;
}

function run_backend(string $name, array $extra = []): string {
    global $VS, $FS, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, ['width' => 64, 'height' => 64, 'headless' => true, 'vsync' => false] + $extra);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_BINDLESS)) { vio_destroy($ctx); return $req ? "FAIL\n  required but VIO_FEATURE_BINDLESS is 0" : "skip (no bindless)"; }
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    if (!$sh) { vio_destroy($ctx); return "FAIL\n  shader with vio_sampler_aniso not created"; }
    $pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1, 1,-1, 1,1, -1,1], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT2]]);
    $data = '';
    for ($y = 0; $y < 64; $y++) $data .= str_repeat(($y & 1) ? "\xFF\xFF\xFF\xFF" : "\x00\x00\x00\xFF", 64);
    $tex = vio_texture($ctx, ['data' => $data, 'width' => 64, 'height' => 64, 'mipmaps' => true]);
    $slot = vio_texture_index($ctx, $tex);
    $c = [];
    foreach ([0, 1] as $mode) {
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniforms($ctx, ['u_slot' => $slot, 'u_mode' => $mode]);
        vio_draw($ctx, $quad);
        vio_end($ctx);
        $c[$mode] = stripe_contrast(vio_read_pixels($ctx));
    }
    vio_destroy($ctx);
    $err = [];
    if ($c[0] > 60) $err[] = sprintf('linear keeps the stripes (contrast %.0f), the footprint test is not anisotropic', $c[0]);
    if ($c[1] < 120) $err[] = sprintf('vio_sampler_aniso blurs the stripes (contrast %.0f, linear %.0f)', $c[1], $c[0]);
    return $err ? "FAIL\n  " . implode("\n  ", $err) : "OK";
}

echo "d3d12: ", run_backend('d3d12'), "\n";
echo "d3d12_hw: ", run_backend('d3d12', ['headless_hardware' => true]), "\n";
echo "vulkan: ", run_backend('vulkan'), "\n";
echo "metal: ", run_backend('metal'), "\n";
echo "DONE\n";
?>
--EXPECTF--
d3d12: %r(OK|skip \(.*\))%r
d3d12_hw: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE
