--TEST--
Persistent instance buffers (vio_instance_buffer / _update / _info / vio_draw_instanced_buffer) and instanced vio_submit_batch records draw byte-identically to vio_draw_instanced; in-place updates, sub-ranges, argument errors, bundles refuse instanced records
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with a 3D pipeline");
?>
--FILE--
<?php
/* Four small quads, each instance a scale + translate matrix. The reference is the
 * per-call vio_draw_instanced with the packed string. Every other path must produce
 * the same pixels:
 *   vio_draw_instanced_buffer (whole buffer, and sub ranges via $firstInstance),
 *   vio_submit_batch with 'instances' => string and => VioInstanceBuffer
 *     ('instance_count', 'first_instance'),
 *   after vio_instance_buffer_update (the quads moved: the image changes, then equals
 *   the reference drawn from the updated string). */
$W = 32;
function inst(float $x, float $y): string {
    return pack('g16', 0.2, 0, 0, 0,  0, 0.2, 0, 0,  0, 0, 1, 0,  $x, $y, 0, 1);
}
$A = [[-0.6, -0.6], [0.6, -0.6], [-0.6, 0.6], [0.6, 0.6]];
$B = [[0.0, -0.6], [0.0, 0.6], [-0.6, 0.0], [0.6, 0.0]];
$packA = ''; foreach ($A as $p) $packA .= inst($p[0], $p[1]);
$packB = ''; foreach ($B as $p) $packB .= inst($p[0], $p[1]);

function run_backend(string $name, bool $hw = false): string {
    global $W, $packA, $packB;
    $opts = ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false];
    if ($hw) $opts['headless_hardware'] = true;
    $ctx = @vio_create($name, $opts);
    if (!$ctx) return "skip (unavailable)";
    if (vio_backend_name($ctx) !== $name) { vio_destroy($ctx); return "skip (unavailable)"; }
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D)"; }
    $fail = [];
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nlayout(location=3) in mat4 aModel;\nvoid main(){ gl_Position = aModel * vec4(aPos, 1.0); }";
    $fs = "#version 330 core\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0.0, 1.0, 0.0, 1.0); }";
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);

    $frame = function (callable $draw) use ($ctx, $pipe): string {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        $draw();
        vio_end($ctx);
        return vio_read_pixels($ctx);
    };

    $refA = $frame(fn () => vio_draw_instanced($ctx, $quad, $packA, 4));
    $refB = $frame(fn () => vio_draw_instanced($ctx, $quad, $packB, 4));
    $refA23 = $frame(fn () => vio_draw_instanced($ctx, $quad, substr($packA, 128), 2));
    if ($refA === $refB) { vio_destroy($ctx); return "skip (instance data not drawn on this backend)"; }   // Vulkan: no instanced mesh path

    $buf = vio_instance_buffer($ctx, $packA);
    if (!$buf) { vio_destroy($ctx); return "FAIL: vio_instance_buffer returned false"; }
    $info = vio_instance_buffer_info($buf);
    if ($info['bytes'] !== 256 || $info['instances'] !== 4) $fail[] = "info " . json_encode($info);
    $native = $info['native'];

    if ($frame(fn () => vio_draw_instanced_buffer($ctx, $quad, $buf, 4)) !== $refA) $fail[] = "vio_draw_instanced_buffer differs";
    if ($frame(fn () => vio_draw_instanced_buffer($ctx, $quad, $buf, 2, 2)) !== $refA23) $fail[] = "vio_draw_instanced_buffer first=2 count=2 differs";
    // drawn again and again from the same buffer
    for ($i = 0; $i < 3; $i++) {
        if ($frame(fn () => vio_draw_instanced_buffer($ctx, $quad, $buf, 4)) !== $refA) { $fail[] = "repeat $i differs"; break; }
    }

    // batch: a string record, a buffer record, a sub range of each
    $batchStr = $frame(fn () => vio_submit_batch($ctx, [['mesh' => $quad, 'pipeline' => $pipe, 'instances' => $packA]]));
    if ($batchStr !== $refA) $fail[] = "batch 'instances' string differs";
    $batchBuf = $frame(fn () => vio_submit_batch($ctx, [['mesh' => $quad, 'pipeline' => $pipe, 'instances' => $buf, 'instance_count' => 4]]));
    if ($batchBuf !== $refA) $fail[] = "batch 'instances' buffer differs";
    $batchBufAll = $frame(fn () => vio_submit_batch($ctx, [['mesh' => $quad, 'pipeline' => $pipe, 'instances' => $buf]]));
    if ($batchBufAll !== $refA) $fail[] = "batch buffer without instance_count differs";
    $sub = $frame(fn () => vio_submit_batch($ctx, [['mesh' => $quad, 'pipeline' => $pipe, 'instances' => $buf, 'first_instance' => 2, 'instance_count' => 2]]));
    if ($sub !== $refA23) $fail[] = "batch buffer first_instance/instance_count differs";
    $subStr = $frame(fn () => vio_submit_batch($ctx, [['mesh' => $quad, 'pipeline' => $pipe, 'instances' => $packA, 'first_instance' => 2, 'instance_count' => 2]]));
    if ($subStr !== $refA23) $fail[] = "batch string first_instance/instance_count differs";
    // two records of one batch: both halves = all
    $halves = $frame(fn () => vio_submit_batch($ctx, [
        ['mesh' => $quad, 'pipeline' => $pipe, 'instances' => $buf, 'first_instance' => 0, 'instance_count' => 2],
        ['mesh' => $quad, 'instances' => $buf, 'first_instance' => 2, 'instance_count' => 2],
    ]));
    if ($halves !== $refA) $fail[] = "two batch records over one buffer differ";

    // in-place update: the quads move to set B
    if (vio_instance_buffer_update($buf, $packB) !== true) $fail[] = "update returned false";
    if ($frame(fn () => vio_draw_instanced_buffer($ctx, $quad, $buf, 4)) !== $refB) $fail[] = "after a whole update the image is not set B";
    // partial update: instances 2..3 back to A
    vio_instance_buffer_update($buf, substr($packA, 128), 128);
    $mixed = substr($packB, 0, 128) . substr($packA, 128);
    $refMixed = $frame(fn () => vio_draw_instanced($ctx, $quad, $mixed, 4));
    if ($frame(fn () => vio_draw_instanced_buffer($ctx, $quad, $buf, 4)) !== $refMixed) $fail[] = "after a partial update the image differs";
    // update between two frames, draw only in the second
    vio_instance_buffer_update($buf, $packA);
    if ($frame(fn () => vio_draw_instanced_buffer($ctx, $quad, $buf, 4)) !== $refA) $fail[] = "update back to A differs";

    // a buffer with spare capacity: filled later by appending in place
    $cap = vio_instance_buffer($ctx, $packA, 512);
    if (!$cap || vio_instance_buffer_info($cap)['instances'] !== 8) $fail[] = "capacity buffer info " . json_encode($cap ? vio_instance_buffer_info($cap) : null);
    elseif ($frame(fn () => vio_draw_instanced_buffer($ctx, $quad, $cap, 4)) !== $refA) $fail[] = "capacity buffer: first instances differ";
    else {
        vio_instance_buffer_update($cap, $packB, 256);
        if ($frame(fn () => vio_draw_instanced_buffer($ctx, $quad, $cap, 4, 4)) !== $refB) $fail[] = "capacity buffer: appended instances differ";
        if ($frame(fn () => vio_draw_instanced_buffer($ctx, $quad, $cap, 8)) !== $frame(fn () => vio_draw_instanced($ctx, $quad, $packA . $packB, 8))) $fail[] = "capacity buffer: all eight differ";
    }

    /* Contract errors. */
    $errs = [];
    $try = function (string $label, callable $f) use (&$errs, &$fail) {
        try { $f(); $fail[] = "$label: expected an exception"; }
        catch (\Throwable $e) { $errs[$label] = get_class($e); }
    };
    $try('empty data', fn () => vio_instance_buffer($ctx, ''));
    $try('not a multiple of 64', fn () => vio_instance_buffer($ctx, str_repeat('x', 65)));
    $try('capacity below the data', fn () => vio_instance_buffer($ctx, $packA, 128));
    $try('capacity not a multiple', fn () => vio_instance_buffer($ctx, $packA, 300));
    $try('update past the end', fn () => vio_instance_buffer_update($buf, $packA, 64));
    $try('update offset not a multiple', fn () => vio_instance_buffer_update($buf, str_repeat('x', 64), 4));
    $try('draw range outside', function () use ($ctx, $pipe, $quad, $buf) {
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        try { vio_draw_instanced_buffer($ctx, $quad, $buf, 5); } finally { vio_end($ctx); }
    });
    foreach ([
        'batch buffer range outside' => [['mesh' => $quad, 'instances' => $buf, 'first_instance' => 3, 'instance_count' => 2]],
        'batch string range outside' => [['mesh' => $quad, 'instances' => $packA, 'instance_count' => 5]],
    ] as $label => $recs) {
        $try($label, function () use ($ctx, $pipe, $recs) {
            vio_begin($ctx);
            vio_bind_pipeline($ctx, $pipe);
            try { vio_submit_batch($ctx, $recs); } finally { vio_end($ctx); }
        });
    }
    $try('bundle with instances', fn () => vio_bundle($ctx, [['mesh' => $quad, 'instances' => $packA]]));
    foreach ($errs as $l => $c) if ($c !== 'ValueError') $fail[] = "$l: $c instead of ValueError";

    $nativeWanted = in_array($name, ['d3d11', 'd3d12'], true);
    if ($native !== $nativeWanted) $fail[] = "native flag " . var_export($native, true) . " on $name";

    unset($buf, $cap, $quad, $pipe);
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'metal', 'vulkan'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "d3d12_hw: ", run_backend('d3d12', true), "\n";
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(?:\[d3d12\] [^\r\n]*\r?\n)*(?:OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
d3d12_hw: %r(?:\[d3d12\] [^\r\n]*\r?\n)*(?:OK|skip \(.*\))%r
DONE
