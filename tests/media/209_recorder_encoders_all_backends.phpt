--TEST--
vio_recorder encoders (OPEN-ITEMS A39, VIDEO-ENCODE-PLAN) on every backend: 'software' and 'auto' recordings read back with vio_video_info / vio_video_frame show the rendered frames (moving and static rectangle at the right places and colours), vio_recorder_info names the encoder; D3D11 can encode without a CPU copy
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!function_exists('vio_video_info')) die('skip no vio_video_info');
$c = @vio_create('auto', ['width' => 8, 'height' => 8, 'headless' => true]);
if (!$c) die('skip no backend');
$t = sys_get_temp_dir() . '/vio_209_probe.mp4';
$r = @vio_recorder($c, ['path' => $t, 'fps' => 24, 'encoder' => 'software']);
if (!$r) die('skip recording unavailable (FFmpeg / libx264)');
vio_recorder_stop($r);
@unlink($t);
?>
--FILE--
<?php
/* 256x144 (hardware encoders have minimum sizes), 24 frames: a red square
 * moving right by 2 px a frame, a green square standing still, dark ground. */
$W = 256; $H = 144; $N = 24;
/* VIO_REQUIRE_ZERO_COPY=d3d11: the hardware host must record D3D11 without a CPU copy */
$requireZero = array_filter(explode(',', (string)getenv('VIO_REQUIRE_ZERO_COPY')));

function record($ctx, string $path, string $encoder) {
    global $W, $H, $N;
    @unlink($path);
    $rec = vio_recorder($ctx, ['path' => $path, 'fps' => 24, 'encoder' => $encoder]);
    if (!$rec) return null;
    for ($f = 0; $f < $N; $f++) {
        vio_clear($ctx, 0.05, 0.05, 0.1, 1);
        vio_begin($ctx);
        vio_rect($ctx, 20 + 2 * $f, 30, 24, 24, ['color' => 0xFFFF0000]);
        vio_rect($ctx, 180, 80, 32, 32, ['color' => 0xFF00FF00]);
        vio_draw_2d($ctx);
        vio_end($ctx);
        if (!vio_recorder_capture($rec, $ctx)) return null;
    }
    $info = vio_recorder_info($rec);
    vio_recorder_stop($rec);
    return $info;
}

function check(string $path): array {
    global $W, $H, $N;
    $fail = [];
    $vi = vio_video_info($path);
    if (!$vi || $vi['width'] !== $W || $vi['height'] !== $H || $vi['frames'] !== $N) return ["video info " . json_encode($vi)];
    foreach ([0, 11, 23] as $f) {
        $fr = vio_video_frame($path, $f);
        if (!$fr) { $fail[] = "frame $f not decoded"; continue; }
        $px = fn($x, $y) => array_map('ord', str_split(substr($fr['data'], ($y * $W + $x) * 4, 3)));
        $near = fn(array $a, array $b) => abs($a[0] - $b[0]) <= 40 && abs($a[1] - $b[1]) <= 40 && abs($a[2] - $b[2]) <= 40;
        $red = $px(20 + 2 * $f + 12, 42);
        $green = $px(196, 96);
        $ground = $px(120, 120);
        $left = $px(20 + 2 * $f - 6, 42);   /* where the square was not yet */
        if (!$near($red, [255, 0, 0])) $fail[] = "frame $f: moving square " . json_encode($red);
        if (!$near($green, [0, 255, 0])) $fail[] = "frame $f: static square " . json_encode($green);
        if (!$near($ground, [13, 13, 26]) || !$near($left, [13, 13, 26])) $fail[] = "frame $f: ground " . json_encode([$ground, $left]);
    }
    if (vio_video_frame($path, $N) !== false) $fail[] = "a frame past the end decoded";
    return $fail;
}

function run_backend(string $name): string {
    global $W, $H, $requireZero;
    $ctx = @vio_create($name, ['width' => $W, 'height' => $H, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    $fail = [];
    $dir = sys_get_temp_dir();
    foreach (['software', 'auto'] as $enc) {
        $path = "$dir/vio_209_{$name}_$enc.mp4";
        $info = record($ctx, $path, $enc);
        if (!$info) { $fail[] = "$enc: recording failed"; continue; }
        if (!is_string($info['encoder']) || $info['encoder'] === '' || $info['frames'] !== 24 || !is_bool($info['hardware']) || !is_bool($info['zero_copy']))
            $fail[] = "$enc: info " . json_encode($info);
        if ($enc === 'software' && ($info['hardware'] || $info['zero_copy'])) $fail[] = "software recorded on " . $info['encoder'];
        if ($enc === 'auto' && in_array($name, $requireZero, true) && !$info['zero_copy'])
            $fail[] = "auto: no zero copy (" . $info['encoder'] . ")";
        if (getenv('VIO_ENCODE_TEST_DEBUG')) fprintf(STDERR, "$name $enc: " . json_encode($info) . "\n");
        foreach (check($path) as $m) $fail[] = "$enc ({$info['encoder']}): $m";
        @unlink($path);
    }
    /* an encoder that does not exist */
    if (@vio_recorder($ctx, ['path' => "$dir/vio_209_none.mp4", 'encoder' => 'no_such_encoder']) !== false) $fail[] = "unknown encoder accepted";
    @unlink("$dir/vio_209_none.mp4");
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

if (vio_video_info(__FILE__) !== false) echo "a text file read as video\n";
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
