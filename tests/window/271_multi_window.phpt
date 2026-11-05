--TEST--
Multiple windows (DESKTOP-APP-PLAN Phase 4): vio_window() opens a second window on the context's device; vio_begin($ctx, ['window' => $w]) renders into it (clear, 2D, a texture made on the context), frames alternate with the main window and each keeps its content, sizes, input and close requests are per window, a window of another context is refused, the main window renders on after the second one is gone; backends not converted yet report VIO_FEATURE_MULTI_WINDOW false and vio_window() false
--EXTENSIONS--
vio
--FILE--
<?php
/* converted in 4a (Metal, D3D11) and 4b (Vulkan); the others follow (4c D3D12, 4d OpenGL) */
$required = ['metal', 'd3d11', 'vulkan'];
$px = function (string $rgba, int $w, int $x, int $y) { $o = ($y * $w + $x) * 4; return [ord($rgba[$o]), ord($rgba[$o + 1]), ord($rgba[$o + 2])]; };
$near = fn(array $a, array $b) => max(array_map(fn($i) => abs($a[$i] - $b[$i]), [0, 1, 2])) <= 8;
$err = function (callable $f) { try { $f(); return 'none'; } catch (ValueError $e) { return 'ValueError'; } };

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    $ctx = @vio_create($b, ['width' => 48, 'height' => 32, 'headless' => true]);
    if (!$ctx || vio_backend_name($ctx) !== $b) { echo "$b: skip (not available)\n"; continue; }
    if (!vio_supports_feature($ctx, VIO_FEATURE_MULTI_WINDOW)) {
        $w = @vio_window($ctx, ['width' => 40, 'height' => 24, 'headless' => true]);
        echo "$b: ", in_array($b, $required, true) ? 'FAIL (not supported)' : ($w === false ? 'unsupported' : 'FAIL (window without the feature)'), "\n";
        vio_destroy($ctx);
        continue;
    }
    $fail = [];
    $w = vio_window($ctx, ['width' => 40, 'height' => 24, 'headless' => true, 'title' => 'Tool']);
    if (!$w instanceof VioWindow) { echo "$b: FAIL (no window)\n"; vio_destroy($ctx); continue; }
    if (vio_window_size($w) !== [40, 24] || vio_framebuffer_size($w) !== [40, 24] || vio_window_size($ctx) !== [48, 32]) $fail[] = 'sizes ' . json_encode([vio_window_size($w), vio_framebuffer_size($w), vio_window_size($ctx)]);

    $tex = vio_texture($ctx, ['data' => str_repeat("\xff\xff\x00\xff", 4), 'width' => 2, 'height' => 2]);
    for ($round = 0; $round < 2; $round++) {
        $main = $round ? [0, 255, 0] : [0, 0, 255];
        $side = $round ? [255, 0, 255] : [255, 0, 0];
        vio_begin($ctx);
        vio_clear($ctx, $main[0] / 255, $main[1] / 255, $main[2] / 255, 1.0);
        vio_rect($ctx, 40, 24, 8, 8, ['color' => 0xFFFFFFFF]);          // white, bottom right
        vio_draw_2d($ctx);
        vio_end($ctx);
        vio_begin($ctx, ['window' => $w]);
        vio_clear($ctx, $side[0] / 255, $side[1] / 255, $side[2] / 255, 1.0);
        vio_rect($ctx, 0, 0, 8, 8, ['color' => 0xFF00FFFF]);            // cyan, top left
        vio_sprite($ctx, $tex, ['x' => 30, 'y' => 14, 'width' => 8, 'height' => 8]);   // yellow, from the context's texture
        vio_draw_2d($ctx);
        vio_end($ctx);
        $pm = vio_read_pixels($ctx);
        $pw = vio_read_pixels($w);
        if (strlen($pm) !== 48 * 32 * 4 || strlen($pw) !== 40 * 24 * 4) { $fail[] = "round $round lengths " . strlen($pm) . '/' . strlen($pw); continue; }
        if (!$near($px($pm, 48, 24, 16), $main)) $fail[] = "round $round main " . json_encode($px($pm, 48, 24, 16));
        if (!$near($px($pm, 48, 43, 27), [255, 255, 255])) $fail[] = "round $round main 2d " . json_encode($px($pm, 48, 43, 27));
        if (!$near($px($pw, 40, 20, 4), $side)) $fail[] = "round $round window " . json_encode($px($pw, 40, 20, 4));
        if (!$near($px($pw, 40, 3, 3), [0, 255, 255])) $fail[] = "round $round 2d " . json_encode($px($pw, 40, 3, 3));
        if (!$near($px($pw, 40, 33, 17), [255, 255, 0])) $fail[] = "round $round sprite " . json_encode($px($pw, 40, 33, 17));
    }

    /* input and close requests per window */
    vio_inject_key($w, VIO_KEY_A, VIO_PRESS);
    if (!vio_key_pressed($w, VIO_KEY_A) || vio_key_pressed($ctx, VIO_KEY_A)) $fail[] = 'keys';
    $asked = 0;
    vio_on_close($w, function () use (&$asked) { $asked++; return null; });
    vio_inject_window_event($w, VIO_WINDOW_EVENT_CLOSE);
    if ($asked !== 1 || !vio_should_close($w) || vio_should_close($ctx)) $fail[] = 'close';

    /* a window belongs to its context (the null backend: one GPU context per backend and process) */
    $other = vio_create('null', ['width' => 16, 'height' => 16, 'headless' => true]);
    if ($other) {
        if ($err(fn() => vio_begin($other, ['window' => $w])) !== 'ValueError') $fail[] = 'foreign window accepted';
        vio_destroy($other);
    }

    vio_destroy_window($w);
    unset($w);
    vio_begin($ctx);
    vio_clear($ctx, 1.0, 1.0, 1.0, 1.0);
    vio_end($ctx);
    if (!$near($px(vio_read_pixels($ctx), 48, 24, 16), [255, 255, 255])) $fail[] = 'main after the window';
    echo "$b: ", $fail ? 'FAIL ' . implode('; ', $fail) : 'OK', "\n";
    vio_destroy($ctx);
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|unsupported|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|unsupported|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE
