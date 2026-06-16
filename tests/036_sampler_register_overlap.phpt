--TEST--
vio_shader: >4 regular samplers + a shadow sampler get non-overlapping D3D SRV registers
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!function_exists('vio_create')) { echo "skip vio_create missing"; return; }
$ctx = @vio_create('auto', ['width' => 16, 'height' => 16, 'headless' => true, 'visible' => false]);
if ($ctx === false) { echo "skip no headless GPU context"; return; }
$backend = vio_backend_name($ctx);
vio_destroy($ctx);
if (stripos($backend, 'd3d') === false) { echo "skip register overlap is D3D-specific (backend: $backend)"; }
?>
--FILE--
<?php
// Regression guard for D3D12 "X4610: SRV binding ranges overlap for range T[4:4]".
// The HLSL register allocator put depth/shadow samplers at a fixed base of 4, so
// a 5th regular sampler collided with the first shadow sampler at t4. The mesh
// shader hit this once it sampled albedo + ssao + sdf_ao + an irradiance probe +
// a reflection cubemap (5 regular) alongside the shadow maps. The fix raises the
// shadow base to 8 (reflect.c + php_vio.c replay + the d3d12 root signature).

$ctx = vio_create('auto', ['width' => 16, 'height' => 16, 'headless' => true, 'visible' => false]);

$vert = "#version 450 core\nvoid main() { gl_Position = vec4(0.0); }";

// 5 regular samplers (the 5th is the one that used to overlap) + a comparison.
$frag = "#version 450 core
layout(location = 0) out vec4 o;
uniform sampler2D   t0;
uniform sampler2D   t1;
uniform sampler2D   t2;
uniform sampler3D   t3;
uniform samplerCube t4;
uniform sampler2DShadow sh;
void main() {
    o = texture(t0, vec2(0.0)) + texture(t1, vec2(0.0)) + texture(t2, vec2(0.0))
      + texture(t3, vec3(0.0)) + texture(t4, vec3(0.0, 1.0, 0.0))
      + vec4(texture(sh, vec3(0.5)));
}";

$shader = vio_shader($ctx, [
    'vertex'   => $vert,
    'fragment' => $frag,
    'format'   => VIO_SHADER_GLSL, // force GLSL -> SPIR-V -> HLSL transpile
]);

var_dump($shader instanceof VioShader);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECTF--
%Abool(true)
OK
