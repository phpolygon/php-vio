--TEST--
vio_gl_info diagnostics + GLSL #version mismatch detection (Issue #3 part 3)
--SKIPIF--
<?php
require __DIR__ . '/skipif_gl.inc';
if (!extension_loaded('vio')) die('skip vio not loaded');
?>
--FILE--
<?php
/* Smoke-test the new diagnostics surface from Issue #3 part 3:
 * - vio_gl_info($ctx) returns a populated array on OpenGL contexts.
 * - It returns false on non-OpenGL backends (null backend here).
 * - vio_shader() rejects #version directives higher than the runtime
 *   provides, with a readable warning. */

$ctx_null = vio_create("null", []);
var_dump(vio_gl_info($ctx_null) === false);

$ctx = vio_create("opengl", ["width" => 16, "height" => 16, "headless" => true]);
if (!$ctx) {
    echo "skip: no headless OpenGL\n";
    exit;
}

$info = vio_gl_info($ctx);
var_dump(is_array($info));
var_dump(isset($info['version']));
var_dump(isset($info['glsl']));
var_dump(is_int($info['glsl']));
var_dump($info['glsl'] >= 330);  // floor is 3.3
var_dump(isset($info['renderer']));
var_dump(isset($info['vendor']));
var_dump(is_array($info['extensions']));
var_dump(is_array($info['features']));
var_dump(isset($info['features']['compute_shader']));
var_dump(isset($info['features']['tessellation']));
var_dump(isset($info['features']['texture_swizzle']));

/* GLSL version mismatch: ask for #version 999 (far above any real driver),
 * expect a warning and false return. */
$bad_vs = "#version 999\nvoid main(){gl_Position=vec4(0);}\n";
$bad_fs = "#version 999\nvoid main(){}\n";
vio_begin($ctx);
$bad = @vio_shader($ctx, ["vertex" => $bad_vs, "fragment" => $bad_fs]);
vio_end($ctx);
var_dump($bad === false);

/* GLSL version that's safe (floor 330) must compile cleanly. */
$ok_vs = "#version 330 core\nvoid main(){gl_Position=vec4(0);}\n";
$ok_fs = "#version 330 core\nvoid main(){}\n";
vio_begin($ctx);
$ok = vio_shader($ctx, ["vertex" => $ok_vs, "fragment" => $ok_fs]);
vio_end($ctx);
var_dump($ok instanceof VioShader);

vio_destroy($ctx);
vio_destroy($ctx_null);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
OK
