--TEST--
Graphics-stage storage buffers (Path B): readback-free render on the NATIVE backend(s) — D3D11/D3D12 (WARP on Windows), Metal, Vulkan, OpenGL
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
// Run wherever a backend that supports vertex-stage storage buffers can be
// created (D3D12/D3D11 on Windows incl. WARP; OpenGL >= 4.3 on Mesa). Skip when
// none is available (e.g. macOS Metal / Linux Vulkan report the feature as 0).
$found = false;
foreach (['d3d12', 'd3d11', 'vulkan', 'metal', 'opengl'] as $be) {
    $c = @vio_create($be, ['width' => 8, 'height' => 8, 'headless' => true, 'vsync' => false]);
    if ($c === false) continue;
    if (vio_supports_feature($c, VIO_FEATURE_VERTEX_STORAGE)) { $found = true; }
    vio_destroy($c);
    if ($found) break;
}
if (!$found) die('skip no backend with VIO_FEATURE_VERTEX_STORAGE available');
?>
--FILE--
<?php
// Renders N instances whose model matrices are produced by a compute pass and
// read straight from the storage buffer by the vertex shader (gl_InstanceIndex)
// — NO readback of the matrices. Exercises every native backend that supports
// the feature, one context at a time, so a single run covers D3D12 + D3D11 on
// Windows (WARP-capable) in addition to OpenGL.

$VP = 64; $N = 4;

$cs = "#version 450\n"
    . "layout(local_size_x=64) in;\n"
    . "layout(std430, binding=0) writeonly buffer OutM { float m[]; };\n"
    . "layout(std140, binding=2) uniform P { int count; float scale; int p0; int p1; };\n"
    . "void main(){ uint g=gl_GlobalInvocationID.x; if(g>=uint(count)) return;\n"
    . "  float x=((g&1u)!=0u)?0.5:-0.5; float y=((g&2u)!=0u)?0.5:-0.5;\n"
    . "  uint o=g*16u; for(uint k=0u;k<16u;k++) m[o+k]=0.0;\n"
    . "  m[o+0u]=scale; m[o+5u]=scale; m[o+10u]=1.0; m[o+15u]=1.0; m[o+12u]=x; m[o+13u]=y; }\n";
$vs = "#version 450\n"
    . "layout(location=0) in vec3 aPos;\n"
    . "layout(std430, binding=0) readonly buffer Instances { mat4 models[]; };\n"
    . "void main(){ gl_Position = models[gl_InstanceIndex] * vec4(aPos,1.0); }\n";
$fs = "#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o=vec4(1.0); }\n";

/** Render the readback-free scene on one context; return true iff the 4 SSBO-placed quads appear and the SSBO is reflected. */
function run_backend($ctx, string $cs, string $vs, string $fs, int $VP, int $N): bool
{
    $sh = vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs]);
    if ($sh === false) return false;
    $info = vio_shader_reflect($sh);
    if (!isset($info['vertex']['storage_buffers']) || count($info['vertex']['storage_buffers']) !== 1) {
        return false;
    }
    $cp = vio_compute_pipeline($ctx, ['source' => $cs]);
    $buf = vio_storage_buffer($ctx, ['size' => $N * 16 * 4, 'stride' => 4]);
    $pipe = vio_pipeline($ctx, ['shader' => $sh]);
    $mesh = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2,0,2,3], 'layout' => [VIO_FLOAT3]]);
    if ($cp === false || $buf === false || $pipe === false || $mesh === false) return false;

    vio_begin($ctx);
    vio_compute_set_uniforms($ctx, $cp, pack('l', $N) . pack('f', 0.15) . pack('l2', 0, 0));
    vio_compute_bind_buffer($ctx, $cp, $buf, 0, VIO_COMPUTE_WRITE);
    vio_compute_dispatch($ctx, $cp, 1, 1, 1);
    vio_viewport($ctx, 0, 0, $VP, $VP);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $pipe);
    vio_bind_storage_buffer($ctx, $buf, 0, VIO_COMPUTE_READ);
    vio_draw_instanced_from_buffer($ctx, $mesh, $N);
    $px = vio_read_pixels($ctx);
    vio_end($ctx);

    // Some backends row-pad the framebuffer width; derive the real stride.
    $len = strlen($px);
    if ($len === 0 || $len % ($VP * 4) !== 0) return false;
    $stride = intdiv($len, $VP * 4);

    $white = 0;
    foreach ([[0.5,0.5],[-0.5,0.5],[0.5,-0.5],[-0.5,-0.5]] as $c) {
        $x = (int) round(($c[0] * 0.5 + 0.5) * $VP);
        $y = (int) round(($c[1] * 0.5 + 0.5) * $VP);
        $x = max(0, min($VP - 1, $x));
        $y = max(0, min($VP - 1, $y));
        $off = ($y * $stride + $x) * 4;
        if ($off + 2 < $len && ord($px[$off]) > 200 && ord($px[$off + 1]) > 200 && ord($px[$off + 2]) > 200) {
            $white++;
        }
    }
    return $white === 4;
}

$tested = 0;
$fail = false;
foreach (['d3d12', 'd3d11', 'vulkan', 'metal', 'opengl'] as $be) {
    $ctx = @vio_create($be, ['width' => $VP, 'height' => $VP, 'title' => 'vs-native', 'headless' => true, 'vsync' => false]);
    if ($ctx === false) continue;
    if (!vio_supports_feature($ctx, VIO_FEATURE_VERTEX_STORAGE)) { vio_destroy($ctx); continue; }
    $bn = vio_backend_name($ctx);
    if (!run_backend($ctx, $cs, $vs, $fs, $VP, $N)) {
        echo "[FAIL] {$bn}: readback-free instances did not render at their SSBO positions\n";
        $fail = true;
    }
    $tested++;
    vio_destroy($ctx);
}

if ($tested === 0) {
    echo "[FAIL] no vertex-storage backend was exercised\n";
    $fail = true;
}
echo $fail ? "FAILED\n" : "OK\n";
?>
--EXPECT--
OK
