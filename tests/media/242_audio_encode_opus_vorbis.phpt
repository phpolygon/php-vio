--TEST--
vio_audio_encode: Ogg Opus / Ogg Vorbis round trip (sample-exact length, signal kept, deterministic bytes, errors)
--EXTENSIONS--
vio
--FILE--
<?php
/** Interleaved s16 PCM: 440 Hz left, 660 Hz right (or mono 440 Hz). */
function tone(int $rate, int $channels, float $seconds): string {
    $pcm = '';
    $n = (int) round($rate * $seconds);
    for ($k = 0; $k < $n; $k++) {
        $pcm .= pack('s', (int) round(sin(2 * M_PI * 440 * $k / $rate) * 12000));
        if ($channels === 2) {
            $pcm .= pack('s', (int) round(sin(2 * M_PI * 660 * $k / $rate) * 9000));
        }
    }
    return $pcm;
}

/** SNR in dB of $got against $ref (both s16 interleaved, same rate), skipping the edges. */
function snr(string $ref, string $got, int $step = 1): float {
    $r = unpack('s*', $ref);
    $g = unpack('s*', $got);
    $sig = 0.0; $err = 0.0;
    $n = count($r);
    for ($k = (int) ($n * 0.1); $k < (int) ($n * 0.9); $k++) {
        $x = $r[$k + 1];
        $y = $g[$k * $step + 1];
        $sig += $x * $x; $err += ($x - $y) * ($x - $y);
    }
    return 10 * log10($sig / max($err, 1e-9));
}

// Opus, stereo, 48 kHz, 128 kbps: decodes at 48 kHz, sample-aligned (pre-skip and end trim)
$pcm = tone(48000, 2, 1.0);
$opus = vio_audio_encode($pcm, 48000, 2, 'opus', ['bitrate' => 128000]);
var_dump(substr($opus, 0, 4) === 'OggS', str_contains(substr($opus, 0, 64), 'OpusHead'));
$d = vio_audio_decode($opus, ['data' => true]);
echo json_encode(array_diff_key($d, ['pcm' => 1])), "\n";
var_dump(snr($pcm, $d['pcm']) > 20);

// Opus, mono speech preset at 24 kHz: OpusHead keeps the input rate, playback is 48 kHz
$pcm = tone(24000, 1, 0.75);
$opus = vio_audio_encode($pcm, 24000, 1, 'opus', ['bitrate' => 32000, 'application' => 'voip', 'signal' => 'voice']);
$d = vio_audio_decode($opus, ['data' => true]);
echo json_encode(array_diff_key($d, ['pcm' => 1])), "\n";
var_dump(snr($pcm, $d['pcm'], 2) > 6);
var_dump(strlen($opus) < strlen($pcm) / 8);
foreach (['frame_ms' => [10, 40, 60], 'complexity' => [0], 'vbr' => [false], 'application' => ['lowdelay'], 'signal' => ['music']] as $k => $vals) {
    foreach ($vals as $v) {
        $o = vio_audio_encode($pcm, 24000, 1, 'opus', [$k => $v]);
        echo $k, '=', var_export($v, true), ': ', vio_audio_info($o, ['data' => true])['frames'], "\n";
    }
}

// Vorbis, stereo, 44.1 kHz, quality 0.5 (oggenc -q5) and managed bitrate
$pcm = tone(44100, 2, 1.0);
$ogg = vio_audio_encode($pcm, 44100, 2, 'vorbis');
var_dump(str_contains(substr($ogg, 0, 64), "\x01vorbis"));
$d = vio_audio_decode($ogg, ['data' => true]);
echo json_encode(array_diff_key($d, ['pcm' => 1])), "\n";
var_dump(snr($pcm, $d['pcm']) > 20);
$abr = vio_audio_encode($pcm, 44100, 2, 'vorbis', ['bitrate' => 64000]);
var_dump(vio_audio_info($abr, ['data' => true])['frames'] === 44100);
$mono = vio_audio_encode(tone(22050, 1, 0.5), 22050, 1, 'vorbis', ['quality' => 0.1]);
echo json_encode(vio_audio_info($mono, ['data' => true])), "\n";

