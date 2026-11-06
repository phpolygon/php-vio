--TEST--
Notifications and taskbar progress (DESKTOP-APP-PLAN Phase 2c): vio_notify / vio_set_progress return what vio_platform_info() says, argument contract everywhere, the Dock tile carries the bar on Cocoa; on Linux against fake D-Bus services (dbus-run-session + python3-dbus, else n/a): Notify with replaces_id, the LauncherEntry signal, the file chooser portal (open with filters and multiple, save with name and folder, cancel)
--EXTENSIONS--
vio
--FILE--
<?php
$dir = sys_get_temp_dir() . DIRECTORY_SEPARATOR . 'vio268-' . getmypid();
@mkdir($dir);
$child = <<<'PHP'
<?php
$ctx = @vio_create('auto', ['width' => 64, 'height' => 48, 'headless' => true]) ?: vio_create('null', ['width' => 64, 'height' => 48]);
$p = vio_platform();
$info = vio_platform_info();
$err = function (callable $f) { try { $f(); return 'none'; } catch (ValueError $e) { return 'ValueError'; } };
$out = [];
$out[] = 'refused ' . implode(' ', [
    $err(fn() => vio_notify('')), $err(fn() => vio_notify('T', 'B', ['id' => str_repeat('x', 64)])),
    $err(fn() => vio_set_progress($ctx, 1.5)), $err(fn() => vio_set_progress($ctx, 0.5, 'stalled')),
]);
$dbus = getenv('VIO_TEST_DBUS') === '1';
if (!$dbus) {
    $out[] = 'calls ' . json_encode([vio_notify('Hello', 'Body') === $info['notifications'],
                                     vio_set_progress($ctx, 0.4) === $info['progress'],
                                     vio_set_progress($ctx, null, 'indeterminate') === $info['progress'],
                                     vio_set_progress($ctx, null) === $info['progress']]);
}
$os = 'n/a';
if ($p === 'cocoa' && extension_loaded('ffi')) {
    $o = FFI::cdef('void *objc_getClass(const char *n); void *sel_registerName(const char *n); void *objc_msgSend(void *o, void *s);', '/usr/lib/libobjc.A.dylib');
    $tile = fn() => $o->objc_msgSend($o->objc_msgSend($o->objc_msgSend($o->objc_getClass('NSApplication'), $o->sel_registerName('sharedApplication')), $o->sel_registerName('dockTile')), $o->sel_registerName('contentView'));
    vio_set_progress($ctx, 0.6);
    $shown = $tile() !== null;   // FFI hands a NULL pointer back as null
    vio_set_progress($ctx, null);
    $os = json_encode(['bar' => $shown, 'gone' => $tile() === null]);
} elseif ($dbus) {
    [, $tmp] = $argv;
    $r = [
        'info' => [$info['notifications'], $info['progress'], $info['file_dialog']],
        'notify' => [vio_notify('Hello', 'Body äö', ['id' => 'job']), vio_notify('Hello', 'Again', ['id' => 'job']), vio_notify('Other')],
        'progress' => [vio_set_progress($ctx, 0.25), vio_set_progress($ctx, null)],
        'open' => vio_file_dialog($ctx, ['title' => 'Pick', 'multiple' => true, 'filters' => [['name' => 'Images', 'extensions' => ['png', 'jpg']]]]),
        'save' => vio_file_dialog($ctx, ['mode' => 'save', 'default_path' => "$tmp/new name.txt"]),
        'cancel' => vio_file_dialog($ctx, ['title' => 'cancel']),
    ];
    $os = json_encode($r, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
}
$out[] = "os $os";
echo $p, ' ', implode(' | ', $out), "\n";
vio_destroy($ctx);
PHP;
/* fake services: org.freedesktop.Notifications, the FileChooser portal, a LauncherEntry listener */
$fake = <<<'PY'
import sys, dbus, dbus.service, dbus.mainloop.glib
from gi.repository import GLib
log = open(sys.argv[1], 'a', buffering=1)
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
class Notes(dbus.service.Object):
    n = 0
    @dbus.service.method('org.freedesktop.Notifications', in_signature='susssasa{sv}i', out_signature='u')
    def Notify(self, app, replaces, icon, summary, body, actions, hints, expire):
        Notes.n += 1
        nid = replaces if replaces else Notes.n
        log.write('notify %s %d %s %s -> %d\n' % (app, replaces, summary, body, nid))
        return nid
class Portal(dbus.service.Object):
    def respond(self, sender, opts, code, uris):
        token = str(opts.get('handle_token', ''))
        path = '/org/freedesktop/portal/desktop/request/%s/%s' % (sender[1:].replace('.', '_'), token)
        req = dbus.service.Object(bus, path)
        def emit():
            msg = dbus.lowlevel.SignalMessage(path, 'org.freedesktop.portal.Request', 'Response')
            msg.append(dbus.UInt32(code), dbus.Dictionary({'uris': dbus.Array(uris, signature='s')}, signature='sv'))
            bus.send_message(msg)
            return False
        GLib.timeout_add(100, emit)
        return dbus.ObjectPath(path)
    @dbus.service.method('org.freedesktop.portal.FileChooser', in_signature='ssa{sv}', out_signature='o', sender_keyword='sender')
    def OpenFile(self, parent, title, opts, sender=None):
        f = [(str(n), [(int(k), str(g)) for k, g in pats]) for n, pats in opts.get('filters', [])]
        log.write('open %s multiple=%s filters=%s\n' % (title, bool(opts.get('multiple', False)), f))
        if title == 'cancel':
            return self.respond(sender, opts, 1, [])
        return self.respond(sender, opts, 0, ['file:///tmp/a%20b.txt', 'file:///tmp/c.png'])
    @dbus.service.method('org.freedesktop.portal.FileChooser', in_signature='ssa{sv}', out_signature='o', sender_keyword='sender')
    def SaveFile(self, parent, title, opts, sender=None):
        folder = bytes(opts.get('current_folder', b'')).rstrip(b'\0').decode()
        name = str(opts.get('current_name', ''))
        log.write('save %s name=%s\n' % (title, name))
        return self.respond(sender, opts, 0, ['file://' + folder.replace(' ', '%20') + '/' + name.replace(' ', '%20')])
def launcher(uri, props):
    log.write('progress %s %.2f %s\n' % (uri, float(props.get('progress', -1)), bool(props.get('progress-visible', False))))
bus.add_signal_receiver(launcher, signal_name='Update', dbus_interface='com.canonical.Unity.LauncherEntry')
n = dbus.service.BusName('org.freedesktop.Notifications', bus); Notes(bus, '/org/freedesktop/Notifications')
p = dbus.service.BusName('org.freedesktop.portal.Desktop', bus); Portal(bus, '/org/freedesktop/portal/desktop')
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
    unset($env['DBUS_SESSION_BUS_ADDRESS']);   // the plain run sees no session bus (as in CI)
    [$line, $errout] = $run("$php " . escapeshellarg("$dir/child.php"), $env);
    if (str_starts_with($line, $platform . ' ')) $results[$platform] = substr($line, strlen($platform) + 1);
    elseif ($platform === 'null') $results[$platform] = "no result: $line $errout";
}

