--TEST--
Bindless textures (VIO_FEATURE_BINDLESS): vio_texture_index() puts a texture into the context's table; one draw samples 64 textures through a non-uniform index into vio_textures[] on every backend that reports the flag
--EXTENSIONS--
vio
--FILE--
<?php
/* BINDLESS-PLAN.md. 64 solid-colour 2x2 textures, an 8x8 grid of quads in ONE
 * non-instanced draw; every quad carries its texture's table slot as a vertex
 * attribute, so the fragment stage indexes vio_textures[] non-uniformly.
 * Contract: the slot is stable for repeated calls, distinct per texture, and
 * the table keeps the texture alive (the PHP variables are dropped before the
 * draw). Rows may come back flipped on some backends: the grid is compared in
 * the orientation that matches. VIO_REQUIRE_BINDLESS makes listed backends
 * mandatory. */
$W = 64; $N = 8;
$VS = "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=1) in float aSlot;\n"
    . "layout(location=0) flat out int vslot;\n"
    . "void main(){ vslot = int(aSlot + 0.5); gl_Position = vec4(aPos, 1.0); }";
$FS = "#version 450\n#extension GL_EXT_nonuniform_qualifier : require\n"
    . "layout(location=0) flat in int vslot;\nlayout(location=0) out vec4 o;\n"
    . "layout(set = 1, binding = 0) uniform texture2D vio_textures[];\n"
    . "layout(set = 1, binding = 1) uniform sampler vio_sampler;\n"
    . "void main(){ o = texture(sampler2D(vio_textures[nonuniformEXT(vslot)], vio_sampler), vec2(0.5)); }";

$opts = ["width" => $W, "height" => $W, "headless" => true, "vsync" => false];
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_BINDLESS') ?: ''));

function colour(int $i): array { return [($i * 29 + 40) % 256, ($i * 71 + 10) % 256, ($i * 113 + 90) % 256]; }
function px(string $p, int $x, int $y, int $w): array { $o = ($y * $w + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }
function near(array $a, array $b): bool { return abs($a[0] - $b[0]) <= 2 && abs($a[1] - $b[1]) <= 2 && abs($a[2] - $b[2]) <= 2; }

function run_backend(string $name): string {
    global $W, $N, $VS, $FS, $opts, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    $tex0 = vio_texture($ctx, ['data' => str_repeat("\xFF\x00\x00\xFF", 4), 'width' => 2, 'height' => 2]);
    if (!vio_supports_feature($ctx, VIO_FEATURE_BINDLESS)) {
        $r = @vio_texture_index($ctx, $tex0);
        vio_destroy($ctx);
        if ($r !== false) return "FAIL\n  vio_texture_index without VIO_FEATURE_BINDLESS returned " . json_encode($r);
        return $req ? "FAIL\n  required but VIO_FEATURE_BINDLESS is 0" : "skip (no bindless)";
    }
    $fail = [];
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    if (!$sh) { vio_destroy($ctx); return "FAIL\n  bindless shader not created"; }
    $pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);

    /* Contract: stable, distinct. */
    $a = vio_texture_index($ctx, $tex0);
    $b = vio_texture_index($ctx, $tex0);
    if (!is_int($a) || $a !== $b) $fail[] = "slot not stable: " . json_encode([$a, $b]);

    $tex = []; $slot = [];
    for ($i = 0; $i < $N * $N; $i++) {
        $c = colour($i);
        $tex[$i] = vio_texture($ctx, ['data' => str_repeat(chr($c[0]) . chr($c[1]) . chr($c[2]) . "\xFF", 4), 'width' => 2, 'height' => 2]);
        $slot[$i] = vio_texture_index($ctx, $tex[$i]);
    }
    if (count(array_unique($slot)) !== $N * $N || in_array($a, $slot, true)) $fail[] = "slots not distinct";

    /* One draw: 64 quads (two triangles each), slot as a per-vertex attribute. */
    $v = [];
    for ($i = 0; $i < $N * $N; $i++) {
        $cx = $i % $N; $cy = intdiv($i, $N);
        $x0 = -1 + 2 * $cx / $N; $x1 = $x0 + 2 / $N; $y0 = -1 + 2 * $cy / $N; $y1 = $y0 + 2 / $N;
        foreach ([[$x0, $y0], [$x1, $y0], [$x1, $y1], [$x0, $y0], [$x1, $y1], [$x0, $y1]] as [$x, $y]) {
            array_push($v, $x, $y, 0.0, (float)$slot[$i]);
        }
    }
    $mesh = vio_mesh($ctx, ['vertices' => $v, 'layout' => [VIO_FLOAT3, VIO_FLOAT1]]);
    /* The table keeps the textures alive. */
    $tex = null; $tex0 = null; gc_collect_cycles();

    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pipe);
    vio_draw($ctx, $mesh);
    vio_end($ctx);
    $p = vio_read_pixels($ctx);
    $cell = intdiv($W, $N);
    $hits = [0, 0]; $first = [null, null];
    foreach ([0, 1] as $flip) {
        for ($i = 0; $i < $N * $N; $i++) {
            $cx = $i % $N; $cy = intdiv($i, $N);
            $row = $flip ? $cy : $N - 1 - $cy;   /* NDC +y up -> row 0 at the top unless flipped */
            $got = px($p, $cx * $cell + ($cell >> 1), $row * $cell + ($cell >> 1), $W);
            if (near($got, colour($i))) $hits[$flip]++;
            elseif ($first[$flip] === null) $first[$flip] = "quad $i " . json_encode($got) . " want " . json_encode(colour($i));
        }
    }
    $best = $hits[0] >= $hits[1] ? 0 : 1;
    if ($hits[$best] !== $N * $N) $fail[] = "{$hits[$best]} of " . ($N * $N) . " quads right, first wrong: {$first[$best]}";
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
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
