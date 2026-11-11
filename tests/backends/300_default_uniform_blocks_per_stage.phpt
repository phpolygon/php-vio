--TEST--
Loose uniforms in the vertex and in the fragment stage of one shader link and take their values on every backend, whatever SPIR-V ids the two stages get (OpenGL named each stage's flattened block after its id: equal ids in both stages gave one uniform with two struct types and the program did not link)
--EXTENSIONS--
vio
--FILE--
<?php
/* The quad lies outside the clip volume until u_shift moves it in, the colour
 * is u_tint * u_gain: both stages must see their own uniforms. Padding
 * statements shift the ids of both stages across a range (and both parities),
 * so some pair meets on the same block id with any glslang. */
$vs = function (int $pad): string {
    $p = '';
    for ($i = 1; $i <= $pad; $i++) $p .= "    float vpad$i = float($i);\n";
    return "#version 450\nlayout(location=0) in vec2 aPos;\nuniform mat4 u_m;\nuniform vec4 u_shift;\n"
         . "void main(){\n$p    gl_Position = u_m * vec4(aPos, 0.0, 1.0) + u_shift;\n}";
};
$fs = function (int $pad): string {
    $p = '';
    for ($i = 1; $i <= $pad; $i++) $p .= "    float fpad$i = float($i);\n";
    return "#version 450\nuniform vec3 u_tint;\nuniform float u_gain;\nlayout(location=0) out vec4 o;\nvoid main(){\n$p    o = vec4(u_tint * u_gain, 1.0);\n}";
};
$QUAD = ['vertices' => [-4,-1, -2,-1, -2,1, -4,1], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT2]];
$ID = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1];

function run_backend(string $name): string {
    global $vs, $fs, $QUAD, $ID;
    $ctx = @vio_create($name, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D pipeline)"; }
    $err = [];
    $quad = vio_mesh($ctx, $QUAD);
    for ($n = 0; $n < 39 && count($err) < 3; $n++) {
        $vpad = intdiv($n, 13); $pad = $n % 13;
        ob_start();
        $sh = vio_shader($ctx, ['vertex' => $vs($vpad), 'fragment' => $fs($pad)]);
        $msg = trim(ob_get_clean());
        if (!$sh) { $err[] = "vertex pad $vpad, fragment pad $pad: shader failed: " . strtok($msg, "\n"); continue; }
        $pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniform($ctx, 'u_m', $ID);
        vio_set_uniform($ctx, 'u_shift', [3.0, 0.0, 0.0, 0.0]);
        vio_set_uniform($ctx, 'u_tint', [1.0, 0.5, 0.0]);
        vio_set_uniform($ctx, 'u_gain', 0.5);
        vio_draw($ctx, $quad);
        vio_end($ctx);
        $px = array_values(unpack('C3', substr(vio_read_pixels($ctx), (8 * 16 + 8) * 4, 3)));
        if (abs($px[0] - 128) > 2 || abs($px[1] - 64) > 2 || $px[2] > 2) $err[] = "vertex pad $vpad, fragment pad $pad: centre " . implode(',', $px) . ", expected 128,64,0";
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
