<?php
/**
 * Backend comparison: the same scenes on every backend this build can open.
 *
 *   php -d extension=vio examples/bench_backends.php [backends] [frames] [--json=file]
 *
 *   backends  comma-separated list, default: every registered backend except "null"
 *   frames    measured frames per scenario (default 600), after 60 warm-up frames
 *
 * Each scenario is drawn into a 1280x720 headless context without vsync. Two
 * times are taken per frame:
 *   submit  vio_begin() .. vio_draw_2d()   what the application thread spends
 *                                           issuing the frame
 *   frame   vio_begin() .. vio_end()       the whole frame including present;
 *                                           without vsync this is the frame
 *                                           time the backend sustains
 *   gpu     vio_gpu_frame_time()           what the GPU spent on the frame
 *                                           (backends with GPU timestamps)
 * Headless Direct3D contexts are asked for the hardware adapter
 * ('headless_hardware'); the adapter each backend ran on is printed.
 */

$argvRest = array_values(array_filter(array_slice($argv, 1), fn($a) => !str_starts_with($a, '--')));
$jsonOut = null;
foreach ($argv as $a) if (str_starts_with($a, '--json=')) $jsonOut = substr($a, 7);
$backends = isset($argvRest[0]) && $argvRest[0] !== ''
    ? explode(',', $argvRest[0])
    : array_values(array_filter(vio_backends(), fn($b) => $b !== 'null'));
$frames = (int)($argvRest[1] ?? 600);
const WARMUP = 60;
const BW = 1280;
const BH = 720;

function find_font(): ?string {
    foreach (['C:/Windows/Fonts/segoeui.ttf', 'C:/Windows/Fonts/arial.ttf', '/System/Library/Fonts/Supplemental/Arial.ttf',
              '/System/Library/Fonts/Helvetica.ttc', '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'] as $f) {
        if (is_file($f)) return $f;
    }
    return null;
}

function m_mul(array $a, array $b): array {
    $r = array_fill(0, 16, 0.0);
    for ($c = 0; $c < 4; $c++) for ($row = 0; $row < 4; $row++) {
        $s = 0.0;
        for ($k = 0; $k < 4; $k++) $s += $a[$k * 4 + $row] * $b[$c * 4 + $k];
        $r[$c * 4 + $row] = $s;
    }
    return $r;
}
function m_perspective(float $fovy, float $aspect, float $n, float $f): array {
    $t = 1.0 / tan($fovy / 2);
    return [$t / $aspect,0,0,0, 0,$t,0,0, 0,0,($f + $n) / ($n - $f),-1, 0,0,(2 * $f * $n) / ($n - $f),0];
}
function m_translate(float $x, float $y, float $z): array { return [1,0,0,0, 0,1,0,0, 0,0,1,0, $x,$y,$z,1]; }

function cube(VioContext $ctx): VioMesh {
    $faces = [[[0,0,1],[1,0,0],[0,1,0]], [[0,0,-1],[-1,0,0],[0,1,0]], [[1,0,0],[0,0,-1],[0,1,0]],
              [[-1,0,0],[0,0,1],[0,1,0]], [[0,1,0],[1,0,0],[0,0,-1]], [[0,-1,0],[1,0,0],[0,0,1]]];
    $v = []; $idx = []; $base = 0;
    foreach ($faces as [$n, $u, $w]) {
        foreach ([[-1,-1],[1,-1],[1,1],[-1,1]] as [$su, $sv]) {
            for ($k = 0; $k < 3; $k++) $v[] = ($n[$k] + $u[$k] * $su + $w[$k] * $sv) * 0.5;
            array_push($v, $n[0], $n[1], $n[2]);
        }
        array_push($idx, $base, $base + 1, $base + 2, $base, $base + 2, $base + 3);
        $base += 4;
    }
    return vio_mesh($ctx, ['vertices' => $v, 'indices' => $idx, 'layout' => [VIO_FLOAT3, VIO_FLOAT3]]);
}

const VS_DRAW = <<<'GLSL'
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
uniform mat4 u_mvp;
uniform vec3 u_tint;
out vec3 vC;
void main() { vC = u_tint * (0.4 + 0.6 * max(dot(aNormal, normalize(vec3(0.4, 0.8, 0.5))), 0.0)); gl_Position = u_mvp * vec4(aPos, 1.0); }
GLSL;
const VS_INST = <<<'GLSL'
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=3) in mat4 aModel;
uniform mat4 u_vp;
out vec3 vC;
void main() { vC = vec3(0.4 + 0.6 * max(dot(aNormal, normalize(vec3(0.4, 0.8, 0.5))), 0.0)); gl_Position = u_vp * aModel * vec4(aPos, 1.0); }
GLSL;
const FS_FLAT = "#version 330 core\nin vec3 vC;\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(vC, 1.0); }";

