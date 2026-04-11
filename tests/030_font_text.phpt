--TEST--
Font loading and text rendering with headless GPU context
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 128, "height" => 64, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

// Load a system TTF font
$font = vio_font($ctx, "/Library/Fonts/Arial Unicode.ttf", 32.0);
var_dump($font instanceof VioFont);

// Loading nonexistent font should fail
$bad = @vio_font($ctx, "/nonexistent/font.ttf");
var_dump($bad === false);

// Clear to black, draw white text, verify pixels changed
vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
vio_begin($ctx);
vio_text($ctx, $font, "Hi", 10.0, 10.0, ["color" => 0xFFFFFFFF]);
vio_draw_2d($ctx);
$pixels = vio_read_pixels($ctx);
vio_end($ctx);

// Check that at least some pixels are non-black (text was rendered)
$non_black = 0;
$total = strlen($pixels) / 4;
for ($i = 0; $i < strlen($pixels); $i += 4) {
    if (ord($pixels[$i]) > 10 || ord($pixels[$i+1]) > 10 || ord($pixels[$i+2]) > 10) {
        $non_black++;
    }
}
echo "text rendered: " . ($non_black > 0 ? "yes" : "no") . "\n";

// Render with different size
$font_small = vio_font($ctx, "/Library/Fonts/Arial Unicode.ttf", 12.0);
var_dump($font_small instanceof VioFont);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
text rendered: yes
bool(true)
OK
