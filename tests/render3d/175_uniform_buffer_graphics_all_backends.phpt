--TEST--
vio_uniform_buffer + vio_bind_buffer feed the uniform blocks of graphics shaders on every backend: a vertex block and a fragment block at different bindings, an update between two draws of one frame, a buffer bound at a binding the shader does not use
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A32. OpenGL flattened the blocks into plain uniforms, so the
 * GL buffer binding reached nothing; D3D12 overwrote the bound root CBV with
 * the shader's own cbuffer at every draw. A block now reads the bytes of the
 * buffer bound at its binding, as of the draw. */
$VS = "#version 450\nlayout(location=0) in vec2 aPos;\n"
    . "layout(std140, binding = 0) uniform Xform { vec4 u_offset; vec4 u_scale; };\n"
    . "void main(){ gl_Position = vec4(aPos * u_scale.xy + u_offset.xy, 0.0, 1.0); }";
$FS = "#version 450\nlayout(location=0) out vec4 o;\n"
    . "layout(std140, binding = 1) uniform Material { vec4 u_color; float u_alpha; };\n"
    . "void main(){ o = vec4(u_color.rgb, u_alpha); }";

function px(string $p, int $x, int $y): string { return bin2hex(substr($p, ($y * 16 + $x) * 4, 3)); }

function run_backend(string $name): string {
    global $VS, $FS;
    $ctx = @vio_create($name, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D pipeline)"; }
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1, 1,-1, 1,1, -1,1], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT2]]);
    $xform = vio_uniform_buffer($ctx, ['size' => 32, 'binding' => 0]);
    $mat   = vio_uniform_buffer($ctx, ['size' => 32, 'binding' => 1, 'data' => pack('f8', 1, 0, 0, 0, 1, 0, 0, 0)]);
    $err = [];

    /* Two draws, the buffers updated in between: left half red, right half green. */
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pipe);
    vio_bind_buffer($ctx, $xform);
    vio_bind_buffer($ctx, $mat);
    vio_update_buffer($xform, pack('f8', -0.5, 0, 0, 0, 0.5, 1, 0, 0));
    vio_draw($ctx, $quad);
    vio_update_buffer($xform, pack('f4', 0.5, 0, 0, 0));          // offset only
    vio_update_buffer($mat, pack('f4', 0, 1, 0, 0));               // colour only, alpha stays 1
    vio_draw($ctx, $quad);
    vio_end($ctx);
    $p = vio_read_pixels($ctx);
    if (($c = px($p, 4, 8)) !== 'ff0000') $err[] = "left (first draw) $c, want ff0000";
    if (($c = px($p, 12, 8)) !== '00ff00') $err[] = "right (second draw) $c, want 00ff00";

    /* A buffer bound at a binding no block uses changes nothing. */
    $stray = vio_uniform_buffer($ctx, ['size' => 32, 'binding' => 5, 'data' => pack('f8', 0, 0, 1, 0, 1, 0, 0, 0)]);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pipe);
    vio_bind_buffer($ctx, $xform);
    vio_bind_buffer($ctx, $mat);
    vio_bind_buffer($ctx, $stray);
    vio_draw($ctx, $quad);
    vio_end($ctx);
    if (($c = px(vio_read_pixels($ctx), 12, 8)) !== '00ff00') $err[] = "stray binding 5: $c, want 00ff00";

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
