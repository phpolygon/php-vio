--TEST--
The vertex input layout follows the mesh layout on every backend: attributes listed out of location order, an attribute the shader does not read between two it does, and the plain flat layout all deliver the right data to the vertex shader
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A31. D3D11 / D3D12 / Metal built the input layout from the
 * shader reflection, packed densely in location order, so a mesh whose memory
 * order differed from that read the wrong offsets (OpenGL always used the
 * mesh's own VAO). Each case draws one full-screen quad whose colour comes
 * from a vertex attribute; the centre pixel must be that colour. */
$VS = "#version 450\nlayout(location=0) in vec2 aPos;\nlayout(location=1) in vec3 aCol;\nlayout(location=0) out vec3 vCol;\n"
    . "void main(){ vCol = aCol; gl_Position = vec4(aPos, 0.0, 1.0); }";
$FS = "#version 450\nlayout(location=0) in vec3 vCol;\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(vCol, 1.0); }";

$pos = [[-1, -1], [1, -1], [1, 1], [-1, 1]];
function verts(array $pos, callable $one): array { $v = []; foreach ($pos as $p) array_push($v, ...$one($p)); return $v; }

$cases = [
    // memory order: colour (loc 1) then position (loc 0)
    'reordered' => [
        'layout' => [['location' => 1, 'components' => 3], ['location' => 0, 'components' => 2]],
        'vertices' => verts($pos, fn($p) => [1, 0, 1, $p[0], $p[1]]),
        'want' => 'ff00ff',
    ],
    // an unused location 7 between position and colour
    'gap' => [
        'layout' => [['location' => 0, 'components' => 2], ['location' => 7, 'components' => 2], ['location' => 1, 'components' => 3]],
        'vertices' => verts($pos, fn($p) => [$p[0], $p[1], 0.5, 0.5, 0, 1, 1]),
        'want' => '00ffff',
    ],
    // the flat form: sequential locations, unchanged behaviour
    'flat' => [
        'layout' => [VIO_FLOAT2, VIO_FLOAT3],
        'vertices' => verts($pos, fn($p) => [$p[0], $p[1], 1, 1, 0]),
        'want' => 'ffff00',
    ],
];

function run_backend(string $name): string {
    global $VS, $FS, $cases;
    $ctx = @vio_create($name, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D pipeline)"; }
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $err = [];
    foreach ($cases as $label => $c) {
        $mesh = vio_mesh($ctx, ['vertices' => $c['vertices'], 'indices' => [0, 1, 2, 0, 2, 3], 'layout' => $c['layout']]);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx); vio_bind_pipeline($ctx, $pipe); vio_draw($ctx, $mesh); vio_end($ctx);
        $px = bin2hex(substr(vio_read_pixels($ctx), (8 * 16 + 8) * 4, 3));
        if ($px !== $c['want']) $err[] = "$label: centre $px, want {$c['want']}";
    }
    vio_destroy($ctx);
    return $err ? "FAIL\n  " . implode("\n  ", $err) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) echo "$b: ", run_backend($b), "\n";
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE
