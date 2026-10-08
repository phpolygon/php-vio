--TEST--
vio_texture_release_index frees a bindless slot: the slot is reused only after the frames that may still read it have finished, the table keeps the texture alive until then, a released texture can re-enter under a new slot, and other slots are unaffected
--EXTENSIONS--
vio
--FILE--
<?php
/* BINDLESS-PLAN Phase 4b / OPEN-ITEMS-PLAN A12. A released slot stays reserved for
 * VIO_BINDLESS_RETIRE_FRAMES (4) vio_begin calls - longer than any backend keeps
 * frames in flight - then holds a null entry and is handed out again. */
$VS = "#version 450\nlayout(location=0) in vec2 aPos;\nvoid main(){ gl_Position = vec4(aPos, 0.0, 1.0); }";
$FS = "#version 450\n#extension GL_EXT_nonuniform_qualifier : require\nlayout(location=0) out vec4 o;\nuniform int u_slot;\n"
    . "layout(set = 1, binding = 0) uniform texture2D vio_textures[];\n"
    . "layout(set = 1, binding = 1) uniform sampler vio_sampler;\n"
    . "void main(){ o = texture(sampler2D(vio_textures[nonuniformEXT(u_slot)], vio_sampler), vec2(0.5)); }";
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_BINDLESS') ?: ''));

function solid(VioContext $c, string $rgb): VioTexture {
    return vio_texture($c, ['data' => str_repeat($rgb . "\xFF", 4), 'width' => 2, 'height' => 2]);
}

function run_backend(string $name): string {
    global $VS, $FS, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_BINDLESS)) {
        $t = solid($ctx, "\xFF\x00\x00");
        $r = @vio_texture_release_index($ctx, $t);
        vio_destroy($ctx);
        if ($r !== false) return "FAIL\n  release without VIO_FEATURE_BINDLESS returned " . json_encode($r);
        return $req ? "FAIL\n  required but VIO_FEATURE_BINDLESS is 0" : "skip (no bindless)";
    }
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1, 1,-1, 1,1, -1,1], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT2]]);
    $draw = function (int $slot) use ($ctx, $pipe, $quad): string {
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniform($ctx, 'u_slot', $slot);
        vio_draw($ctx, $quad);
        vio_end($ctx);
        return bin2hex(substr(vio_read_pixels($ctx), (8 * 16 + 8) * 4, 3));
    };
    $err = [];

    $red = solid($ctx, "\xFF\x00\x00");
    $r = vio_texture_index($ctx, $red);
    if (($c = $draw($r)) !== 'ff0000') $err[] = "red slot $r draws $c";

    if (vio_texture_release_index($ctx, $red) !== true) $err[] = "release did not return true";
    if (vio_texture_release_index($ctx, $red) !== false) $err[] = "second release did not return false";
    if (@vio_texture_release_index($ctx, solid($ctx, "\x00\x00\x00")) !== false) $err[] = "release of a texture outside the table did not return false";

    /* Still retiring: neither the released texture nor a new one gets the slot. */
    $again = vio_texture_index($ctx, $red);
    if ($again === $r) $err[] = "re-indexing the released texture reused its retiring slot";
    vio_texture_release_index($ctx, $red);         // the re-entered slot retires too
    unset($red);                                   // only the table holds it now
    $green = solid($ctx, "\x00\xFF\x00");
    $g = vio_texture_index($ctx, $green);
    if ($g === $r) $err[] = "a new texture got the retiring slot $r";

    for ($f = 0; $f < 5; $f++) { vio_begin($ctx); vio_end($ctx); }

    $blue = solid($ctx, "\x00\x00\xFF");
    $b = vio_texture_index($ctx, $blue);
    if (!in_array($b, [$r, $again], true)) $err[] = "slot of a retired entry not reused (got $b, retired $r / $again)";
    if (($c = $draw($b)) !== '0000ff') $err[] = "reused slot $b draws $c";
    if (($c = $draw($g)) !== '00ff00') $err[] = "green slot $g draws $c";

    vio_destroy($ctx);
    return $err ? "FAIL\n  " . implode("\n  ", $err) : "OK";
}

foreach (['d3d12', 'vulkan', 'metal', 'opengl', 'd3d11'] as $b) echo "$b: ", run_backend($b), "\n";
echo "DONE\n";
?>
--EXPECTF--
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
DONE
