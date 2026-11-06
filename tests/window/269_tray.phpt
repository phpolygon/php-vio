--TEST--
Tray icon (DESKTOP-APP-PLAN Phase 2c-2): vio_tray / vio_tray_remove return what vio_platform_info()['tray'] says, argument contract; Win32 through real messages to the tray window (click -> on_click, menu command -> on_select, right click opens the menu); Linux as a StatusNotifierItem against a fake watcher on a private bus (registration, properties, the dbusmenu layout, a clicked item, Activate; dbus-run-session + python3-dbus, else n/a)
--EXTENSIONS--
vio
--FILE--
<?php
$dir = sys_get_temp_dir() . DIRECTORY_SEPARATOR . 'vio269-' . getmypid();
@mkdir($dir);
$child = <<<'PHP'
<?php
$ctx = @vio_create('auto', ['width' => 64, 'height' => 48, 'headless' => true]) ?: vio_create('null', ['width' => 64, 'height' => 48]);
$p = vio_platform();
$info = vio_platform_info();
$err = function (callable $f) { try { $f(); return 'none'; } catch (ValueError $e) { return 'ValueError'; } };
$out = [];
$out[] = 'refused ' . implode(' ', [
    $err(fn() => vio_tray(['icon' => [['data' => 'x', 'width' => 16, 'height' => 16]]])),
    $err(fn() => vio_tray(['on_select' => 'no such function'])),
    $err(fn() => vio_tray(['menu' => [['label' => 'no id']]])),
]);
$events = [];
$opts = [
    'icon' => [['data' => str_repeat("\x20\x80\xff\xff", 16 * 16), 'width' => 16, 'height' => 16],
               ['data' => str_repeat("\x20\x80\xff\xff", 32 * 32), 'width' => 32, 'height' => 32]],
    'tooltip' => 'vio test',
    'menu' => [['id' => 'show', 'label' => 'Show'], ['separator' => true], ['label' => 'More', 'children' => [['id' => 'quit', 'label' => 'Quit']]]],
    'on_select' => function (string $id) use (&$events) { $events[] = "select $id"; },
    'on_click' => function () use (&$events) { $events[] = 'click'; },
];
$shown = vio_tray($opts);
$out[] = 'set ' . json_encode([$shown === $info['tray'], vio_tray(['tooltip' => 'again'] + $opts) === $info['tray']]);
$os = 'n/a';
if ($shown && $p === 'win32' && extension_loaded('ffi')) {
    $u = FFI::cdef('intptr_t FindWindowExW(intptr_t parent, intptr_t after, const uint16_t *cls, const uint16_t *title);
                    int PostMessageW(intptr_t hwnd, unsigned int msg, uintptr_t wparam, intptr_t lparam);', 'user32.dll');
    $cls = $u->new('uint16_t[9]');
    foreach (str_split('vio_tray') as $i => $c) $cls[$i] = ord($c);
    $tray = $u->FindWindowExW(-3 /* HWND_MESSAGE */, 0, $cls, null);
    $u->PostMessageW($tray, 0x8001 /* WM_APP + 1 */, 0, (1 << 16) | 0x0202 /* id 1, WM_LBUTTONUP */);
    $u->PostMessageW($tray, 0x0111 /* WM_COMMAND */, 1000 + 1 /* tag of 'quit' */, 0);
    vio_poll_events($ctx);
    putenv('VIO_TEST_DIALOG=300:cancel');
    $u->PostMessageW($tray, 0x8001, 0, (1 << 16) | 0x0205 /* WM_RBUTTONUP: the menu, dismissed */);
    vio_poll_events($ctx);
    putenv('VIO_TEST_DIALOG');
    $os = json_encode(['window' => $tray !== 0, 'events' => $events, 'notify' => vio_notify('vio test', 'from the tray')]);
} elseif ($shown && getenv('VIO_TEST_DBUS') === '1') {
    for ($i = 0; $i < 100 && count($events) < 2; $i++) vio_wait_events($ctx, 0.1);
    $os = json_encode(['events' => $events]);
}
$out[] = "os $os";
$out[] = 'remove ' . json_encode([vio_tray_remove() === $info['tray'], vio_tray_remove()]);
echo $p, ' ', implode(' | ', $out), "\n";
vio_destroy($ctx);
PHP;
/* a StatusNotifierWatcher that, once an item registers, reads it and clicks it */
$fake = <<<'PY'
import sys, dbus, dbus.service, dbus.mainloop.glib
from gi.repository import GLib
log = open(sys.argv[1], 'a', buffering=1)
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
def walk(node, out):
    i, props, kids = node
    if i:
        out.append('sep' if props.get('type') == 'separator' else '%s%s' % (props.get('label'), '>' if props.get('children-display') == 'submenu' else ''))
    for k in kids:
        walk(k, out)
def poke(service):
    item = bus.get_object(service, '/StatusNotifierItem')
    p = dbus.Interface(item, 'org.freedesktop.DBus.Properties').GetAll('org.kde.StatusNotifierItem')
    pix = p['IconPixmap']
    log.write('item id=%s title=%s menu=%s ismenu=%s pixmap=%dx%d argb=%s\n' % (p['Id'], p['Title'], p['Menu'], bool(p['ItemIsMenu']),
              pix[0][0], pix[0][1], bytes(pix[0][2][:4]).hex()))
    menu = dbus.Interface(bus.get_object(service, str(p['Menu'])), 'com.canonical.dbusmenu')
    rev, layout = menu.GetLayout(0, -1, dbus.Array([], signature='s'))
    labels = []
    walk(layout, labels)
    log.write('layout %s\n' % ' '.join(labels))
    ids = {}
    def find(node):
        i, props, kids = node
        if props.get('label'): ids[str(props['label'])] = int(i)
        for k in kids: find(k)
    find(layout)
    menu.Event(ids['Quit'], 'clicked', dbus.Int32(0, variant_level=1), dbus.UInt32(0))
    dbus.Interface(item, 'org.kde.StatusNotifierItem').Activate(0, 0)
    log.write('poked\n')
    return False
class Watcher(dbus.service.Object):
    @dbus.service.method('org.kde.StatusNotifierWatcher', in_signature='s', out_signature='', sender_keyword='sender')
    def RegisterStatusNotifierItem(self, service, sender=None):
        log.write('register %s\n' % ('ok' if service.startswith('org.kde.StatusNotifierItem-') else service))
        GLib.timeout_add(200, lambda: poke(service))
w = dbus.service.BusName('org.kde.StatusNotifierWatcher', bus); Watcher(bus, '/StatusNotifierWatcher')
log.write('ready\n')
GLib.MainLoop().run()
PY;
file_put_contents("$dir/child.php", $child);
file_put_contents("$dir/fake.py", $fake);
$ext = '';
foreach (['ffi'] as $e) foreach ([PHP_EXTENSION_DIR . "/$e.so", dirname(PHP_BINARY) . "/ext/php_$e.dll"] as $f) if (is_file($f)) $ext .= ' -d extension=' . escapeshellarg($f);
$php = escapeshellarg(PHP_BINARY) . ' -n -d extension_dir=' . escapeshellarg(ini_get('extension_dir')) . ' -d extension=vio' . $ext . ' -d ffi.enable=1';
$run = function (string $cmd, array $env) {
    $p = proc_open($cmd, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, null, $env);
    $out = trim(stream_get_contents($pipes[1]));
    $err = trim(stream_get_contents($pipes[2]));
    proc_close($p);
    return [$out, $err];
};
$results = [];
foreach (['win32', 'cocoa', 'x11', 'glfw', 'wayland', 'null'] as $platform) {
    if (PHP_OS_FAMILY === 'Linux' && in_array($platform, ['x11', 'glfw'], true) && !getenv('DISPLAY')) continue;
    if ($platform === 'wayland' && !getenv('WAYLAND_DISPLAY')) continue;
    $env = getenv();
    $env['VIO_PLATFORM'] = $platform;
    unset($env['DBUS_SESSION_BUS_ADDRESS']);   // the plain run has no session bus (as in CI), so no real tray on a desktop
    [$line, $errout] = $run("$php " . escapeshellarg("$dir/child.php"), $env);
    if (str_starts_with($line, $platform . ' ')) $results[$platform] = substr($line, strlen($platform) + 1);
    elseif ($platform === 'null' || $platform === vio_platform()) $results[$platform] = "no result: $line $errout";
}
$dbusResult = 'n/a';
$have = PHP_OS_FAMILY === 'Linux' && trim((string)shell_exec('command -v dbus-run-session')) !== ''
     && trim((string)shell_exec('python3 -c "import dbus, gi; print(1)" 2>/dev/null')) === '1';
$lp = getenv('WAYLAND_DISPLAY') ? 'wayland' : (getenv('DISPLAY') ? 'x11' : null);
if ($have && $lp) {
    $log = "$dir/dbus.log";
    $script = 'python3 ' . escapeshellarg("$dir/fake.py") . ' ' . escapeshellarg($log) . ' & '
            . 'for i in $(seq 50); do grep -q ready ' . escapeshellarg($log) . ' 2>/dev/null && break; sleep 0.1; done; '
            . "$php -d vio.app_id=org.example.Vio " . escapeshellarg("$dir/child.php") . '; kill %1';
    $env = getenv();
    $env['VIO_PLATFORM'] = $lp;
    $env['VIO_TEST_DBUS'] = '1';
    [$line] = $run('dbus-run-session -- sh -c ' . escapeshellarg($script), $env);
    $logLines = is_file($log) ? array_values(array_filter(file($log, FILE_IGNORE_NEW_LINES), fn($l) => $l !== 'ready')) : [];
    $dbusResult = json_encode(['child' => $line, 'log' => $logLines], JSON_UNESCAPED_SLASHES);
}
array_map('unlink', glob("$dir/*"));
rmdir($dir);

$bad = [];
foreach ($results as $platform => $line) {
    $has = in_array($platform, ['win32', 'cocoa'], true);
    $want = 'refused ValueError ValueError ValueError | set [true,true] | os ';
    $tail = ' | remove [true,false]';
    $os = str_starts_with($line, $want) && str_ends_with($line, $tail) ? substr($line, strlen($want), -strlen($tail)) : null;
    $wantOs = $platform === 'win32' ? ['{"window":true,"events":["click","select quit"],"notify":true}', 'n/a'] : ['n/a'];
    if ($os === null || !in_array($os, $wantOs, true)) $bad[] = "$platform: $line";
}
if ($dbusResult !== 'n/a') {
    $d = json_decode($dbusResult, true);
    $wantChild = "$lp refused ValueError ValueError ValueError | set [true,true] | os " . json_encode(['events' => ['select quit', 'click']]) . ' | remove [true,false]';
    $wantLog = ['register ok', 'item id=org.example.Vio title=again menu=/MenuBar ismenu=True pixmap=32x32 argb=ff2080ff', 'layout Show sep More> Quit', 'poked'];
    if ($d['child'] !== $wantChild || $d['log'] !== $wantLog) $bad[] = "dbus: $dbusResult";
}
echo isset($results['null'], $results[vio_platform()]) && !$bad ? "OK\n" : "FAIL " . json_encode([$results, $dbusResult, $bad], JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES) . "\n";
?>
--EXPECT--
OK
