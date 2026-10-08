--TEST--
Render-pass switches within one frame on every backend (swapchain, MSAA, cube face, array layer, depth-only, async compute, mid-frame readback and compute wait); on Vulkan the core of dynamic rendering, synchronization2 and timeline semaphores is reported
--EXTENSIONS--
vio
--FILE--
<?php
/* VULKAN-MODERN-PLAN (OPEN-ITEMS A37). One frame, 32 steps; step i
 *   - draws column i of the 64x8 swapchain in colour c(i) (32 columns of 2 px,
 *     full height, so the readback orientation does not matter) - every return
 *     to the swapchain must keep the columns drawn before (LOAD),
 *   - draws c(i) over a 4x MSAA target, read back mid-frame every 8th step,
 *   - clears cube face i % 6 and draws c(i) over array layer i % 4,
 *   - binds a depth-only target and draws into it,
 *   - every 8th step dispatches an async kernel that adds 1 to 64 counters;
 *     at step 16 vio_compute_wait runs mid-frame.
 * After the frame: every column, the MSAA target (c(31)), each cube face and
 * array layer (their last step) and the counters (4) are checked. Vulkan must
 * report dynamic_rendering, synchronization2 and timeline_semaphore in
 * vio_backend_info()['caps']. */
$W = 64; $H = 8; $STEPS = 32;
function c(int $i): array { return [($i * 37 + 20) % 256, ($i * 91 + 40) % 256, ($i * 53 + 80) % 256]; }
function px(string $p, int $x, int $y, int $w): array { $o = ($y * $w + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }
function near(array $a, array $b): bool { for ($k = 0; $k < 3; $k++) if (abs($a[$k] - $b[$k]) > 2) return false; return true; }

$VS = "#version 450\nlayout(location=0) in vec2 aPos;\nuniform vec4 u_rect;\n"
    . "void main(){ gl_Position = vec4(mix(u_rect.x, u_rect.y, aPos.x), aPos.y * 2.0 - 1.0, 0.5, 1.0); }";
$FS = "#version 450\nlayout(location=0) out vec4 o;\nuniform vec4 u_color;\nvoid main(){ o = u_color; }";
$CS = "#version 450\nlayout(local_size_x = 64) in;\nlayout(std430, binding = 0) buffer B { uint v[]; } b;\n"
    . "void main(){ b.v[gl_GlobalInvocationID.x] += 1u; }";

function run_backend(string $name): string {
    global $W, $H, $STEPS, $VS, $FS, $CS;
    $ctx = @vio_create($name, ['width' => $W, 'height' => $H, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE) || !vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET)) {
        vio_destroy($ctx); return "skip (no 3D pipeline / render targets)";
    }
    $fail = [];
    if ($name === 'vulkan') {
        $caps = vio_backend_info($ctx)['caps'] ?? [];
        foreach (['dynamic_rendering', 'synchronization2', 'timeline_semaphore'] as $k)
            if (($caps[$k] ?? false) !== true) $fail[] = "caps[$k] not reported";
    }
    $msaa  = vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_MSAA);
    $cubeF = vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_CUBE);
    $layer = vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_LAYERED);
    $depth = vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_DEPTH);
    $comp  = vio_supports_feature($ctx, VIO_FEATURE_COMPUTE);

    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]),
                                'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $dpipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]),
                                 'depth_test' => true, 'cull_mode' => VIO_CULL_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [0,0, 1,0, 1,1, 0,1], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT2]]);
    $rtM = $msaa  ? vio_render_target($ctx, ['width' => 16, 'height' => 16, 'samples' => 4]) : null;
    $rtC = $cubeF ? vio_render_target($ctx, ['cube' => true, 'size' => 8]) : null;
    $rtA = $layer ? vio_render_target($ctx, ['width' => 8, 'height' => 8, 'layers' => 4]) : null;
    $rtD = $depth ? vio_render_target($ctx, ['width' => 8, 'height' => 8, 'depth_only' => true]) : null;
    $cp = $buf = null;
    if ($comp) {
        $cp  = vio_compute_pipeline($ctx, ['source' => $CS]);
        $buf = vio_storage_buffer($ctx, ['size' => 256, 'data' => str_repeat("\0", 256), 'stride' => 4]);
        if ($cp && $buf) vio_compute_bind_buffer($ctx, $cp, $buf, 0, VIO_COMPUTE_WRITE); else $comp = false;
    }
    $draw = function (array $rect, array $col) use ($ctx, $quad) {
        vio_set_uniform($ctx, 'u_rect', $rect);
        vio_set_uniform($ctx, 'u_color', [$col[0] / 255, $col[1] / 255, $col[2] / 255, 1.0]);
        vio_draw($ctx, $quad);
    };
    $full = [-1.0, 1.0, 0.0, 0.0];

    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    for ($i = 0; $i < $STEPS; $i++) {
        $col = c($i);
        /* swapchain column */
        vio_bind_pipeline($ctx, $pipe);
        $draw([-1 + 2 * $i / $STEPS, -1 + 2 * ($i + 1) / $STEPS, 0, 0], $col);
        if ($rtM) {
            vio_bind_render_target($ctx, $rtM);
            vio_bind_pipeline($ctx, $pipe);
            $draw($full, $col);
            vio_unbind_render_target($ctx);
            if ($i % 8 === 7) {
                $got = px(vio_read_render_target($rtM), 8, 8, 16);
                if (!near($got, $col)) $fail[] = "step $i: MSAA mid-frame " . json_encode($got) . " want " . json_encode($col);
            }
        }
        if ($rtC) {
            vio_bind_render_target($ctx, $rtC, $i % 6);
            vio_clear($ctx, $col[0] / 255, $col[1] / 255, $col[2] / 255, 1.0);
            vio_unbind_render_target($ctx);
        }
        if ($rtA) {
            vio_bind_render_target($ctx, $rtA, $i % 4);
            vio_bind_pipeline($ctx, $pipe);
            $draw($full, $col);
            vio_unbind_render_target($ctx);
        }
        if ($rtD) {
            vio_bind_render_target($ctx, $rtD);
            vio_clear($ctx, 0, 0, 0, 1);
            vio_bind_pipeline($ctx, $dpipe);
            $draw($full, $col);
            vio_unbind_render_target($ctx);
        }
        if ($comp && $i % 8 === 3) vio_compute_dispatch($ctx, $cp, 1, 1, 1, ['async' => true]);
        if ($comp && $i === 16) vio_compute_wait($ctx);
    }
    vio_end($ctx);

    $p = vio_read_pixels($ctx);
    for ($i = 0; $i < $STEPS; $i++) {
        $got = px($p, $i * 2 + 1, $H >> 1, $W);
        if (!near($got, c($i))) { $fail[] = "column $i: " . json_encode($got) . " want " . json_encode(c($i)); break; }
    }
    if ($rtM && !near($got = px(vio_read_render_target($rtM), 8, 8, 16), c(31))) $fail[] = "MSAA after the frame " . json_encode($got);
    if ($rtC) for ($f = 0; $f < 6; $f++) {
        $last = $f <= 1 ? 30 + $f : 24 + $f;
        if (!near($got = px(vio_read_render_target($rtC, $f), 4, 4, 8), c($last))) $fail[] = "cube face $f " . json_encode($got) . " want " . json_encode(c($last));
    }
    if ($rtA) for ($l = 0; $l < 4; $l++) {
        if (!near($got = px(vio_read_render_target($rtA, $l), 4, 4, 8), c(28 + $l))) $fail[] = "array layer $l " . json_encode($got) . " want " . json_encode(c(28 + $l));
    }
    if ($comp) {
        $v = array_values(unpack('V*', vio_storage_buffer_read($ctx, $buf)));
        if ($v !== array_fill(0, 64, 4)) $fail[] = "counters " . json_encode(array_slice($v, 0, 4)) . ", want 4";
    }
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", array_slice($fail, 0, 8)) : "OK";
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
