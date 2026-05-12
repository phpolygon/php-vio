--TEST--
vio_destroy lifecycle: double-destroy, destroy-mid-frame, use-after-destroy
--EXTENSIONS--
vio
--FILE--
<?php
// ── Double destroy: second call must be a safe no-op. ───────────────
$ctx1 = vio_create("null");
var_dump($ctx1 instanceof VioContext);
vio_destroy($ctx1);
vio_destroy($ctx1); // second destroy — must not crash, must not double-free
echo "double destroy OK\n";

// ── Destroy mid-frame: vio_begin was called but vio_end was not.
// The destroy must clean up the in-flight frame state instead of
// leaving it dangling (which would crash on the next vio_create). ───
$ctx2 = vio_create("null");
vio_begin($ctx2);
// Intentionally no vio_end here.
vio_destroy($ctx2);
echo "destroy mid-frame OK\n";

// ── Use after destroy: subsequent calls on a destroyed ctx must
// emit warnings (or fail false), never crash. ────────────────────────
$ctx3 = vio_create("null");
vio_destroy($ctx3);

set_error_handler(function () { return true; }); // swallow warnings
$shouldClose = @vio_should_close($ctx3);
$size        = @vio_window_size($ctx3);
@vio_begin($ctx3);
@vio_clear($ctx3, 0.0, 0.0, 0.0, 1.0);
@vio_end($ctx3);
@vio_poll_events($ctx3);
restore_error_handler();
echo "use after destroy did not crash\n";

// ── Many create / destroy cycles: leak detection proxy. PHP-side
// heap growth across 100 ctx lifecycles must stay bounded. The Zend
// allocator allocates in chunks (2 MiB by default), so the bound has
// to account for one chunk-reservation event. Anything noticeably
// above that indicates an actual leak. ──────────────────────────────
gc_collect_cycles();
$baseline = memory_get_usage(true);
for ($i = 0; $i < 100; $i++) {
    $c = vio_create("null");
    vio_destroy($c);
    unset($c);
}
gc_collect_cycles();
$delta = memory_get_usage(true) - $baseline;
$delta_mb = $delta / 1024 / 1024;
// 4 MiB cap: 2 MiB chunk reservation + a comfortable margin for
// per-context refcount bookkeeping. Used to leak ~10 MiB per 100
// cycles before the cache-the-RT-wrapper fix.
var_dump($delta_mb < 4.0);

// ── Destroying one ctx must not affect a second live ctx. ───────────
$ctxA = vio_create("null");
$ctxB = vio_create("null");
vio_destroy($ctxA);
// ctxB still usable
vio_begin($ctxB);
vio_clear($ctxB, 0.0, 0.0, 0.0, 1.0);
vio_end($ctxB);
vio_destroy($ctxB);
echo "isolated destroy OK\n";

echo "OK\n";
?>
--EXPECT--
bool(true)
double destroy OK
destroy mid-frame OK
use after destroy did not crash
bool(true)
isolated destroy OK
OK
