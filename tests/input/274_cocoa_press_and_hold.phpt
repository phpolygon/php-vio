--TEST--
Cocoa: holding a key repeats it instead of opening the accent picker (ApplePressAndHoldEnabled is off for the process once the platform is up); keys go through the input method since the IME work, where press-and-hold would otherwise apply
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (PHP_OS_FAMILY !== 'Darwin') die('skip macOS only');
if (vio_platform() !== 'cocoa') die('skip needs the Cocoa platform');
if (!extension_loaded('ffi')) die('skip needs FFI');
?>
--FILE--
<?php
$ctx = vio_create('auto', ['width' => 32, 'height' => 32, 'headless' => true]);
$lib = '/usr/lib/libobjc.A.dylib';
$o = FFI::cdef('void *sel_registerName(const char *s); void *objc_getClass(const char *n); void *objc_msgSend(void *o, void *s);', $lib);
$os = FFI::cdef('void *objc_msgSend(void *o, void *s, const char *str);', $lib);
$ob = FFI::cdef('signed char objc_msgSend(void *o, void *s, void *key);', $lib);
$key = $os->objc_msgSend($o->objc_getClass('NSString'), $o->sel_registerName('stringWithUTF8String:'), 'ApplePressAndHoldEnabled');
$defaults = $o->objc_msgSend($o->objc_getClass('NSUserDefaults'), $o->sel_registerName('standardUserDefaults'));
/* set, and off: a missing key reads as false through boolForKey but means "on" to the input method */
$value = FFI::cdef('void *objc_msgSend(void *o, void *s, void *key);', $lib)->objc_msgSend($defaults, $o->sel_registerName('objectForKey:'), $key);
var_dump($value !== null);
var_dump((bool)$ob->objc_msgSend($defaults, $o->sel_registerName('boolForKey:'), $key));
vio_destroy($ctx);
?>
--EXPECT--
bool(true)
bool(false)
