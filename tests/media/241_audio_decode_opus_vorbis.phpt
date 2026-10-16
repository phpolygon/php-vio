--TEST--
Ogg Opus / Ogg Vorbis decoding: vio_audio_formats, vio_audio_info, vio_audio_decode (files made by ffmpeg's libopus/libvorbis)
--EXTENSIONS--
vio
--FILE--
<?php
$fx = __DIR__ . '/fixtures';

$f = vio_audio_formats();
var_dump(in_array('opus', $f['decode'], true), in_array('vorbis', $f['decode'], true));
var_dump(in_array('flac', $f['decode'], true), in_array('wav', $f['decode'], true), in_array('mp3', $f['decode'], true));
var_dump($f['encode'] === ['opus', 'vorbis']);
var_dump(str_starts_with($f['versions']['opus'], 'libopus 1.'), str_contains($f['versions']['vorbis'], 'libVorbis'));

/** Mono channel 0 as ints, zero-crossing frequency and RMS. */
function analyse(array $d): array {
    $s = unpack('s*', $d['pcm']);
    $ch = $d['channels'];
    $zc = 0; $sum = 0.0; $prev = null; $n = 0;
    for ($k = 1; $k <= count($s); $k += $ch) {
        $v = $s[$k];
        if ($prev !== null && ($prev < 0) !== ($v < 0)) $zc++;
        $prev = $v; $sum += $v * $v; $n++;
    }
    return [$zc / 2 / $d['duration'], sqrt($sum / $n)];
}

// 0.5 s, 440 Hz sine, mono, encoded at 24 kHz / 32 kbps (voip)
$i = vio_audio_info("$fx/tone440_mono_24k.opus");
echo json_encode($i), "\n";
$d = vio_audio_decode("$fx/tone440_mono_24k.opus");
var_dump($d['frames'] === 24000, strlen($d['pcm']) === 24000 * 2);
[$hz, $rms] = analyse($d);
var_dump(abs($hz - 440) < 5, $rms > 1000);

// 0.5 s, 440 Hz sine, stereo, 44.1 kHz, Vorbis q5
$i = vio_audio_info("$fx/tone440_stereo_44k.ogg");
echo json_encode($i), "\n";
$d = vio_audio_decode("$fx/tone440_stereo_44k.ogg", ['format' => 'f32']);
var_dump($d['frames'] === 22050, strlen($d['pcm']) === 22050 * 2 * 4);
$f32 = unpack('g*', $d['pcm']);
var_dump(max($f32) < 1.0 && max($f32) > 0.05);
$d16 = vio_audio_decode("$fx/tone440_stereo_44k.ogg");
[$hz] = analyse($d16);
var_dump(abs($hz - 440) < 5);

// bytes instead of a path; any PHP stream wrapper for paths
$bytes = file_get_contents("$fx/tone440_mono_24k.opus");
var_dump(vio_audio_info($bytes, ['data' => true])['frames'] === 24000);
var_dump(vio_audio_info('data://application/octet-stream;base64,' . base64_encode($bytes))['format'] === 'opus');

// errors
var_dump(@vio_audio_info(__DIR__ . '/fixtures/does-not-exist.opus'));
var_dump(vio_audio_info('definitely not audio', ['data' => true]));
var_dump(vio_audio_decode(substr($bytes, 0, 40), ['data' => true]));
try {
    vio_audio_decode("$fx/tone440_mono_24k.opus", ['format' => 'u8']);
} catch (ValueError $e) {
    echo $e->getMessage(), "\n";
}
echo "OK\n";
?>
--EXPECTF--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
{"format":"opus","channels":1,"sample_rate":48000,"input_sample_rate":24000,"frames":24000,"duration":0.5}
bool(true)
bool(true)
bool(true)
bool(true)
{"format":"vorbis","channels":2,"sample_rate":44100,"input_sample_rate":44100,"frames":22050,"duration":0.5}
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(false)

Warning: vio_audio_info(): Cannot decode audio (unsupported or corrupt data, error %i) in %s on line %d
bool(false)

Warning: vio_audio_decode(): Cannot decode audio (unsupported or corrupt data, error %i) in %s on line %d
bool(false)
vio_audio_decode(): Argument #2 ($options) option "format" must be "s16" or "f32"
OK
