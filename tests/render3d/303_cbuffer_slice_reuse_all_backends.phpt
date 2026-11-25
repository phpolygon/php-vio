--TEST--
Constant blocks stay correct when a draw re-uses its shader's previous slice: two pipelines alternate, the fragment block changes only on some draws and the vertex block on every draw, across several frames, with vio_draw, vio_draw_instanced and vio_submit_batch
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with a 3D pipeline");
?>
--FILE--
<?php
/* D3D12 gives every draw a 256-byte slice of the per-frame constant ring and copies
 * the shader's block into it. A draw whose block equals what the same shader pushed
 * before (same frame) now points the root CBV at that earlier slice instead. This test
 * draws 12 quads per frame through two pipelines in an order that makes the fragment
 * block (u_tint) unchanged for some draws and the vertex block (u_offset) different
 * for all; every quad's centre pixel must carry the tint that is current for ITS
 * shader (sticky per shader, also across frames). */
$W = 64; $H = 48;
function tint(int $i, int $f): array { return [(($i * 37 + $f * 11) % 200 + 30) / 255, (($i * 53 + $f * 23) % 200 + 30) / 255, (($i * 17 + $f * 71) % 200 + 30) / 255, 1.0]; }

function run_backend(string $name, string $mode): string {
    global $W, $H;
    $ctx = @vio_create($name, ['width' => $W, 'height' => $H, 'headless' => true, 'vsync' => false]);
    if (!$ctx) return "skip (unavailable)";
    if (vio_backend_name($ctx) !== $name) { vio_destroy($ctx); return "skip (unavailable)"; }
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D)"; }
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nlayout(location=3) in mat4 aModel;\nuniform vec2 u_offset;\nuniform int u_inst;\n"
        . "void main(){ mat4 m = (u_inst == 1) ? aModel : mat4(1.0); gl_Position = m * vec4(aPos * 0.12 + vec3(u_offset, 0.0), 1.0); }";
    $fsA = "#version 330 core\nlayout(location=0) out vec4 o;\nuniform vec4 u_tint;\nvoid main(){ o = vec4(u_tint.rgb, 1.0); }";
    $fsB = "#version 330 core\nlayout(location=0) out vec4 o;\nuniform vec4 u_tint;\nvoid main(){ o = vec4(u_tint.bgr, 1.0); }";
    $mk = fn ($fs) => vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $pipes = [$mk($fsA), $mk($fsB)];
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $ident = pack('g16', 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    $fail = [];
    $cur = [[0.0, 0.0, 0.0, 1.0], [0.0, 0.0, 0.0, 1.0]];   // sticky tint per shader

    for ($f = 0; $f < 4; $f++) {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        $expect = [];
        $records = [];
        $bound = -1;
        for ($i = 0; $i < 12; $i++) {
            $s = ($i + $f) % 2;
            $off = [-0.75 + 0.5 * ($i % 4), -0.6 + 0.6 * intdiv($i, 4)];
            $uni = ['u_offset' => $off, 'u_inst' => $i % 2];
            if (($i * 7 + $f) % 3 !== 0) { $cur[$s] = tint($i, $f); $uni['u_tint'] = $cur[$s]; }
            $expect[] = [$off, $s, $cur[$s]];
            if ($mode === 'batch') {
                $r = ['mesh' => $quad, 'uniforms' => $uni];
                if ($s !== $bound) { $r['pipeline'] = $pipes[$s]; $bound = $s; }
                if ($i % 2) { $r['instances'] = $ident; $r['instance_count'] = 1; }
                $records[] = $r;
            } else {
                vio_bind_pipeline($ctx, $pipes[$s]);
                vio_set_uniforms($ctx, $uni);
                if ($i % 2) vio_draw_instanced($ctx, $quad, $ident, 1); else vio_draw($ctx, $quad);
            }
        }
        if ($mode === 'batch') vio_submit_batch($ctx, $records);
        vio_end($ctx);
        $px = vio_read_pixels($ctx);
        foreach ($expect as $i => [$off, $s, $c]) {
            $x = (int) (($off[0] * 0.5 + 0.5) * $W);
            $y = (int) ((1 - ($off[1] * 0.5 + 0.5)) * $H);
            $o = ($y * $W + $x) * 4;
            $got = [ord($px[$o]), ord($px[$o + 1]), ord($px[$o + 2])];
            $want = $s === 0 ? [$c[0], $c[1], $c[2]] : [$c[2], $c[1], $c[0]];
            foreach ($want as $k => $v) {
                if (abs($got[$k] - $v * 255) > 3) { $fail[] = "frame $f quad $i (shader $s) got " . json_encode($got) . " want " . json_encode(array_map(fn ($v) => (int) round($v * 255), $want)); break; }
            }
            if (count($fail) > 3) break 2;
        }
    }
    unset($pipes, $quad);
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'metal', 'vulkan'] as $b) {
    foreach (['call', 'batch'] as $m) echo "$b $m: ", run_backend($b, $m), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
opengl call: %r(OK|skip \(.*\))%r
opengl batch: %r(OK|skip \(.*\))%r
d3d11 call: %r(OK|skip \(.*\))%r
d3d11 batch: %r(OK|skip \(.*\))%r
d3d12 call: %r(?:\[d3d12\] [^\r\n]*\r?\n)*(?:OK|skip \(.*\))%r
d3d12 batch: %r(?:\[d3d12\] [^\r\n]*\r?\n)*(?:OK|skip \(.*\))%r
metal call: %r(OK|skip \(.*\))%r
metal batch: %r(OK|skip \(.*\))%r
vulkan call: %r(OK|skip \(.*\))%r
vulkan batch: %r(OK|skip \(.*\))%r
DONE
