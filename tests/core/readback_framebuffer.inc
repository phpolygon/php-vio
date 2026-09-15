<?php
/**
 * Shared body of tests 130/131: size a real window so its framebuffer differs
 * from the configured size, draw four coloured quadrants, and check that both
 * readback functions return that whole framebuffer.
 *
 * The caller sets VIO_FORCE_CONTENT_SCALE, which is what makes the two sizes
 * differ on an unscaled machine.
 */

function readback_quadrant_name(int $r, int $g, int $b): string
{
    $hi = static fn(int $v): bool => $v > 160;
    $lo = static fn(int $v): bool => $v < 96;
    return match (true) {
        $hi($r) && $lo($g) && $lo($b) => 'red',
        $lo($r) && $hi($g) && $lo($b) => 'green',
        $lo($r) && $lo($g) && $hi($b) => 'blue',
        $hi($r) && $hi($g) && $hi($b) => 'white',
        default => sprintf('#%02x%02x%02x', $r, $g, $b),
    };
}

/** @return list<string> mismatches, empty when every quadrant is where it belongs */
function readback_check_quadrants(callable $sample, int $w, int $h, string $what): array
{
    $expect = ['TL' => [0.25, 0.25, 'red'], 'TR' => [0.75, 0.25, 'green'],
               'BL' => [0.25, 0.75, 'blue'], 'BR' => [0.75, 0.75, 'white']];
    $fail = [];
    foreach ($expect as $corner => [$fx, $fy, $colour]) {
        [$r, $g, $b] = $sample((int) ($w * $fx), (int) ($h * $fy));
        $got = readback_quadrant_name($r, $g, $b);
        if ($got !== $colour) $fail[] = "$what $corner is $got, expected $colour";
    }
    return $fail;
}

function readback_follows_framebuffer(string $backend, int $logicalW, int $logicalH): string
{
    $ctx = @vio_create($backend, ['width' => $logicalW, 'height' => $logicalH, 'title' => 'readback', 'vsync' => false]);
    if (!$ctx) return 'unavailable';

    vio_set_window_size($ctx, $logicalW, $logicalH);

    for ($i = 0; $i < 30; $i++) {
        vio_poll_events($ctx);
        [$w, $h] = vio_window_size($ctx);
        $hw = $w / 2; $hh = $h / 2;
        vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
        vio_begin($ctx);
        vio_rect($ctx, 0, 0, $hw, $hh, ['color' => 0xFFFF0000]);
        vio_rect($ctx, $hw, 0, $hw, $hh, ['color' => 0xFF00FF00]);
        vio_rect($ctx, 0, $hh, $hw, $hh, ['color' => 0xFF0000FF]);
        vio_rect($ctx, $hw, $hh, $hw, $hh, ['color' => 0xFFFFFFFF]);
        vio_draw_2d($ctx);
        vio_end($ctx);
    }

    [$fbW, $fbH] = vio_framebuffer_size($ctx);
    $fail = [];

    /* Without the divergence the test proves nothing - say so rather than pass. */
    if ($fbW === $logicalW && $fbH === $logicalH) {
        vio_destroy($ctx);
        return "FAIL\n  framebuffer {$fbW}x{$fbH} equals the configured size; the scale override did not apply";
    }

    $pixels = vio_read_pixels($ctx);
    if (!is_string($pixels)) {
        $fail[] = 'vio_read_pixels returned no pixels';
    } elseif (strlen($pixels) !== $fbW * $fbH * 4) {
        $fail[] = sprintf('vio_read_pixels returned %d pixels, framebuffer is %dx%d = %d',
            intdiv(strlen($pixels), 4), $fbW, $fbH, $fbW * $fbH);
    } else {
        $fail = array_merge($fail, readback_check_quadrants(static function (int $x, int $y) use ($pixels, $fbW): array {
            $i = ($y * $fbW + $x) * 4;
            return [ord($pixels[$i]), ord($pixels[$i + 1]), ord($pixels[$i + 2])];
        }, $fbW, $fbH, 'read_pixels'));
    }

    $png = sys_get_temp_dir() . DIRECTORY_SEPARATOR . "vio_readback_{$backend}_" . getmypid() . '.png';
    @unlink($png);
    if (!vio_save_screenshot($ctx, $png) || !is_file($png)) {
        $fail[] = 'vio_save_screenshot wrote nothing';
    } else {
        $info = getimagesize($png);
        if (!$info || $info[0] !== $fbW || $info[1] !== $fbH) {
            $fail[] = sprintf('vio_save_screenshot wrote %s, framebuffer is %dx%d',
                $info ? "{$info[0]}x{$info[1]}" : 'an unreadable file', $fbW, $fbH);
        } elseif (extension_loaded('gd')) {
            $img = imagecreatefrompng($png);
            $fail = array_merge($fail, readback_check_quadrants(static function (int $x, int $y) use ($img): array {
                $c = imagecolorat($img, $x, $y);
                return [($c >> 16) & 255, ($c >> 8) & 255, $c & 255];
            }, $fbW, $fbH, 'save_screenshot'));
        }
        @unlink($png);
    }

    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : 'ok';
}
