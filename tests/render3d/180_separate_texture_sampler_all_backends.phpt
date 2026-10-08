--TEST--
Separate texture and sampler objects in set 0 (texture(sampler2D(u_tex, u_smp), uv)) work on every backend: the texture takes the unit vio_set_uniform gives it, like a sampler2D
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A23. The Vulkan 3D pipeline rejected separate textures / samplers
 * with a notice and drew nothing, and OpenGL could not compile them (GLSL for GL has
 * no separate types): both now combine each pair into a sampler named after the
 * texture. D3D already bound them by unit; Metal keeps them separate. */
$VS = "#version 450\nlayout(location=0) in vec2 aPos;\nlayout(location=0) out vec2 uv;\nvoid main(){ uv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }";
$FS = "#version 450\nlayout(location=0) in vec2 uv;\nlayout(location=0) out vec4 o;\n"
    . "layout(binding = 0) uniform texture2D u_tex;\nlayout(binding = 1) uniform texture2D u_tex2;\nlayout(binding = 2) uniform sampler u_smp;\n"
    . "void main(){ o = gl_FragCoord.x < 8.0 ? texture(sampler2D(u_tex, u_smp), uv) : texture(sampler2D(u_tex2, u_smp), uv); }";

function run_backend(string $name): string {
    global $VS, $FS;
    $ctx = @vio_create($name, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D pipeline)"; }
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    if (!$sh) { vio_destroy($ctx); return "FAIL\n  shader with separate texture / sampler rejected"; }
    $pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1, 1,-1, 1,1, -1,1], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT2]]);
    $red   = vio_texture($ctx, ['data' => str_repeat("\xFF\x00\x00\xFF", 4), 'width' => 2, 'height' => 2]);
    $green = vio_texture($ctx, ['data' => str_repeat("\x00\xFF\x00\xFF", 4), 'width' => 2, 'height' => 2]);
    $err = [];
    /* Units swapped against the declaration order: u_tex reads unit 1, u_tex2 unit 0. */
    foreach ([[0, 1, 'ff0000', '00ff00'], [1, 0, '00ff00', 'ff0000']] as [$ua, $ub, $wl, $wr]) {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniforms($ctx, ['u_tex' => $ua, 'u_tex2' => $ub]);
        vio_bind_texture($ctx, $red, 0);
        vio_bind_texture($ctx, $green, 1);
        vio_draw($ctx, $quad);
        vio_end($ctx);
        $p = vio_read_pixels($ctx);
        $l = bin2hex(substr($p, (8 * 16 + 3) * 4, 3));
        $r = bin2hex(substr($p, (8 * 16 + 12) * 4, 3));
        if ($l !== $wl || $r !== $wr) $err[] = "units u_tex=$ua u_tex2=$ub: left $l right $r, want $wl / $wr";
    }
    vio_destroy($ctx);
    return $err ? "FAIL\n  " . implode("\n  ", $err) : "OK";
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
