--TEST--
Menus (DESKTOP-APP-PLAN Phase 2b): vio_menu_bar (nested menus, separators, shortcuts, enabled / checked) calls back with the chosen id, vio_popup_menu returns one or null; argument contract everywhere; the real menu on Win32 (GetMenu, WM_COMMAND) and Cocoa (NSApp.mainMenu, performActionForItemAtIndex:, an app menu whose Quit asks vio_on_close)
--EXTENSIONS--
vio
--FILE--
<?php
$child = <<<'PHP'
<?php
$ctx = @vio_create('auto', ['width' => 64, 'height' => 48, 'headless' => true]) ?: vio_create('null', ['width' => 64, 'height' => 48]);
$p = vio_platform();
$info = vio_platform_info();
$err = function (callable $f) { try { $f(); return 'none'; } catch (ValueError $e) { return 'ValueError'; } };
$out = [];
$out[] = 'caps ' . json_encode([$info['menu_bar'], $info['popup_menu']]);
$noop = fn() => null;
$out[] = 'refused ' . implode(' ', [
    $err(fn() => vio_menu_bar($ctx, [['label' => 'File']], $noop)),                                   // a top menu needs children
    $err(fn() => vio_menu_bar($ctx, [['label' => 'File', 'children' => [['label' => 'x']]]], $noop)),   // an item needs an id
    $err(fn() => vio_menu_bar($ctx, [['label' => 'F', 'children' => [['id' => 'a', 'label' => 'A'], ['id' => 'a', 'label' => 'B']]]], $noop)),   // ids unique
    $err(fn() => vio_menu_bar($ctx, [['label' => 'F', 'children' => [['id' => 'a', 'label' => 'A', 'shortcut' => 'Primary+Banana']]]], $noop)),
    $err(fn() => vio_popup_menu($ctx, [], 0, 0)),
]);
$chosen = [];
$menus = [
    ['label' => 'File', 'children' => [
        ['id' => 'open', 'label' => 'Open…', 'shortcut' => 'Primary+O'],
        ['id' => 'save', 'label' => 'Save', 'shortcut' => 'Primary+Shift+S'],
        ['separator' => true],
        ['label' => 'Recent', 'children' => [['id' => 'recent.a', 'label' => 'a.txt'], ['id' => 'recent.b', 'label' => 'b.txt', 'enabled' => false]]],
    ]],
    ['label' => 'View', 'children' => [['id' => 'grid', 'label' => 'Grid', 'checked' => true, 'shortcut' => 'F5']]],
];
$set = vio_menu_bar($ctx, $menus, function (string $id) use (&$chosen) { $chosen[] = $id; });
$out[] = 'set ' . json_encode($set === $info['menu_bar']);
$out[] = 'inject ' . json_encode([vio_inject_menu($ctx, 'save'), vio_inject_menu($ctx, 'recent.a'), vio_inject_menu($ctx, 'nope')]) . ' ' . json_encode($chosen);
$chosen = [];

$closeAsked = 0;
vio_on_close($ctx, function () use (&$closeAsked) { $closeAsked++; return false; });
$os = 'n/a';
$win = vio_native_window_handle($ctx);
if (extension_loaded('ffi') && $win && $p === 'win32') {
    $u = FFI::cdef(<<<'C'
intptr_t GetMenu(intptr_t hwnd);
int GetMenuItemCount(intptr_t menu);
intptr_t GetSubMenu(intptr_t menu, int pos);
int GetMenuStringW(intptr_t menu, unsigned int item, uint16_t *buf, int max, unsigned int flags);
unsigned int GetMenuItemID(intptr_t menu, int pos);
unsigned int GetMenuState(intptr_t menu, unsigned int id, unsigned int flags);
int PostMessageW(intptr_t hwnd, unsigned int msg, uintptr_t wparam, intptr_t lparam);
C, 'user32.dll');
    $str = function ($m, $pos) use ($u) { $b = $u->new('uint16_t[128]'); $n = $u->GetMenuStringW($m, $pos, $b, 128, 0x400 /* MF_BYPOSITION */); return mb_convert_encoding(FFI::string(FFI::addr($b[0]), $n * 2), 'UTF-8', 'UTF-16LE'); };
    $bar = $u->GetMenu($win);
    $file = $u->GetSubMenu($bar, 0);
    $recent = $u->GetSubMenu($file, 3);
    $view = $u->GetSubMenu($bar, 1);
    $saveId = $u->GetMenuItemID($file, 1);
    $u->PostMessageW($win, 0x0111 /* WM_COMMAND */, $saveId, 0);
    vio_poll_events($ctx);
    $os = json_encode([
        'top' => [$str($bar, 0), $str($bar, 1)], 'file' => [$u->GetMenuItemCount($file), $str($file, 0), $str($file, 1)],
        'recent_b_disabled' => ($u->GetMenuState($recent, 1, 0x400) & 0x3) !== 0,
        'grid_checked' => ($u->GetMenuState($view, 0, 0x400) & 0x8) !== 0,
        'chosen' => $chosen,
    ], JSON_UNESCAPED_UNICODE);
} elseif (extension_loaded('ffi') && $p === 'cocoa') {
    $o = FFI::cdef(<<<'C'
void *objc_getClass(const char *name);
void *sel_registerName(const char *name);
void *objc_msgSend(void *obj, void *sel);
C, '/usr/lib/libobjc.A.dylib');
    $oi = FFI::cdef('void *objc_msgSend(void *obj, void *sel, long i);', '/usr/lib/libobjc.A.dylib');
    $ol = FFI::cdef('long objc_msgSend(void *obj, void *sel);', '/usr/lib/libobjc.A.dylib');
    $os2 = FFI::cdef('const char *objc_msgSend(void *obj, void *sel);', '/usr/lib/libobjc.A.dylib');
    $S = fn($n) => $o->sel_registerName($n);
    $app = $o->objc_msgSend($o->objc_getClass('NSApplication'), $S('sharedApplication'));
    $main = $o->objc_msgSend($app, $S('mainMenu'));
    $item = fn($menu, $i) => $oi->objc_msgSend($menu, $S('itemAtIndex:'), $i);
    $title = fn($it) => $os2->objc_msgSend($o->objc_msgSend($it, $S('title')), $S('UTF8String'));   // const char * comes back as a string
    $sub = fn($it) => $o->objc_msgSend($it, $S('submenu'));
    $file = $sub($item($main, 1));
    $keq = fn($it) => $os2->objc_msgSend($o->objc_msgSend($it, $S('keyEquivalent')), $S('UTF8String'));
    $mask = fn($it) => $ol->objc_msgSend($it, $S('keyEquivalentModifierMask'));
    $oi->objc_msgSend($file, $S('performActionForItemAtIndex:'), 1);   // Save
    $appMenu = $sub($item($main, 0));
    $quit = $ol->objc_msgSend($appMenu, $S('numberOfItems')) - 1;
    $oi->objc_msgSend($appMenu, $S('performActionForItemAtIndex:'), $quit);
    vio_poll_events($ctx);
    $os = json_encode([
        'top' => [$title($item($main, 1)), $title($item($main, 2))], 'file' => [$ol->objc_msgSend($file, $S('numberOfItems')), $title($item($file, 0)), $title($item($file, 1))],
        'save_key' => [$keq($item($file, 1)), $mask($item($file, 1)) === ((1 << 20) | (1 << 17))],   // Cmd + Shift
        'recent_b_disabled' => $ol->objc_msgSend($item($sub($item($file, 3)), 1), $S('isEnabled')) === 0,
        'grid_checked' => $ol->objc_msgSend($item($sub($item($main, 2)), 0), $S('state')) === 1,
        'chosen' => $chosen, 'quit_asks_close' => $closeAsked === 1 && !vio_should_close($ctx),
    ], JSON_UNESCAPED_UNICODE);
}
$out[] = "os $os";

