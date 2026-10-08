--TEST--
Fragment-stage storage buffers (VIO_FEATURE_FRAGMENT_STORAGE): vio_bind_fragment_storage_buffer binds a writable std430 buffer for the fragment shader (writes and atomics), readable mid-frame and after the frame
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A15 (part 1, the base of the sampler feedback emulation).
 * An 8 x 8 target, a full-screen quad. The fragment shader counts its
 * invocations with atomicAdd (binding 1, slot 0) and stores 1 + x + 8y per
 * pixel (slot 1 + x + 8y) - every pixel shaded exactly once:
 *   A. one draw: count 64, every slot its value; read mid-frame (the frame
 *      flushes) and after vio_end
 *   B. a second draw in the same frame adds 64 more
 *   C. a second buffer at binding 2 next to the first (two bindings at once)
 *   D. contract: binding 4 -> ValueError; null unbinds; without the flag the
 *      call warns and returns false. */
$W = 8;
$VS = "#version 450\nlayout(location = 0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$FS = "#version 450\n"
    . "layout(std430, binding = 1) buffer Out { uint count; uint px[]; } outb;\n"
    . "layout(std430, binding = 2) buffer Out2 { uint total; } out2;\n"
    . "layout(location = 0) out vec4 o;\n"
    . "void main(){ ivec2 p = ivec2(gl_FragCoord.xy); uint i = uint(p.x + 8 * p.y);\n"
    . "  atomicAdd(outb.count, 1u); outb.px[i] = i + 1u; atomicAdd(out2.total, i + 1u);\n"
    . "  o = vec4(1.0); }";

function run_backend(string $name): string {
    global $W, $VS, $FS;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    $buf = vio_storage_buffer($ctx, ['size' => 4 * 65]);
    if (!vio_supports_feature($ctx, VIO_FEATURE_FRAGMENT_STORAGE)) {
        $r = @vio_bind_fragment_storage_buffer($ctx, $buf, 1);
        vio_destroy($ctx);
        return $r === false ? "skip (no fragment storage)" : "FAIL\n  bound without VIO_FEATURE_FRAGMENT_STORAGE";
    }
    $fail = [];
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    $pipe = $sh ? vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]) : false;
    if (!$pipe) { vio_destroy($ctx); return "FAIL\n  pipeline not created"; }
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $buf2 = vio_storage_buffer($ctx, ['size' => 4]);
    $sum = 64 * 65 / 2;

    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pipe);
    if (vio_bind_fragment_storage_buffer($ctx, $buf, 1) !== true) $fail[] = "bind returned no true";
    vio_bind_fragment_storage_buffer($ctx, $buf2, 2);
    vio_draw($ctx, $quad);
    $mid = array_values(unpack('V*', vio_storage_buffer_read($ctx, $buf)));
    vio_draw($ctx, $quad);
    vio_end($ctx);
    $end = array_values(unpack('V*', vio_storage_buffer_read($ctx, $buf)));
    $tot = unpack('V', vio_storage_buffer_read($ctx, $buf2))[1];

    $px = range(1, 64);
    if ($mid[0] !== 64 || array_slice($mid, 1) !== $px) $fail[] = "A: mid-frame count {$mid[0]}, slots " . json_encode(array_slice($mid, 1, 4)) . "...";
    if ($end[0] !== 128 || array_slice($end, 1) !== $px) $fail[] = "B: after two draws count {$end[0]}";
    if ($tot !== 2 * $sum) $fail[] = "C: second binding total $tot, want " . (2 * $sum);
    $p = vio_read_pixels($ctx);
    if (ord($p[0]) !== 255) $fail[] = "the colour target was not written next to the buffers";

    try { vio_bind_fragment_storage_buffer($ctx, $buf, 4); $fail[] = "D: binding 4 accepted"; } catch (\ValueError $e) {}
    if (vio_bind_fragment_storage_buffer($ctx, null, 1) !== true) $fail[] = "D: unbinding returned no true";
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
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
