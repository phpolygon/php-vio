--TEST--
vio_audio_init / vio_audio_backend / vio_audio_backends: choosing the miniaudio backend, the null device plays in real time
--EXTENSIONS--
vio
--ENV--
VIO_AUDIO_BACKEND=
--FILE--
<?php
$all = vio_audio_backends();
var_dump(in_array('null', $all, true), end($all));
var_dump(vio_audio_backend());          // not initialized yet

foreach ([['backends' => ['nosuchcard']], ['backends' => []], ['backends' => 'null']] as $bad) {
    try {
        vio_audio_init($bad);
    } catch (ValueError|TypeError $e) {
        echo get_class($e), ': ', $e->getMessage(), "\n";
    }
}
var_dump(vio_audio_backend());          // a failed call does not initialize

var_dump(vio_audio_init(['backends' => ['NULL']]));
var_dump(vio_audio_backend());
var_dump(vio_audio_init());             // already running: fine
var_dump(vio_audio_init(['backends' => ['alsa', 'wasapi', 'coreaudio']]));

// 0.25 s of a 1 kHz tone at 22.05 kHz: resampled and mixed on the null device's clock
$n = 5512;
$pcm = '';
for ($i = 0; $i < $n; $i++) $pcm .= pack('v', (int) (sin($i * 2 * M_PI * 1000 / 22050) * 8000) & 0xFFFF);
$wav = 'RIFF' . pack('V', 36 + $n * 2) . 'WAVE' . 'fmt ' . pack('VvvVVvv', 16, 1, 1, 22050, 44100, 2, 16)
     . 'data' . pack('V', $n * 2) . $pcm;
$s = vio_audio_load_data($wav);
vio_audio_play($s, ['volume' => 0.0]);
$t = microtime(true);
usleep(100000);
var_dump(vio_audio_playing($s));        // still playing after 0.1 s
while (vio_audio_playing($s) && microtime(true) - $t < 5.0) usleep(5000);
$dt = microtime(true) - $t;
var_dump(!vio_audio_playing($s), $dt > 0.18 && $dt < 2.0);
echo "OK\n";
?>
--EXPECTF--
bool(true)
string(4) "null"
bool(false)
ValueError: vio_audio_init(): Argument #1 ($options) option "backends" contains an unknown audio backend %s
ValueError: vio_audio_init(): Argument #1 ($options) option "backends" must not be empty
TypeError: vio_audio_init(): Argument #1 ($options) option "backends" must be of type array, string given
bool(false)
bool(true)
string(4) "null"
bool(true)

Warning: vio_audio_init(): Audio engine already initialized (backend "null"); vio_audio_init() must come before the first sound in %s on line %d
bool(false)
bool(true)
bool(true)
bool(true)
OK
