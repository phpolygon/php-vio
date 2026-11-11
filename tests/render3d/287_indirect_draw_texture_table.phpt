--TEST--
Indirect and storage-instanced draws bind their texture table (D3D12 removed the device when the first draw of a frame sampled a texture through vio_draw_indirect)
--EXTENSIONS--
vio
--FILE--
<?php
/* Before: d3d12_draw_indirect and d3d12_draw_instanced_from_storage skipped the texture
 * table that every other draw path builds. The root table then pointed at what an earlier
 * draw left, or at nothing: the first such draw of a frame whose shader sampled (a shadow
 * map, a material texture) removed the device (DXGI_ERROR_DEVICE_REMOVED). */
$VS = "#version 450\nlayout(location=0) in vec3 a_pos;\nvoid main(){ gl_Position = vec4(a_pos, 1.0); }";
$FS = "#version 450\nuniform sampler2D u_tex;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texture(u_tex, vec2(0.5)); }";

function run_backend(string $name): string
{
    global $VS, $FS;
    $ctx = @vio_create($name, ['width' => 4, 'height' => 4, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!defined('VIO_FEATURE_INDIRECT_DRAW') || !vio_supports_feature($ctx, VIO_FEATURE_INDIRECT_DRAW)) {
        vio_destroy($ctx);
        return "skip (no indirect draws)";
    }
    $fail = [];
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    $p = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
    $green = vio_texture($ctx, ['data' => "\x00\xff\x00\xff", 'width' => 1, 'height' => 1]);
    $args = vio_storage_buffer($ctx, ['data' => pack('V4', 3, 1, 0, 0), 'stride' => 4, 'indirect' => true]);

    foreach (['indirect', 'storage'] as $kind) {
        if ($kind === 'storage' && !function_exists('vio_draw_instanced_from_buffer')) continue;
        for ($frame = 0; $frame < 3; $frame++) {
            vio_begin($ctx);
            vio_clear($ctx, 0, 0, 0, 1);
            vio_bind_pipeline($ctx, $p);
            vio_bind_texture($ctx, $green, 0);
            vio_set_uniform($ctx, 'u_tex', 0);
            // The first draw of the frame: no earlier draw left a table behind.
            if ($kind === 'indirect') {
                vio_draw_indirect($ctx, $tri, $args, 1);
            } else {
                vio_draw_instanced_from_buffer($ctx, $tri, 1);
            }
            vio_end($ctx);
        }
        $px = vio_read_pixels($ctx);
        if (!is_string($px) || strlen($px) < 16) { $fail[] = "$kind: no read-back (device lost?)"; continue; }
        $o = (2 * 4 + 2) * 4;
        $got = [ord($px[$o]), ord($px[$o + 1]), ord($px[$o + 2])];
        if ($got[0] > 5 || $got[1] < 250 || $got[2] > 5) $fail[] = "$kind: " . json_encode($got) . ', want green';
    }
    vio_destroy($ctx);

    return $fail ? 'FAIL: ' . implode('; ', $fail) : 'OK';
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) echo "$b: ", run_backend($b), "\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