// deterministic: same input + options = same bytes; another serial changes them
var_dump(vio_audio_encode($pcm, 44100, 2, 'vorbis') === $ogg);
$a = vio_audio_encode($pcm, 48000, 2);
var_dump($a === vio_audio_encode($pcm, 48000, 2), $a !== vio_audio_encode($pcm, 48000, 2, 'opus', ['serial' => 1234]));

// errors
var_dump(vio_audio_encode(tone(44100, 1, 0.1), 44100, 1, 'opus'));
var_dump(vio_audio_encode('', 48000, 1, 'opus'));
var_dump(vio_audio_encode($pcm, 44100, 2, 'vorbis', ['quality' => 3.0]));
var_dump(vio_audio_encode(tone(48000, 1, 0.1), 48000, 1, 'opus', ['frame_ms' => 7]));
var_dump(vio_audio_encode(tone(48000, 1, 0.1), 48000, 1, 'opus', ['complexity' => 11]));
foreach ([
    fn() => vio_audio_encode("\x00\x00\x00", 48000, 1),
    fn() => vio_audio_encode($pcm, 48000, 3),
    fn() => vio_audio_encode($pcm, 48000, 2, 'mp3'),
    fn() => vio_audio_encode($pcm, 48000, 2, 'opus', ['application' => 'radio']),
    fn() => vio_audio_encode($pcm, 48000, 2, 'opus', ['bitrate' => '64k']),
] as $fn) {
    try { $fn(); } catch (ValueError|TypeError $e) { echo get_class($e), ': ', $e->getMessage(), "\n"; }
}
echo "OK\n";
?>
--EXPECTF--
bool(true)
bool(true)
{"format":"opus","channels":2,"sample_rate":48000,"input_sample_rate":48000,"frames":48000,"duration":1}
bool(true)
{"format":"opus","channels":1,"sample_rate":48000,"input_sample_rate":24000,"frames":36000,"duration":0.75}
bool(true)
bool(true)
frame_ms=10: 36000
frame_ms=40: 36000
frame_ms=60: 36000
complexity=0: 36000
vbr=false: 36000
application='lowdelay': 36000
signal='music': 36000
bool(true)
{"format":"vorbis","channels":2,"sample_rate":44100,"input_sample_rate":44100,"frames":44100,"duration":1}
bool(true)
bool(true)
{"format":"vorbis","channels":1,"sample_rate":22050,"input_sample_rate":22050,"frames":11025,"duration":0.5}
bool(true)
bool(true)
bool(true)

Warning: vio_audio_encode(): Opus needs a sample rate of 8000, 12000, 16000, 24000 or 48000 Hz, got 44100 (resample first) in %s on line %d
bool(false)

Warning: vio_audio_encode(): no PCM frames to encode in %s on line %d
bool(false)

Warning: vio_audio_encode(): Vorbis quality must be between -0.1 and 1.0, got 3 in %s on line %d
bool(false)

Warning: vio_audio_encode(): frame_ms must be 10, 20, 40 or 60, got 7 in %s on line %d
bool(false)

Warning: vio_audio_encode(): invalid Opus encoder option (bitrate 500..512000, complexity 0..10) in %s on line %d
bool(false)
ValueError: vio_audio_encode(): Argument #1 ($pcm) length must be a multiple of 2 * $channels bytes (interleaved signed 16-bit)
ValueError: vio_audio_encode(): Argument #3 ($channels) must be 1 or 2
ValueError: vio_audio_encode(): Argument #4 ($codec) must be "opus" or "vorbis"
ValueError: vio_audio_encode(): Argument #5 ($options) option "application" must be "audio", "voip" or "lowdelay"
TypeError: vio_audio_encode(): Argument #5 ($options) option "bitrate" must be of type int, string given
OK
