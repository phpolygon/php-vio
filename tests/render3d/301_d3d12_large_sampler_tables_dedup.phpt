--TEST--
D3D12 sampler tables of shaders over 16 registers are deduplicated per frame: a later pass keeps its own filter after many such draws
--EXTENSIONS--
vio
--ENV--
VIO_D3D_HEADLESS_HARDWARE=1
--FILE--
<?php
/* Before: a shader reading more than 16 texture registers (a lit mesh shader with its
 * shadow maps and a point-shadow array reaches register 17) took a FRESH sampler block
 * of its own length for every draw - only the 16-register tables were deduplicated.
 * A frame's slice of the 2048-entry sampler heap holds 28-42 such blocks, so after that
 * many draws the ring was dry and every following draw, whatever its shader, silently
 * kept the last block built: a small fullscreen pass that samples a linear, clamped 3D
 * colour table then ran with the big shader's NEAREST sampler (an identity table gave
 * 8 colour steps instead of 250) - hard rings in a smooth sky.
 * 200 draws of an 18-sampler shader, then a 5-sampler pass sampling a linear 3D LUT:
 * the output must be as smooth as with no big draw before it. */
$W = 256; $H = 4;
$VS = "#version 450\nlayout(location = 0) in vec3 aPos;\nlayout(location = 0) out vec2 vUv;\nvoid main(){ vUv = aPos.xy * 0.5 + 0.5; gl_Position = vec4(aPos, 1.0); }";
$BIGFS = "#version 450\nlayout(location = 0) in vec2 vUv;\n";
for ($i = 0; $i < 18; $i++) $BIGFS .= "uniform sampler2D u_t$i;\n";
$BIGFS .= "layout(location = 0) out vec4 o;\nvoid main(){ vec4 s = vec4(0.0);\n";
for ($i = 0; $i < 18; $i++) $BIGFS .= "  s += texture(u_t$i, vUv);\n";
$BIGFS .= "  o = vec4(s.rgb * 0.01, 1.0); }";
$LUTFS = "#version 450\nlayout(location = 0) in vec2 vUv;\n"
    . "uniform sampler2D u_scene;\nuniform sampler2D u_bloom;\nuniform sampler2D u_exposure;\n"
    . "uniform sampler3D u_lut_a;\nuniform sampler3D u_lut_b;\n"
    . "layout(location = 0) out vec4 o;\nvoid main(){\n"
    . "  vec3 c = vec3(vUv.x * 0.999 + 0.0005) + (texture(u_scene, vUv).rgb + texture(u_bloom, vUv).rgb + texture(u_exposure, vUv).rgb) * 0.0;\n"
    . "  const float N = 8.0; vec3 uvw = c * ((N - 1.0) / N) + 0.5 / N;\n"
    . "  o = vec4(texture(u_lut_a, uvw).rgb * 0.5 + texture(u_lut_b, uvw).rgb * 0.5, 1.0); }";

$options = ['width' => $W, 'height' => $H, 'headless' => true, 'vsync' => false,
    'shader_model' => 6, 'headless_hardware' => true];
/* VIO_TEST_ADAPTER=Intel runs it on the second hardware adapter. */
if (($adapter = getenv('VIO_TEST_ADAPTER')) !== false && $adapter !== '') $options['adapter'] = $adapter;
$ctx = @vio_create('d3d12', $options);
if ($ctx && vio_backend_name($ctx) !== 'd3d12') { vio_destroy($ctx); $ctx = null; }
if (!$ctx) { echo "skip (no d3d12)\n"; exit; }
$info = vio_backend_info($ctx);
if (($info['samplers']['total'] ?? 0) < 18 || !vio_supports_feature($ctx, VIO_FEATURE_TEXTURE_3D)) {
    echo "skip (fewer than 18 texture units)\n";
    exit;
}

$quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
$big = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $BIGFS]);
$lutShader = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $LUTFS]);
if (!$big || !$lutShader) { echo "FAIL shader\n"; exit; }
$pBig = vio_pipeline($ctx, ['shader' => $big, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
$pLut = vio_pipeline($ctx, ['shader' => $lutShader, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
$near = vio_texture($ctx, ['data' => "\x40\x40\x40\xff", 'width' => 1, 'height' => 1, 'filter' => VIO_FILTER_NEAREST, 'wrap' => VIO_WRAP_REPEAT]);
$lin = vio_texture($ctx, ['data' => "\x40\x40\x40\xff", 'width' => 1, 'height' => 1]);
/* identity colour table, 8 x 8 x 8 */
$n = 8; $data = '';
for ($b = 0; $b < $n; $b++) for ($g = 0; $g < $n; $g++) for ($r = 0; $r < $n; $r++) {
    $data .= chr((int) round($r * 255 / ($n - 1))) . chr((int) round($g * 255 / ($n - 1))) . chr((int) round($b * 255 / ($n - 1))) . "\xff";
}
$lut = vio_texture_3d($ctx, ['data' => $data, 'width' => $n, 'height' => $n, 'depth' => $n,
    'filter' => VIO_FILTER_LINEAR, 'wrap' => VIO_WRAP_CLAMP]);
$scene = vio_render_target($ctx, ['width' => 8, 'height' => 8]);

/* One frame: $bigDraws draws of the 18-sampler shader into a target, then the LUT pass;
 * returns the number of distinct red values along the output row. */
function levels(int $bigDraws): int
{
    global $ctx, $W, $quad, $pBig, $pLut, $near, $lin, $lut, $scene;
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_render_target($ctx, $scene);
    vio_bind_pipeline($ctx, $pBig);
    for ($i = 0; $i < 18; $i++) vio_set_uniform($ctx, "u_t$i", $i);
    for ($d = 0; $d < $bigDraws; $d++) {
        for ($i = 0; $i < 18; $i++) vio_bind_texture($ctx, ($i + $d) % 3 === 0 ? $lin : $near, $i);
        vio_draw($ctx, $quad);
    }
    vio_unbind_render_target($ctx);
    vio_bind_pipeline($ctx, $pLut);
    foreach (['u_scene' => 0, 'u_bloom' => 1, 'u_exposure' => 2, 'u_lut_a' => 3, 'u_lut_b' => 4] as $name => $unit) {
        vio_set_uniform($ctx, $name, $unit);
    }
    vio_bind_texture($ctx, $near, 0);
    vio_bind_texture($ctx, $near, 1);
    vio_bind_texture($ctx, $near, 2);
    vio_bind_texture($ctx, $lut, 3);
    vio_bind_texture($ctx, $lut, 4);
    vio_draw($ctx, $quad);
    vio_end($ctx);
    $px = vio_read_pixels($ctx);
    $set = [];
    for ($x = 0; $x < $W; $x++) $set[ord($px[(2 * $W + $x) * 4])] = 1;

    return count($set);
}

$reference = levels(0);
$after = levels(200);
echo "reference levels: ", $reference > 200 ? "smooth" : "FAIL $reference", "\n";
echo "after 200 big draws: ", $after >= $reference * 0.85 ? "smooth" : "FAIL $after of $reference", "\n";
$again = levels(200);
echo "next frame: ", $again >= $reference * 0.85 ? "smooth" : "FAIL $again of $reference", "\n";
?>
--EXPECTF--
%r(skip \(.*\)|reference levels: smooth
after 200 big draws: smooth
next frame: smooth)%r
