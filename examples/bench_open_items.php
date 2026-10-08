<?php
/**
 * Measurement series for the OPEN-ITEMS features that should pay off in
 * performance: uniform paths (A32), draw submission (A40 / instancing),
 * per-draw texture binds vs bindless (A12), TLAS refit / rebuild / full build
 * (A14) and the cost of a per-frame readback (A36).
 *
 *   php -d extension=vio examples/bench_open_items.php [backends] [objects] [frames] [json-out] [load]
 *
 *   backends  comma- or '+'-separated ('+' survives cmd.exe), default all
 *   load      light | medium | heavy, '+'-joined, or both (light + heavy) / all (default both)
 *
 * Two axes:
 *   - submission (the scenarios): how the same objects reach the GPU;
 *   - GPU load: "light" = 256x256, tiny quads, a flat fragment stage - the GPU
 *     idles, the frame is bound by the CPU cost of the draws. "medium" =
 *     1920x1080, quads 2x larger, a 24-step fragment loop - CPU and GPU cost of
 *     the same order. "heavy" = 1920x1080, quads 4x larger (about ten screens of
 *     overdraw), a 96-step loop - the GPU is the limit, which shows how much of
 *     a CPU saving is left once the GPU is busy.
 *
 * Every (backend, load) runs in its own process. CPU time is the wall time of
 * vio_begin .. vio_end: recording and submission, plus any wait on an older
 * frame's fence when the GPU falls behind. GPU time is vio_gpu_frame_time
 * (timestamps of the last finished frame). Roughly: frame time = max(CPU, GPU).
 * Headless D3D runs on WARP unless VIO_D3D_HEADLESS_HARDWARE=1.
 */

$backends = preg_split('/[,+]/', $argv[1] ?? 'opengl,d3d11,d3d12,vulkan,metal');
$N        = (int)($argv[2] ?? 2000);
$FRAMES   = (int)($argv[3] ?? 40);
$JSON     = $argv[4] ?? null;
$LOADARG  = $argv[5] ?? 'both';
$loads    = $LOADARG === 'all' ? ['light', 'medium', 'heavy'] : ($LOADARG === 'both' ? ['light', 'heavy'] : preg_split('/[,+]/', $LOADARG));
$WARMUP   = 5;

if (count($backends) > 1 || count($loads) > 1) {
    $all = [];
    foreach ($loads as $load) {
        foreach ($backends as $b) {
            $tmp = tempnam(sys_get_temp_dir(), 'viobench');
            $cmd = escapeshellarg(PHP_BINARY)
                 . ' -n -d extension_dir=' . escapeshellarg(ini_get('extension_dir'))
                 . ' -d extension=' . (PHP_OS_FAMILY === 'Windows' ? 'php_vio.dll' : 'vio')
                 . ' ' . escapeshellarg(__FILE__) . ' ' . escapeshellarg($b) . " $N $FRAMES " . escapeshellarg($tmp) . " $load";
            passthru($cmd);
            $r = json_decode((string)@file_get_contents($tmp), true);
            @unlink($tmp);
            if (is_array($r)) $all[$load][$b] = $r;
        }
        print_summary($all[$load] ?? [], $N, $load);
    }
    if ($JSON) file_put_contents($JSON, json_encode(['objects' => $N, 'frames' => $FRAMES, 'results' => $all], JSON_PRETTY_PRINT));
    exit(0);
}

$res = run_backend($backends[0], $N, $FRAMES, $WARMUP, $loads[0]);
if ($JSON) file_put_contents($JSON, json_encode($res));
else print_summary([$backends[0] => $res], $N, $loads[0]);

/* ───────────────────────────────────────────────────────────────── */

