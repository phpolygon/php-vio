--TEST--
Recorded draw sequences (vio_bundle / vio_draw_bundle, OPEN-ITEMS A38) on every backend: a bundle renders byte-identically to the same draws issued one by one, in the swapchain and in a render target; uniforms set after recording do not change it; it keeps its objects alive; malformed records throw
--EXTENSIONS--
vio
--FILE--
<?php
/* BUNDLE-PLAN.md phase 1. Six quads (3x2 grid), a pipeline switch flat <->
 * textured, textures per record; every record sets every uniform it uses
 * (the bundle contract), so the one-by-one frame sets them the same way. */
$W = 48; $H = 32;
$VS = "#version 450\nlayout(location=0) in vec2 aPos;\nlayout(location=0) out vec2 uv;\nuniform vec2 u_offset;\n"
    . "void main(){ uv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos * vec2(0.3, 0.45) + u_offset, 0.0, 1.0); }";
$FS_FLAT = "#version 450\nlayout(location=0) in vec2 uv;\nlayout(location=0) out vec4 o;\nuniform vec4 u_tint;\nvoid main(){ o = u_tint; }";
$FS_TEX = "#version 450\nlayout(location=0) in vec2 uv;\nlayout(location=0) out vec4 o;\nuniform vec4 u_tint;\nuniform sampler2D u_tex;\n"
        . "void main(){ o = texture(u_tex, uv) * u_tint; }";

function checker(array $a, array $b): string {
    $px = '';
    for ($y = 0; $y < 4; $y++) for ($x = 0; $x < 4; $x++) { $c = (($x + $y) & 1) ? $a : $b; $px .= chr($c[0]) . chr($c[1]) . chr($c[2]) . chr(255); }
    return $px;
}

