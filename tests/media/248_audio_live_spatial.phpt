--TEST--
Live audio parameters on an offline engine: pitch, pan, 2D/3D switch, attenuation models, cone, relative positioning, listener up vector, Doppler (source, listener, speed of sound), vio_audio_render
--EXTENSIONS--
vio
--ENV--
VIO_AUDIO_BACKEND=
--FILE--
<?php
// Offline engine: no device, the mix advances only through vio_audio_render(),
// so every measurement below is deterministic.
var_dump(vio_audio_listener_state());               // before init
var_dump(vio_audio_init(['device' => false]));
var_dump(vio_audio_backend());
var_dump(vio_audio_init(['device' => false]));      // same mode again: fine
var_dump(@vio_audio_init(['device' => true]));      // other mode: refused
$ls = vio_audio_listener_state();
echo "listener: rate {$ls['sample_rate']} ch {$ls['channels']} up ", json_encode($ls['up']), " c ", round($ls['speed_of_sound'], 1), "\n";

const RATE = 44100;
// 1 s mono 1 kHz sine at amplitude 0.5 (exactly 1000 cycles: loops seamlessly)
function tone(): string {
    $pcm = '';
    for ($i = 0; $i < RATE; $i++) $pcm .= pack('v', (int) round(sin($i * 2 * M_PI * 1000 / RATE) * 16384) & 0xFFFF);
    return 'RIFF' . pack('V', 36 + strlen($pcm)) . 'WAVE' . 'fmt ' . pack('VvvVVvv', 16, 1, 1, RATE, RATE * 2, 2, 16)
         . 'data' . pack('V', strlen($pcm)) . $pcm;
}
// Discard the smoothing/resampler settle time, then measure 0.1 s.
function block(): array {
    vio_audio_render(2048);
    $f = array_values(unpack('g*', vio_audio_render(4410)));
    $l = $r = [];
    for ($i = 0; $i < count($f); $i += 2) { $l[] = $f[$i]; $r[] = $f[$i + 1]; }
    return [$l, $r];
}
function rms(array $x): float { $s = 0.0; foreach ($x as $v) $s += $v * $v; return sqrt($s / max(1, count($x))); }
function freq(array $x): float {
    $first = $last = null; $n = 0;
    for ($i = 1; $i < count($x); $i++) {
        if ($x[$i - 1] < 0 && $x[$i] >= 0) {
            $t = $i - 1 + $x[$i - 1] / ($x[$i - 1] - $x[$i]);
            if ($first === null) $first = $t;
            $last = $t; $n++;
        }
    }
    return $n > 1 ? ($n - 1) * RATE / ($last - $first) : 0.0;
}
function near(float $a, float $b, float $tol): string { return abs($a - $b) <= $tol * max(abs($b), 1e-9) ? 'ok' : sprintf('FAIL (%.4f, want %.4f)', $a, $b); }
function mono(): array { [$l, $r] = block(); return [rms($l), rms($r), freq($l)]; }

$wav = tone();

// ── defaults ────────────────────────────────────────────────────────
$s = vio_audio_load_data($wav);
$st = vio_audio_state($s);
echo "defaults: pitch {$st['pitch']} pan {$st['pan']} spatial ", var_export($st['spatial'], true),
     " relative ", var_export($st['relative'], true), " {$st['attenuation']} min {$st['min_distance']} max ",
     is_infinite($st['max_distance']) ? 'INF' : $st['max_distance'], " rolloff {$st['rolloff']} doppler {$st['doppler']}",
     " length ", round($st['length'], 3), "\n";

// ── 2D sound: spatialization off, the listener does not matter ──────
vio_audio_play($s, ['loop' => true, 'spatial' => false]);
[$l0, $r0, $f0] = mono();
echo "2D: L ", near($l0, 0.5 / sqrt(2), 0.03), " R ", near($r0, $l0, 0.01), " f ", near($f0, 1000, 0.01), "\n";
vio_audio_listener(1000.0, 0.0, 0.0, 1.0, 0.0, 0.0);
vio_audio_position($s, -500.0, 0.0, 0.0);
[$l, $r] = mono();
echo "2D far listener: L ", near($l, $l0, 0.01), " R ", near($r, $r0, 0.01), "\n";
vio_audio_listener(0.0, 0.0, 0.0, 0.0, 0.0, -1.0);

// ── live pan ────────────────────────────────────────────────────────
vio_audio_pan($s, -1.0);
[$l, $r] = mono();
echo "pan -1: L ", near($l, $l0, 0.02), " R silent ", var_export($r < 0.01 * $l0, true), "\n";
vio_audio_pan($s, 5.0);                              // clamped to 1
[$l, $r] = mono();
echo "pan 5 (clamped): L silent ", var_export($l < 0.01 * $l0, true), " R ", near($r, $r0, 0.02), " state ", vio_audio_state($s)['pan'], "\n";
vio_audio_pan($s, 0.0);

// ── live pitch ──────────────────────────────────────────────────────
vio_audio_pitch($s, 2.0);
[, , $f] = mono();
echo "pitch 2: f ", near($f, 2000, 0.02), "\n";
vio_audio_pitch($s, 0.5);
[, , $f] = mono();
echo "pitch 0.5: f ", near($f, 500, 0.02), "\n";
vio_audio_stop($s);

// ── 3D sound in front of the listener: attenuation models ──────────
$p = vio_audio_load_data($wav);
vio_audio_play($p, ['loop' => true, 'position' => [0, 0, -1]]);
[$l1, $r1] = mono();
$ref = $l1 + $r1;
$gain = function () use ($ref): float { [$l, $r] = mono(); return ($l + $r) / $ref; };
vio_audio_position($p, 0.0, 0.0, -10.0);
echo "inverse d=10: ", near($gain(), 0.1, 0.03), "\n";
vio_audio_attenuation($p, 'inverse', 10.0);
echo "inverse min 10: ", near($gain(), 1.0, 0.03), "\n";
vio_audio_attenuation($p, 'linear', 1.0, 11.0);
echo "linear 1..11: ", near($gain(), 0.1, 0.03), "\n";
vio_audio_attenuation($p, 'exponential', rolloff: 2.0);
echo "exponential rolloff 2: ", near($gain(), 0.01, 0.05), "\n";
vio_audio_attenuation($p, 'inverse', rolloff: 1.0, min_gain: 0.5);
echo "inverse min_gain 0.5: ", near($gain(), 0.5, 0.03), "\n";
vio_audio_attenuation($p, 'inverse', min_gain: 0.0, max_distance: 5.0);
echo "inverse max_distance 5: ", near($gain(), 0.2, 0.03), "\n";
vio_audio_attenuation($p, 'inverse', max_distance: INF);
$st = vio_audio_state($p);
echo "state: {$st['attenuation']} min {$st['min_distance']} max ", is_infinite($st['max_distance']) ? 'INF' : $st['max_distance'],
     " rolloff {$st['rolloff']} gains {$st['min_gain']}..{$st['max_gain']}\n";

// ── cone: facing away from the listener → outer gain ────────────────
vio_audio_position($p, 0.0, 0.0, -1.0);
vio_audio_cone($p, 0.5, 1.0, 0.25);
vio_audio_direction($p, 0.0, 0.0, 1.0);              // towards the listener
echo "cone facing: ", near($gain(), 1.0, 0.03), "\n";
vio_audio_direction($p, 0.0, 0.0, -1.0);             // away
echo "cone away: ", near($gain(), 0.25, 0.03), "\n";
vio_audio_cone($p, 2 * M_PI, 2 * M_PI, 1.0);
echo "cone state: ", json_encode(array_map(fn($v) => round($v, 3), vio_audio_state($p)['cone'])), "\n";

// ── left/right and the listener up vector ──────────────────────────
vio_audio_position($p, 5.0, 0.0, 0.0);               // to the right (facing -Z, up +Y)
[$l, $r] = mono();
echo "source right: R > L ", var_export($r > 2 * $l, true), "\n";
vio_audio_listener(0.0, 0.0, 0.0, 0.0, 0.0, -1.0, 0.0, -1.0, 0.0);   // upside down
[$l, $r] = mono();
echo "upside down: L > R ", var_export($l > 2 * $r, true), " up ", json_encode(vio_audio_listener_state()['up']), "\n";
vio_audio_listener(0.0, 0.0, 0.0, 0.0, 0.0, -1.0);   // six arguments keep the up vector
echo "up kept: ", json_encode(vio_audio_listener_state()['up']), "\n";
vio_audio_listener(0.0, 0.0, 0.0, 0.0, 0.0, -1.0, 0.0, 1.0, 0.0);

