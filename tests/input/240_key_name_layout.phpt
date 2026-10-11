--TEST--
Input: vio_key_name - the layout character of printable keys (lower case, one character), null for non-printable keys and on the null platform, on every platform built in
--EXTENSIONS--
vio
--FILE--
<?php
// Layout-agnostic checks (CI runs US, developers QWERTZ/AZERTY): every letter
// key types one lower-case character; keys that type nothing are null.
$child = <<<'PHP'
<?php
$out = [];
$letters = [];
for ($k = VIO_KEY_A; $k <= VIO_KEY_Z; $k++) {
    $n = vio_key_name($k);
    $letters[] = $n;
}
$known = array_filter($letters, fn($n) => $n !== null);
$out[] = 'letters ' . ($known === [] ? 'unknown'
    : (count($known) === 26 && !array_filter($known, fn($n) => !preg_match('/^.$/u', $n) || preg_match('/\p{Lu}/u', $n)) ? 'ok' : 'BAD ' . json_encode($letters)));
$out[] = 'nonprintable ' . json_encode([vio_key_name(VIO_KEY_LEFT_SHIFT), vio_key_name(VIO_KEY_ENTER), vio_key_name(VIO_KEY_UP), vio_key_name(-1), vio_key_name(99999)]);
$out[] = 'space ' . json_encode(vio_key_name(VIO_KEY_SPACE) === null || vio_key_name(VIO_KEY_SPACE) === ' ');
echo vio_platform(), ' ', implode(' | ', $out), "\n";
PHP;
$file = tempnam(sys_get_temp_dir(), 'vio240');
file_put_contents($file, $child);
$results = [];
foreach (['win32', 'cocoa', 'x11', 'glfw', 'null'] as $platform) {
    $cmd = escapeshellarg(PHP_BINARY) . ' -n -d extension_dir=' . escapeshellarg(ini_get('extension_dir'))
         . ' -d extension=vio ' . escapeshellarg($file);
    $env = getenv();
    $env['VIO_PLATFORM'] = $platform;
    $p = proc_open($cmd, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, null, $env);
    $line = trim(stream_get_contents($pipes[1]));
    stream_get_contents($pipes[2]);
    proc_close($p);
    if (str_starts_with($line, $platform . ' ')) $results[$platform] = substr($line, strlen($platform) + 1);
}
unlink($file);

$bad = [];
foreach ($results as $platform => $line) {
    // only the null platform may not know the layout
    $wantLetters = $platform === 'null' ? 'letters unknown' : 'letters ok';
    if ($line !== "$wantLetters | nonprintable [null,null,null,null,null] | space true") $bad[] = "$platform: $line";
}
echo isset($results['null']) && count($results) >= 2 && !$bad ? "OK\n" : "FAIL " . json_encode([$results, $bad]) . "\n";

// In-process on the active platform: the same contract.
var_dump(vio_key_name(VIO_KEY_LEFT_CONTROL));
?>
--EXPECT--
OK
NULL
