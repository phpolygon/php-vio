--TEST--
Accessibility tree (DESKTOP-APP-PLAN Phase 5a, AccessKit): vio_a11y_update returns what vio_platform_info()['accessibility'] promises, the contract holds everywhere (roles, actions, ids, bounds, references into the tree); on Linux a screen reader's view over AT-SPI (own D-Bus session, python3-gi Atspi): names, roles, value and checked state of the nodes, a click from AT-SPI reaches vio_on_a11y_action, a later update (changed label) reaches AT-SPI
--EXTENSIONS--
vio
--FILE--
<?php
$err = function (callable $f) { try { $f(); return 'none'; } catch (ValueError $e) { return 'ValueError'; } catch (TypeError $e) { return 'TypeError'; } };
$avail = vio_platform_info()['accessibility'];
$ctx = @vio_create('auto', ['width' => 64, 'height' => 48, 'headless' => true]) ?: vio_create('null', ['width' => 64, 'height' => 48]);
$node = fn(array $n) => ['nodes' => [$n + ['id' => 'root', 'role' => 'window']]];

/* contract: everywhere */
$c = [
    $err(fn() => vio_a11y_update($ctx, ['nodes' => [['role' => 'button']]])),
    $err(fn() => vio_a11y_update($ctx, $node(['role' => 'spaceship']))),
    $err(fn() => vio_a11y_update($ctx, $node(['actions' => ['fly']]))),
    $err(fn() => vio_a11y_update($ctx, $node(['bounds' => [0, 0, 10]]))),
    $err(fn() => vio_a11y_update($ctx, $node(['bounds' => [0, 0, -1, 4]]))),
    $err(fn() => vio_a11y_update($ctx, ['nodes' => [['id' => 'a', 'role' => 'button'], ['id' => 'a', 'role' => 'label']]])),
    $err(fn() => vio_a11y_update($ctx, $node(['label' => 5]))),
    $err(fn() => vio_a11y_update($ctx, ['nodes' => 'x'])),
    $err(fn() => vio_on_a11y_action($ctx, 'not callable')),
];
echo 'contract ', implode(' ', $c), "\n";
$r = vio_a11y_update($ctx, ['nodes' => [['id' => 'root', 'role' => 'window', 'label' => 'Main', 'children' => ['ok']],
                                         ['id' => 'ok', 'role' => 'button', 'label' => 'OK', 'bounds' => [1, 2, 30, 20], 'actions' => ['click']]]]);
echo 'update ', json_encode($r === $avail), "\n";
if ($avail) {
    /* references are checked against the tree */
    echo 'refs ', implode(' ', [
        $err(fn() => vio_a11y_update($ctx, ['focus' => 'nope'])),
        $err(fn() => vio_a11y_update($ctx, ['nodes' => [['id' => 'root', 'role' => 'window', 'children' => ['ok', 'ghost']]]])),
        $err(fn() => vio_a11y_update($ctx, ['root' => 'ghost'])),
        $err(fn() => vio_a11y_update($ctx, ['focus' => 'ok'])),
    ]), "\n";
} else {
    echo "refs ValueError ValueError ValueError none\n";
}
vio_destroy($ctx);

