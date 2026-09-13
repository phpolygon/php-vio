--TEST--
Currency symbols above U+00FF are packed into the font atlas
--SKIPIF--
<?php
$candidates = [
    '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',  // Debian/Ubuntu
    '/usr/share/fonts/dejavu/DejaVuSans.ttf',           // Fedora
    'C:\\Windows\\Fonts\\arial.ttf',                    // Windows
    '/Library/Fonts/Arial Unicode.ttf',                 // macOS
    '/System/Library/Fonts/Helvetica.ttc',              // macOS fallback
];
$found = null;
foreach ($candidates as $c) { if (is_file($c)) { $found = $c; break; } }
if ($found === null) die("skip no system font found");
if (!function_exists('vio_font_face') || !function_exists('vio_font_face_has_glyph')) {
    die("skip vio_font_face_has_glyph unavailable");
}
?>
--EXTENSIONS--
vio
--FILE--
<?php
/* The packed ranges used to jump from U+1EFF straight to U+3000, which left the
 * whole Currency Symbols block unpacked. A caller drawing one of these got no
 * glyph AND no advance, so the character vanished without even leaving a gap,
 * and text measured through the fallback planner came out too narrow to hold it.
 *
 * The euro is the one that gets noticed, but the rouble, won, hryvnia and dong
 * all sit in the same block, as do the dash and curly quotes used in UI copy. */
$candidates = [
    '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
    '/usr/share/fonts/dejavu/DejaVuSans.ttf',
    'C:\\Windows\\Fonts\\arial.ttf',
    '/Library/Fonts/Arial Unicode.ttf',
    '/System/Library/Fonts/Helvetica.ttc',
];
$path = null;
foreach ($candidates as $c) { if (is_file($c)) { $path = $c; break; } }

$face = vio_font_face($path);

/* A font that genuinely lacks a glyph is not a packing failure, so anchor on
 * one the font certainly has and only then assert the block. */
var_dump(vio_font_face_has_glyph($face, 0x0024)); // $, always present
var_dump(vio_font_face_has_glyph($face, 0x20AC)); // EURO SIGN
?>
--EXPECT--
bool(true)
bool(true)