/** Runs $body once per frame; returns CPU / GPU averages in ms. */
function measure($ctx, callable $body, int $frames, int $warmup): array
{
    $cpu = []; $gpu = [];
    for ($f = 0; $f < $warmup + $frames; $f++) {
        $t0 = hrtime(true);
        vio_begin($ctx);
        vio_clear($ctx, 0, 0, 0, 1);
        $body();
        vio_end($ctx);
        $t1 = hrtime(true);
        if ($f < $warmup) continue;
        $cpu[] = ($t1 - $t0) / 1e6;
        $g = vio_gpu_frame_time($ctx);
        if ($g >= 0) $gpu[] = $g;
    }
    sort($cpu);
    return [
        'cpu_ms'  => round(array_sum($cpu) / count($cpu), 3),
        'cpu_p50' => round($cpu[intdiv(count($cpu), 2)], 3),
        'gpu_ms'  => $gpu ? round(array_sum($gpu) / count($gpu), 3) : null,
    ];
}

/** Load levels: target size, quad edge factor, fragment loop steps. */
function load_params(string $load): array
{
    return match ($load) {
        'heavy'  => [1920, 1080, 4.0, 96],
        'medium' => [1920, 1080, 2.0, 24],
        default  => [256, 256, 1.0, 0],
    };
}

/** Fragment work of the load level: a flat colour or an N-step hash loop. */
function cost_fn(int $steps): string
{
    if ($steps <= 0) return "vec4 vio_cost(vec4 c){ return c; }\n";
    return "vec4 vio_cost(vec4 c){ vec3 a = c.rgb + gl_FragCoord.xyx * 0.001;\n"
         . "  for (int k = 0; k < $steps; k++) a = fract(sin(a * 12.9898 + float(k)) * 43758.5453);\n"
         . "  return vec4(mix(c.rgb, a, 0.002), c.a); }\n";
}

