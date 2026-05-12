--TEST--
Performance regression gates: hot API calls stay under absolute thresholds
--EXTENSIONS--
vio
--FILE--
<?php
// Per-call timings for the hot 2D / 3D API entry points. These are
// not micro-benchmarks (timing variance on CI / loaded host is too
// high for that) — they are coarse smoke checks that flag the
// "regression made every call 100× slower" class of bug.
//
// Thresholds are picked deliberately loose: anything under 100 µs
// per call is fine. Real values on an unloaded dev box sit well
// below 5 µs. A hit on the threshold means an O(N²) loop or a
// missing cache snuck in.
$ctx = vio_create("null");

$N = 1000;
$thresholds_us = [
    'vio_begin/end'      => 100.0,
    'vio_clear'          => 100.0,
    'vio_set_uniform'    => 100.0,
    'vio_rect'           => 100.0,
    'vio_push_transform' => 50.0,
    'vio_pop_transform'  => 50.0,
];

function bench(string $name, int $n, callable $body, array $thresholds): bool {
    $t0 = hrtime(true);
    for ($i = 0; $i < $n; $i++) $body();
    $elapsed_ns = hrtime(true) - $t0;
    $per_call_us = $elapsed_ns / 1000.0 / $n;
    $limit = $thresholds[$name];
    $ok = $per_call_us < $limit;
    echo sprintf("%-22s avg %6.2f µs (<%.0f µs: %s)\n",
                 $name, $per_call_us, $limit, $ok ? 'OK' : 'SLOW');
    return $ok;
}

// vio_begin/end pair
$pass1 = bench('vio_begin/end', $N, function () use ($ctx) {
    vio_begin($ctx);
    vio_end($ctx);
}, $thresholds_us);

// vio_clear must NOT be inside a frame for the null backend to avoid
// confusing the in-frame guard; clear before, then begin/end empty.
$pass2 = bench('vio_clear', $N, function () use ($ctx) {
    vio_clear($ctx, 0.1, 0.2, 0.3, 1.0);
}, $thresholds_us);

// vio_set_uniform inside a single long frame
vio_begin($ctx);
$pass3 = bench('vio_set_uniform', $N, function () use ($ctx) {
    vio_set_uniform($ctx, 'u_x', 1.5);
}, $thresholds_us);

$pass4 = bench('vio_rect', $N, function () use ($ctx) {
    vio_rect($ctx, 0.0, 0.0, 4.0, 4.0, ['fill' => 0xFFFF0000]);
}, $thresholds_us);

$pass5 = bench('vio_push_transform', $N, function () use ($ctx) {
    vio_push_transform($ctx, 1.0, 0.0, 0.0, 1.0, 0.5, 0.5);
    vio_pop_transform($ctx); // pop here to keep stack bounded
}, $thresholds_us);

$pass6 = bench('vio_pop_transform', $N, function () use ($ctx) {
    vio_push_transform($ctx, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    vio_pop_transform($ctx);
}, $thresholds_us);
vio_end($ctx);

// All within budget?
var_dump($pass1 && $pass2 && $pass3 && $pass4 && $pass5 && $pass6);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECTF--
%s
%s
%s
%s
%s
%s
bool(true)
OK