// ── relative positioning follows the listener ───────────────────────
vio_audio_position($p, 0.0, 0.0, -10.0);
$g = $gain();
vio_audio_spatial($p, true, true);
vio_audio_listener(1000.0, 50.0, 0.0, 0.0, 0.0, -1.0);
echo "relative: ", near($gain(), $g, 0.03), " state ", var_export(vio_audio_state($p)['relative'], true), "\n";
vio_audio_spatial($p, true, false);
echo "absolute again, far: ", var_export($gain() < 0.02, true), "\n";
vio_audio_listener(0.0, 0.0, 0.0, 0.0, 0.0, -1.0);

// ── Doppler ─────────────────────────────────────────────────────────
vio_audio_position($p, 0.0, 0.0, -50.0);
vio_audio_velocity($p, 0.0, 0.0, 100.0);             // approaching at 100 u/s
[, , $f] = mono();
echo "source approaching: f ", near($f, 1000 * 343.3 / 243.3, 0.02), "\n";
vio_audio_velocity($p, 0.0, 0.0, -100.0);            // receding
[, , $f] = mono();
echo "source receding: f ", near($f, 1000 * 343.3 / 443.3, 0.02), "\n";
vio_audio_speed_of_sound(686.6);
[, , $f] = mono();
echo "c = 686.6: f ", near($f, 1000 * 686.6 / 786.6, 0.02), "\n";
vio_audio_speed_of_sound(343.3);
vio_audio_doppler($p, 0.0);
[, , $f] = mono();
echo "doppler 0: f ", near($f, 1000, 0.01), "\n";
vio_audio_doppler($p, 1.0);
vio_audio_velocity($p, 0.0, 0.0, 0.0);
vio_audio_listener_velocity(0.0, 0.0, -100.0);       // listener moves towards the source
[, , $f] = mono();
echo "listener approaching: f ", near($f, 1000 * 443.3 / 343.3, 0.02), "\n";
echo "listener velocity: ", json_encode(vio_audio_listener_state()['velocity']), "\n";
vio_audio_listener_velocity(0.0, 0.0, 0.0);

// ── play options: validated before anything changes ─────────────────
$q = vio_audio_load_data($wav);
foreach ([
    ['attenuation' => 'bogus'], ['position' => [1, 2]], ['velocity' => 'fast'], ['doppler' => -1],
    ['min_distance' => 5, 'max_distance' => 2], ['rolloff' => -0.5],
] as $bad) {
    try { vio_audio_play($q, $bad + ['spatial' => false]); }
    catch (ValueError|TypeError $e) { echo get_class($e), ': ', $e->getMessage(), "\n"; }
}
$st = vio_audio_state($q);
echo "after rejected plays: playing ", var_export($st['playing'], true), " spatial ", var_export($st['spatial'], true), "\n";
vio_audio_play($q, ['spatial' => true, 'position' => ['x' => 1, 'y' => 2, 'z' => 3], 'velocity' => [4, 5, 6],
    'direction' => [0, 0, 1], 'doppler' => 0.5, 'attenuation' => 'Linear', 'min_distance' => 2, 'max_distance' => 20,
    'rolloff' => 0.5, 'min_gain' => 0.1, 'max_gain' => 0.9, 'relative' => true, 'pitch' => 1.5, 'pan' => 0.25]);
$st = vio_audio_state($q);
echo "play options: ", json_encode([$st['playing'], $st['spatial'], $st['relative'], $st['position'], $st['velocity'], $st['direction'],
    $st['doppler'], $st['attenuation'], $st['min_distance'], $st['max_distance'], $st['rolloff'], round($st['min_gain'], 2),
    round($st['max_gain'], 2), $st['pitch'], $st['pan']]), "\n";

// ── argument errors ─────────────────────────────────────────────────
$calls = [
    fn() => vio_audio_pitch($q, 0.0),
    fn() => vio_audio_pitch($q, INF),
    fn() => vio_audio_pan($q, NAN),
    fn() => vio_audio_doppler($q, -0.1),
    fn() => vio_audio_attenuation($q, 'cubic'),
    fn() => vio_audio_attenuation($q, 'inverse', 10.0, 5.0),
    fn() => vio_audio_attenuation($q, 'inverse', max_distance: 1.0),   // below current min 2
    fn() => vio_audio_attenuation($q, 'inverse', min_gain: 0.95),      // above current max 0.9
    fn() => vio_audio_attenuation($q, 'inverse', -1.0),
    fn() => vio_audio_cone($q, 1.0, 0.5, 0.5),
    fn() => vio_audio_cone($q, 0.0, 1.0, 1.5),
    fn() => vio_audio_listener(0, 0, 0, 0, 0, -1, 0.0),
    fn() => vio_audio_listener(0, 0, 0, 0, 0, -1, 0.0, 0.0, 0.0),
    fn() => vio_audio_speed_of_sound(0.0),
    fn() => vio_audio_render(-1),
    fn() => vio_audio_init(['device' => false, 'backends' => ['null']]),
    fn() => vio_audio_init(['sample_rate' => 48000]),
];
foreach ($calls as $c) {
    try { $c(); echo "no error\n"; }
    catch (ValueError $e) { echo $e->getMessage(), "\n"; }
}
echo "state unchanged: ", vio_audio_state($q)['attenuation'], " ", vio_audio_state($q)['min_distance'], "\n";

