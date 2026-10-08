--TEST--
vio_submit_batch renders byte-identically to the same draws issued one by one: pipeline switches, textures per draw, sticky uniform deltas, a texture unit bound before the batch
--EXTENSIONS--
vio
--FILE--
<?php
/* BATCH-SUBMIT-PLAN phase 0. Six quads in a 3x2 grid, drawn once with
 * vio_bind_pipeline / vio_bind_texture / vio_set_uniforms / vio_draw and once
 * with one vio_submit_batch call. The records exercise:
 *   - a pipeline switch (flat colour <-> textured) and a record without 'pipeline'
 *     that keeps the previous one,
 *   - 'textures' per record and a record without 'textures' that keeps the unit,
 *   - uniform deltas: u_tint is set once and must stick for the next records,
 *     u_offset changes on every record.
 * Both frames are read back and must match byte for byte. */
$W = 48; $H = 32;
$VS = "#version 450\nlayout(location=0) in vec2 aPos;\nlayout(location=0) out vec2 uv;\nuniform vec2 u_offset;\n"
    . "void main(){ uv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos * vec2(0.3, 0.45) + u_offset, 0.0, 1.0); }";
$FS_FLAT = "#version 450\nlayout(location=0) in vec2 uv;\nlayout(location=0) out vec4 o;\nuniform vec4 u_tint;\n"
    . "void main(){ o = u_tint; }";
$FS_TEX = "#version 450\nlayout(location=0) in vec2 uv;\nlayout(location=0) out vec4 o;\nuniform vec4 u_tint;\nuniform sampler2D u_tex;\n"
    . "void main(){ o = texture(u_tex, uv) * u_tint; }";

function checker(array $a, array $b): string {
    $px = '';
    for ($y = 0; $y < 4; $y++) for ($x = 0; $x < 4; $x++) {
        $c = (($x + $y) & 1) ? $a : $b;
        $px .= chr($c[0]) . chr($c[1]) . chr($c[2]) . chr(255);
    }
    return $px;
}

function run_backend(string $name): string {
    global $W, $H, $VS, $FS_FLAT, $FS_TEX;
    $ctx = @vio_create($name, ['width' => $W, 'height' => $H, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D pipeline)"; }

    $quad = vio_mesh($ctx, ['vertices' => [-1,-1, 1,-1, 1,1, -1,1], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT2]]);
    $base = ['depth_test' => false, 'cull_mode' => VIO_CULL_NONE];
    $flat = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS_FLAT])] + $base);
    $tex  = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS_TEX])] + $base);
    $t1 = vio_texture($ctx, ['data' => checker([255, 0, 0], [0, 0, 255]), 'width' => 4, 'height' => 4, 'filter' => VIO_FILTER_NEAREST]);
    $t2 = vio_texture($ctx, ['data' => checker([0, 255, 0], [255, 255, 0]), 'width' => 4, 'height' => 4, 'filter' => VIO_FILTER_NEAREST]);

    $off = fn(int $i): array => [-0.66 + 0.66 * ($i % 3), $i < 3 ? 0.5 : -0.5];
    $records = [
        ['mesh' => $quad, 'pipeline' => $flat, 'uniforms' => ['u_tint' => [1.0, 0.5, 0.25, 1.0], 'u_offset' => $off(0)]],
        ['mesh' => $quad, 'uniforms' => ['u_offset' => $off(1)]],                                     // keeps pipeline + u_tint
        ['mesh' => $quad, 'pipeline' => $tex, 'textures' => [0 => $t1],
         'uniforms' => ['u_tint' => [1.0, 1.0, 1.0, 1.0], 'u_tex' => 0, 'u_offset' => $off(2)]],
        ['mesh' => $quad, 'uniforms' => ['u_offset' => $off(3)]],                                     // keeps texture t1
        ['mesh' => $quad, 'textures' => [0 => $t2], 'uniforms' => ['u_tint' => [0.5, 0.5, 0.5, 1.0], 'u_offset' => $off(4)]],
        ['mesh' => $quad, 'pipeline' => $flat, 'uniforms' => ['u_offset' => $off(5)]],                // flat, u_tint sticks per shader
    ];

    $frame = function (bool $batched) use ($ctx, $records, $t2): string {
        vio_clear($ctx, 0.1, 0.1, 0.1, 1.0);
        vio_begin($ctx);
        vio_bind_texture($ctx, $t2, 0);   // bound before the batch; record 3 replaces it
        if ($batched) {
            vio_submit_batch($ctx, $records);
        } else {
            foreach ($records as $r) {
                if (isset($r['pipeline'])) vio_bind_pipeline($ctx, $r['pipeline']);
                foreach ($r['textures'] ?? [] as $slot => $t) vio_bind_texture($ctx, $t, $slot);
                if (isset($r['uniforms'])) vio_set_uniforms($ctx, $r['uniforms']);
                vio_draw($ctx, $r['mesh']);
            }
        }
        vio_end($ctx);
        return vio_read_pixels($ctx);
    };

    $single = $frame(false);
    $batch = $frame(true);
    vio_destroy($ctx);
    if (strlen($single) !== $W * $H * 4) return "FAIL\n  readback size " . strlen($single);
    if ($single === $batch) return "OK";
    $first = -1;
    for ($i = 0; $i < strlen($single); $i++) if ($single[$i] !== $batch[$i]) { $first = intdiv($i, 4); break; }
    return sprintf("FAIL\n  first differing pixel (%d,%d): single %s, batch %s", $first % $W, intdiv($first, $W),
        bin2hex(substr($single, $first * 4, 4)), bin2hex(substr($batch, $first * 4, 4)));
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
