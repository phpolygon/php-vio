--TEST--
Audio playback lifecycle: load, play, pause, resume, stop, volume
--EXTENSIONS--
vio
--FILE--
<?php
// Generate a minimal WAV file (44100Hz, mono, 16-bit, 0.1 sec silence)
$samples = 4410; // 0.1 seconds
$data_size = $samples * 2; // 16-bit = 2 bytes per sample
$file_size = 36 + $data_size;

$wav = "RIFF";
$wav .= pack('V', $file_size);
$wav .= "WAVE";
$wav .= "fmt ";
$wav .= pack('V', 16);       // chunk size
$wav .= pack('v', 1);        // PCM format
$wav .= pack('v', 1);        // mono
$wav .= pack('V', 44100);    // sample rate
$wav .= pack('V', 88200);    // byte rate
$wav .= pack('v', 2);        // block align
$wav .= pack('v', 16);       // bits per sample
$wav .= "data";
$wav .= pack('V', $data_size);
$wav .= str_repeat("\x00\x00", $samples); // silence

$path = tempnam(sys_get_temp_dir(), 'vio_audio_') . '.wav';
file_put_contents($path, $wav);

// Load the WAV
$sound = vio_audio_load($path);
var_dump($sound instanceof VioSound);

// Initially not playing
var_dump(vio_audio_playing($sound) === false);

// Play
vio_audio_play($sound, ['volume' => 0.0, 'loop' => false]); // silent
var_dump(vio_audio_playing($sound) === true);

// Set volume
vio_audio_volume($sound, 0.5);
echo "volume set OK\n";

// Pause
vio_audio_pause($sound);
var_dump(vio_audio_playing($sound) === false);

// Resume
vio_audio_resume($sound);
var_dump(vio_audio_playing($sound) === true);

// Stop (resets to beginning)
vio_audio_stop($sound);
var_dump(vio_audio_playing($sound) === false);

// Play with loop option
vio_audio_play($sound, ['loop' => true, 'volume' => 0.0]);
var_dump(vio_audio_playing($sound) === true);

// Stop again
vio_audio_stop($sound);

unlink($path);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
volume set OK
bool(true)
bool(true)
bool(true)
bool(true)
OK