var_dump(strlen(vio_audio_render(0)), strlen(vio_audio_render(10)));
echo "OK\n";
?>
--EXPECT--
bool(false)
bool(true)
string(7) "offline"
bool(true)
bool(false)
listener: rate 44100 ch 2 up [0,1,0] c 343.3
defaults: pitch 1 pan 0 spatial true relative false inverse min 1 max INF rolloff 1 doppler 1 length 1
2D: L ok R ok f ok
2D far listener: L ok R ok
pan -1: L ok R silent true
pan 5 (clamped): L silent true R ok state 1
pitch 2: f ok
pitch 0.5: f ok
inverse d=10: ok
inverse min 10: ok
linear 1..11: ok
exponential rolloff 2: ok
inverse min_gain 0.5: ok
inverse max_distance 5: ok
state: inverse min 1 max INF rolloff 1 gains 0..1
cone facing: ok
cone away: ok
cone state: {"inner_angle":6.283,"outer_angle":6.283,"outer_gain":1}
source right: R > L true
upside down: L > R true up [0,-1,0]
up kept: [0,-1,0]
relative: ok state true
absolute again, far: true
source approaching: f ok
source receding: f ok
c = 686.6: f ok
doppler 0: f ok
listener approaching: f ok
listener velocity: [0,0,-100]
ValueError: vio_audio_play(): Argument #2 ($options) option "attenuation" must be one of "none", "inverse", "linear", "exponential"
ValueError: vio_audio_play(): Argument #2 ($options) option "position" must be [x, y, z]
TypeError: vio_audio_play(): Argument #2 ($options) option "velocity" must be of type array, string given
ValueError: vio_audio_play(): Argument #2 ($options) option "doppler" must be a finite number greater than or equal to 0
ValueError: vio_audio_play(): Argument #2 ($options) max_distance (2) must not be less than min_distance (5)
ValueError: vio_audio_play(): Argument #2 ($options) rolloff must be a finite number greater than or equal to 0
after rejected plays: playing false spatial true
play options: [true,true,true,[1,2,3],[4,5,6],[0,0,1],0.5,"linear",2,20,0.5,0.1,0.9,1.5,0.25]
vio_audio_pitch(): Argument #2 ($pitch) must be a finite number greater than 0
vio_audio_pitch(): Argument #2 ($pitch) must be a finite number greater than 0
vio_audio_pan(): Argument #2 ($pan) must not be NAN
vio_audio_doppler(): Argument #2 ($factor) must be a finite number greater than or equal to 0
vio_audio_attenuation(): Argument #2 ($model) must be one of "none", "inverse", "linear", "exponential"
vio_audio_attenuation(): Argument #4 ($max_distance) max_distance (5) must not be less than min_distance (10)
vio_audio_attenuation(): Argument #4 ($max_distance) max_distance (1) must not be less than min_distance (2)
vio_audio_attenuation(): Argument #6 ($min_gain) max_gain (0.9) must not be less than min_gain (0.95)
vio_audio_attenuation(): Argument #3 ($min_distance) must be a finite number greater than or equal to 0
vio_audio_cone(): Argument #3 ($outer_angle) must be between $inner_angle and 2*M_PI (radians)
vio_audio_cone(): Argument #4 ($outer_gain) must be between 0 and 1
vio_audio_listener(): Argument #7 ($up_x) the up vector needs all three of $up_x, $up_y, $up_z (or none)
vio_audio_listener(): Argument #7 ($up_x) the up vector must not be zero
vio_audio_speed_of_sound(): Argument #1 ($speed) must be a finite number greater than 0
vio_audio_render(): Argument #1 ($frames) must be between 0 and 2880000
vio_audio_init(): Argument #1 ($options) options "backends" and "device" => false exclude each other
vio_audio_init(): Argument #1 ($options) options "sample_rate" and "channels" need "device" => false
state unchanged: linear 2
int(0)
int(80)
OK