function run_backend(string $name, int $N, int $frames, int $warmup, string $load): array
{
    [$W, $H, $scale, $steps] = load_params($load);
    $heavy = $load !== 'light';   /* readback and TLAS are measured on the light run only */
    $ctx = @vio_create($name, ['width' => $W, 'height' => $H, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return ['skip' => 'unavailable'];
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return ['skip' => 'no 3D pipeline']; }
    $info = @vio_gpu_info();
    $out = ['adapter' => is_array($info) ? ($info['name'] ?? '?') : '?', 'load' => $load, 'size' => "{$W}x{$H}", 'scenarios' => []];
    $base = ['depth_test' => false, 'cull_mode' => VIO_CULL_NONE];
    $COST = cost_fn($steps);

    /* N quads spread over the target; medium / heavy: 2x / 4x the edge */
    $side = (int)ceil(sqrt($N));
    $s = 0.8 / $side * $scale;
    $offs = []; $cols = []; $packed = []; $mats = '';
    for ($i = 0; $i < $N; $i++) {
        $x = -0.95 + 1.9 * (($i % $side) + 0.5) / $side;
        $y = -0.95 + 1.9 * (intdiv($i, $side) + 0.5) / $side;
        $offs[$i] = [$x, $y, 0.0, 0.0];
        $cols[$i] = [($i % 7) / 7, ($i % 11) / 11, ($i % 13) / 13, 1.0];
        $packed[$i] = pack('f8', $x, $y, 0, 0, ...$cols[$i]);
        $mats .= pack('f16', $s, 0, 0, 0, 0, $s, 0, 0, 0, 0, 1, 0, $x, $y, 0, 1);
    }
    $quad = vio_mesh($ctx, ['vertices' => [-$s,-$s, $s,-$s, $s,$s, -$s,$s], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT2]]);

    /* ── 1. uniforms: vio_set_uniform x2 / vio_set_uniforms / UBO update ── */
    $VS_U = "#version 450\nlayout(location=0) in vec2 aPos;\nuniform vec4 u_offset;\nvoid main(){ gl_Position = vec4(aPos + u_offset.xy, 0.0, 1.0); }";
    $FS_U = "#version 450\nlayout(location=0) out vec4 o;\nuniform vec4 u_color;\n{$COST}void main(){ o = vio_cost(u_color); }";
    $VS_B = "#version 450\nlayout(location=0) in vec2 aPos;\nlayout(std140, binding = 0) uniform Obj { vec4 u_offset; vec4 u_color; };\n"
          . "layout(location=0) out vec4 vcol;\nvoid main(){ vcol = u_color; gl_Position = vec4(aPos + u_offset.xy, 0.0, 1.0); }";
    $FS_B = "#version 450\nlayout(location=0) in vec4 vcol;\nlayout(location=0) out vec4 o;\n{$COST}void main(){ o = vio_cost(vcol); }";
    $pU = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS_U, 'fragment' => $FS_U])] + $base);
    $pB = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS_B, 'fragment' => $FS_B])] + $base);
    $ubo = vio_uniform_buffer($ctx, ['size' => 32, 'binding' => 0]);

    $out['scenarios']['uniform: vio_set_uniform x2'] = measure($ctx, function () use ($ctx, $pU, $quad, $offs, $cols, $N) {
        vio_bind_pipeline($ctx, $pU);
        for ($i = 0; $i < $N; $i++) {
            vio_set_uniform($ctx, 'u_offset', $offs[$i]);
            vio_set_uniform($ctx, 'u_color', $cols[$i]);
            vio_draw($ctx, $quad);
        }
    }, $frames, $warmup);
    $out['scenarios']['uniform: vio_set_uniforms'] = measure($ctx, function () use ($ctx, $pU, $quad, $offs, $cols, $N) {
        vio_bind_pipeline($ctx, $pU);
        for ($i = 0; $i < $N; $i++) {
            vio_set_uniforms($ctx, ['u_offset' => $offs[$i], 'u_color' => $cols[$i]]);
            vio_draw($ctx, $quad);
        }
    }, $frames, $warmup);
    $out['scenarios']['uniform: UBO update'] = measure($ctx, function () use ($ctx, $pB, $ubo, $quad, $packed, $N) {
        vio_bind_pipeline($ctx, $pB);
        vio_bind_buffer($ctx, $ubo);
        for ($i = 0; $i < $N; $i++) {
            vio_update_buffer($ubo, $packed[$i]);
            vio_draw($ctx, $quad);
        }
    }, $frames, $warmup);

    /* ── 2. submission: vio_submit_batch / instanced ── */
    $records = [];
    for ($i = 0; $i < $N; $i++) $records[] = ['mesh' => $quad, 'uniforms' => ['u_offset' => $offs[$i], 'u_color' => $cols[$i]]];
    $records[0]['pipeline'] = $pU;
    $out['scenarios']['submit: vio_submit_batch'] = measure($ctx, function () use ($ctx, $records) {
        vio_submit_batch($ctx, $records);
    }, $frames, $warmup);
    $VS_I = "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=3) in mat4 aModel;\n"
          . "void main(){ gl_Position = aModel * vec4(aPos, 1.0); }";
    $FS_I = "#version 450\nlayout(location=0) out vec4 o;\n{$COST}void main(){ o = vio_cost(vec4(0.4, 0.7, 1.0, 1.0)); }";
    $pI = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS_I, 'fragment' => $FS_I])] + $base);
    $quad3 = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $out['scenarios']['submit: vio_draw_instanced'] = measure($ctx, function () use ($ctx, $pI, $quad3, $mats, $N) {
        vio_bind_pipeline($ctx, $pI);
        vio_draw_instanced($ctx, $quad3, $mats, $N);
    }, $frames, $warmup);

    /* ── 3. textures: a bind per draw vs one bindless draw ── */
    $T = min($N, 256);   /* distinct textures */
    $tex = []; $slots = [];
    $bindless = vio_supports_feature($ctx, VIO_FEATURE_BINDLESS);
    for ($t = 0; $t < $T; $t++) {
        $tex[$t] = vio_texture($ctx, ['data' => str_repeat(chr($t) . chr(255 - $t) . chr(($t * 7) & 255) . "\xFF", 16), 'width' => 4, 'height' => 4]);
        if ($bindless) $slots[$t] = vio_texture_index($ctx, $tex[$t]);
    }
    $FS_T = "#version 450\nlayout(location=0) out vec4 o;\nuniform sampler2D u_tex;\n{$COST}void main(){ o = vio_cost(texture(u_tex, vec2(0.5))); }";
    $pT = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS_U, 'fragment' => $FS_T])] + $base);
    $out['scenarios']['textures: bind per draw'] = measure($ctx, function () use ($ctx, $pT, $quad, $offs, $tex, $T, $N) {
        vio_bind_pipeline($ctx, $pT);
        vio_set_uniform($ctx, 'u_tex', 0);
        for ($i = 0; $i < $N; $i++) {
            vio_bind_texture($ctx, $tex[$i % $T], 0);
            vio_set_uniform($ctx, 'u_offset', $offs[$i]);
            vio_draw($ctx, $quad);
        }
    }, $frames, $warmup);
    $pX = false;
    if ($bindless) {
        $VS_X = "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=1) in float aSlot;\nlayout(location=0) flat out int vslot;\n"
              . "void main(){ vslot = int(aSlot + 0.5); gl_Position = vec4(aPos, 1.0); }";
        $FS_X = "#version 450\n#extension GL_EXT_nonuniform_qualifier : require\nlayout(location=0) flat in int vslot;\nlayout(location=0) out vec4 o;\n"
              . "layout(set = 1, binding = 0) uniform texture2D vio_textures[];\nlayout(set = 1, binding = 1) uniform sampler vio_sampler;\n"
              . "{$COST}void main(){ o = vio_cost(texture(sampler2D(vio_textures[nonuniformEXT(vslot)], vio_sampler), vec2(0.5))); }";
        $shX = @vio_shader($ctx, ['vertex' => $VS_X, 'fragment' => $FS_X]);
        $pX = $shX ? vio_pipeline($ctx, ['shader' => $shX] + $base) : false;
    }
    if ($pX) {
        $v = [];
        for ($i = 0; $i < $N; $i++) {
            [$x, $y] = $offs[$i]; $sl = (float)$slots[$i % $T];
            foreach ([[-$s,-$s], [$s,-$s], [$s,$s], [-$s,-$s], [$s,$s], [-$s,$s]] as [$dx, $dy]) array_push($v, $x + $dx, $y + $dy, 0.0, $sl);
        }
        $merged = vio_mesh($ctx, ['vertices' => $v, 'layout' => [VIO_FLOAT3, VIO_FLOAT1]]);
        $out['scenarios']['textures: bindless, one draw'] = measure($ctx, function () use ($ctx, $pX, $merged) {
            vio_bind_pipeline($ctx, $pX);
            vio_draw($ctx, $merged);
        }, $frames, $warmup);
    } else {
        $out['scenarios']['textures: bindless, one draw'] = ['skip' => 'no bindless'];
    }

    if (!$heavy) {
        /* ── 4. per-frame readback ── */
        $light = function () use ($ctx, $pI, $quad3, $mats) { vio_bind_pipeline($ctx, $pI); vio_draw_instanced($ctx, $quad3, $mats, 64); };
        $out['scenarios']['frame: light, no readback'] = measure($ctx, $light, $frames, $warmup);
        $rb = [];
        for ($f = 0; $f < $warmup + $frames; $f++) {
            $t0 = hrtime(true);
            vio_begin($ctx); vio_clear($ctx, 0, 0, 0, 1); $light(); vio_end($ctx);
            vio_read_pixels($ctx);
            if ($f >= $warmup) $rb[] = (hrtime(true) - $t0) / 1e6;
        }
        sort($rb);
        $out['scenarios']['frame: light + vio_read_pixels'] = ['cpu_ms' => round(array_sum($rb) / count($rb), 3), 'cpu_p50' => round($rb[intdiv(count($rb), 2)], 3), 'gpu_ms' => null];
    }
    vio_destroy($ctx);

    /* ── 5. TLAS (independent of the load axis, measured once) ── */
    if (!$heavy) $out['scenarios'] += bench_tlas($name);
    return $out;
}

