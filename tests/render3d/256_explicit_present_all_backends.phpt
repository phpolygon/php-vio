--TEST--
vio_begin($ctx, ['present' => bool]) (VIO_FEATURE_EXPLICIT_PRESENT) on every backend: true presents a frame whose render target was bound before vio_begin, false presents nothing (vio_read_pixels keeps the last frame, the target still renders), many unpresented frames in a row, and only Vulkan's old bind-before-begin rule is deprecated
--EXTENSIONS--
vio
--FILE--
<?php
/* A target bound before vio_begin used to make a Vulkan frame "not presented",
 * and only on Vulkan: the engine had to bind after vio_begin. The option says it
 * outright, the same on every backend. */
$VS = "#version 450\nlayout(location=0) in vec3 a_pos;\nvoid main(){ gl_Position = vec4(a_pos, 1.0); }";
$FLAT = "#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(1.0, 0.5, 0.0, 1.0); }";
$BLIT = "#version 450\nlayout(binding=0) uniform sampler2D u_tex;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texture(u_tex, vec2(0.5)); }";
$W = 16;
function close_to(array $a, array $b): bool { foreach ($b as $i => $v) if (abs($a[$i] - $v) > 4) return false; return true; }
function centre(string $p, int $w): array { $o = ((intdiv($w, 2)) * $w + intdiv($w, 2)) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }

function run_backend(string $name): string {
    global $VS, $FLAT, $BLIT, $W;
    $ctx = @vio_create($name, ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_EXPLICIT_PRESENT)) return "FAIL: no VIO_FEATURE_EXPLICIT_PRESENT";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D pipeline)"; }
    $flat = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FLAT]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $blit = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $BLIT]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
    $rt = vio_render_target($ctx, ['width' => 8, 'height' => 8]);
    $fail = [];
    $deprecated = 0;
    set_error_handler(function (int $no, string $msg) use (&$deprecated, &$fail) {
        if ($no === E_DEPRECATED) { $deprecated++; return true; }
        $fail[] = "warning: $msg";
        return true;
    });

    $blue = function (array $opts = []) use ($ctx) {
        vio_begin($ctx, $opts);
        vio_clear($ctx, 0, 0, 1, 1);
        vio_end($ctx);
    };
    /* Render into the target, which is bound before vio_begin, then blit it to the screen. */
    $warm = function (array $opts, bool $blitIt) use ($ctx, $rt, $flat, $blit, $tri) {
        vio_bind_render_target($ctx, $rt);
        vio_begin($ctx, $opts);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $flat);
        vio_draw($ctx, $tri);
        vio_unbind_render_target($ctx);
        if ($blitIt) {
            vio_clear($ctx, 0, 1, 0, 1);
            vio_bind_pipeline($ctx, $blit);
            vio_set_uniform($ctx, 'u_tex', 0);
            vio_bind_texture($ctx, vio_render_target_texture($rt), 0);
            vio_draw($ctx, $tri);
        }
        vio_end($ctx);
    };

    // 'present' => true: the target bound before vio_begin is just a target.
    $blue(['present' => true]);
    if (!close_to($c = centre(vio_read_pixels($ctx), $W), [0, 0, 255])) $fail[] = "true: plain frame " . json_encode($c);
    $warm(['present' => true], true);
    if (!close_to($c = centre(vio_read_pixels($ctx), $W), [255, 128, 0])) $fail[] = "true: target bound before vio_begin, frame " . json_encode($c);
    if (!close_to($c = centre(vio_read_render_target($rt), 8), [255, 128, 0])) $fail[] = "true: target " . json_encode($c);

    // 'present' => false: nothing reaches the screen, the target still renders.
    $blue(['present' => true]);
    vio_bind_render_target($ctx, $rt);
    vio_begin($ctx, ['present' => false]);
    vio_clear($ctx, 1, 0, 1, 1);
    vio_unbind_render_target($ctx);
    vio_end($ctx);
    if (!close_to($c = centre(vio_read_render_target($rt), 8), [255, 0, 255])) $fail[] = "false: target " . json_encode($c);
    if (!close_to($c = centre(vio_read_pixels($ctx), $W), [0, 0, 255])) $fail[] = "false: target frame reached the screen " . json_encode($c);
    $warm(['present' => false], true);   // a screen draw in an unpresented frame
    if (!close_to($c = centre(vio_read_pixels($ctx), $W), [0, 0, 255])) $fail[] = "false: blit reached the screen " . json_encode($c);
    // more unpresented frames than any swapchain has images, then a presented one
    for ($i = 0; $i < 8; $i++) $warm(['present' => false], $i % 2 === 0);
    vio_begin($ctx, ['present' => false]);
    vio_clear($ctx, 1, 1, 1, 1);
    vio_end($ctx);
    if (!close_to($c = centre(vio_read_pixels($ctx), $W), [0, 0, 255])) $fail[] = "false: 10 frames later " . json_encode($c);
    $warm(['present' => true], true);
    if (!close_to($c = centre(vio_read_pixels($ctx), $W), [255, 128, 0])) $fail[] = "true after false " . json_encode($c);

    // No option: the old rule - only Vulkan reads the bind order, once deprecated.
    $warm([], false);
    $warm([], false);
    $blue();
    if (!close_to($c = centre(vio_read_pixels($ctx), $W), [0, 0, 255])) $fail[] = "no option: plain frame " . json_encode($c);
    $want = $name === 'vulkan' ? 1 : 0;
    if ($deprecated !== $want) $fail[] = "E_DEPRECATED $deprecated times, want $want";

    restore_error_handler();
    vio_destroy($ctx);
    return $fail ? 'FAIL: ' . implode('; ', $fail) : 'OK';
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) echo "$b: ", run_backend($b), "\n";
// The flag is vio's own: the null backend has it too, and a bad option type stays harmless.
$n = vio_create('null', ['width' => 8, 'height' => 8, 'headless' => true]);
echo 'null: ', vio_supports_feature($n, VIO_FEATURE_EXPLICIT_PRESENT) ? 'flag' : 'no flag', "\n";
vio_begin($n, ['present' => false]);
vio_end($n);
vio_begin($n, ['present' => null]);
vio_end($n);
echo "done\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
null: flag
done
