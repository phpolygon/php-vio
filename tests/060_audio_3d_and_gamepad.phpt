--TEST--
3D audio (listener / position) and vio_gamepad_name
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 64, "height" => 64, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

// ── Listener: (position, forward) ────────────────────────────────────
// Sets the 3D audio listener pose. No context arg — listener is global
// per process. In a headless context the audio engine isn't initialised
// (no playback device), so the call emits a warning. We suppress that
// for the API-shape check but verify the warning IS emitted.
$warning_seen = 0;
set_error_handler(function ($_, $msg) use (&$warning_seen) {
    if (str_contains($msg, 'Audio engine not initialized')) $warning_seen++;
    return true;
});

vio_audio_listener(0.0, 0.0, 0.0, 0.0, 0.0, -1.0);
vio_audio_listener(10.0, 5.0, -3.0, 1.0, 0.0, 0.0);

// Degenerate forward vector (zero-length) — must not crash regardless
// of whether the audio engine is active.
vio_audio_listener(0.0, 0.0, 0.0, 0.0, 0.0, 0.0);

// Extreme finite coordinates — same.
vio_audio_listener(-1e6, -1e6, -1e6, 0.0, 1.0, 0.0);
vio_audio_listener(1e6, 1e6, 1e6, 0.0, 1.0, 0.0);
restore_error_handler();

echo "listener calls dispatched\n";
var_dump($warning_seen >= 5); // one per call when audio is not initialised

// ── Source position: needs a VioSound from vio_audio_load(path) ─────
// Generate a tiny valid WAV (44-byte header, zero samples). Audio may
// be unavailable on headless / CI; treat both outcomes as OK.
$wav = "RIFF" . pack("V", 36) . "WAVE"
     . "fmt " . pack("VvvVVvv", 16, 1, 1, 8000, 16000, 2, 16)
     . "data" . pack("V", 0);
$path = tempnam(sys_get_temp_dir(), "vio_audio_") . ".wav";
file_put_contents($path, $wav);

$sound = @vio_audio_load($path);
if ($sound instanceof VioSound) {
    @vio_audio_position($sound, 1.0, 2.0, 3.0);
    @vio_audio_position($sound, -10.0, 0.0, 0.0);
}
@unlink($path);
echo "source position dispatched\n";

// ── Gamepad name ────────────────────────────────────────────────────
// Returns the controller's name string, or null when no pad is
// connected at the given slot (return type is nullable string).
$name0 = vio_gamepad_name(0);
var_dump(is_string($name0) || $name0 === null);

// Out-of-range slot — must return null.
$name99 = vio_gamepad_name(99);
var_dump($name99 === null);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
listener calls dispatched
bool(true)
source position dispatched
bool(true)
bool(true)
OK
