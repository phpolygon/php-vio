--TEST--
vio_audio_info / vio_audio_decode / vio_audio_load read Ogg Opus and Ogg Vorbis from inside a PHAR
--EXTENSIONS--
vio
phar
--INI--
phar.readonly=0
--ENV--
VIO_AUDIO_BACKEND=null
--FILE--
<?php
$fx = __DIR__ . '/fixtures';
// PHAR: info, decode and load read through the stream wrapper
$pharPath = sys_get_temp_dir() . '/vio_audio_245_' . getmypid() . '.phar';
@unlink($pharPath);
$phar = new Phar($pharPath);
$phar['voice/line.opus'] = file_get_contents("$fx/tone440_mono_24k.opus");
$phar['music/theme.ogg'] = file_get_contents("$fx/tone440_stereo_44k.ogg");
unset($phar);
$i = vio_audio_info("phar://$pharPath/voice/line.opus");
echo 'phar opus: ', $i['format'], ' ', $i['frames'], "\n";
$d = vio_audio_decode("phar://$pharPath/music/theme.ogg");
echo 'phar vorbis: ', $d['format'], ' ', $d['channels'], ' ', strlen($d['pcm']), "\n";
var_dump(vio_audio_load("phar://$pharPath/voice/line.opus") instanceof VioSound);
Phar::unlinkArchive($pharPath);
echo "OK\n";
?>
--EXPECT--
phar opus: opus 24000
phar vorbis: vorbis 2 88200
bool(true)
OK
