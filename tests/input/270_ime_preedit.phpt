--TEST--
IME composition (DESKTOP-APP-PLAN Phase 3): vio_on_preedit gets the composition and its selection in codepoints ('' when done), vio_inject_preedit takes the same path, vio_ime_rect / vio_ime_enable return what the platform has (Win32 IMM32, Cocoa, X11 XIM preedit callbacks, Wayland text-input-v3; not GLFW); on Cocoa through the view's NSTextInputClient (setMarkedText -> preedit, insertText -> chars + '', firstRectForCharacterRange -> the vio_ime_rect size)
--EXTENSIONS--
vio
--FILE--
<?php
$child = <<<'PHP'
<?php
$ctx = @vio_create('auto', ['width' => 64, 'height' => 48, 'headless' => true]) ?: vio_create('null', ['width' => 64, 'height' => 48]);
$p = vio_platform();
if ($p !== getenv('VIO_PLATFORM')) { echo "skip\n"; exit; }
$info = vio_platform_info();
$log = [];
vio_on_preedit($ctx, function (string $t, int $s, int $e) use (&$log) { $log[] = [$t, $s, $e]; });
$err = function (callable $f) { try { $f(); return 'none'; } catch (ValueError $e) { return 'ValueError'; } };
$out = [];
$out[] = 'info ' . json_encode($info['ime_preedit']);
vio_inject_preedit($ctx, 'にほ', 1, 2);
vio_inject_preedit($ctx, '😀a');
vio_inject_preedit($ctx, '');
$out[] = 'inject ' . json_encode($log, JSON_UNESCAPED_UNICODE) . ' ' . $err(fn() => vio_inject_preedit($ctx, 'ab', 3)) . ' ' . $err(fn() => vio_inject_preedit($ctx, 'ab', 2, 1));
$log = [];
$out[] = 'calls ' . json_encode([vio_ime_rect($ctx, 10, 20, 5, 16), vio_ime_enable($ctx, false), vio_ime_enable($ctx, true)]) . ' ' . $err(fn() => vio_ime_rect($ctx, 0, 0, -1, 1));
$os = 'n/a';
$win = vio_native_window_handle($ctx);
if ($p === 'cocoa' && extension_loaded('ffi') && $win) {
    $lib = '/usr/lib/libobjc.A.dylib';
    $o = FFI::cdef('typedef struct { unsigned long loc, len; } NSRange; void *objc_getClass(const char *n); void *sel_registerName(const char *n); void *objc_msgSend(void *o, void *s);', $lib);
    $ostr = FFI::cdef('void *objc_msgSend(void *o, void *s, const char *c);', $lib);
    $omark = FFI::cdef('typedef struct { unsigned long loc, len; } NSRange; void objc_msgSend(void *o, void *s, void *str, NSRange sel, NSRange rep);', $lib);
    $oins = FFI::cdef('typedef struct { unsigned long loc, len; } NSRange; void objc_msgSend(void *o, void *s, void *str, NSRange rep);', $lib);
    $S = fn($n) => $o->sel_registerName($n);
    $str = fn(string $t) => $ostr->objc_msgSend($o->objc_getClass('NSString'), $S('stringWithUTF8String:'), $t);
    /* each FFI instance has its own NSRange type */
    $range = function ($ffi, int $loc, int $len) { $r = $ffi->new('NSRange'); $r->loc = $loc; $r->len = $len; return $r; };
    $notFound = PHP_INT_MAX;   // NSNotFound
    $view = $o->objc_msgSend($o->cast('void *', $win), $S('contentView'));
    vio_begin($ctx); vio_end($ctx);   // a frame: vio_chars_typed is per frame
    $omark->objc_msgSend($view, $S('setMarkedText:selectedRange:replacementRange:'), $str('にほん'), $range($omark, 1, 1), $range($omark, $notFound, 0));
    $omark->objc_msgSend($view, $S('setMarkedText:selectedRange:replacementRange:'), $str('😀a'), $range($omark, 2, 0), $range($omark, $notFound, 0));   // UTF-16 2 = after the emoji
    $oins->objc_msgSend($view, $S('insertText:replacementRange:'), $str('日本'), $range($oins, $notFound, 0));
    $chars = vio_chars_typed($ctx);
    /* firstRect: the vio_ime_rect size in screen points (arm64 returns the struct in registers, x86_64 through memory) */
    $size = null;
    if (php_uname('m') === 'arm64') {
        $orect = FFI::cdef('typedef struct { unsigned long loc, len; } NSRange; typedef struct { double x, y, w, h; } NSRect; NSRect objc_msgSend(void *o, void *s, NSRange r, void *actual);', $lib);
        $r = $orect->objc_msgSend($view, $S('firstRectForCharacterRange:actualRange:'), $range($orect, 0, 0), null);
        $size = [$r->w, $r->h];
    }
    $os = json_encode(['preedit' => $log, 'chars' => $chars, 'rect' => $size], JSON_UNESCAPED_UNICODE);
}
$out[] = "os $os";
echo $p, ' ', implode(' | ', $out), "\n";
vio_destroy($ctx);
PHP;
$file = tempnam(sys_get_temp_dir(), 'vio270');
file_put_contents($file, $child);
$ext = '';
foreach (['ffi'] as $e) foreach ([PHP_EXTENSION_DIR . "/$e.so", dirname(PHP_BINARY) . "/ext/php_$e.dll"] as $f) if (is_file($f)) $ext .= ' -d extension=' . escapeshellarg($f);
$results = [];
foreach (['win32', 'cocoa', 'x11', 'glfw', 'wayland', 'null'] as $platform) {
    if (PHP_OS_FAMILY === 'Linux' && in_array($platform, ['x11', 'glfw'], true) && !getenv('DISPLAY')) continue;
    if ($platform === 'wayland' && !getenv('WAYLAND_DISPLAY')) continue;
    $cmd = escapeshellarg(PHP_BINARY) . ' -n -d extension_dir=' . escapeshellarg(ini_get('extension_dir')) . ' -d extension=vio' . $ext . ' -d ffi.enable=1 ' . escapeshellarg($file);
    $env = getenv();
    $env['VIO_PLATFORM'] = $platform;
    $proc = proc_open($cmd, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, null, $env);
    $line = trim(stream_get_contents($pipes[1]));
    $errout = trim(stream_get_contents($pipes[2]));
    proc_close($proc);
    if (str_starts_with($line, $platform . ' ')) $results[$platform] = substr($line, strlen($platform) + 1);
    elseif ($platform === 'null' || $platform === vio_platform()) $results[$platform] = "no result: $line $errout";
}
unlink($file);
$bad = [];
foreach ($results as $platform => $line) {
    $has = in_array($platform, ['win32', 'cocoa', 'x11', 'wayland'], true);   // GLFW reports no composition
    $want = 'info ' . json_encode($has) . ' | inject [["にほ",1,2],["😀a",2,2],["",0,0]] ValueError ValueError | calls '
          . json_encode([$has, $has, $has]) . ' ValueError | os ';
    $os = str_starts_with($line, $want) ? substr($line, strlen($want)) : null;
    $wantOs = $platform === 'cocoa'
        ? ['{"preedit":[["にほん",1,2],["😀a",1,1],["",0,0]],"chars":"日本","rect":[5,16]}', '{"preedit":[["にほん",1,2],["😀a",1,1],["",0,0]],"chars":"日本","rect":null}', 'n/a']
        : ['n/a'];
    if ($os === null || !in_array($os, $wantOs, true)) $bad[] = "$platform: $line";
}
echo isset($results['null'], $results[vio_platform()]) && !$bad ? "OK\n" : "FAIL " . json_encode([$results, $bad], JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE) . "\n";
?>
--EXPECT--
OK
