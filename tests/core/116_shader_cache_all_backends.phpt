--TEST--
On-disk shader cache: vio_create(['shader_cache' => dir]) stores compiled stages and serves them to the next context
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if ($c) { $any = true; vio_destroy($c); }
}
if (!$any) die("skip no GPU backend");
?>
--FILE--
<?php
/* GAP-PHASE5-PLAN Block 4. First context: compiling a shader stores its
 * artifact (DXBC per stage on D3D11/D3D12, a program binary on GL >= 4.1).
 * Second context in the same directory: the same source is a cache hit and the
 * shader still draws. Vulkan (2D only) writes its VkPipelineCache blob when the
 * context is destroyed; OpenGL below 4.1 has no program binaries and is
 * reported as such. */
$dir = sys_get_temp_dir() . '/vio-shader-cache-' . getmypid();
@mkdir($dir);
function files(string $dir): int { return count(array_filter(glob($dir . '/*') ?: [], 'is_file')); }
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }

function run_backend(string $name, string $dir): string {
    $W = 16;
    $opts = ["width" => $W, "height" => $W, "headless" => true, "vsync" => false, "shader_cache" => $dir];
    $ctx = @vio_create($name, $opts);
    if (!$ctx) return "skip (unavailable)";
    $has3d = vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE);
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $fs = "#version 330 core\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0.0, 0.0, 1.0, 1.0); }";
    $draw = static function ($ctx) use ($vs, $fs, $fmt, $W): array {
        $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
        $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_draw($ctx, $quad);
        vio_end($ctx);
        return px(vio_read_pixels($ctx), $W >> 1, $W >> 1, $W);
    };
    $fail = [];
    $before = vio_shader_cache_stats();
    if (($before['dir'] ?? null) !== $dir) $fail[] = "stats dir " . json_encode($before['dir'] ?? null);
    $files0 = files($dir);
    if ($has3d) {
        $p = $draw($ctx);
        if ($p !== [0, 0, 255]) $fail[] = "first draw not blue " . json_encode($p);
    } else {
        vio_begin($ctx); vio_rect($ctx, 2, 2, 8, 8, ['fill' => 0xFF0000FF]); vio_draw_2d($ctx); vio_end($ctx);
    }
    $mid = vio_shader_cache_stats();
    vio_destroy($ctx);
    $files1 = files($dir);

    /* GL < 4.1: program binaries unavailable — nothing stored, and that is fine. */
    $glNoBinary = $name === 'opengl' && $mid['stores'] === $before['stores'];
    if ($has3d && !$glNoBinary && $mid['stores'] <= $before['stores']) $fail[] = "first context stored nothing";
    if ($has3d && !$glNoBinary && $files1 <= $files0) $fail[] = "no cache files written";
    if (!$has3d && $files1 <= $files0) $fail[] = "pipeline cache file not written on destroy";

    if ($has3d && !$glNoBinary) {
        $ctx = @vio_create($name, $opts);
        if (!$ctx) return "FAIL\n  second context unavailable";
        $p = $draw($ctx);
        if ($p !== [0, 0, 255]) $fail[] = "cached draw not blue " . json_encode($p);
        $after = vio_shader_cache_stats();
        if ($after['hits'] <= $mid['hits']) $fail[] = "second context did not hit the cache (hits {$mid['hits']} -> {$after['hits']})";
        vio_destroy($ctx);
    }
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : ($glNoBinary ? "OK (no program binaries below GL 4.1)" : "OK");
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan'] as $b) {
    echo "$b: ", run_backend($b, $dir), "\n";
}
foreach (glob($dir . '/*') ?: [] as $f) @unlink($f);
@rmdir($dir);
echo "DONE\n";
?>
--EXPECTF--
opengl: %s
d3d11: %s
d3d12: %s
vulkan: %s
DONE
