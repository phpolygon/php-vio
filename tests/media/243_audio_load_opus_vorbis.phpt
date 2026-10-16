--TEST--
vio_audio_load / vio_audio_load_data play Ogg Opus, Ogg Vorbis and stream-wrapper paths
--EXTENSIONS--
vio
--ENV--
VIO_AUDIO_BACKEND=null
--FILE--
<?php
$fx = __DIR__ . '/fixtures';

// a short WAV for the stream-wrapper path
$n = 2205;
$wav = 'RIFF' . pack('V', 36 + $n * 2) . 'WAVE' . 'fmt ' . pack('VvvVVvv', 16, 1, 1, 44100, 88200, 2, 16)
     . 'data' . pack('V', $n * 2) . str_repeat("\x00\x00", $n);

$sounds = [
    'opus file'   => vio_audio_load("$fx/tone440_mono_24k.opus"),
    'vorbis file' => vio_audio_load("$fx/tone440_stereo_44k.ogg"),
    'opus bytes'  => vio_audio_load_data(file_get_contents("$fx/tone440_mono_24k.opus")),
    'vorbis bytes'=> vio_audio_load_data(file_get_contents("$fx/tone440_stereo_44k.ogg")),
    'encoded'     => vio_audio_load_data(vio_audio_encode(str_repeat(pack('s', 0), 4800), 48000, 1)),
    'wav bytes'   => vio_audio_load_data($wav),
    'wav data://' => vio_audio_load('data://audio/wav;base64,' . base64_encode($wav)),
];
foreach ($sounds as $name => $s) {
    echo $name, ': ', $s instanceof VioSound ? 'VioSound' : var_export($s, true), "\n";
}

// playback lifecycle on a decoded-from-memory source (silent)
$s = $sounds['opus file'];
vio_audio_play($s, ['volume' => 0.0, 'loop' => true]);
var_dump(vio_audio_playing($s));
vio_audio_pause($s);
var_dump(vio_audio_playing($s));
vio_audio_resume($s);
vio_audio_stop($s);
var_dump(vio_audio_playing($s));
// the audio thread decodes a 0.5 s file to its end and the sound stops by itself
function play_until_end(VioSound $s): float {
    vio_audio_play($s, ['volume' => 0.0]);
    $t = microtime(true);
    while (vio_audio_playing($s) && microtime(true) - $t < 5.0) usleep(10000);
    if (vio_audio_playing($s)) return -1.0;
    return microtime(true) - $t;
}
// the null backend clocks the mixer in real time: a 0.5 s source is pulled
// through decoder and mixer for ~0.5 s and then stops by itself
var_dump(vio_audio_backend());
$n2 = 22050;
$wavHalf = 'RIFF' . pack('V', 36 + $n2 * 2) . 'WAVE' . 'fmt ' . pack('VvvVVvv', 16, 1, 1, 44100, 88200, 2, 16)
         . 'data' . pack('V', $n2 * 2) . str_repeat("\x00\x00", $n2);
foreach (['wav (control)' => vio_audio_load_data($wavHalf), 'opus' => $sounds['opus bytes'], 'vorbis' => $sounds['vorbis file']] as $name => $snd) {
    $t = play_until_end($snd);
    echo $name, ': ', $t > 0.35 && $t < 3.0 ? 'played ~0.5 s' : sprintf('%.3f s', $t), "\n";
}
vio_audio_play($sounds['vorbis bytes'], ['volume' => 0.0]);
usleep(20000);
unset($sounds); // frees sounds while one plays

// errors
var_dump(vio_audio_load_data('not audio at all'));
try {
    vio_audio_load_data('');
} catch (ValueError $e) {
    echo $e->getMessage(), "\n";
}
var_dump(vio_audio_load("$fx/missing.opus"));
echo "OK\n";
?>
--EXPECTF--
opus file: VioSound
vorbis file: VioSound
opus bytes: VioSound
vorbis bytes: VioSound
encoded: VioSound
wav bytes: VioSound
wav data://: VioSound
bool(true)
bool(false)
bool(false)
string(4) "null"
wav (control): played ~0.5 s
opus: played ~0.5 s
vorbis: played ~0.5 s

Warning: vio_audio_load_data(): Failed to load audio file "(memory)" (error %i) in %s on line %d
bool(false)
vio_audio_load_data(): Argument #1 ($data) must not be empty

Warning: vio_audio_load(): Failed to load audio file "%s" (error %i) in %s on line %d
bool(false)
OK
