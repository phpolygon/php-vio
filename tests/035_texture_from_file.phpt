--TEST--
Texture loading from PNG file
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 32, "height" => 32, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

// First create a PNG file via screenshot
vio_clear($ctx, 1.0, 0.0, 0.0, 1.0); // red
vio_begin($ctx); vio_end($ctx);
$png_path = tempnam(sys_get_temp_dir(), 'vio_tex_') . '.png';
vio_save_screenshot($ctx, $png_path);
var_dump(file_exists($png_path));

// Load texture from file
$tex = vio_texture($ctx, ['file' => $png_path]);
var_dump($tex instanceof VioTexture);

// Use it as a sprite
vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
vio_begin($ctx);
vio_sprite($ctx, $tex, ['x' => 0, 'y' => 0, 'width' => 32, 'height' => 32]);
vio_draw_2d($ctx);
$pixels = vio_read_pixels($ctx);
vio_end($ctx);

// Should have some non-black pixels (the red texture was drawn)
$vis = 0;
for ($i = 0; $i < strlen($pixels); $i += 4) {
    if (ord($pixels[$i]) > 10) $vis++;
}
echo "texture from file rendered: " . ($vis > 0 ? "yes" : "no") . "\n";

// Loading nonexistent file should fail
$bad = @vio_texture($ctx, ['file' => '/nonexistent/image.png']);
var_dump($bad === false); // true

// Async load of same file
$handle = vio_texture_load_async($png_path);
var_dump(is_resource($handle));

// Poll until done
$result = null;
for ($i = 0; $i < 200; $i++) {
    $result = vio_texture_load_poll($handle);
    if ($result !== null) break;
    usleep(1000);
}
var_dump(is_array($result));
var_dump($result['width'] === 32);
var_dump($result['height'] === 32);
var_dump(strlen($result['data']) === 32 * 32 * 4);

unlink($png_path);
vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
texture from file rendered: yes
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
OK
