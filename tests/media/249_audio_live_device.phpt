--TEST--
Live audio parameters on a device engine (null backend): listener calls before init warn, setters reach the playing sound, vio_audio_render needs an offline engine
--EXTENSIONS--
vio
--ENV--
VIO_AUDIO_BACKEND=
--FILE--
<?php
// Before the engine runs: listener calls warn (as vio_audio_listener always did), no state.
vio_audio_listener_velocity(1.0, 0.0, 0.0);
vio_audio_speed_of_sound(340.0);
var_dump(vio_audio_listener_state(), vio_audio_render(16));

var_dump(vio_audio_init(['backends' => ['null']]));
var_dump(vio_audio_backend());
var_dump(@vio_audio_init(['device' => false]));     // device engine already runs
var_dump(vio_audio_render(16));

$n = 22050;
$pcm = '';
for ($i = 0; $i < $n; $i++) $pcm .= pack('v', (int) (sin($i * 2 * M_PI * 440 / 22050) * 8000) & 0xFFFF);
$wav = 'RIFF' . pack('V', 36 + $n * 2) . 'WAVE' . 'fmt ' . pack('VvvVVvv', 16, 1, 1, 22050, 44100, 2, 16)
     . 'data' . pack('V', $n * 2) . $pcm;

// Music-style 2D sound: spatialization off at play time, then live changes while it plays.
$m = vio_audio_load_data($wav);
vio_audio_play($m, ['volume' => 0.0, 'loop' => true, 'spatial' => false]);
vio_audio_pitch($m, 1.25);
vio_audio_pan($m, -0.5);
vio_audio_velocity($m, 1.0, 2.0, 3.0);
vio_audio_direction($m, 0.0, 0.0, 1.0);
vio_audio_doppler($m, 0.0);
vio_audio_attenuation($m, 'exponential', 2.0, 50.0, 1.5);
vio_audio_cone($m, M_PI / 2, M_PI, 0.3);
usleep(50000);
$st = vio_audio_state($m);
echo json_encode([$st['playing'], $st['looping'], $st['spatial'], $st['pitch'], $st['pan'], $st['velocity'],
    $st['direction'], $st['doppler'], $st['attenuation'], $st['min_distance'], $st['max_distance'], $st['rolloff'],
    round($st['cone']['inner_angle'], 4), round($st['cone']['outer_angle'], 4), round($st['cone']['outer_gain'], 2)]), "\n";
var_dump($st['cursor'] > 0.0, abs($st['length'] - 1.0) < 0.001);

// Listener: the classic six-argument call, then with an up vector, plus velocity and speed of sound.
vio_audio_listener(1.0, 2.0, 3.0, 0.0, 0.0, -1.0);
vio_audio_listener(1.0, 2.0, 3.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0);
vio_audio_listener_velocity(0.0, 0.0, -5.0);
vio_audio_speed_of_sound(340.0);
$ls = vio_audio_listener_state();
echo json_encode([$ls['position'], $ls['direction'], $ls['up'], $ls['velocity'], $ls['speed_of_sound'], $ls['channels']]), "\n";

vio_audio_spatial($m, true, true);
$st = vio_audio_state($m);
var_dump($st['spatial'], $st['relative']);
vio_audio_stop($m);
var_dump(vio_audio_state($m)['playing']);

// A VioSound that never loaded: setters do nothing, state says so.
$empty = new VioSound();
vio_audio_pitch($empty, 2.0);
vio_audio_attenuation($empty, 'linear', 1.0, 2.0);
var_dump(vio_audio_state($empty));
echo "OK\n";
?>
--EXPECTF--
Warning: vio_audio_listener_velocity(): Audio engine not initialized in %s on line %d

Warning: vio_audio_speed_of_sound(): Audio engine not initialized in %s on line %d

Warning: vio_audio_render(): vio_audio_render() needs an offline engine: vio_audio_init(['device' => false]) before the first sound in %s on line %d
bool(false)
bool(false)
bool(true)
string(4) "null"
bool(false)

Warning: vio_audio_render(): vio_audio_render() needs an offline engine: vio_audio_init(['device' => false]) before the first sound in %s on line %d
bool(false)
[true,true,false,1.25,-0.5,[1,2,3],[0,0,1],0,"exponential",2,50,1.5,1.5708,3.1416,0.3]
bool(true)
bool(true)
[[1,2,3],[1,0,0],[0,0,1],[0,0,-5],340,2]
bool(true)
bool(true)
bool(false)
array(1) {
  ["loaded"]=>
  bool(false)
}
OK