/* Linux against the fake services */
$dbusResult = 'n/a';
$have = PHP_OS_FAMILY === 'Linux' && trim((string)shell_exec('command -v dbus-run-session')) !== ''
     && trim((string)shell_exec('python3 -c "import dbus, gi; print(1)" 2>/dev/null')) === '1';
$lp = getenv('WAYLAND_DISPLAY') ? 'wayland' : (getenv('DISPLAY') ? 'x11' : null);
if ($have && $lp) {
    $log = "$dir/dbus.log";
    $script = 'python3 ' . escapeshellarg("$dir/fake.py") . ' ' . escapeshellarg($log) . ' & '
            . 'for i in $(seq 50); do grep -q ready ' . escapeshellarg($log) . ' 2>/dev/null && break; sleep 0.1; done; '
            . "$php -d vio.app_id=org.example.Vio " . escapeshellarg("$dir/child.php") . ' ' . escapeshellarg($dir) . '; kill %1';
    $env = getenv();
    $env['VIO_PLATFORM'] = $lp;
    $env['VIO_TEST_DBUS'] = '1';
    [$line] = $run('dbus-run-session -- sh -c ' . escapeshellarg($script), $env);
    $logLines = is_file($log) ? array_values(array_filter(file($log, FILE_IGNORE_NEW_LINES), fn($l) => $l !== 'ready')) : [];
    $dbusResult = json_encode(['child' => $line, 'log' => $logLines], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
}
array_map('unlink', glob("$dir/*"));
rmdir($dir);

$bad = [];
foreach ($results as $platform => $line) {
    $want = 'refused ValueError ValueError ValueError ValueError | calls [true,true,true,true] | os ';
    $os = str_starts_with($line, $want) ? substr($line, strlen($want)) : null;
    $wantOs = $platform === 'cocoa' && extension_loaded('ffi') || ($platform === 'cocoa' && $ext) ? ['{"bar":true,"gone":true}', 'n/a'] : ['n/a'];
    if ($os === null || !in_array($os, $wantOs, true)) $bad[] = "$platform: $line";
}
if ($dbusResult !== 'n/a') {
    $d = json_decode($dbusResult, true);
    $wantChild = $lp . ' refused ValueError ValueError ValueError ValueError | os '
        . json_encode(['info' => [true, true, true], 'notify' => [true, true, true], 'progress' => [true, true],
                       'open' => ['/tmp/a b.txt', '/tmp/c.png'], 'save' => [sys_get_temp_dir() . '/vio268-' . getmypid() . '/new name.txt'], 'cancel' => null],
                      JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    $wantLog = [
        'notify org.example.Vio 0 Hello Body äö -> 1', 'notify org.example.Vio 1 Hello Again -> 1', 'notify org.example.Vio 0 Other  -> 3',
        'progress application://org.example.Vio.desktop 0.25 True', 'progress application://org.example.Vio.desktop 0.00 False',
        "open Pick multiple=True filters=[('Images', [(0, '*.png'), (0, '*.jpg')])]", 'save Save name=new name.txt', 'open cancel multiple=False filters=[]',
    ];
    if ($d['child'] !== $wantChild || $d['log'] !== $wantLog) $bad[] = "dbus: $dbusResult";
}
echo isset($results['null'], $results[vio_platform()]) && !$bad ? "OK\n" : "FAIL " . json_encode([$results, $dbusResult, $bad], JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES) . "\n";
?>
--EXPECT--
OK
