--TEST--
Bindless cubemaps and texture arrays: vio_texture_index takes a VioCubemap (vio_cubes[], set 1 binding 5) and a layered VioTexture (vio_texture_arrays[], binding 6) in the same slot space as 2D textures; release and reuse work across kinds; 3D textures stay out
--EXTENSIONS--
vio
--FILE--
<?php
/* BINDLESS-PLAN Phase 4b / OPEN-ITEMS-PLAN A12. */
$VS = "#version 450\nlayout(location=0) in vec2 aPos;\nvoid main(){ gl_Position = vec4(aPos, 0.0, 1.0); }";
$FS = "#version 450\n#extension GL_EXT_nonuniform_qualifier : require\nlayout(location=0) out vec4 o;\n"
    . "uniform int u_slot;\nuniform int u_kind;\nuniform vec4 u_arg;\n"
    . "layout(set = 1, binding = 0) uniform texture2D vio_textures[];\n"
    . "layout(set = 1, binding = 1) uniform sampler vio_sampler;\n"
    . "layout(set = 1, binding = 5) uniform textureCube vio_cubes[];\n"
    . "layout(set = 1, binding = 6) uniform texture2DArray vio_texture_arrays[];\n"
    . "void main(){\n"
    . "  if (u_kind == 0)      o = texture(sampler2D(vio_textures[nonuniformEXT(u_slot)], vio_sampler), vec2(0.5));\n"
    . "  else if (u_kind == 1) o = texture(samplerCube(vio_cubes[nonuniformEXT(u_slot)], vio_sampler), u_arg.xyz);\n"
    . "  else                  o = texture(sampler2DArray(vio_texture_arrays[nonuniformEXT(u_slot)], vio_sampler), vec3(0.5, 0.5, u_arg.w));\n"
    . "}";
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_BINDLESS') ?: ''));

function rgba(array $c, int $n): string { return str_repeat(chr($c[0]) . chr($c[1]) . chr($c[2]) . "\xFF", $n); }

function run_backend(string $name): string {
    global $VS, $FS, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_BINDLESS)) { vio_destroy($ctx); return $req ? "FAIL\n  required but VIO_FEATURE_BINDLESS is 0" : "skip (no bindless)"; }
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    if (!$sh) { vio_destroy($ctx); return "FAIL\n  shader with vio_cubes / vio_texture_arrays not created"; }
    $pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1, 1,-1, 1,1, -1,1], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT2]]);
    $draw = function (int $slot, int $kind, array $arg) use ($ctx, $pipe, $quad): string {
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniforms($ctx, ['u_slot' => $slot, 'u_kind' => $kind, 'u_arg' => $arg]);
        vio_draw($ctx, $quad);
        vio_end($ctx);
        return bin2hex(substr(vio_read_pixels($ctx), (8 * 16 + 8) * 4, 3));
    };
    $err = [];

    $faces = [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 0], [0, 255, 255], [255, 0, 255]];   // +X -X +Y -Y +Z -Z
    $cube = vio_cubemap($ctx, ['pixels' => array_map(fn($c) => array_merge(...array_fill(0, 16, [$c[0], $c[1], $c[2], 255])), $faces),
                               'width' => 4, 'height' => 4]);
    $arr  = vio_texture($ctx, ['data' => rgba([255, 128, 0], 4) . rgba([0, 128, 255], 4), 'width' => 2, 'height' => 2, 'layers' => 2]);
    $flat = vio_texture($ctx, ['data' => rgba([255, 255, 255], 4), 'width' => 2, 'height' => 2]);
    $t = vio_texture_index($ctx, $flat);
    $c = vio_texture_index($ctx, $cube);
    $a = vio_texture_index($ctx, $arr);
    if (!is_int($c) || !is_int($a) || count(array_unique([$t, $c, $a])) !== 3) $err[] = "slots not distinct ints: " . json_encode([$t, $c, $a]);
    if (vio_texture_index($ctx, $cube) !== $c) $err[] = "cube slot not stable";

    if (is_int($c)) {
        foreach ([[[1, 0, 0], 'ff0000'], [[-1, 0, 0], '00ff00'], [[0, 0, 1], '00ffff'], [[0, 0, -1], 'ff00ff']] as [$dir, $want]) {
            $got = $draw($c, 1, [$dir[0], $dir[1], $dir[2], 0]);
            if ($got !== $want) $err[] = "cube " . json_encode($dir) . ": $got, want $want";
        }
    }
    if (is_int($a)) {
        if (($got = $draw($a, 2, [0, 0, 0, 0])) !== 'ff8000') $err[] = "array layer 0: $got";
        if (($got = $draw($a, 2, [0, 0, 0, 1])) !== '0080ff') $err[] = "array layer 1: $got";
    }
    if (($got = $draw($t, 0, [0, 0, 0, 0])) !== 'ffffff') $err[] = "2D next to them: $got";

    /* A released cube slot comes back as a 2D slot. */
    if (vio_texture_release_index($ctx, $cube) !== true) $err[] = "cube release";
    for ($f = 0; $f < 5; $f++) { vio_begin($ctx); vio_end($ctx); }
    $red = vio_texture($ctx, ['data' => rgba([255, 0, 0], 4), 'width' => 2, 'height' => 2]);
    $r = vio_texture_index($ctx, $red);
    if ($r !== $c) $err[] = "retired cube slot $c not reused (got " . json_encode($r) . ")";
    if (($got = $draw($r, 0, [0, 0, 0, 0])) !== 'ff0000') $err[] = "2D in the former cube slot: $got";

    $vol = vio_texture_3d($ctx, ['data' => rgba([0, 0, 0], 8), 'width' => 2, 'height' => 2, 'depth' => 2]);
    if ($vol && @vio_texture_index($ctx, $vol) !== false) $err[] = "3D texture accepted";

    vio_destroy($ctx);
    return $err ? "FAIL\n  " . implode("\n  ", $err) : "OK";
}

foreach (['d3d12', 'vulkan', 'metal'] as $b) echo "$b: ", run_backend($b), "\n";
echo "DONE\n";
?>
--EXPECTF--
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE
