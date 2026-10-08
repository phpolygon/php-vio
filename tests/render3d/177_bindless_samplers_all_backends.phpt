--TEST--
Bindless sampler variants: set 1 bindings 1-4 are linear/repeat, nearest/repeat, linear/clamp and nearest/clamp samplers for the table on every backend with bindless textures
--EXTENSIONS--
vio
--FILE--
<?php
/* BINDLESS-PLAN Phase 4b / OPEN-ITEMS-PLAN A12. A 2x1 texture, red | blue, sampled at
 * u = 0.4 (inside the texture: linear mixes 70 % red / 30 % blue, nearest is red) and
 * u = 1.1 (outside: repeat wraps to 0.1, clamp stays on the blue edge texel). */
$VS = "#version 450\nlayout(location=0) in vec2 aPos;\nvoid main(){ gl_Position = vec4(aPos, 0.0, 1.0); }";
$FS = "#version 450\n#extension GL_EXT_nonuniform_qualifier : require\nlayout(location=0) out vec4 o;\n"
    . "uniform int u_slot;\nuniform int u_mode;\nuniform float u_u;\n"
    . "layout(set = 1, binding = 0) uniform texture2D vio_textures[];\n"
    . "layout(set = 1, binding = 1) uniform sampler vio_sampler;\n"
    . "layout(set = 1, binding = 2) uniform sampler vio_sampler_nearest;\n"
    . "layout(set = 1, binding = 3) uniform sampler vio_sampler_clamp;\n"
    . "layout(set = 1, binding = 4) uniform sampler vio_sampler_nearest_clamp;\n"
    . "void main(){\n"
    . "  vec2 uv = vec2(u_u, 0.5);\n"
    . "  if (u_mode == 0)      o = texture(sampler2D(vio_textures[nonuniformEXT(u_slot)], vio_sampler), uv);\n"
    . "  else if (u_mode == 1) o = texture(sampler2D(vio_textures[nonuniformEXT(u_slot)], vio_sampler_nearest), uv);\n"
    . "  else if (u_mode == 2) o = texture(sampler2D(vio_textures[nonuniformEXT(u_slot)], vio_sampler_clamp), uv);\n"
    . "  else                  o = texture(sampler2D(vio_textures[nonuniformEXT(u_slot)], vio_sampler_nearest_clamp), uv);\n"
    . "}";
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_BINDLESS') ?: ''));

/* [mode, u] => what the centre pixel must be */
$cases = [
    'linear repeat @0.4'  => [0, 0.4, 'mix'],  'linear repeat @1.1'  => [0, 1.1, 'mix'],
    'nearest repeat @0.4' => [1, 0.4, 'red'],  'nearest repeat @1.1' => [1, 1.1, 'red'],
    'linear clamp @0.4'   => [2, 0.4, 'mix'],  'linear clamp @1.1'   => [2, 1.1, 'blue'],
    'nearest clamp @0.4'  => [3, 0.4, 'red'],  'nearest clamp @1.1'  => [3, 1.1, 'blue'],
];
function kind(array $c): string {
    if ($c[0] > 240 && $c[2] < 15) return 'red';
    if ($c[2] > 240 && $c[0] < 15) return 'blue';
    if ($c[0] > 150 && $c[0] < 210 && $c[2] > 45 && $c[2] < 105) return 'mix';
    return 'other ' . json_encode($c);
}

function run_backend(string $name): string {
    global $VS, $FS, $cases, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_BINDLESS)) { vio_destroy($ctx); return $req ? "FAIL\n  required but VIO_FEATURE_BINDLESS is 0" : "skip (no bindless)"; }
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    if (!$sh) { vio_destroy($ctx); return "FAIL\n  shader with the sampler variants not created"; }
    $pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1, 1,-1, 1,1, -1,1], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT2]]);
    $tex = vio_texture($ctx, ['data' => "\xFF\x00\x00\xFF\x00\x00\xFF\xFF", 'width' => 2, 'height' => 1]);
    $slot = vio_texture_index($ctx, $tex);
    $err = [];
    foreach ($cases as $label => [$mode, $u, $want]) {
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniforms($ctx, ['u_slot' => $slot, 'u_mode' => $mode, 'u_u' => $u]);
        vio_draw($ctx, $quad);
        vio_end($ctx);
        $p = vio_read_pixels($ctx);
        $o = (8 * 16 + 8) * 4;
        $got = kind([ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]);
        if ($got !== $want) $err[] = "$label: $got, want $want";
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