function pipeline(VioContext $ctx, string $vs): VioPipeline {
    $fmt = vio_backend_name($ctx) === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $sh = vio_shader($ctx, ['vertex' => $vs, 'fragment' => FS_FLAT, 'format' => $fmt]);
    if (!$sh) throw new RuntimeException('shader failed');
    $p = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => true, 'cull_mode' => VIO_CULL_NONE]);
    if (!$p) throw new RuntimeException('pipeline failed');
    return $p;
}

/* Scenarios: name => [needs, setup(ctx) => state, draw(ctx, state, frame)] */
$scenarios = [
    '2D: 500 rects' => [[VIO_FEATURE_NATIVE_2D_BATCH], fn($ctx) => null, function ($ctx, $s, $f) {
        for ($i = 0; $i < 500; $i++)
            vio_rect($ctx, ($i * 37 + $f) % (BW - 40), ($i * 53) % (BH - 30), 40, 30, ['color' => 0xFF204060 + $i * 517]);
    }],
    '2D: 200 rects + 200 rounded rects + 50 texts' => [[VIO_FEATURE_NATIVE_2D_BATCH], function ($ctx) {
        $font = find_font();
        return $font ? vio_font($ctx, $font, 18.0) : null;
    }, function ($ctx, $font, $f) {
        for ($i = 0; $i < 200; $i++) {
            vio_rect($ctx, ($i * 37 + $f) % (BW - 40), ($i * 53) % (BH - 30), 40, 30, ['color' => 0xFF204060 + $i * 517]);
            vio_rounded_rect($ctx, ($i * 61) % (BW - 60), ($i * 29 + $f) % (BH - 40), 60, 40, 8.0, ['color' => 0xC0406020 + $i * 1031]);
        }
        if ($font) for ($i = 0; $i < 50; $i++)
            vio_text($ctx, $font, "Label $i: frame time", 20 + ($i % 5) * 250, 30 + intdiv($i, 5) * 68, ['color' => 0xFFFFFFFF]);
    }],
    '2D: 1000 rects + 100 texts' => [[VIO_FEATURE_NATIVE_2D_BATCH], function ($ctx) {
        $font = find_font();
        return $font ? vio_font($ctx, $font, 18.0) : null;
    }, function ($ctx, $font, $f) {
        for ($i = 0; $i < 1000; $i++)
            vio_rect($ctx, ($i * 37 + $f) % (BW - 40), ($i * 53) % (BH - 30), 40, 30, ['color' => 0xFF204060 + $i * 517]);
        if ($font) for ($i = 0; $i < 100; $i++)
            vio_text($ctx, $font, "Label $i: frame time", 20 + ($i % 5) * 250, 30 + intdiv($i, 5) * 34, ['color' => 0xFFFFFFFF]);
    }],
    '3D: 1000 draws, 2 uniforms each' => [[VIO_FEATURE_3D_PIPELINE], function ($ctx) {
        $vp = m_mul(m_perspective(deg2rad(50), BW / BH, 0.1, 200.0), m_translate(0, 0, -45));
        $mvps = [];
        for ($i = 0; $i < 1000; $i++)
            $mvps[] = m_mul($vp, m_translate(($i % 40) * 1.5 - 29, intdiv($i, 40) * 1.5 - 18, 0));
        return ['pipe' => pipeline($ctx, VS_DRAW), 'cube' => cube($ctx), 'mvps' => $mvps];
    }, function ($ctx, $s, $f) {
        vio_bind_pipeline($ctx, $s['pipe']);
        foreach ($s['mvps'] as $i => $mvp) {
            vio_set_uniform($ctx, 'u_mvp', $mvp);
            vio_set_uniform($ctx, 'u_tint', [($i % 7) / 7, 0.6, 1.0 - ($i % 5) / 5]);
            vio_draw($ctx, $s['cube']);
        }
    }],
    '3D: 10 000 instances, 1 draw' => [[VIO_FEATURE_3D_PIPELINE, VIO_FEATURE_INSTANCED_DRAW], function ($ctx) {
        $mats = '';
        for ($i = 0; $i < 10000; $i++)
            $mats .= pack('f16', ...m_translate(($i % 125) * 1.4 - 87, intdiv($i, 125) * 1.4 - 55, 0));
        return ['pipe' => pipeline($ctx, VS_INST), 'cube' => cube($ctx), 'mats' => $mats,
                'vp' => m_mul(m_perspective(deg2rad(50), BW / BH, 0.1, 400.0), m_translate(0, 0, -130))];
    }, function ($ctx, $s, $f) {
        vio_bind_pipeline($ctx, $s['pipe']);
        vio_set_uniform($ctx, 'u_vp', $s['vp']);
        vio_draw_instanced($ctx, $s['cube'], $s['mats'], 10000);
    }],
];