function run_backend(string $name): string {
    global $W, $H, $VS, $FS_FLAT, $FS_TEX;
    $ctx = @vio_create($name, ['width' => $W, 'height' => $H, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D pipeline)"; }
    $fail = [];
    $base = ['depth_test' => false, 'cull_mode' => VIO_CULL_NONE];
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1, 1,-1, 1,1, -1,1], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT2]]);
    $flat = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS_FLAT])] + $base);
    $tex  = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS_TEX])] + $base);
    $t1 = vio_texture($ctx, ['data' => checker([255, 0, 0], [0, 0, 255]), 'width' => 4, 'height' => 4, 'filter' => VIO_FILTER_NEAREST]);
    $t2 = vio_texture($ctx, ['data' => checker([0, 255, 0], [255, 255, 0]), 'width' => 4, 'height' => 4, 'filter' => VIO_FILTER_NEAREST]);
    $off = fn(int $i): array => [-0.66 + 0.66 * ($i % 3), $i < 3 ? 0.5 : -0.5];
    $records = [
        ['mesh' => $quad, 'pipeline' => $flat, 'uniforms' => ['u_tint' => [1.0, 0.5, 0.25, 1.0], 'u_offset' => $off(0)]],
        ['mesh' => $quad, 'uniforms' => ['u_tint' => [0.2, 0.4, 0.8, 1.0], 'u_offset' => $off(1)]],
        ['mesh' => $quad, 'pipeline' => $tex, 'textures' => [0 => $t1], 'uniforms' => ['u_tint' => [1.0, 1.0, 1.0, 1.0], 'u_tex' => 0, 'u_offset' => $off(2)]],
        ['mesh' => $quad, 'textures' => [0 => $t2], 'uniforms' => ['u_tint' => [0.5, 0.5, 0.5, 1.0], 'u_tex' => 0, 'u_offset' => $off(3)]],
        ['mesh' => $quad, 'textures' => [0 => $t1], 'uniforms' => ['u_tint' => [0.25, 1.0, 0.5, 1.0], 'u_tex' => 0, 'u_offset' => $off(4)]],
        ['mesh' => $quad, 'pipeline' => $flat, 'uniforms' => ['u_tint' => [0.9, 0.1, 0.6, 1.0], 'u_offset' => $off(5)]],
    ];
    $oneByOne = function () use ($ctx, $records) {
        foreach ($records as $r) {
            if (isset($r['pipeline'])) vio_bind_pipeline($ctx, $r['pipeline']);
            foreach ($r['textures'] ?? [] as $slot => $t) vio_bind_texture($ctx, $t, $slot);
            vio_set_uniforms($ctx, $r['uniforms']);
            vio_draw($ctx, $r['mesh']);
        }
    };
    $bundle = vio_bundle($ctx, $records);
    if (!$bundle instanceof VioBundle) { vio_destroy($ctx); return "FAIL\n  vio_bundle did not return a VioBundle"; }
    $info = vio_bundle_info($bundle);
    if (($info['draws'] ?? null) !== 6 || !is_bool($info['native'] ?? null) || !is_string($info['method'] ?? null))
        $fail[] = "info " . json_encode($info);

    $frame = function (callable $body) use ($ctx): string {
        vio_clear($ctx, 0.1, 0.1, 0.1, 1.0);
        vio_begin($ctx);
        $body();
        vio_end($ctx);
        return vio_read_pixels($ctx);
    };
    $want = $frame($oneByOne);
    /* uniforms set after recording: no effect on the bundle */
    $got = $frame(function () use ($ctx, $flat, $bundle) {
        vio_bind_pipeline($ctx, $flat);
        vio_set_uniforms($ctx, ['u_tint' => [0.0, 0.0, 0.0, 1.0], 'u_offset' => [5.0, 5.0]]);
        if (!vio_draw_bundle($ctx, $bundle)) throw new Exception('vio_draw_bundle returned false');
    });
    if ($got !== $want) $fail[] = "swapchain: bundle differs from the one-by-one draws";
    /* backends that record natively (BUNDLE-PLAN phases 2-4) */
    $info = vio_bundle_info($bundle);
    if (in_array($name, ['vulkan', 'd3d11', 'd3d12'], true) && ($info['native'] !== true || $info['method'] === 'replay'))
        $fail[] = "$name: not recorded natively: " . json_encode($info);
    /* a second frame plays it again */
    if ($frame(fn() => vio_draw_bundle($ctx, $bundle)) !== $want) $fail[] = "second frame differs";

    /* the bundle keeps its objects */
    $records = $flat = $tex = $t1 = $t2 = $quad = null;
    gc_collect_cycles();
    if ($frame(fn() => vio_draw_bundle($ctx, $bundle)) !== $want) $fail[] = "after dropping the objects: differs";

    /* into a render target */
    if (vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET)) {
        $rt = vio_render_target($ctx, ['width' => $W, 'height' => $H]);
        $rtFrame = function (callable $body) use ($ctx, $rt): string {
            vio_begin($ctx);
            vio_bind_render_target($ctx, $rt);
            vio_clear($ctx, 0.1, 0.1, 0.1, 1.0);
            $body();
            vio_unbind_render_target($ctx);
            vio_end($ctx);
            return vio_read_render_target($rt);
        };
        $a = $rtFrame(fn() => vio_draw_bundle($ctx, $bundle));
        $b = $rtFrame(fn() => vio_draw_bundle($ctx, $bundle));
        if ($a !== $b) $fail[] = "render target: two plays differ";
        if (strlen($a) !== $W * $H * 4) $fail[] = "render target readback size " . strlen($a);
        /* the target holds the same picture as the swapchain, up to the row order */
        $rows = fn(string $p): array => str_split($p, $W * 4);
        if ($rows($a) !== $rows($want) && array_reverse($rows($a)) !== $rows($want)) $fail[] = "render target: differs from the swapchain picture";
    }

    /* A large bundle (more uniform bytes than a first native recording reserves)
     * and a bundle freed inside the frame that drew it. */
    $flat2 = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS_FLAT])] + $base);
    $quad2 = vio_mesh($ctx, ['vertices' => [-1,-1, 1,-1, 1,1, -1,1], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT2]]);
    $many = [];
    for ($i = 0; $i < 1200; $i++) {
        $many[] = ['mesh' => $quad2, 'pipeline' => $flat2, 'uniforms' => [
            'u_tint' => [($i % 7) / 6, ($i % 5) / 4, ($i % 3) / 2, 1.0],
            'u_offset' => [-0.66 + 0.66 * ($i % 3), $i % 2 ? 0.5 : -0.5]]];
    }
    $big = vio_bundle($ctx, $many);
    $wantBig = $frame(function () use ($ctx, $many) {
        foreach ($many as $r) { vio_bind_pipeline($ctx, $r['pipeline']); vio_set_uniforms($ctx, $r['uniforms']); vio_draw($ctx, $r['mesh']); }
    });
    for ($k = 0; $k < 3; $k++) {
        if ($frame(fn() => vio_draw_bundle($ctx, $big)) !== $wantBig) { $fail[] = "large bundle differs in frame $k"; break; }
    }
    if (in_array($name, ['vulkan', 'd3d11', 'd3d12'], true) && vio_bundle_info($big)['native'] !== true)
        $fail[] = "$name: large bundle not native after three frames";
    $frame(function () use ($ctx, &$big) { vio_draw_bundle($ctx, $big); $big = null; gc_collect_cycles(); });
    if ($frame(fn() => vio_draw_bundle($ctx, $bundle)) !== $want) $fail[] = "after freeing a bundle mid-frame: differs";

    /* contract */
    if (@vio_draw_bundle($ctx, $bundle) !== false) $fail[] = "vio_draw_bundle outside a frame accepted";
    foreach ([[], [['pipeline' => null]], [['mesh' => 'x']], [['mesh' => vio_mesh($ctx, ['vertices' => [0,0, 1,0, 0,1], 'layout' => [VIO_FLOAT2]]), 'uniforms' => [3 => 1.0]]]] as $k => $bad) {
        try { vio_bundle($ctx, $bad); $fail[] = "malformed record set $k accepted"; } catch (ValueError $e) {}
    }
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
