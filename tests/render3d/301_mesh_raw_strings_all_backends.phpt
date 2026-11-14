--TEST--
vio_mesh from raw strings: packed float32 'vertices' and packed uint16 / uint32 'indices' build the same mesh as the PHP arrays (index width, rendering), bad lengths and out-of-range indices are rejected
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
/* Distant streamed cells cost ~600 bytes of PHP array per triangle; a baked cell
 * already holds the vertex / index bytes. 'vertices' => string (packed little-endian
 * float32) and 'indices' => string ('index_type' declares the packed width) are used
 * in place. The mesh must behave exactly like the array form. */
$W = 24;
$verts = [-0.9,-0.9,0, 0.9,-0.9,0, 0.9,0.9,0, -0.9,0.9,0];   // pos3 only
$idx = [0,1,2, 0,2,3];
$rawV = pack('g*', ...$verts);
$raw16 = pack('v*', ...$idx);
$raw32 = pack('V*', ...$idx);

function draw_px($ctx, $pipe, $mesh, int $W): string {
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pipe);
    vio_draw($ctx, $mesh);
    vio_end($ctx);
    return vio_read_pixels($ctx);
}

function run_backend(string $name): string {
    global $W, $verts, $idx, $rawV, $raw16, $raw32;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D)"; }
    $fail = [];
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $fs = "#version 330 core\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0.0, 1.0, 0.0, 1.0); }";
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);

    $ref = vio_mesh($ctx, ['vertices' => $verts, 'indices' => $idx, 'layout' => [VIO_FLOAT3]]);
    $m16 = vio_mesh($ctx, ['vertices' => $rawV, 'indices' => $raw16, 'index_type' => VIO_INDEX_UINT16, 'layout' => [VIO_FLOAT3]]);
    $m32 = vio_mesh($ctx, ['vertices' => $rawV, 'indices' => $raw32, 'layout' => [VIO_FLOAT3]]);
    $mflat = vio_mesh($ctx, ['vertices' => $rawV, 'layout' => [VIO_FLOAT3]]);    // raw, unindexed (first triangle of the fan only: 4 vertices = 1 triangle)
    if (!$ref || !$m16 || !$m32 || !$mflat) { vio_destroy($ctx); return "FAIL: a mesh was not created"; }
    if (vio_mesh_index_bytes($m16) !== 2) $fail[] = "raw uint16 indices should report 2, got " . vio_mesh_index_bytes($m16);
    if (vio_mesh_index_bytes($m32) !== 4) $fail[] = "raw uint32 indices should report 4, got " . vio_mesh_index_bytes($m32);

    $want = draw_px($ctx, $pipe, $ref, $W);
    foreach (['uint16' => $m16, 'uint32' => $m32] as $label => $m) {
        if (draw_px($ctx, $pipe, $m, $W) !== $want) $fail[] = "$label raw mesh renders differently from the array mesh";
    }
    // the quad covers the centre: green
    $o = ((int)($W / 2) * $W + (int)($W / 2)) * 4;
    if (ord($want[$o + 1]) < 200) $fail[] = "reference quad is not green in the centre";

    /* Rejections: each one a warning and false, no crash. */
    $warn = [];
    set_error_handler(function (int $no, string $msg) use (&$warn) { $warn[] = $msg; return true; });
    $bad = [
        'odd vertex bytes' => ['vertices' => $rawV . "x", 'layout' => [VIO_FLOAT3]],
        'not a vertex multiple' => ['vertices' => pack('g*', 0, 0, 0, 1), 'layout' => [VIO_FLOAT3]],
        'index out of range' => ['vertices' => $rawV, 'indices' => pack('v*', 0, 1, 9), 'index_type' => VIO_INDEX_UINT16, 'layout' => [VIO_FLOAT3]],
        'index bytes not a multiple' => ['vertices' => $rawV, 'indices' => $raw16 . "x", 'index_type' => VIO_INDEX_UINT16, 'layout' => [VIO_FLOAT3]],
        'index width undeclared' => ['vertices' => $rawV, 'indices' => $raw16, 'index_type' => 3, 'layout' => [VIO_FLOAT3]],
    ];
    foreach ($bad as $label => $cfg) {
        $n = count($warn);
        $r = vio_mesh($ctx, $cfg);
        if ($r !== false) $fail[] = "$label: expected false";
        if (count($warn) === $n) $fail[] = "$label: expected a warning";
    }
    restore_error_handler();

    unset($ref, $m16, $m32, $mflat, $pipe);
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'metal', 'vulkan'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
DONE