putenv('VIO_TEST_DIALOG=300:cancel');
$r = vio_popup_menu($ctx, [['id' => 'cut', 'label' => 'Cut'], ['separator' => true], ['id' => 'paste', 'label' => 'Paste']], 10, 10);
putenv('VIO_TEST_DIALOG');
$out[] = 'popup ' . json_encode($r);
$out[] = 'remove ' . json_encode(vio_menu_bar($ctx, [], $noop) === $info['menu_bar']);
echo $p, ' ', implode(' | ', $out), "\n";
vio_destroy($ctx);
PHP;
$file = tempnam(sys_get_temp_dir(), 'vio267');
file_put_contents($file, $child);
$ext = '';
foreach (['ffi', 'mbstring'] as $e) foreach ([PHP_EXTENSION_DIR . "/$e.so", dirname(PHP_BINARY) . "/ext/php_$e.dll"] as $f) if (is_file($f)) $ext .= ' -d extension=' . escapeshellarg($f);
$results = [];
foreach (['win32', 'cocoa', 'x11', 'glfw', 'wayland', 'null'] as $platform) {
    if (PHP_OS_FAMILY === 'Linux' && in_array($platform, ['x11', 'glfw'], true) && !getenv('DISPLAY')) continue;
    if ($platform === 'wayland' && !getenv('WAYLAND_DISPLAY')) continue;
    $cmd = escapeshellarg(PHP_BINARY) . ' -n -d extension_dir=' . escapeshellarg(ini_get('extension_dir'))
         . ' -d extension=vio' . $ext . ' -d ffi.enable=1 ' . escapeshellarg($file);
    $env = getenv();
    $env['VIO_PLATFORM'] = $platform;
    $proc = proc_open($cmd, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, null, $env);
    $line = trim(stream_get_contents($pipes[1]));
    $errout = trim(stream_get_contents($pipes[2]));
    proc_close($proc);
    if (str_starts_with($line, $platform . ' ')) $results[$platform] = substr($line, strlen($platform) + 1);
    elseif ($platform === 'null') $results[$platform] = "no result: $line $errout";
}
unlink($file);

$wantOs = [
    'win32' => '{"top":["File","View"],"file":[4,"Open…\tCtrl+O","Save\tCtrl+Shift+S"],"recent_b_disabled":true,"grid_checked":true,"chosen":["save"]}',
    'cocoa' => '{"top":["File","View"],"file":[4,"Open…","Save"],"save_key":["s",true],"recent_b_disabled":true,"grid_checked":true,"chosen":["save"],"quit_asks_close":true}',
];
$bad = [];
foreach ($results as $platform => $line) {
    $has = in_array($platform, ['win32', 'cocoa'], true);
    $want = 'caps ' . json_encode([$has, $has]) . ' | refused ValueError ValueError ValueError ValueError ValueError | set true'
          . ' | inject ' . ($has ? '[true,true,false] ["save","recent.a"]' : '[false,false,false] []') . ' | os ';
    $tail = ' | popup ' . ($has ? 'null' : 'false') . ' | remove true';
    $os = str_starts_with($line, $want) && str_ends_with($line, $tail) ? substr($line, strlen($want), -strlen($tail)) : null;
    if ($os === null || !in_array($os, isset($wantOs[$platform]) ? [$wantOs[$platform], 'n/a'] : ['n/a'], true)) $bad[] = "$platform: $line";
}
echo isset($results['null'], $results[vio_platform()]) && !$bad ? "OK\n" : "FAIL " . json_encode([$results, $bad], JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE) . "\n";
?>
--EXPECT--
OK
