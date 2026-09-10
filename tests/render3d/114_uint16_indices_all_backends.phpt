--TEST--
16-bit index buffers: chosen automatically when every index fits, drawn correctly on every backend
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null' || $b === 'vulkan') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with a 3D pipeline");
?>
--FILE--
<?php
/* GAP-PHASE5-PLAN Block 2. vio_mesh() stores uint16 indices when the largest
 * index is < 65536 and uint32 otherwise (or when 'index_type' forces it). The
 * quad below is drawn through the 16-bit buffer; the draw must pick R16 / GL_UNSIGNED_SHORT
 * / MTLIndexTypeUInt16 or the second triangle reads garbage. */
$W = 16;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b): bool { return abs($a[0]-$b[0]) <= 3 && abs($a[1]-$b[1]) <= 3 && abs($a[2]-$b[2]) <= 3; }

function run_backend(string $name): string {
    global $W;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D)"; }
    $fail = [];
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $fs = "#version 330 core\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0.0, 1.0, 0.0, 1.0); }";
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);

    $verts = [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0];
    $small = vio_mesh($ctx, ['vertices' => $verts, 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $wide  = vio_mesh($ctx, ['vertices' => $verts, 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3], 'index_type' => VIO_INDEX_UINT32]);
    $plain = vio_mesh($ctx, ['vertices' => $verts, 'layout' => [VIO_FLOAT3]]);
    if (vio_mesh_index_bytes($small) !== 2) $fail[] = "small mesh should use 2-byte indices, got " . vio_mesh_index_bytes($small);
    if (vio_mesh_index_bytes($wide) !== 4)  $fail[] = "forced uint32 mesh should use 4-byte indices, got " . vio_mesh_index_bytes($wide);
    if (vio_mesh_index_bytes($plain) !== 0) $fail[] = "unindexed mesh should report 0";
    /* A big index range: pad vertices so index 70000 exists (positions never used). */
    $big = array_merge($verts, array_fill(0, (70001 - 4) * 3, 0.0));
    $large = vio_mesh($ctx, ['vertices' => $big, 'indices' => [0,1,2, 0,2,70000], 'layout' => [VIO_FLOAT3]]);
    if (vio_mesh_index_bytes($large) !== 4) $fail[] = "index >= 65536 must select 4-byte indices";

    foreach (['small' => $small, 'wide' => $wide] as $label => $mesh) {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_draw($ctx, $mesh);
        vio_end($ctx);
        $p = vio_read_pixels($ctx);
        /* Both triangles of the quad: top-right and bottom-left corners both green. */
        foreach ([[2, 2], [$W - 3, $W - 3], [$W - 3, 2], [2, $W - 3]] as [$x, $y]) {
            if (!near(px($p, $x, $y, $W), [0, 255, 0])) { $fail[] = "$label mesh: pixel ($x,$y) not green " . json_encode(px($p, $x, $y, $W)); break; }
        }
    }
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'metal'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %s
d3d11: %s
d3d12: %s
metal: %s
DONE