function bench_tlas(string $name): array
{
    $opts = ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false, 'shader_model' => 6];
    foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $c) $opts['dxc_dir'] = dirname($c);
    $ctx = @vio_create($name, $opts);
    if (!$ctx || vio_backend_name($ctx) !== $name || !vio_supports_feature($ctx, VIO_FEATURE_RAY_QUERY)) {
        if ($ctx) vio_destroy($ctx);
        return ['tlas: refit (256 inst.)' => ['skip' => 'no ray query']];
    }
    $K = 256; $REP = 20;
    $mesh = vio_mesh($ctx, ['vertices' => [-1,-1,0.5, 1,-1,0.5, 1,1,0.5, -1,1,0.5], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $inst = function (int $k, float $dx) use ($mesh): array {
        $l = [];
        for ($i = 0; $i < $k; $i++) $l[] = ['mesh' => $mesh, 'transform' => [0.05,0,0,0, 0,0.05,0,0, 0,0,1,0, ($i % 16) * 0.1 - 0.8 + $dx, intdiv($i, 16) * 0.1 - 0.8, 0, 1]];
        return $l;
    };
    $as = vio_acceleration_structure($ctx, $inst($K, 0.0));
    $time = function (callable $f) use ($REP): float {
        $f(0); $t0 = hrtime(true);
        for ($r = 0; $r < $REP; $r++) $f($r);
        return round((hrtime(true) - $t0) / 1e6 / $REP, 3);
    };
    $res = [];
    $lists = [$inst($K, 0.0), $inst($K, 0.01)];
    $res['tlas: refit (256 inst.)'] = ['cpu_ms' => $time(function ($r) use ($ctx, $as, $lists) { vio_acceleration_structure_update($ctx, $as, $lists[$r & 1]); }), 'gpu_ms' => null];
    $lists2 = [$inst($K, 0.0), $inst($K - 1, 0.0)];
    $res['tlas: rebuild (256 inst.)'] = ['cpu_ms' => $time(function ($r) use ($ctx, $as, $lists2) { vio_acceleration_structure_update($ctx, $as, $lists2[$r & 1]); }), 'gpu_ms' => null];
    $res['tlas: full build (256 inst.)'] = ['cpu_ms' => $time(function ($r) use ($ctx, $inst, $K) { $a = vio_acceleration_structure($ctx, $inst($K, 0.0)); unset($a); }), 'gpu_ms' => null];
    vio_destroy($ctx);
    return $res;
}

function print_summary(array $all, int $N, string $load): void
{
    if (!$all) return;
    $rows = [];
    foreach ($all as $r) foreach (array_keys($r['scenarios'] ?? []) as $k) $rows[$k] = true;
    $names = array_keys($all);
    $first = reset($all);
    printf("\n== load: %s (%s) - %d objects. CPU = ms/frame (vio_begin..vio_end), GPU = vio_gpu_frame_time; TLAS: ms/operation ==\n",
           $load, $first['size'] ?? '?', $N);
    foreach ($all as $b => $r) printf("  %-7s %s\n", $b, $r['skip'] ?? ($r['adapter'] ?? ''));
    printf("%-34s", 'scenario');
    foreach ($names as $b) printf(" | %-19s", "$b CPU / GPU");
    echo "\n", str_repeat('-', 34 + 22 * count($names)), "\n";
    foreach (array_keys($rows) as $k) {
        printf("%-34s", $k);
        foreach ($names as $b) {
            $c = $all[$b]['scenarios'][$k] ?? null;
            if (!$c) $cell = '-';
            elseif (isset($c['skip'])) $cell = 'skip';
            else $cell = sprintf('%7.3f / %s', $c['cpu_ms'], $c['gpu_ms'] === null ? '   -  ' : sprintf('%6.3f', $c['gpu_ms']));
            printf(" | %-19s", $cell);
        }
        echo "\n";
    }
}
