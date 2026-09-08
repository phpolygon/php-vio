--TEST--
D3D12: frame_count is configurable, clamped, and every value renders
--SKIPIF--
<?php
if (PHP_OS_FAMILY !== 'Windows') die('skip Windows only');
if (!in_array('d3d12', vio_backends(), true)) die('skip D3D12 backend not compiled in');
?>
--EXTENSIONS--
vio
--FILE--
<?php
// 'frame_count' selects how many frames the CPU may run ahead of the GPU.
// It is NOT cosmetic: the value divides the per-frame SRV / constant-buffer /
// instance-heap slices AND indexes the fixed-length frames[] array.
//
// Two things must hold:
//  1. Every accepted value actually renders (the heap slices must stay valid —
//     going 2 -> 3 shrinks each slice from 1/2 to 1/3 of its heap).
//  2. Out-of-range values are CLAMPED, not trusted. This arrives straight from a
//     PHP array, so an unclamped 99 would index frames[99] on a 3-element array
//     and a 0 would divide the heaps by zero. Both must be impossible.

function run(string $label, array $opts): void
{
    $ctx = @vio_create('d3d12', $opts + ['width' => 64, 'height' => 64, 'headless' => true]);
    if (!$ctx instanceof VioContext) {
        echo "SKIP: WARP unavailable\n";
        exit;
    }

    // Three frames, so the frame slots wrap around at least once for count 2 and 3
    // and each slot's allocator gets Reset after its fence has been waited on.
    for ($i = 0; $i < 3; $i++) {
        vio_begin($ctx);
        vio_viewport($ctx, 0, 0, 64, 64);
        vio_clear($ctx, 0.2, 0.4, 0.6, 1.0);
        vio_end($ctx);
    }

    vio_destroy($ctx);
    echo "{$label} OK\n";
}

run('default (absent)', []);
run('explicit 2',       ['frame_count' => 2]);
run('explicit 3',       ['frame_count' => 3]);

// Out of range in both directions — must clamp, not crash, not overflow frames[].
run('zero -> default',  ['frame_count' => 0]);
run('one -> min',       ['frame_count' => 1]);
run('99 -> max',        ['frame_count' => 99]);
run('negative -> default', ['frame_count' => -5]);

echo "done\n";
?>
--EXPECTF--
%Adefault (absent) OK
explicit 2 OK
explicit 3 OK
zero -> default OK
one -> min OK
99 -> max OK
negative -> default OK
done
