--TEST--
vio_font / vio_font_load_async refuse files that are not fonts instead of crashing
--SKIPIF--
<?php require __DIR__ . '/../skipif_gl.inc'; ?>
--EXTENSIONS--
vio
--FILE--
<?php
// A Git LFS pointer, an HTML error page and zeros: stbtt_GetFontOffsetForIndex() is -1 for
// all of them, and passing that on to stbtt_InitFont() used to read far outside the buffer.
$files = [
    'lfs pointer' => "version https://git-lfs.github.com/spec/v1\noid sha256:64f8be\nsize 407056\n",
    'html page'   => "<!DOCTYPE html><html><body>404 Not Found</body></html>\n",
    'zeros'       => str_repeat("\0", 64),
];

$ctx = vio_create("opengl", ["width" => 64, "height" => 64, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

foreach ($files as $name => $bytes) {
    $path = tempnam(sys_get_temp_dir(), 'vio_font_');
    file_put_contents($path, $bytes);

    echo $name, ' sync: ';
    var_dump(@vio_font($ctx, $path, 16.0));

    echo $name, ' async: ';
    $handle = vio_font_load_async($ctx, $path, 16.0);
    $result = null;
    for ($i = 0; $i < 5000; $i++) {
        $result = vio_font_load_poll($handle);
        if ($result !== null) { break; }
        usleep(1000);
    }
    var_dump($result);

    unlink($path);
}

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
lfs pointer sync: bool(false)
lfs pointer async: bool(false)
html page sync: bool(false)
html page async: bool(false)
zeros sync: bool(false)
zeros async: bool(false)
OK