/* Linux: what a screen reader sees, over AT-SPI */
$linux = 'n/a';
$have = fn(string $c) => trim((string)shell_exec('command -v ' . $c . ' 2>/dev/null')) !== '';
if (PHP_OS_FAMILY === 'Linux' && $avail && (getenv('DISPLAY') || getenv('WAYLAND_DISPLAY')) && $have('dbus-run-session') && $have('python3')
    && trim((string)shell_exec("python3 -c \"import gi; gi.require_version('Atspi', '2.0'); from gi.repository import Atspi; print('ok')\" 2>/dev/null")) === 'ok') {
    $launcher = '';
    foreach (['/usr/libexec/at-spi-bus-launcher', '/usr/lib/at-spi2-core/at-spi-bus-launcher', '/usr/lib/x86_64-linux-gnu/at-spi-bus-launcher'] as $l) if (is_file($l)) $launcher = $l;
    $child = <<<'PHP'
<?php
$ctx = vio_create('auto', ['width' => 200, 'height' => 100, 'headless' => true, 'title' => 'vio a11y']);
$tree = fn(string $save) => ['focus' => 'name', 'nodes' => [
    ['id' => 'root', 'role' => 'window', 'label' => 'vio a11y test', 'children' => ['save', 'name', 'agree']],
    ['id' => 'save', 'role' => 'button', 'label' => $save, 'bounds' => [10, 10, 80, 24], 'actions' => ['click']],
    ['id' => 'name', 'role' => 'text_input', 'label' => 'Name', 'value' => 'Hendrik', 'bounds' => [100, 10, 90, 24], 'actions' => ['focus', 'set_value']],
    ['id' => 'agree', 'role' => 'check_box', 'label' => 'Zustimmen', 'checked' => true, 'bounds' => [10, 50, 80, 20], 'actions' => ['click']],
]];
$clicked = false;
vio_on_a11y_action($ctx, function ($id, $action, $value) use (&$clicked) {
    echo "action $id $action " . json_encode($value) . "\n";
    if ($id === 'save' && $action === 'click') $clicked = true;
});
echo 'ready ', json_encode(vio_a11y_update($ctx, $tree('Speichern'))), "\n";
$t = microtime(true);
$sent = false;
while (microtime(true) - $t < 20 && !file_exists($argv[1])) {
    vio_wait_events($ctx, 0.05);
    if ($clicked && !$sent) { vio_a11y_update($ctx, ['nodes' => [$tree('Gespeichert')['nodes'][1]]]); $sent = true; echo "updated\n"; }
}
vio_destroy($ctx);
PHP;
    $py = <<<'PY'
import os, subprocess, sys, time
import gi
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi, Gio, GLib
import json
php, child, stop = json.loads(sys.argv[1]), sys.argv[2], sys.argv[3]
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
# screen reader on: AccessKit registers with the AT-SPI bus only then
for _ in range(50):
    try:
        bus.call_sync('org.a11y.Bus', '/org/a11y/bus', 'org.freedesktop.DBus.Properties', 'Set',
                      GLib.Variant('(ssv)', ('org.a11y.Status', 'IsEnabled', GLib.Variant('b', True))), None, 0, 2000, None)
        break
    except Exception:
        time.sleep(0.1)
proc = subprocess.Popen(php + [child, stop], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
out = []
import threading
threading.Thread(target=lambda: [out.append(l.strip()) for l in proc.stdout], daemon=True).start()
Atspi.init()
def find_window():
    desk = Atspi.get_desktop(0)
    for i in range(desk.get_child_count()):
        app = desk.get_child_at_index(i)
        if app is None: continue
        for j in range(app.get_child_count()):
            w = app.get_child_at_index(j)
            if w is not None and w.get_name() == 'vio a11y test': return w
    return None
win = None
t = time.time()
while win is None and time.time() - t < 15:
    win = find_window()
    if win is None: time.sleep(0.2)
res = []
if win is None:
    res.append('no window')
else:
    kids = [win.get_child_at_index(i) for i in range(win.get_child_count())]
    def desc(k):
        d = [k.get_name(), {'push button': 'button'}.get(k.get_role_name(), k.get_role_name())]   # older AT-SPI: 'push button'
        try: d.append(Atspi.Text.get_text(k, 0, -1) if 'Text' in k.get_interfaces() else None)
        except Exception as e: d.append(repr(e))
        d.append(k.get_state_set().contains(Atspi.StateType.CHECKED))
        return d
    res.append([desc(k) for k in kids])
    save = kids[0]
    act = save.get_action_iface()
    names = [Atspi.Action.get_action_name(save, i) for i in range(Atspi.Action.get_n_actions(save))] if act else []
    res.append(names)
    if act: Atspi.Action.do_action(save, 0)
    t = time.time()
    while time.time() - t < 10 and save.get_name() != 'Gespeichert':
        time.sleep(0.1)
        save.clear_cache() if hasattr(save, 'clear_cache') else None
    res.append(save.get_name())
open(stop, 'w').close()
proc.wait(10)
time.sleep(0.2)
import json
print(json.dumps({'atspi': res, 'php': out}))
PY;
    $dir = sys_get_temp_dir() . '/vio273-' . getmypid();
    @mkdir($dir);
    file_put_contents("$dir/child.php", $child);
    file_put_contents("$dir/q.py", $py);
    $php = json_encode([PHP_BINARY, '-n', '-d', 'extension_dir=' . ini_get('extension_dir'), '-d', 'extension=vio']);
    file_put_contents("$dir/run.sh", "export PHP_ARGS='$php'\n" . ($launcher ? escapeshellarg($launcher) . " --launch-immediately &\nsleep 0.5\n" : '')
        . 'python3 ' . escapeshellarg("$dir/q.py") . ' "$PHP_ARGS" ' . escapeshellarg("$dir/child.php") . ' ' . escapeshellarg("$dir/stop") . "\nkill %1 2>/dev/null\n");
    $raw = trim((string)shell_exec('dbus-run-session -- sh ' . escapeshellarg("$dir/run.sh") . ' 2>&1'));
    array_map('unlink', glob("$dir/*"));
    @rmdir($dir);
    $line = '';
    foreach (explode("\n", $raw) as $l) if (str_starts_with($l, '{"atspi"')) $line = $l;
    $j = json_decode($line, true);
    $want = ['atspi' => [[['Speichern', 'button', null, false], ['Name', 'entry', 'Hendrik', false], ['Zustimmen', 'check box', null, true]], ['click'], 'Gespeichert'],
             'php' => ['ready true', 'action save click null', 'updated']];
    $linux = $j === $want ? 'OK' : 'FAIL ' . $raw;
}
/* macOS: what VoiceOver sees, in-process through NSAccessibility on the content view
 * (no Accessibility permission needed for the process's own elements) */
$mac = 'n/a';
if (PHP_OS_FAMILY === 'Darwin' && $avail && extension_loaded('ffi')) {
    $ctx = vio_create('auto', ['width' => 200, 'height' => 100, 'headless' => true]);
    $tree = fn(string $save) => ['focus' => 'name', 'nodes' => [
        ['id' => 'root', 'role' => 'window', 'label' => 'vio a11y test', 'children' => ['save', 'name', 'agree']],
        ['id' => 'save', 'role' => 'button', 'label' => $save, 'bounds' => [10, 10, 80, 24], 'actions' => ['click']],
        ['id' => 'name', 'role' => 'text_input', 'label' => 'Name', 'value' => 'Hendrik', 'bounds' => [100, 10, 90, 24], 'actions' => ['focus', 'set_value']],
        ['id' => 'agree', 'role' => 'check_box', 'label' => 'Zustimmen', 'checked' => true, 'bounds' => [10, 50, 80, 20], 'actions' => ['click']],
    ]];
    $got = [];
    vio_on_a11y_action($ctx, function ($id, $action, $value) use (&$got) { $got[] = "$id $action " . json_encode($value); });
    vio_a11y_update($ctx, $tree('Speichern'));
    $lib = '/usr/lib/libobjc.A.dylib';
    $o  = FFI::cdef('void *sel_registerName(const char *s); void *objc_msgSend(void *o, void *s);', $lib);
    $oi = FFI::cdef('void *objc_msgSend(void *o, void *s, unsigned long i);', $lib);
    $ou = FFI::cdef('unsigned long objc_msgSend(void *o, void *s);', $lib);
    $os = FFI::cdef('const char *objc_msgSend(void *o, void *s);', $lib);
    $m = fn($obj, string $sel) => $o->objc_msgSend($obj, $o->sel_registerName($sel));
    $str = fn($ns) => $ns === null ? null : $os->objc_msgSend($m($ns, 'description'), $o->sel_registerName('UTF8String'));
    $kids = function ($el) use ($m, $o, $oi, $ou) {
        $c = $m($el, 'accessibilityChildren');
        $n = $c === null ? 0 : $ou->objc_msgSend($c, $o->sel_registerName('count'));
        $r = [];
        for ($i = 0; $i < $n; $i++) $r[] = $oi->objc_msgSend($c, $o->sel_registerName('objectAtIndex:'), $i);
        return $r;
    };
    $view = $m($o->cast('void *', vio_native_window_handle($ctx)), 'contentView');
    $root = $kids($view)[0] ?? null;
    $nodes = $root ? $kids($root) : [];
    $desc = array_map(fn($e) => [$str($m($e, 'accessibilityRole')), $str($m($e, 'accessibilityTitle')), $str($m($e, 'accessibilityValue'))], $nodes);
    $pressed = $nodes ? (bool)FFI::cdef('signed char objc_msgSend(void *o, void *s);', $lib)->objc_msgSend($nodes[0], $o->sel_registerName('accessibilityPerformPress')) : false;
    vio_poll_events($ctx);
    vio_a11y_update($ctx, ['nodes' => [$tree('Gespeichert')['nodes'][1]]]);
    $after = $root ? $str($m($kids($root)[0], 'accessibilityTitle')) : null;
    $want = [[['AXButton', 'Speichern', null], ['AXTextField', 'Name', 'Hendrik'], ['AXCheckBox', 'Zustimmen', '1']], true, ['save click null'], 'Gespeichert'];
    $mac = [$desc, $pressed, $got, $after] === $want ? 'OK' : 'FAIL ' . json_encode([$desc, $pressed, $got, $after]);
    vio_destroy($ctx);
}
if ($mac === 'n/a' && PHP_OS_FAMILY === 'Darwin' && getenv('VIO_REQUIRE_A11Y')) $mac = 'FAIL (required)';
echo "macos $mac\n";

/* Windows: what a screen reader sees, over UI Automation (PowerShell's UIAutomationClient) */
$win = 'n/a';
if (PHP_OS_FAMILY === 'Windows' && $avail) {
    $dir = sys_get_temp_dir() . '\\vio273-' . getmypid();
    @mkdir($dir);
    $child = <<<'PHP'
<?php
$ctx = vio_create('auto', ['width' => 240, 'height' => 120, 'title' => 'vio a11y']);
$tree = fn(string $save) => ['focus' => 'name', 'nodes' => [
    ['id' => 'root', 'role' => 'window', 'label' => 'vio a11y test', 'children' => ['save', 'name', 'agree']],
    ['id' => 'save', 'role' => 'button', 'label' => $save, 'bounds' => [10, 10, 80, 24], 'actions' => ['click']],
    ['id' => 'name', 'role' => 'text_input', 'label' => 'Name', 'value' => 'Hendrik', 'bounds' => [100, 10, 90, 24], 'actions' => ['focus', 'set_value']],
    ['id' => 'agree', 'role' => 'check_box', 'label' => 'Zustimmen', 'checked' => true, 'bounds' => [10, 50, 80, 20], 'actions' => ['click']],
]];
$clicked = false;
vio_on_a11y_action($ctx, function ($id, $action, $value) use (&$clicked) {
    echo "action $id $action " . json_encode($value) . "\n";
    if ($id === 'save' && $action === 'click') $clicked = true;
});
echo 'ready ', json_encode(vio_a11y_update($ctx, $tree('Speichern'))), ' ', vio_native_window_handle($ctx), "\n";
$t = microtime(true);
$sent = false;
while (microtime(true) - $t < 30 && !file_exists($argv[1])) {
    vio_wait_events($ctx, 0.05);
    if ($clicked && !$sent) { vio_a11y_update($ctx, ['nodes' => [$tree('Gespeichert')['nodes'][1]]]); $sent = true; echo "updated\n"; }
}
vio_destroy($ctx);
PHP;
    $ps = <<<'PS1'
param([long]$hwnd)
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$A = [System.Windows.Automation.AutomationElement]
$root = $A::FromHandle([IntPtr]$hwnd)
$res = @()
$kids = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
$list = @()
foreach ($k in $kids) {
    $c = $k.Current
    if ($c.ControlType.ProgrammaticName -notin @('ControlType.Button', 'ControlType.Edit', 'ControlType.CheckBox')) { continue }
    $v = $null; $t = $null
    $p = $null
    if ($k.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$p)) { $v = $p.Current.Value }
    if ($k.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$p)) { $t = [string]$p.Current.ToggleState }
    $list += ,@($c.Name, $c.ControlType.ProgrammaticName, $v, $t)
}
$res += ,$list
$save = $kids | Where-Object { $_.Current.Name -eq 'Speichern' } | Select-Object -First 1
$p = $null
if ($save -and $save.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$p)) { $p.Invoke(); $res += 'invoked' } else { $res += 'no invoke' }
$name = $null
for ($i = 0; $i -lt 100; $i++) {
    Start-Sleep -Milliseconds 100
    $now = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition) | Where-Object { $_.Current.ControlType.ProgrammaticName -eq 'ControlType.Button' } | Select-Object -First 1
    if ($now) { $name = $now.Current.Name }
    if ($name -eq 'Gespeichert') { break }
}
$res += $name
ConvertTo-Json -Compress -Depth 5 $res
PS1;
    file_put_contents("$dir\\child.php", $child);
    file_put_contents("$dir\\q.ps1", $ps);
    $out = "$dir\\out.txt";
    $proc = proc_open([PHP_BINARY, '-n', '-d', 'extension_dir=' . ini_get('extension_dir'), '-d', 'extension=vio', "$dir\\child.php", "$dir\\stop"],
                      [1 => ['file', $out, 'w'], 2 => ['file', "$dir\\err.txt", 'w']], $pipes);
    $hwnd = 0;
    for ($i = 0; $i < 200 && !$hwnd; $i++) {
        usleep(50000);
        if (preg_match('/^ready true (\d+)/m', (string)@file_get_contents($out), $m)) $hwnd = (int)$m[1];
    }
    $uia = $hwnd ? trim((string)shell_exec('powershell -NoProfile -ExecutionPolicy Bypass -File ' . escapeshellarg("$dir\\q.ps1") . ' ' . $hwnd . ' 2>&1')) : 'no window';
    touch("$dir\\stop");
    for ($i = 0; $i < 100 && proc_get_status($proc)['running']; $i++) usleep(100000);
    if (proc_get_status($proc)['running']) proc_terminate($proc);
    proc_close($proc);
    $php = array_values(array_filter(array_map('trim', explode("\n", (string)@file_get_contents($out)))));
    $php = array_map(fn($l) => preg_replace('/^(ready true) \d+$/', '$1', $l), $php);
    array_map('unlink', glob("$dir\\*"));
    @rmdir($dir);
    $j = json_decode($uia, true);
    $want = [[['Speichern', 'ControlType.Button', null, null], ['Name', 'ControlType.Edit', 'Hendrik', null], ['Zustimmen', 'ControlType.CheckBox', null, 'On']], 'invoked', 'Gespeichert'];
    $php = array_values(array_filter($php, fn($l) => $l !== 'action save focus null'));   // UIA focuses before Invoke
    $win = $j === $want && $php === ['ready true', 'action save click null', 'updated'] ? 'OK' : 'FAIL ' . json_encode([$uia, $php]);
}
if ($win === 'n/a' && PHP_OS_FAMILY === 'Windows' && getenv('VIO_REQUIRE_A11Y')) $win = 'FAIL (required)';
echo "windows $win\n";

/* VIO_REQUIRE_A11Y=1 (Linux CI with AccessKit and AT-SPI): no skip */
if ($linux === 'n/a' && PHP_OS_FAMILY === 'Linux' && getenv('VIO_REQUIRE_A11Y')) $linux = 'FAIL (required: ' . json_encode(['accessibility' => $avail]) . ')';
echo "linux $linux\n";
?>
--EXPECTF--
contract ValueError ValueError ValueError ValueError ValueError ValueError TypeError TypeError TypeError
update true
refs ValueError ValueError ValueError none
macos %r(OK|n.a)%r
windows %r(OK|n.a)%r
linux %r(OK|n.a)%r