function pct(array $sorted, float $p): float { return $sorted[(int)min(count($sorted) - 1, floor($p * count($sorted)))]; }

$report = ['vio' => phpversion('vio'), 'php' => PHP_VERSION, 'os' => php_uname('s') . ' ' . php_uname('r'), 'frames' => $frames, 'results' => []];
foreach ($backends as $backend) {
    foreach ($scenarios as $name => [$needs, $setup, $draw]) {
        $ctx = @vio_create($backend, ['width' => BW, 'height' => BH, 'headless' => true, 'vsync' => false, 'headless_hardware' => true]);
        if (!$ctx) { $report['results'][$backend][$name] = ['skipped' => 'no context']; continue; }
        $missing = array_filter($needs, fn($f) => !vio_supports_feature($ctx, $f));
        if ($missing) { $report['results'][$backend][$name] = ['skipped' => 'feature missing']; vio_destroy($ctx); continue; }
        $info = vio_backend_info($ctx);
        $report['adapters'][$backend] = ($info['device'] ?? '?') . ' (' . ($info['device_type'] ?? '?') . ', ' . ($info['api'] ?? '') . ')';
        try {
            $state = $setup($ctx);
            $submit = []; $frame = []; $gpu = [];
            for ($f = 0; $f < WARMUP + $frames; $f++) {
                $t0 = hrtime(true);
                vio_begin($ctx);
                vio_clear($ctx, 0.08, 0.09, 0.12, 1.0);
                $draw($ctx, $state, $f);
                vio_draw_2d($ctx);
                $t1 = hrtime(true);
                vio_end($ctx);
                $t2 = hrtime(true);
                if ($f >= WARMUP) { $submit[] = ($t1 - $t0) / 1000.0; $frame[] = ($t2 - $t0) / 1000.0; $g = vio_gpu_frame_time($ctx); if ($g > 0) $gpu[] = $g * 1000.0;   /* 0 or -1: no timestamps (OpenGL on macOS reports 0) */ }
            }
            if (function_exists('vio_gpu_flush')) vio_gpu_flush($ctx);
            sort($submit); sort($frame); sort($gpu);
            $report['results'][$backend][$name] = [
                'submit_median_us' => round(pct($submit, 0.5)), 'frame_median_us' => round(pct($frame, 0.5)),
                'frame_p95_us' => round(pct($frame, 0.95)), 'frame_p99_us' => round(pct($frame, 0.99)),
                'gpu_median_us' => $gpu ? round(pct($gpu, 0.5)) : null,
            ];
        } catch (Throwable $e) {
            $report['results'][$backend][$name] = ['skipped' => $e->getMessage()];
        }
        unset($state);
        vio_destroy($ctx);
    }
}

echo "php-vio {$report['vio']}, PHP {$report['php']}, {$report['os']}, " . BW . 'x' . BH . ", $frames frames per scenario\n";
foreach ($report['adapters'] ?? [] as $b => $a) echo "  $b: $a\n";
foreach (['submit_median_us' => 'submit, median (us)', 'frame_median_us' => 'frame, median (us)', 'frame_p99_us' => 'frame, p99 (us)', 'gpu_median_us' => 'GPU time of the frame, median (us)'] as $key => $title) {
    echo "\n$title\n| Scenario | " . implode(' | ', $backends) . " |\n|---|" . str_repeat('---:|', count($backends)) . "\n";
    foreach (array_keys($scenarios) as $name) {
        $cells = array_map(fn($b) => isset($report['results'][$b][$name][$key]) ? (string)$report['results'][$b][$name][$key] : '–', $backends);
        echo "| $name | " . implode(' | ', $cells) . " |\n";
    }
}
if ($jsonOut) file_put_contents($jsonOut, json_encode($report, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES));
