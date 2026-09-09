<?php
/**
 * Feature gallery — renders one PNG per feature path into docs/gallery/.
 *
 *   php -d extension=vio examples/gallery.php [backend] [outdir]
 *
 * Every scene runs headless on the chosen backend ('auto' by default) and is
 * captured with vio_save_screenshot(). Scenes whose feature flag the backend
 * does not report are skipped with a note, so the script works on any backend.
 */

$backend = $argv[1] ?? 'auto';
$outdir  = $argv[2] ?? __DIR__ . '/../docs/gallery';
$only    = isset($argv[3]) ? explode(',', $argv[3]) : null;   // optional comma-separated scene filter
if (!is_dir($outdir) && !mkdir($outdir, 0777, true)) {
    fwrite(STDERR, "cannot create $outdir\n");
    exit(1);
}
$outdir = rtrim(str_replace('\\', '/', realpath($outdir)), '/');

const W = 640;
const H = 400;

/* ── Matrix helpers (column-major, GLSL layout) ─────────────────────── */
function m_identity(): array { return [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]; }
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
function m_ortho(float $l, float $r, float $b, float $t, float $n, float $f): array {
    return [2/($r-$l),0,0,0, 0,2/($t-$b),0,0, 0,0,-2/($f-$n),0, -($r+$l)/($r-$l),-($t+$b)/($t-$b),-($f+$n)/($f-$n),1];
}
function v_norm(array $v): array { $l = sqrt($v[0]*$v[0] + $v[1]*$v[1] + $v[2]*$v[2]) ?: 1; return [$v[0]/$l, $v[1]/$l, $v[2]/$l]; }
function v_cross(array $a, array $b): array { return [$a[1]*$b[2]-$a[2]*$b[1], $a[2]*$b[0]-$a[0]*$b[2], $a[0]*$b[1]-$a[1]*$b[0]]; }
function m_lookat(array $eye, array $at, array $up): array {
    $f = v_norm([$at[0]-$eye[0], $at[1]-$eye[1], $at[2]-$eye[2]]);
    $s = v_norm(v_cross($f, $up));
    $u = v_cross($s, $f);
    return [$s[0],$u[0],-$f[0],0, $s[1],$u[1],-$f[1],0, $s[2],$u[2],-$f[2],0,
            -($s[0]*$eye[0]+$s[1]*$eye[1]+$s[2]*$eye[2]), -($u[0]*$eye[0]+$u[1]*$eye[1]+$u[2]*$eye[2]),
             ($f[0]*$eye[0]+$f[1]*$eye[1]+$f[2]*$eye[2]), 1];
}
function m_rot_y(float $a): array { $c = cos($a); $s = sin($a); return [$c,0,-$s,0, 0,1,0,0, $s,0,$c,0, 0,0,0,1]; }
function m_rot_x(float $a): array { $c = cos($a); $s = sin($a); return [1,0,0,0, 0,$c,$s,0, 0,-$s,$c,0, 0,0,0,1]; }
function m_translate(float $x, float $y, float $z): array { return [1,0,0,0, 0,1,0,0, 0,0,1,0, $x,$y,$z,1]; }
function m_scale(float $s): array { return [$s,0,0,0, 0,$s,0,0, 0,0,$s,0, 0,0,0,1]; }

/* ── Meshes ─────────────────────────────────────────────────────────── */
function mesh_cube(VioContext $ctx): VioMesh {
    $faces = [ // normal, u axis, v axis
        [[0,0,1],[1,0,0],[0,1,0]], [[0,0,-1],[-1,0,0],[0,1,0]],
        [[1,0,0],[0,0,-1],[0,1,0]], [[-1,0,0],[0,0,1],[0,1,0]],
        [[0,1,0],[1,0,0],[0,0,-1]], [[0,-1,0],[1,0,0],[0,0,1]],
    ];
    $v = []; $idx = []; $base = 0;
    foreach ($faces as [$n, $u, $w]) {
        foreach ([[-1,-1,0,0],[1,-1,1,0],[1,1,1,1],[-1,1,0,1]] as [$su, $sv, $tu, $tv]) {
            for ($k = 0; $k < 3; $k++) $v[] = ($n[$k] + $u[$k] * $su + $w[$k] * $sv) * 0.5;
            array_push($v, $n[0], $n[1], $n[2], $tu, $tv);
        }
        array_push($idx, $base, $base + 1, $base + 2, $base, $base + 2, $base + 3);
        $base += 4;
    }
    return vio_mesh($ctx, ['vertices' => $v, 'indices' => $idx, 'layout' => [VIO_FLOAT3, VIO_FLOAT3, VIO_FLOAT2]]);
}
function mesh_sphere(VioContext $ctx, int $rings = 24, int $segs = 48): VioMesh {
    $v = []; $idx = [];
    for ($r = 0; $r <= $rings; $r++) {
        $phi = M_PI * $r / $rings;
        for ($s = 0; $s <= $segs; $s++) {
            $th = 2 * M_PI * $s / $segs;
            $x = sin($phi) * cos($th); $y = cos($phi); $z = sin($phi) * sin($th);
            array_push($v, $x, $y, $z, $x, $y, $z, $s / $segs, $r / $rings);
        }
    }
    for ($r = 0; $r < $rings; $r++) for ($s = 0; $s < $segs; $s++) {
        $a = $r * ($segs + 1) + $s; $b = $a + $segs + 1;
        array_push($idx, $a, $b, $a + 1, $b, $b + 1, $a + 1);
    }
    return vio_mesh($ctx, ['vertices' => $v, 'indices' => $idx, 'layout' => [VIO_FLOAT3, VIO_FLOAT3, VIO_FLOAT2]]);
}
function mesh_plane(VioContext $ctx, float $size, float $uvRepeat = 1.0): VioMesh {
    $s = $size / 2; $u = $uvRepeat;
    return vio_mesh($ctx, ['vertices' => [-$s,0,-$s, 0,1,0, 0,0,  $s,0,-$s, 0,1,0, $u,0,  $s,0,$s, 0,1,0, $u,$u,  -$s,0,$s, 0,1,0, 0,$u],
                           'indices' => [0,2,1, 0,3,2], 'layout' => [VIO_FLOAT3, VIO_FLOAT3, VIO_FLOAT2]]);
}
/* Quad for displaying a render-target texture: RT textures are row-0-at-TOP on
 * D3D11 / D3D12 / Metal and row-0-at-BOTTOM on OpenGL (GL renders into the
 * texture bottom-up), so the v axis is flipped there. */
function rt_quad(VioContext $ctx, float $x0 = -1, float $y0 = -1, float $x1 = 1, float $y1 = 1): VioMesh {
    $gl = vio_backend_name($ctx) === 'opengl';
    [$vt, $vb] = $gl ? [1, 0] : [0, 1];   // v at the top edge / bottom edge of the quad
    return vio_mesh($ctx, ['vertices' => [$x0,$y0,0,0,$vb, $x1,$y0,0,1,$vb, $x1,$y1,0,1,$vt, $x0,$y1,0,0,$vt],
                           'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);
}

/* Fullscreen quad with uv (0,0) at the top-left of the frame. */
function mesh_quad(VioContext $ctx, float $x0 = -1, float $y0 = -1, float $x1 = 1, float $y1 = 1, float $u1 = 1, float $v1 = 1): VioMesh {
    return vio_mesh($ctx, ['vertices' => [$x0,$y0,0,0,$v1, $x1,$y0,0,$u1,$v1, $x1,$y1,0,$u1,0, $x0,$y1,0,0,0],
                           'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);
}

/* ── Textures ───────────────────────────────────────────────────────── */
function tex_checker(VioContext $ctx, int $size, int $cells, array $a, array $b, array $opts = []): VioTexture {
    $d = '';
    for ($y = 0; $y < $size; $y++) for ($x = 0; $x < $size; $x++) {
        $c = ((int)($x * $cells / $size) + (int)($y * $cells / $size)) % 2 ? $a : $b;
        $d .= chr($c[0]) . chr($c[1]) . chr($c[2]) . "\xFF";
    }
    return vio_texture($ctx, ['data' => $d, 'width' => $size, 'height' => $size] + $opts);
}
function tex_brick(VioContext $ctx, int $size = 128): VioTexture {
    $d = '';
    for ($y = 0; $y < $size; $y++) for ($x = 0; $x < $size; $x++) {
        $row = (int)($y / 16); $xx = $x + ($row % 2 ? 16 : 0);
        $mortar = ($y % 16 < 2) || ($xx % 32 < 2);
        $n = (sin($x * 0.7) * cos($y * 0.9) + 1) * 12;
        [$r, $g, $b] = $mortar ? [190, 185, 178] : [178 + $n, 78 + $n * 0.5, 58];
        $d .= chr((int)$r) . chr((int)$g) . chr((int)$b) . "\xFF";
    }
    return vio_texture($ctx, ['data' => $d, 'width' => $size, 'height' => $size, 'mipmaps' => true, 'anisotropy' => 8]);
}

/* ── Shaders ────────────────────────────────────────────────────────── */
function shader_fmt(VioContext $ctx): int { return vio_backend_name($ctx) === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL; }
function pipe(VioContext $ctx, string $vs, string $fs, array $opts = []): VioPipeline {
    $sh = vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => shader_fmt($ctx)]);
    if (!$sh) throw new RuntimeException("shader failed");
    $p = vio_pipeline($ctx, ['shader' => $sh] + $opts + ['cull_mode' => VIO_CULL_NONE]);
    if (!$p) throw new RuntimeException("pipeline failed");
    return $p;
}
const VS_LIT = <<<'GLSL'
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUv;
uniform mat4 u_mvp;
uniform mat4 u_model;
out vec3 vN; out vec2 vUv; out vec3 vW;
void main() {
    vec4 w = u_model * vec4(aPos, 1.0);
    vW = w.xyz; vN = mat3(u_model) * aNormal; vUv = aUv;
    gl_Position = u_mvp * vec4(aPos, 1.0);
}
GLSL;
const FS_LIT_TEX = <<<'GLSL'
#version 330 core
in vec3 vN; in vec2 vUv; in vec3 vW;
uniform sampler2D u_tex;
uniform vec3 u_light; uniform vec3 u_eye; uniform vec3 u_tint;
layout(location=0) out vec4 o;
void main() {
    vec3 n = normalize(vN); vec3 l = normalize(u_light - vW); vec3 v = normalize(u_eye - vW);
    float diff = max(dot(n, l), 0.0);
    float spec = pow(max(dot(n, normalize(l + v)), 0.0), 48.0);
    vec3 albedo = texture(u_tex, vUv).rgb * u_tint;
    o = vec4(albedo * (0.18 + 0.82 * diff) + vec3(0.35) * spec, 1.0);
}
GLSL;
const VS_QUAD = "#version 330 core\nlayout(location=0) in vec3 aPos;\nlayout(location=1) in vec2 aUv;\nout vec2 vUv;\nvoid main(){ vUv = aUv; gl_Position = vec4(aPos, 1.0); }";
const FS_BLIT = "#version 330 core\nin vec2 vUv; uniform sampler2D u_tex;\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(texture(u_tex, vUv).rgb, 1.0); }";

/* ── Scene runner ───────────────────────────────────────────────────── */
$results = [];
function scene(string $name, array $features, callable $fn): void {
    global $backend, $outdir, $results, $only;
    if ($only && !in_array($name, $only, true)) return;
    $t0 = microtime(true);
    $ctx = vio_create($backend, ['width' => W, 'height' => H, 'headless' => true, 'vsync' => false]);
    if (!$ctx) { $results[$name] = 'no context'; return; }
    foreach ($features as $f) {
        if (!vio_supports_feature($ctx, $f)) { $results[$name] = "skipped (feature $f unavailable on " . vio_backend_name($ctx) . ")"; vio_destroy($ctx); return; }
    }
    try {
        $fn($ctx, $outdir . "/$name.png");
        $results[$name] = sprintf('ok (%.2f s)', microtime(true) - $t0);
    } catch (Throwable $e) {
        $results[$name] = 'FAILED: ' . $e->getMessage();
    }
    $GLOBALS['font_cache'] = [];
    vio_destroy($ctx);
}
/* Fonts are cached per scene: rasterising a system font's full glyph set is
 * the most expensive call in this script, and one atlas per (file, size) is
 * plenty. The cache is dropped in scene() before the context is destroyed. */
$font_cache = [];
function font(VioContext $ctx, float $size, array $candidates = []): ?VioFont {
    global $font_cache;
    $candidates = $candidates ?: ['C:/Windows/Fonts/segoeui.ttf', 'C:/Windows/Fonts/arial.ttf',
        '/System/Library/Fonts/Supplemental/Arial.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'];
    foreach ($candidates as $c) {
        if (!is_file($c)) continue;
        $key = "$c|$size";
        if (!isset($font_cache[$key])) { $f = @vio_font($ctx, $c, $size); if (!$f) continue; $font_cache[$key] = $f; }
        return $font_cache[$key];
    }
    return null;
}
function label(VioContext $ctx, string $text, float $x = 12, float $y = 28, int $color = 0xFFFFFFFF, float $size = 18): void {
    $f = font($ctx, $size);
    if ($f) vio_text($ctx, $f, $text, $x, $y, ['color' => $color, 'z' => 100]);
}
function draw3d(VioContext $ctx, VioPipeline $p, VioMesh $m, array $uniforms, ?VioTexture $tex = null): void {
    vio_bind_pipeline($ctx, $p);
    foreach ($uniforms as $k => $v) vio_set_uniform($ctx, $k, $v);
    if ($tex) { vio_set_uniform($ctx, 'u_tex', 0); vio_bind_texture($ctx, $tex, 0); }
    vio_draw($ctx, $m);
}

/* ═══════════════════════════════════════════════════════════════════════
 * 1. 2D shapes — z-sorted batch renderer
 * ═══════════════════════════════════════════════════════════════════════ */
scene('2d_shapes', [VIO_FEATURE_NATIVE_2D_BATCH], function (VioContext $ctx, string $out) {
    vio_begin($ctx);
    vio_clear($ctx, 0.09, 0.10, 0.14, 1.0);
    for ($i = 0; $i < 12; $i++) {
        $t = $i / 11;
        $c = 0xFF000000 | ((int)(60 + 180 * $t) << 16) | ((int)(120 - 80 * $t) << 8) | (int)(220 - 160 * $t);
        vio_rect($ctx, 30 + $i * 48, 60 + sin($i * 0.6) * 20, 36, 120, ['color' => $c, 'z' => 1]);
    }
    for ($i = 0; $i < 6; $i++) {
        vio_circle($ctx, 90 + $i * 95, 290, 40 - $i * 4, ['color' => 0xB0FFFFFF, 'outline' => true, 'segments' => 48, 'z' => 3]);
        vio_circle($ctx, 90 + $i * 95, 290, 30 - $i * 3, ['color' => 0xFF000000 | (0x30 + $i * 0x28) | (0xC0 << 8) | ((0xFF - $i * 0x20) << 16), 'z' => 2]);
    }
    for ($i = 0; $i < 20; $i++) {
        vio_line($ctx, 20, 380 - $i * 3, 620, 200 + $i * 8, ['color' => 0x40FFD070 | ((int)(255 * $i / 20) << 24), 'z' => 4]);
    }
    vio_rect($ctx, 470, 40, 150, 100, ['color' => 0x80FF4060, 'z' => 5]);
    vio_rect($ctx, 500, 70, 120, 90, ['color' => 0x8040C0FF, 'z' => 6]);
    label($ctx, 'vio_rect / vio_circle / vio_line — z-sorted 2D batch', 12, 28);
    vio_draw_2d($ctx);
    vio_end($ctx);
    if (!vio_save_screenshot($ctx, $out)) throw new RuntimeException('screenshot');
});

/* ═══════════════════════════════════════════════════════════════════════
 * 2. Sprites + text (HarfBuzz shaping, word wrap)
 * ═══════════════════════════════════════════════════════════════════════ */
scene('2d_sprites_text', [VIO_FEATURE_NATIVE_2D_BATCH], function (VioContext $ctx, string $out) {
    $tex = tex_checker($ctx, 64, 8, [255, 200, 40], [40, 60, 120], ['filter' => VIO_FILTER_NEAREST]);
    $brick = tex_brick($ctx, 128);
    vio_begin($ctx);
    vio_clear($ctx, 0.12, 0.12, 0.16, 1.0);
    vio_sprite($ctx, $tex, ['x' => 24, 'y' => 50, 'scale_x' => 2.0, 'scale_y' => 2.0]);
    vio_sprite($ctx, $brick, ['x' => 170, 'y' => 50, 'width' => 128, 'height' => 128]);
    vio_sprite($ctx, $tex, ['x' => 320, 'y' => 50, 'width' => 128, 'height' => 128, 'color' => 0x80FF8080]);
    $f = font($ctx, 26);
    if ($f) {
        vio_text($ctx, $f, 'Text rendering', 24, 230, ['color' => 0xFFFFFFFF]);
        vio_text($ctx, $f, 'Ligatures: fi fl — Kerning: AV To', 24, 264, ['color' => 0xFFB0D8FF]);
        $ar = font($ctx, 26, ['C:/Windows/Fonts/segoeui.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf']);
        if ($ar && VIO_HAS_SHAPING) vio_text($ctx, $ar, 'العربية RTL + joining · Ελληνικά · Кириллица', 24, 300, ['color' => 0xFFFFD080]);
        $th = font($ctx, 26, ['C:/Windows/Fonts/LeelawUI.ttf', 'C:/Windows/Fonts/segoeui.ttf']);
        if ($th && VIO_HAS_SHAPING) vio_text($ctx, $th, 'ภาษาไทย clustering', 24, 336, ['color' => 0xFF80FFB0]);
        $small = font($ctx, 15);
        vio_text($ctx, $small, "Word wrap with 'max_width': the quick brown fox jumps over the lazy dog and keeps running across the line.", 470, 60, ['color' => 0xFFDDDDDD, 'max_width' => 160]);
    }
    label($ctx, 'vio_sprite (scale, tint, mipmapped) + vio_text (' . (VIO_HAS_SHAPING ? 'HarfBuzz shaping' : 'legacy path') . ')', 12, 28, 0xFFFFFFFF, 16);
    vio_draw_2d($ctx);
    vio_end($ctx);
    if (!vio_save_screenshot($ctx, $out)) throw new RuntimeException('screenshot');
});

/* ═══════════════════════════════════════════════════════════════════════
 * 3. 3D: lit, textured meshes with depth test
 * ═══════════════════════════════════════════════════════════════════════ */
scene('3d_lit_meshes', [VIO_FEATURE_3D_PIPELINE], function (VioContext $ctx, string $out) {
    $p = pipe($ctx, VS_LIT, FS_LIT_TEX, ['depth_test' => true]);
    $cube = mesh_cube($ctx); $sphere = mesh_sphere($ctx); $plane = mesh_plane($ctx, 8, 4);
    $brick = tex_brick($ctx); $check = tex_checker($ctx, 64, 8, [230, 230, 230], [90, 90, 100], ['mipmaps' => true, 'anisotropy' => 8]);
    $proj = m_perspective(deg2rad(45), W / H, 0.1, 50); $eye = [3.2, 2.2, 4.0];
    $view = m_lookat($eye, [0, 0.3, 0], [0, 1, 0]); $vp = m_mul($proj, $view);
    $light = [3.0, 5.0, 2.0];
    vio_begin($ctx);
    vio_clear($ctx, 0.07, 0.08, 0.11, 1.0);
    $m = m_translate(0, -0.5, 0);
    draw3d($ctx, $p, $plane, ['u_mvp' => m_mul($vp, $m), 'u_model' => $m, 'u_light' => $light, 'u_eye' => $eye, 'u_tint' => [1, 1, 1]], $check);
    $m = m_mul(m_translate(-1.2, 0.0, 0.0), m_rot_y(0.6));
    draw3d($ctx, $p, $cube, ['u_mvp' => m_mul($vp, $m), 'u_model' => $m, 'u_light' => $light, 'u_eye' => $eye, 'u_tint' => [1, 1, 1]], $brick);
    $m = m_mul(m_translate(1.1, 0.25, 0.2), m_scale(0.75));
    draw3d($ctx, $p, $sphere, ['u_mvp' => m_mul($vp, $m), 'u_model' => $m, 'u_light' => $light, 'u_eye' => $eye, 'u_tint' => [0.4, 0.7, 1.0]], $check);
    label($ctx, 'vio_mesh + vio_shader (GLSL → SPIR-V → HLSL/MSL) + vio_pipeline, depth test, textures', 12, 28, 0xFFFFFFFF, 15);
    vio_draw_2d($ctx);
    vio_end($ctx);
    if (!vio_save_screenshot($ctx, $out)) throw new RuntimeException('screenshot');
});

/* ═══════════════════════════════════════════════════════════════════════
 * 4. Instancing: one draw call, 20x20 cubes
 * ═══════════════════════════════════════════════════════════════════════ */
scene('instancing', [VIO_FEATURE_3D_PIPELINE, VIO_FEATURE_INSTANCED_DRAW], function (VioContext $ctx, string $out) {
    $vs = <<<'GLSL'
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUv;
layout(location=3) in mat4 aModel;
uniform mat4 u_vp;
out vec3 vN; out vec3 vW; out vec3 vC;
void main() {
    vec4 w = aModel * vec4(aPos, 1.0);
    vW = w.xyz; vN = mat3(aModel) * aNormal;
    vC = 0.5 + 0.5 * vec3(sin(w.x * 0.9), cos(w.z * 0.7), sin(w.y * 3.0 + 1.0));
    gl_Position = u_vp * w;
}
GLSL;
    $fs = <<<'GLSL'
#version 330 core
in vec3 vN; in vec3 vW; in vec3 vC;
uniform vec3 u_light;
layout(location=0) out vec4 o;
void main() {
    float d = max(dot(normalize(vN), normalize(u_light - vW)), 0.0);
    o = vec4(vC * (0.25 + 0.75 * d), 1.0);
}
GLSL;
    $p = pipe($ctx, $vs, $fs, ['depth_test' => true]);
    $cube = mesh_cube($ctx);
    $mats = []; $n = 0;
    for ($x = -10; $x < 10; $x++) for ($z = -10; $z < 10; $z++) {
        $h = 0.3 + 1.2 * abs(sin($x * 0.5) * cos($z * 0.45));
        $m = m_mul(m_translate($x * 0.55, $h / 2 - 1.0, $z * 0.55), [0.42,0,0,0, 0,$h,0,0, 0,0,0.42,0, 0,0,0,1]);
        foreach ($m as $f) $mats[] = $f;
        $n++;
    }
    $proj = m_perspective(deg2rad(40), W / H, 0.1, 60);
    $view = m_lookat([7.5, 5.5, 8.5], [0, -0.6, 0], [0, 1, 0]);
    vio_begin($ctx);
    vio_clear($ctx, 0.06, 0.07, 0.10, 1.0);
    vio_bind_pipeline($ctx, $p);
    vio_set_uniform($ctx, 'u_vp', m_mul($proj, $view));
    vio_set_uniform($ctx, 'u_light', [6.0, 10.0, 4.0]);
    vio_draw_instanced($ctx, $cube, $mats, $n);
    label($ctx, "vio_draw_instanced — $n cubes, one draw call (mat4 per instance at locations 3..6)", 12, 28, 0xFFFFFFFF, 15);
    vio_draw_2d($ctx);
    vio_end($ctx);
    if (!vio_save_screenshot($ctx, $out)) throw new RuntimeException('screenshot');
});

/* ═══════════════════════════════════════════════════════════════════════
 * 5. Render target + post-process pass
 * ═══════════════════════════════════════════════════════════════════════ */
scene('render_target_postprocess', [VIO_FEATURE_3D_PIPELINE, VIO_FEATURE_RENDER_TARGET], function (VioContext $ctx, string $out) {
    $p = pipe($ctx, VS_LIT, FS_LIT_TEX, ['depth_test' => true]);
    $fs = <<<'GLSL'
#version 330 core
in vec2 vUv; uniform sampler2D u_tex; uniform vec2 u_res;
layout(location=0) out vec4 o;
void main() {
    vec2 uv = vUv;
    vec2 c = uv - 0.5;
    float r = dot(c, c);
    vec2 d = c * r * 0.06;                                // chromatic aberration
    vec3 col = vec3(texture(u_tex, uv + d).r, texture(u_tex, uv).g, texture(u_tex, uv - d).b);
    col *= 1.0 - smoothstep(0.15, 0.7, r) * 0.9;          // vignette
    col *= 0.92 + 0.08 * sin(uv.y * u_res.y * 3.1416);    // scanlines
    o = vec4(col, 1.0);
}
GLSL;
    $post = pipe($ctx, VS_QUAD, $fs, ['depth_test' => false]);
    $rt = vio_render_target($ctx, ['width' => W, 'height' => H]);
    $cube = mesh_cube($ctx); $brick = tex_brick($ctx);
    $proj = m_perspective(deg2rad(45), W / H, 0.1, 50); $eye = [0, 1.6, 4.2];
    $vp = m_mul($proj, m_lookat($eye, [0, 0, 0], [0, 1, 0]));
    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt);
    vio_clear($ctx, 0.15, 0.22, 0.32, 1.0);
    for ($i = 0; $i < 5; $i++) {
        $m = m_mul(m_mul(m_translate(-2.2 + $i * 1.1, 0, 0), m_rot_y($i * 0.5)), m_rot_x(0.3 * $i));
        draw3d($ctx, $p, $cube, ['u_mvp' => m_mul($vp, $m), 'u_model' => $m, 'u_light' => [2, 4, 3], 'u_eye' => $eye, 'u_tint' => [1, 1, 1]], $brick);
    }
    vio_unbind_render_target($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $post);
    vio_set_uniform($ctx, 'u_tex', 0);
    vio_bind_texture($ctx, vio_render_target_texture($rt), 0);
    vio_set_uniform($ctx, 'u_res', [W, H]);
    vio_draw($ctx, rt_quad($ctx));
    label($ctx, 'vio_render_target → vio_render_target_texture → post-process pass (aberration, vignette, scanlines)', 12, 28, 0xFFFFFFFF, 13);
    vio_draw_2d($ctx);
    vio_end($ctx);
    if (!vio_save_screenshot($ctx, $out)) throw new RuntimeException('screenshot');
});

/* ═══════════════════════════════════════════════════════════════════════
 * 6. Cube render target → environment map with mip roughness
 * ═══════════════════════════════════════════════════════════════════════ */
scene('cubemap_environment', [VIO_FEATURE_3D_PIPELINE, VIO_FEATURE_RENDER_TARGET_CUBE, VIO_FEATURE_MIPMAP_GEN], function (VioContext $ctx, string $out) {
    $fsSky = <<<'GLSL'
#version 330 core
in vec2 vUv; uniform vec3 u_fwd; uniform vec3 u_right; uniform vec3 u_up;
layout(location=0) out vec4 o;
void main() {
    vec2 p = vUv * 2.0 - 1.0;
    vec3 d = normalize(u_fwd + u_right * p.x + u_up * p.y);
    float t = clamp(d.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 sky = mix(vec3(0.95, 0.75, 0.45), vec3(0.25, 0.45, 0.95), pow(t, 0.6));
    vec3 sunDir = normalize(vec3(0.6, 0.35, 0.7));
    float sun = pow(max(dot(d, sunDir), 0.0), 400.0) * 3.0 + pow(max(dot(d, sunDir), 0.0), 8.0) * 0.4;
    vec3 ground = vec3(0.22, 0.2, 0.18) * (0.6 + 0.4 * fract(sin(dot(floor(d.xz / max(abs(d.y), 0.05) * 3.0), vec2(12.9898, 78.233))) * 43758.5453));
    o = vec4(d.y < 0.0 ? ground : sky + sun, 1.0);
}
GLSL;
    $sky = pipe($ctx, VS_QUAD, $fsSky, ['depth_test' => false]);
    $vsRefl = <<<'GLSL'
#version 330 core
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aNormal; layout(location=2) in vec2 aUv;
uniform mat4 u_mvp; uniform mat4 u_model;
out vec3 vN; out vec3 vW;
void main(){ vec4 w = u_model * vec4(aPos,1.0); vW = w.xyz; vN = mat3(u_model)*aNormal; gl_Position = u_mvp*vec4(aPos,1.0); }
GLSL;
    $fsRefl = <<<'GLSL'
#version 330 core
in vec3 vN; in vec3 vW;
uniform samplerCube u_env; uniform vec3 u_eye; uniform float u_lod; uniform vec3 u_tint;
layout(location=0) out vec4 o;
void main(){
    vec3 n = normalize(vN); vec3 v = normalize(vW - u_eye);
    vec3 r = reflect(v, n);
    float fres = 0.15 + 0.85 * pow(1.0 - max(dot(n, -v), 0.0), 4.0);
    vec3 env = textureLod(u_env, r, u_lod).rgb;
    o = vec4(mix(u_tint * 0.35, env, fres * 0.8 + 0.2), 1.0);
}
GLSL;
    $refl = pipe($ctx, $vsRefl, $fsRefl, ['depth_test' => true]);
    $quad = mesh_quad($ctx); $sphere = mesh_sphere($ctx, 32, 64);
    $size = 256; $rt = vio_render_target($ctx, ['cube' => true, 'size' => $size, 'mipmaps' => true]);
    if (!$rt) throw new RuntimeException('cube RT');
    $faces = [ [[1,0,0],[0,0,-1],[0,-1,0]], [[-1,0,0],[0,0,1],[0,-1,0]], [[0,1,0],[1,0,0],[0,0,1]],
               [[0,-1,0],[1,0,0],[0,0,-1]], [[0,0,1],[1,0,0],[0,-1,0]], [[0,0,-1],[-1,0,0],[0,-1,0]] ];
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);   /* swapchain depth must start at 1.0 for the spheres */
    foreach ($faces as $i => [$fwd, $right, $up]) {
        vio_bind_render_target($ctx, $rt, $i);
        vio_bind_pipeline($ctx, $sky);
        vio_set_uniform($ctx, 'u_fwd', $fwd); vio_set_uniform($ctx, 'u_right', $right); vio_set_uniform($ctx, 'u_up', $up);
        vio_draw($ctx, $quad);
    }
    vio_unbind_render_target($ctx);
    vio_generate_mipmaps($ctx, $rt);
    $env = vio_render_target_cubemap($rt);
    $proj = m_perspective(deg2rad(38), W / H, 0.1, 50); $eye = [0, 0.4, 6.2];
    $vp = m_mul($proj, m_lookat($eye, [0, 0, 0], [0, 1, 0]));
    /* Backdrop: the environment itself, seen from the camera. */
    vio_bind_pipeline($ctx, $sky);
    vio_set_uniform($ctx, 'u_fwd', [0, 0, -1]); vio_set_uniform($ctx, 'u_right', [1.6, 0, 0]); vio_set_uniform($ctx, 'u_up', [0, -1, 0]);   /* uv row 0 is the top */
    vio_draw($ctx, $quad);
    $maxLod = (int)floor(log($size, 2));
    foreach ([0.0, $maxLod * 0.35, $maxLod * 0.6, $maxLod * 0.85] as $i => $lod) {
        $m = m_mul(m_translate(-2.55 + $i * 1.7, 0, 0), m_scale(0.75));
        vio_bind_pipeline($ctx, $refl);
        vio_set_uniform($ctx, 'u_mvp', m_mul($vp, $m)); vio_set_uniform($ctx, 'u_model', $m);
        vio_set_uniform($ctx, 'u_eye', $eye); vio_set_uniform($ctx, 'u_lod', $lod); vio_set_uniform($ctx, 'u_tint', [0.9, 0.9, 0.95]);
        vio_set_uniform($ctx, 'u_env', 0); vio_bind_cubemap($ctx, $env, 0);
        vio_draw($ctx, $sphere);
    }
    label($ctx, 'Cube render target (6 faces) + vio_generate_mipmaps → samplerCube textureLod by roughness', 12, 28, 0xFF202020, 15);
    vio_draw_2d($ctx);
    vio_end($ctx);
    if (!vio_save_screenshot($ctx, $out)) throw new RuntimeException('screenshot');
});

/* ═══════════════════════════════════════════════════════════════════════
 * 7. Multiple render targets (G-buffer)
 * ═══════════════════════════════════════════════════════════════════════ */
scene('mrt_gbuffer', [VIO_FEATURE_3D_PIPELINE, VIO_FEATURE_MRT], function (VioContext $ctx, string $out) {
    $fs = <<<'GLSL'
#version 330 core
in vec3 vN; in vec2 vUv; in vec3 vW;
uniform sampler2D u_tex; uniform vec3 u_light; uniform vec3 u_eye; uniform vec3 u_tint;
layout(location=0) out vec4 oAlbedo;
layout(location=1) out vec4 oNormal;
layout(location=2) out vec4 oDepth;
void main(){
    oAlbedo = vec4(texture(u_tex, vUv).rgb * u_tint, 1.0);
    oNormal = vec4(normalize(vN) * 0.5 + 0.5, 1.0);
    oDepth  = vec4(vec3(clamp(length(vW - u_eye) / 9.0, 0.0, 1.0)), 1.0);
}
GLSL;
    $formats = [VIO_FORMAT_RGBA8, VIO_FORMAT_RGBA16F, VIO_FORMAT_R16F];
    $p = pipe($ctx, VS_LIT, $fs, ['depth_test' => true, 'attachments' => $formats]);
    $blit = pipe($ctx, VS_QUAD, FS_BLIT, ['depth_test' => false]);
    $rt = vio_render_target($ctx, ['width' => 320, 'height' => 200, 'attachments' => $formats]);
    if (!$rt) throw new RuntimeException('MRT');
    $cube = mesh_cube($ctx); $sphere = mesh_sphere($ctx); $plane = mesh_plane($ctx, 8, 4);
    $brick = tex_brick($ctx); $check = tex_checker($ctx, 64, 8, [230, 230, 230], [90, 90, 100], ['mipmaps' => true]);
    $proj = m_perspective(deg2rad(45), 320 / 200, 0.1, 50); $eye = [3.2, 2.2, 4.0];
    $vp = m_mul($proj, m_lookat($eye, [0, 0.3, 0], [0, 1, 0]));
    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt);
    vio_clear($ctx, 0.05, 0.05, 0.08, 1.0);
    $m = m_translate(0, -0.5, 0);
    draw3d($ctx, $p, $plane, ['u_mvp' => m_mul($vp, $m), 'u_model' => $m, 'u_light' => [3, 5, 2], 'u_eye' => $eye, 'u_tint' => [1, 1, 1]], $check);
    $m = m_mul(m_translate(-1.2, 0, 0), m_rot_y(0.6));
    draw3d($ctx, $p, $cube, ['u_mvp' => m_mul($vp, $m), 'u_model' => $m, 'u_light' => [3, 5, 2], 'u_eye' => $eye, 'u_tint' => [1, 1, 1]], $brick);
    $m = m_mul(m_translate(1.1, 0.25, 0.2), m_scale(0.75));
    draw3d($ctx, $p, $sphere, ['u_mvp' => m_mul($vp, $m), 'u_model' => $m, 'u_light' => [3, 5, 2], 'u_eye' => $eye, 'u_tint' => [0.4, 0.7, 1.0]], $check);
    vio_unbind_render_target($ctx);
    vio_clear($ctx, 0.02, 0.02, 0.03, 1.0);
    $panels = [[-1, 0, 0, 1, 'albedo RGBA8'], [0, 0, 1, 1, 'normals RGBA16F'], [-0.5, -1, 0.5, 0, 'distance R16F']];
    foreach ($panels as $i => [$x0, $y0, $x1, $y1, $name]) {
        $q = rt_quad($ctx, $x0, $y0, $x1, $y1);
        vio_bind_pipeline($ctx, $blit);
        vio_set_uniform($ctx, 'u_tex', 0);
        vio_bind_texture($ctx, vio_render_target_texture($rt, $i), 0);
        vio_draw($ctx, $q);
        label($ctx, $name, ($x0 + 1) * W / 2 + 10, (1 - $y1) * H / 2 + 24, 0xFFFFFFFF, 15);
    }
    label($ctx, 'MRT: one fragment shader, three attachments', 12, H - 12, 0xFFFFFFFF, 15);
    vio_draw_2d($ctx);
    vio_end($ctx);
    if (!vio_save_screenshot($ctx, $out)) throw new RuntimeException('screenshot');
});

/* ═══════════════════════════════════════════════════════════════════════
 * 8. MSAA render target vs. single sample (4x magnified)
 * ═══════════════════════════════════════════════════════════════════════ */
scene('msaa_render_target', [VIO_FEATURE_3D_PIPELINE, VIO_FEATURE_RENDER_TARGET_MSAA], function (VioContext $ctx, string $out) {
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nuniform mat4 u_m;\nvoid main(){ gl_Position = u_m * vec4(aPos, 1.0); }";
    $fs = "#version 330 core\nuniform vec3 u_c;\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(u_c, 1.0); }";
    $p = pipe($ctx, $vs, $fs, ['depth_test' => false]);
    $blit = pipe($ctx, VS_QUAD, FS_BLIT, ['depth_test' => false]);
    $tri = vio_mesh($ctx, ['vertices' => [-0.8,-0.7,0, 0.9,-0.2,0, -0.3,0.85,0], 'layout' => [VIO_FLOAT3]]);
    $ring = []; $idx = [];
    for ($i = 0; $i <= 40; $i++) { $a = $i / 40 * 2 * M_PI; array_push($ring, cos($a) * 0.55, sin($a) * 0.55, 0, cos($a) * 0.62, sin($a) * 0.62, 0); if ($i < 40) array_push($idx, $i*2, $i*2+1, $i*2+2, $i*2+1, $i*2+3, $i*2+2); }
    $ringMesh = vio_mesh($ctx, ['vertices' => $ring, 'indices' => $idx, 'layout' => [VIO_FLOAT3]]);
    vio_begin($ctx);
    vio_clear($ctx, 0.02, 0.02, 0.03, 1.0);
    foreach ([[1, -1, 'samples => 1'], [4, 0, 'samples => 4 (resolved)']] as [$samples, $x0, $name]) {
        $rt = vio_render_target($ctx, ['width' => 80, 'height' => 50, 'samples' => $samples]);
        vio_bind_render_target($ctx, $rt);
        vio_clear($ctx, 0.12, 0.14, 0.2, 1.0);
        vio_bind_pipeline($ctx, $p);
        vio_set_uniform($ctx, 'u_m', m_rot_y(0)); vio_set_uniform($ctx, 'u_c', [1.0, 0.55, 0.15]); vio_draw($ctx, $tri);
        vio_set_uniform($ctx, 'u_m', m_translate(0.15, 0.1, 0)); vio_set_uniform($ctx, 'u_c', [0.3, 0.9, 1.0]); vio_draw($ctx, $ringMesh);
        vio_unbind_render_target($ctx);
        $q = rt_quad($ctx, $x0, -1, $x0 + 1, 1);
        vio_bind_pipeline($ctx, $blit);
        vio_set_uniform($ctx, 'u_tex', 0);
        vio_bind_texture($ctx, vio_render_target_texture($rt), 0);
        vio_draw($ctx, $q);
        label($ctx, $name, ($x0 + 1) * W / 2 + 10, 28, 0xFFFFFFFF, 16);
    }
    label($ctx, '80x50 render targets magnified 4x — vio_render_target([\'samples\' => N])', 12, H - 12, 0xFFFFFFFF, 15);
    vio_draw_2d($ctx);
    vio_end($ctx);
    if (!vio_save_screenshot($ctx, $out)) throw new RuntimeException('screenshot');
});

/* ═══════════════════════════════════════════════════════════════════════
 * 9. Sampler state grid: filter x wrap
 * ═══════════════════════════════════════════════════════════════════════ */
scene('sampler_filter_wrap', [VIO_FEATURE_3D_PIPELINE], function (VioContext $ctx, string $out) {
    $blit = pipe($ctx, VS_QUAD, FS_BLIT, ['depth_test' => false]);
    $d = ''; $cols = [[255,70,70],[70,220,90],[70,110,255],[250,220,60]];
    for ($y = 0; $y < 4; $y++) for ($x = 0; $x < 4; $x++) { $c = $cols[($x + $y) % 4]; $d .= chr($c[0]) . chr($c[1]) . chr($c[2]) . "\xFF"; }
    vio_begin($ctx);
    vio_clear($ctx, 0.05, 0.05, 0.07, 1.0);
    $filters = [VIO_FILTER_NEAREST => 'NEAREST', VIO_FILTER_LINEAR => 'LINEAR'];
    $wraps = [VIO_WRAP_REPEAT => 'REPEAT', VIO_WRAP_CLAMP => 'CLAMP', VIO_WRAP_MIRROR => 'MIRROR'];
    $fi = 0;
    foreach ($filters as $filter => $fname) {
        $wi = 0;
        foreach ($wraps as $wrap => $wname) {
            $tex = vio_texture($ctx, ['data' => $d, 'width' => 4, 'height' => 4, 'filter' => $filter, 'wrap' => $wrap]);
            $x0 = -1 + $wi * 2 / 3 + 0.03; $x1 = $x0 + 2 / 3 - 0.06;
            $y1 = 1 - $fi * 1.0 - 0.14; $y0 = $y1 - 0.72;
            $q = vio_mesh($ctx, ['vertices' => [$x0,$y0,0,-0.5,1.8, $x1,$y0,0,1.8,1.8, $x1,$y1,0,1.8,-0.5, $x0,$y1,0,-0.5,-0.5], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);
            vio_bind_pipeline($ctx, $blit); vio_set_uniform($ctx, 'u_tex', 0); vio_bind_texture($ctx, $tex, 0); vio_draw($ctx, $q);
            label($ctx, "$fname / $wname", ($x0 + 1) * W / 2, (1 - $y1) * H / 2 - 6, 0xFFFFFFFF, 15);
            $wi++;
        }
        $fi++;
    }
    label($ctx, "vio_texture(['filter' => …, 'wrap' => …]) — 4x4 texture, UVs -0.5..1.8", 12, H - 10, 0xFFCCCCCC, 14);
    vio_draw_2d($ctx);
    vio_end($ctx);
    if (!vio_save_screenshot($ctx, $out)) throw new RuntimeException('screenshot');
});

/* ═══════════════════════════════════════════════════════════════════════
 * 10. Anisotropic filtering at a grazing angle
 * ═══════════════════════════════════════════════════════════════════════ */
scene('anisotropy', [VIO_FEATURE_3D_PIPELINE], function (VioContext $ctx, string $out) {
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nlayout(location=1) in vec3 aN;\nlayout(location=2) in vec2 aUv;\nuniform mat4 u_mvp;\nout vec2 vUv;\nvoid main(){ vUv = aUv; gl_Position = u_mvp * vec4(aPos, 1.0); }";
    $p = pipe($ctx, $vs, FS_BLIT, ['depth_test' => true]);
    $plane = mesh_plane($ctx, 40, 40);
    $proj = m_perspective(deg2rad(50), (W / 2) / H, 0.05, 100);
    $view = m_lookat([0, 0.35, 0], [0, 0.0, -10], [0, 1, 0]);
    $mvp = m_mul($proj, m_mul($view, m_translate(0, 0, -18)));
    vio_begin($ctx);
    vio_clear($ctx, 0.05, 0.05, 0.07, 1.0);
    foreach ([[1, 0, 'anisotropy => 1 (trilinear)'], [16, W / 2, 'anisotropy => 16']] as [$aniso, $vx, $name]) {
        $tex = tex_checker($ctx, 256, 32, [235, 235, 235], [40, 40, 50], ['mipmaps' => true, 'anisotropy' => $aniso]);
        vio_viewport($ctx, (int)$vx, 0, (int)(W / 2), H);
        vio_bind_pipeline($ctx, $p);
        vio_set_uniform($ctx, 'u_mvp', $mvp);
        vio_set_uniform($ctx, 'u_tex', 0); vio_bind_texture($ctx, $tex, 0);
        vio_draw($ctx, $plane);
        label($ctx, $name, $vx + 12, 28, 0xFFFFFFFF, 16);
    }
    vio_viewport($ctx, 0, 0, W, H);
    label($ctx, "vio_texture(['mipmaps' => true, 'anisotropy' => N]) — same plane, grazing angle", 12, 52, 0xFFCCCCCC, 14);
    vio_draw_2d($ctx);
    vio_end($ctx);
    if (!vio_save_screenshot($ctx, $out)) throw new RuntimeException('screenshot');
});

/* ═══════════════════════════════════════════════════════════════════════
 * 11. Compute shader → storage image
 * ═══════════════════════════════════════════════════════════════════════ */
scene('compute_storage_image', [VIO_FEATURE_COMPUTE, VIO_FEATURE_STORAGE_IMAGE, VIO_FEATURE_3D_PIPELINE], function (VioContext $ctx, string $out) {
    $cs = <<<'GLSL'
#version 430
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(binding = 0, rgba8) writeonly uniform image2D u_out;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(u_out);
    if (p.x >= sz.x || p.y >= sz.y) return;
    vec2 c = (vec2(p) / vec2(sz) - vec2(0.68, 0.5)) * vec2(3.2 * float(sz.x) / float(sz.y), 3.2) * 0.5;
    vec2 z = vec2(0.0);
    int i = 0;
    for (; i < 200; i++) { z = vec2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y) + c; if (dot(z, z) > 4.0) break; }
    float t = i >= 200 ? 0.0 : (float(i) - log2(log2(dot(z, z)))) / 40.0;
    vec3 col = i >= 200 ? vec3(0.0) : 0.5 + 0.5 * cos(6.2831 * (t + vec3(0.0, 0.33, 0.67)));
    imageStore(u_out, p, vec4(col, 1.0));
}
GLSL;
    $tex = vio_texture($ctx, ['width' => W, 'height' => H, 'storage' => true, 'filter' => VIO_FILTER_LINEAR]);
    $cp = vio_compute_pipeline($ctx, ['source' => $cs]);
    if (!$tex || !$cp) throw new RuntimeException('compute setup');
    vio_compute_bind_image($ctx, $cp, $tex, 0, VIO_COMPUTE_WRITE);
    vio_compute_dispatch($ctx, $cp, (int)ceil(W / 8), (int)ceil(H / 8), 1);
    $blit = pipe($ctx, VS_QUAD, FS_BLIT, ['depth_test' => false]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0,0,0, 1,-1,0,1,0, 1,1,0,1,1, -1,1,0,0,1], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $blit); vio_set_uniform($ctx, 'u_tex', 0); vio_bind_texture($ctx, $tex, 0); vio_draw($ctx, $quad);
    label($ctx, 'GLSL compute shader → image2D storage texture (vio_compute_bind_image) → sampled', 12, 28, 0xFFFFFFFF, 15);
    vio_draw_2d($ctx);
    vio_end($ctx);
    if (!vio_save_screenshot($ctx, $out)) throw new RuntimeException('screenshot');
});

/* ═══════════════════════════════════════════════════════════════════════
 * 12. 3D texture: volume ray-march
 * ═══════════════════════════════════════════════════════════════════════ */
scene('texture_3d_volume', [VIO_FEATURE_3D_PIPELINE, VIO_FEATURE_TEXTURE_3D], function (VioContext $ctx, string $out) {
    $n = 48; $d = '';
    for ($z = 0; $z < $n; $z++) for ($y = 0; $y < $n; $y++) for ($x = 0; $x < $n; $x++) {
        $px = ($x + 0.5) / $n - 0.5; $py = ($y + 0.5) / $n - 0.5; $pz = ($z + 0.5) / $n - 0.5;
        $r = sqrt($px*$px + $py*$py + $pz*$pz);
        $shell = exp(-pow(($r - 0.32) * 14, 2));
        $core = exp(-pow($r * 9, 2));
        $swirl = 0.5 + 0.5 * sin(atan2($py, $px) * 5 + $pz * 20);
        $dens = min(1.0, $shell * $swirl + $core * 1.2);
        $d .= chr((int)(255 * min(1, $dens * 1.6))) . chr((int)(255 * $dens * $swirl)) . chr((int)(255 * $core)) . chr((int)(255 * $dens));
    }
    $vol = vio_texture_3d($ctx, ['data' => $d, 'width' => $n, 'height' => $n, 'depth' => $n]);
    if (!$vol) throw new RuntimeException('texture_3d');
    $fs = <<<'GLSL'
#version 330 core
in vec2 vUv; uniform sampler3D u_vol; uniform float u_aspect; uniform float u_angle;
layout(location=0) out vec4 o;
void main() {
    vec2 p = (vUv - 0.5) * vec2(u_aspect, 1.0) * 1.15;
    float c = cos(u_angle), s = sin(u_angle);
    vec3 ro = vec3(0.0, 0.25, -2.2); vec3 rd = normalize(vec3(p, 2.0));
    ro = vec3(ro.x * c - ro.z * s, ro.y, ro.x * s + ro.z * c); rd = vec3(rd.x * c - rd.z * s, rd.y, rd.x * s + rd.z * c);
    vec3 acc = vec3(0.0); float alpha = 0.0;
    for (int i = 0; i < 96; i++) {
        vec3 pos = ro + rd * (1.2 + float(i) * 0.02);
        if (all(lessThan(abs(pos), vec3(0.5)))) {
            vec4 v = texture(u_vol, pos + 0.5);
            float a = v.a * 0.09;
            acc += (1.0 - alpha) * a * v.rgb * 1.4;
            alpha += (1.0 - alpha) * a;
        }
    }
    vec3 bg = mix(vec3(0.05, 0.05, 0.08), vec3(0.12, 0.1, 0.2), vUv.y);
    o = vec4(acc + bg * (1.0 - alpha), 1.0);
}
GLSL;
    $p = pipe($ctx, VS_QUAD, $fs, ['depth_test' => false]);
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $p);
    vio_set_uniform($ctx, 'u_vol', 0); vio_bind_texture($ctx, $vol, 0);
    vio_set_uniform($ctx, 'u_aspect', W / H); vio_set_uniform($ctx, 'u_angle', 0.6);
    vio_draw($ctx, mesh_quad($ctx));
    label($ctx, "vio_texture_3d ({$n}³ RGBA8 volume) — sampler3D, ray-marched in the fragment shader", 12, 28, 0xFFFFFFFF, 14);
    vio_draw_2d($ctx);
    vio_end($ctx);
    if (!vio_save_screenshot($ctx, $out)) throw new RuntimeException('screenshot');
});

/* ═══════════════════════════════════════════════════════════════════════
 * 13. Depth-only render target → shadow map
 * ═══════════════════════════════════════════════════════════════════════ */
scene('shadow_map', [VIO_FEATURE_3D_PIPELINE, VIO_FEATURE_RENDER_TARGET_DEPTH], function (VioContext $ctx, string $out) {
    $vsDepth = "#version 330 core\nlayout(location=0) in vec3 aPos;\nlayout(location=1) in vec3 aN;\nlayout(location=2) in vec2 aUv;\nuniform mat4 u_mvp;\nvoid main(){ gl_Position = u_mvp * vec4(aPos, 1.0); }";
    $fsDepth = "#version 330 core\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(1.0); }";
    $pDepth = pipe($ctx, $vsDepth, $fsDepth, ['depth_test' => true, 'depth_bias' => 1.5, 'slope_scaled_depth_bias' => 2.0]);
    $vs = <<<'GLSL'
#version 330 core
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aNormal; layout(location=2) in vec2 aUv;
uniform mat4 u_mvp; uniform mat4 u_model; uniform mat4 u_lightvp;
out vec3 vN; out vec2 vUv; out vec3 vW; out vec4 vL;
void main(){ vec4 w = u_model * vec4(aPos, 1.0); vW = w.xyz; vN = mat3(u_model) * aNormal; vUv = aUv;
    vL = u_lightvp * w; gl_Position = u_mvp * vec4(aPos, 1.0); }
GLSL;
    $fs = <<<'GLSL'
#version 330 core
in vec3 vN; in vec2 vUv; in vec3 vW; in vec4 vL;
uniform sampler2D u_tex; uniform sampler2D u_shadow; uniform vec3 u_lightdir; uniform vec3 u_tint; uniform float u_flip;
layout(location=0) out vec4 o;
void main(){
    vec3 n = normalize(vN);
    float diff = max(dot(n, normalize(u_lightdir)), 0.0);
    vec3 pl = vL.xyz / vL.w;
    vec2 suv = pl.xy * 0.5 + 0.5;
    suv.y = mix(suv.y, 1.0 - suv.y, u_flip);
    float depth = pl.z * 0.5 + 0.5;
    float lit = 1.0;
    if (all(greaterThanEqual(suv, vec2(0.0))) && all(lessThanEqual(suv, vec2(1.0)))) {
        float s = 0.0;
        for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
            float d = texture(u_shadow, suv + vec2(x, y) / 1024.0).r;
            s += depth - 0.0015 > d ? 0.0 : 1.0;
        }
        lit = s / 9.0;
    }
    vec3 albedo = texture(u_tex, vUv).rgb * u_tint;
    o = vec4(albedo * (0.2 + 0.8 * diff * lit), 1.0);
}
GLSL;
    $p = pipe($ctx, $vs, $fs, ['depth_test' => true]);
    $shadowRt = vio_render_target($ctx, ['width' => 1024, 'height' => 1024, 'depth_only' => true]);
    if (!$shadowRt) throw new RuntimeException('depth RT');
    $cube = mesh_cube($ctx); $sphere = mesh_sphere($ctx); $plane = mesh_plane($ctx, 10, 5);
    $brick = tex_brick($ctx); $check = tex_checker($ctx, 64, 8, [225, 225, 225], [150, 150, 160], ['mipmaps' => true, 'anisotropy' => 8]);
    $lightPos = [4.0, 6.0, 3.0];
    $lightVp = m_mul(m_ortho(-5, 5, -5, 5, 1, 16), m_lookat($lightPos, [0, 0, 0], [0, 1, 0]));
    $proj = m_perspective(deg2rad(45), W / H, 0.1, 50); $eye = [4.5, 3.0, 5.0];
    $vp = m_mul($proj, m_lookat($eye, [0, 0.2, 0], [0, 1, 0]));
    $objects = [
        [$plane,  m_translate(0, -0.5, 0), $check, [1, 1, 1]],
        [$cube,   m_mul(m_translate(-1.0, 0.0, 0.3), m_rot_y(0.5)), $brick, [1, 1, 1]],
        [$cube,   m_mul(m_translate(0.9, 0.5, -0.8), m_mul(m_rot_y(-0.3), m_scale(0.8))), $brick, [0.8, 0.9, 1.0]],
        [$sphere, m_mul(m_translate(1.4, 0.1, 1.3), m_scale(0.6)), $check, [1.0, 0.5, 0.3]],
    ];
    vio_begin($ctx);
    vio_bind_render_target($ctx, $shadowRt);
    vio_clear($ctx, 1, 1, 1, 1);
    foreach ($objects as [$m, $model]) draw3d($ctx, $pDepth, $m, ['u_mvp' => m_mul($lightVp, $model)]);
    vio_unbind_render_target($ctx);
    vio_clear($ctx, 0.07, 0.08, 0.11, 1.0);
    $shadowTex = vio_render_target_texture($shadowRt);
    foreach ($objects as [$m, $model, $tex, $tint]) {
        vio_bind_pipeline($ctx, $p);
        vio_set_uniform($ctx, 'u_mvp', m_mul($vp, $model)); vio_set_uniform($ctx, 'u_model', $model);
        vio_set_uniform($ctx, 'u_lightvp', $lightVp); vio_set_uniform($ctx, 'u_lightdir', $lightPos);
        vio_set_uniform($ctx, 'u_tint', $tint);
        vio_set_uniform($ctx, 'u_flip', vio_backend_name($ctx) === 'opengl' ? 0.0 : 1.0);
        vio_set_uniform($ctx, 'u_tex', 0); vio_bind_texture($ctx, $tex, 0);
        vio_set_uniform($ctx, 'u_shadow', 1); vio_bind_texture($ctx, $shadowTex, 1);
        vio_draw($ctx, $m);
    }
    label($ctx, "Depth-only render target (1024², depth_bias) → shadow map sampled with 3x3 PCF", 12, 28, 0xFFFFFFFF, 15);
    vio_draw_2d($ctx);
    vio_end($ctx);
    if (!vio_save_screenshot($ctx, $out)) throw new RuntimeException('screenshot');
});

/* ═══════════════════════════════════════════════════════════════════════
 * 14. HDR render target + tone mapping
 * ═══════════════════════════════════════════════════════════════════════ */
scene('hdr_tonemap', [VIO_FEATURE_3D_PIPELINE, VIO_FEATURE_RENDER_TARGET_HDR], function (VioContext $ctx, string $out) {
    $fsEmit = <<<'GLSL'
#version 330 core
in vec3 vN; in vec2 vUv; in vec3 vW;
uniform sampler2D u_tex; uniform vec3 u_light; uniform vec3 u_eye; uniform vec3 u_tint;
layout(location=0) out vec4 o;
void main(){
    vec3 n = normalize(vN); vec3 l = normalize(u_light - vW); vec3 v = normalize(u_eye - vW);
    float diff = max(dot(n, l), 0.0);
    float spec = pow(max(dot(n, normalize(l + v)), 0.0), 64.0);
    vec3 albedo = texture(u_tex, vUv).rgb * u_tint;
    o = vec4(albedo * (0.2 + 1.5 * diff) + vec3(12.0) * spec, 1.0);   // HDR: highlights far above 1.0
}
GLSL;
    $fsTone = <<<'GLSL'
#version 330 core
in vec2 vUv; uniform sampler2D u_tex; uniform float u_exposure; uniform float u_mode;
layout(location=0) out vec4 o;
void main(){
    vec3 c = texture(u_tex, vUv).rgb * u_exposure;
    if (u_mode > 0.5) { c = (c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14); }   // ACES fit
    o = vec4(pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);
}
GLSL;
    $p = pipe($ctx, VS_LIT, $fsEmit, ['depth_test' => true, 'hdr' => true]);
    $tone = pipe($ctx, VS_QUAD, $fsTone, ['depth_test' => false]);
    $rt = vio_render_target($ctx, ['width' => 320, 'height' => 400, 'hdr' => true]);
    $sphere = mesh_sphere($ctx, 32, 64); $plane = mesh_plane($ctx, 8, 4);
    $check = tex_checker($ctx, 64, 8, [200, 200, 210], [80, 80, 95], ['mipmaps' => true]);
    $proj = m_perspective(deg2rad(45), 320 / 400, 0.1, 50); $eye = [0, 1.6, 4.0];
    $vp = m_mul($proj, m_lookat($eye, [0, 0.2, 0], [0, 1, 0]));
    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt);
    vio_clear($ctx, 0.02, 0.02, 0.03, 1.0);
    $m = m_translate(0, -0.6, 0);
    draw3d($ctx, $p, $plane, ['u_mvp' => m_mul($vp, $m), 'u_model' => $m, 'u_light' => [1.5, 3.0, 2.5], 'u_eye' => $eye, 'u_tint' => [1, 1, 1]], $check);
    foreach ([[-0.9, [1.0, 0.3, 0.2]], [0.9, [0.2, 0.5, 1.0]]] as [$x, $tint]) {
        $m = m_mul(m_translate($x, 0.1, 0), m_scale(0.7));
        draw3d($ctx, $p, $sphere, ['u_mvp' => m_mul($vp, $m), 'u_model' => $m, 'u_light' => [1.5, 3.0, 2.5], 'u_eye' => $eye, 'u_tint' => $tint], $check);
    }
    vio_unbind_render_target($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    foreach ([[-1, 0.0, 'RGBA16F clamped (no tone map)'], [0, 1.0, 'ACES tone map, exposure 0.8']] as [$x0, $mode, $name]) {
        $q = rt_quad($ctx, $x0, -1, $x0 + 1, 1);
        vio_bind_pipeline($ctx, $tone);
        vio_set_uniform($ctx, 'u_tex', 0); vio_bind_texture($ctx, vio_render_target_texture($rt), 0);
        vio_set_uniform($ctx, 'u_exposure', $mode > 0.5 ? 0.8 : 1.0); vio_set_uniform($ctx, 'u_mode', $mode);
        vio_draw($ctx, $q);
        label($ctx, $name, ($x0 + 1) * W / 2 + 10, 28, 0xFFFFFFFF, 15);
    }
    label($ctx, "vio_render_target(['hdr' => true]) — 16-bit float colour, highlights above 1.0", 12, H - 10, 0xFFCCCCCC, 14);
    vio_draw_2d($ctx);
    vio_end($ctx);
    if (!vio_save_screenshot($ctx, $out)) throw new RuntimeException('screenshot');
});

/* ═══════════════════════════════════════════════════════════════════════
 * 15. Vertex-stage storage buffer: compute writes instance matrices, the
 *     vertex shader reads them (no CPU round trip)
 * ═══════════════════════════════════════════════════════════════════════ */
scene('compute_vertex_storage', [VIO_FEATURE_3D_PIPELINE, VIO_FEATURE_COMPUTE, VIO_FEATURE_VERTEX_STORAGE], function (VioContext $ctx, string $out) {
    $n = 512;
    $cs = <<<'GLSL'
#version 430
layout(local_size_x = 64) in;
layout(std430, binding = 1) writeonly buffer Out { mat4 models[]; };
layout(std140, binding = 2) uniform Params { float time; float count; vec2 pad; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uint(count)) return;
    float t = float(i) / count;
    float a = t * 6.2831 * 6.0 + time;
    float r = 0.8 + 3.2 * t;
    vec3 p = vec3(cos(a) * r, (t - 0.5) * 3.0 + 0.4 * sin(a * 3.0 + time), sin(a) * r);
    float s = 0.09 + 0.12 * (1.0 - t);
    float c = cos(a), sn = sin(a);
    models[i] = mat4(vec4(c * s, 0.0, -sn * s, 0.0), vec4(0.0, s, 0.0, 0.0), vec4(sn * s, 0.0, c * s, 0.0), vec4(p, 1.0));
}
GLSL;
    $vs = <<<'GLSL'
#version 430
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aNormal; layout(location=2) in vec2 aUv;
layout(std430, binding = 0) readonly buffer Inst { mat4 models[]; };
layout(std140, binding = 0) uniform VP { mat4 u_vp; };   // UBO block at b0: portable across GL transpile + D3D root signature
out vec3 vN; out vec3 vW; out vec3 vC;
void main(){
    mat4 m = models[gl_InstanceID];
    vec4 w = m * vec4(aPos, 1.0);
    vW = w.xyz; vN = mat3(m) * aNormal;
    float t = float(gl_InstanceID) / 512.0;
    vC = 0.55 + 0.45 * cos(6.2831 * (t + vec3(0.0, 0.33, 0.67)));
    gl_Position = u_vp * w;
}
GLSL;
    $fs = "#version 430\nin vec3 vN; in vec3 vW; in vec3 vC;\nuniform vec3 u_light;\nlayout(location=0) out vec4 o;\nvoid main(){ float d = max(dot(normalize(vN), normalize(u_light - vW)), 0.0); o = vec4(vC * (0.3 + 0.7 * d), 1.0); }";
    $cp = vio_compute_pipeline($ctx, ['source' => $cs]);
    /* stride 4 == raw (ByteAddressBuffer) view, matching the transpiled `buffer { mat4 models[]; }`. */
    $buf = vio_storage_buffer($ctx, ['size' => $n * 64, 'stride' => 4]);
    if (!$cp || !$buf) throw new RuntimeException('compute/storage setup');
    vio_compute_bind_buffer($ctx, $cp, $buf, 1, VIO_COMPUTE_WRITE);
    vio_compute_set_uniforms($ctx, $cp, pack('f4', 0.7, $n, 0, 0));
    vio_compute_dispatch($ctx, $cp, (int)ceil($n / 64), 1, 1);
    $p = pipe($ctx, $vs, $fs, ['depth_test' => true]);
    $cube = mesh_cube($ctx);
    $proj = m_perspective(deg2rad(42), W / H, 0.1, 60);
    $vp = m_mul($proj, m_lookat([6.5, 3.5, 6.5], [0, 0, 0], [0, 1, 0]));
    vio_begin($ctx);
    vio_clear($ctx, 0.05, 0.06, 0.09, 1.0);
    vio_bind_pipeline($ctx, $p);
    vio_set_uniform($ctx, 'u_vp', $vp); vio_set_uniform($ctx, 'u_light', [5, 8, 4]);
    vio_bind_storage_buffer($ctx, $buf, 0, VIO_COMPUTE_READ);
    vio_draw_instanced_from_buffer($ctx, $cube, $n);
    label($ctx, "Compute writes $n instance matrices → storage buffer → read by the vertex stage (no readback)", 12, 28, 0xFFFFFFFF, 14);
    vio_draw_2d($ctx);
    vio_end($ctx);
    if (!vio_save_screenshot($ctx, $out)) throw new RuntimeException('screenshot');
});

/* ═══════════════════════════════════════════════════════════════════════
 * 16. Geometry shader — point sprites: one vertex in, a screen-aligned quad out
 * ═══════════════════════════════════════════════════════════════════════ */
scene('geometry_shader', [VIO_FEATURE_3D_PIPELINE, VIO_FEATURE_GEOMETRY], function (VioContext $ctx, string $out) {
    /* #version 450: location qualifiers on the varyings the GS reads. The
     * geometry stage takes its input position from vData[0] (a user varying),
     * not gl_in[0].gl_Position - the portable form for the D3D backends. */
    $vs = <<<'GLSL'
#version 450
layout(location=0) in vec4 aPosSize;   // xyz = position, w = sprite size
layout(location=1) in vec3 aColor;
layout(location=0) out vec4 vData;
layout(location=1) out vec3 vColor;
void main() { vData = aPosSize; vColor = aColor; gl_Position = vec4(aPosSize.xyz, 1.0); }
GLSL;
    $gs = <<<'GLSL'
#version 450
layout(points) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location=0) in vec4 vData[];
layout(location=1) in vec3 vColor[];
layout(location=0) out vec2 gUv;
layout(location=1) out vec3 gColor;
uniform float u_scale;
uniform float u_aspect;
void main() {
    vec4 c = vec4(vData[0].xyz, 1.0);
    float s = vData[0].w * u_scale;
    vec2 h = vec2(s / u_aspect, s);
    gColor = vColor[0];
    gUv = vec2(0.0, 0.0); gl_Position = c + vec4(-h.x, -h.y, 0.0, 0.0); EmitVertex();
    gUv = vec2(1.0, 0.0); gl_Position = c + vec4( h.x, -h.y, 0.0, 0.0); EmitVertex();
    gUv = vec2(0.0, 1.0); gl_Position = c + vec4(-h.x,  h.y, 0.0, 0.0); EmitVertex();
    gUv = vec2(1.0, 1.0); gl_Position = c + vec4( h.x,  h.y, 0.0, 0.0); EmitVertex();
    EndPrimitive();
}
GLSL;
    $fs = <<<'GLSL'
#version 450
layout(location=0) in vec2 gUv;
layout(location=1) in vec3 gColor;
layout(location=0) out vec4 o;
void main() {
    float d = length(gUv - vec2(0.5)) * 2.0;
    float core = 1.0 - smoothstep(0.55, 0.75, d);
    float glow = exp(-d * d * 3.0) * 0.5;
    o = vec4(gColor * (core + glow) * 0.55, 1.0);
}
GLSL;
    $sh = vio_shader($ctx, ['vertex' => $vs, 'geometry' => $gs, 'fragment' => $fs, 'format' => shader_fmt($ctx)]);
    if (!$sh) throw new RuntimeException('geometry shader failed');
    $p = vio_pipeline($ctx, ['shader' => $sh, 'topology' => VIO_POINTS, 'blend' => VIO_BLEND_ADDITIVE,
                             'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    if (!$p) throw new RuntimeException('pipeline failed');
    /* A galaxy spiral of points: position + size + colour per vertex, ONE
     * vertex each - the GS turns every one into a sprite quad. */
    $v = []; $n = 700;
    mt_srand(7);
    for ($i = 0; $i < $n; $i++) {
        $t = $i / $n;
        $arm = $i % 3;
        $a = $t * 9.0 + $arm * (2 * M_PI / 3) + (mt_rand() / mt_getrandmax() - 0.5) * 0.6;
        $r = 0.08 + $t * 0.82 + (mt_rand() / mt_getrandmax() - 0.5) * 0.12;
        $x = cos($a) * $r; $y = sin($a) * $r * 0.62;
        $size = 0.012 + (1.0 - $t) * 0.05 + (mt_rand() / mt_getrandmax()) * 0.02;
        $col = [0.35 + 0.65 * $t, 0.45 + 0.35 * (1 - $t) * 0.8, 1.0 - 0.55 * $t];
        if ($arm === 1) $col = [$col[2], $col[0], $col[1]];
        array_push($v, $x, $y, 0.0, $size, $col[0], $col[1], $col[2]);
    }
    $points = vio_mesh($ctx, ['vertices' => $v, 'layout' => [VIO_FLOAT4, VIO_FLOAT3]]);
    vio_begin($ctx);
    vio_clear($ctx, 0.02, 0.02, 0.05, 1.0);
    vio_bind_pipeline($ctx, $p);
    vio_set_uniform($ctx, 'u_scale', 1.0);
    vio_set_uniform($ctx, 'u_aspect', W / H);
    vio_draw($ctx, $points);
    label($ctx, "Geometry stage: $n vertices (VIO_POINTS) → one sprite quad per point, emitted by the GS", 12, 28, 0xFFFFFFFF, 14);
    vio_draw_2d($ctx);
    vio_end($ctx);
    if (!vio_save_screenshot($ctx, $out)) throw new RuntimeException('screenshot');
});

/* ═══════════════════════════════════════════════════════════════════════
 * 17. Tessellation — quad patches displaced in the evaluation stage
 * ═══════════════════════════════════════════════════════════════════════ */
scene('tessellation', [VIO_FEATURE_3D_PIPELINE, VIO_FEATURE_TESSELLATION], function (VioContext $ctx, string $out) {
    $vs = <<<'GLSL'
#version 450
layout(location=0) in vec3 aPos;
void main() { gl_Position = vec4(aPos, 1.0); }
GLSL;
    /* Control shader: passes the 4 control points through and sets every
     * tessellation level from u_level (its own constant block on D3D). */
    $tcs = <<<'GLSL'
#version 450
layout(vertices = 4) out;
uniform float u_level;
void main() {
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    if (gl_InvocationID == 0) {
        gl_TessLevelOuter[0] = u_level; gl_TessLevelOuter[1] = u_level;
        gl_TessLevelOuter[2] = u_level; gl_TessLevelOuter[3] = u_level;
        gl_TessLevelInner[0] = u_level; gl_TessLevelInner[1] = u_level;
    }
}
GLSL;
    /* Evaluation shader: bilinear patch position, procedural height, MVP. */
    $tes = <<<'GLSL'
#version 450
layout(quads, equal_spacing, ccw) in;
uniform mat4 u_mvp;
layout(location=0) out vec3 vW;
float height(vec2 p) { return 0.28 * sin(p.x * 2.6) * cos(p.y * 2.2) + 0.12 * sin(p.x * 6.0 + p.y * 4.0); }
void main() {
    vec2 uv = gl_TessCoord.xy;
    vec4 a = mix(gl_in[0].gl_Position, gl_in[1].gl_Position, uv.x);
    vec4 b = mix(gl_in[3].gl_Position, gl_in[2].gl_Position, uv.x);
    vec4 p = mix(a, b, uv.y);
    p.y = height(p.xz);
    vW = p.xyz;
    gl_Position = u_mvp * vec4(p.xyz, 1.0);
}
GLSL;
    /* Faceted lighting from screen-space derivatives makes the generated
     * triangles visible: the density is the tessellation level. */
    $fs = <<<'GLSL'
#version 450
layout(location=0) in vec3 vW;
uniform vec3 u_light;
layout(location=0) out vec4 o;
void main() {
    vec3 n = normalize(cross(dFdx(vW), dFdy(vW)));
    if (n.y < 0.0) n = -n;
    float d = max(dot(n, normalize(u_light - vW)), 0.0);
    float h = clamp(vW.y * 1.6 + 0.5, 0.0, 1.0);
    vec3 base = mix(vec3(0.12, 0.35, 0.55), vec3(0.95, 0.75, 0.35), h);
    o = vec4(base * (0.25 + 0.75 * d), 1.0);
}
GLSL;
    $sh = vio_shader($ctx, ['vertex' => $vs, 'tess_control' => $tcs, 'tess_eval' => $tes, 'fragment' => $fs, 'format' => shader_fmt($ctx)]);
    if (!$sh) throw new RuntimeException('tessellation shader failed');
    $p = vio_pipeline($ctx, ['shader' => $sh, 'patch_vertices' => 4, 'depth_test' => true, 'cull_mode' => VIO_CULL_NONE]);
    if (!$p) throw new RuntimeException('pipeline failed');
    /* Two 3x3 grids of quad patches (4 control points each, no indices) on
     * the xz plane: left half drawn at level 2, right half at level 18. */
    $grid = function (float $x0, float $x1, float $z0, float $z1, int $n) {
        $v = [];
        for ($i = 0; $i < $n; $i++) for ($j = 0; $j < $n; $j++) {
            $ax = $x0 + ($x1 - $x0) * $i / $n; $bx = $x0 + ($x1 - $x0) * ($i + 1) / $n;
            $az = $z0 + ($z1 - $z0) * $j / $n; $bz = $z0 + ($z1 - $z0) * ($j + 1) / $n;
            array_push($v, $ax, 0, $az, $bx, 0, $az, $bx, 0, $bz, $ax, 0, $bz);
        }
        return $v;
    };
    $left  = vio_mesh($ctx, ['vertices' => $grid(-2.05, -0.05, -1.5, 1.5, 3), 'layout' => [VIO_FLOAT3]]);
    $right = vio_mesh($ctx, ['vertices' => $grid(0.05, 2.05, -1.5, 1.5, 3), 'layout' => [VIO_FLOAT3]]);
    $proj = m_perspective(deg2rad(40), W / H, 0.1, 30);
    $mvp = m_mul($proj, m_lookat([0.0, 2.6, 4.2], [0, -0.1, 0], [0, 1, 0]));
    vio_begin($ctx);
    vio_clear($ctx, 0.05, 0.06, 0.09, 1.0);
    vio_bind_pipeline($ctx, $p);
    vio_set_uniform($ctx, 'u_mvp', $mvp);
    vio_set_uniform($ctx, 'u_light', [3.0, 5.0, 3.0]);
    vio_set_uniform($ctx, 'u_level', 2.0);
    vio_draw($ctx, $left);
    vio_set_uniform($ctx, 'u_level', 18.0);
    vio_draw($ctx, $right);
    label($ctx, 'Tessellation: 3x3 quad patches, height in the evaluation stage — u_level 2 (left) vs 18 (right)', 12, 28, 0xFFFFFFFF, 14);
    vio_draw_2d($ctx);
    vio_end($ctx);
    if (!vio_save_screenshot($ctx, $out)) throw new RuntimeException('screenshot');
});

/* ── Report ─────────────────────────────────────────────────────────── */
$probe = vio_create($backend, ['width' => 8, 'height' => 8, 'headless' => true, 'vsync' => false]);
echo "backend: ", $probe ? vio_backend_name($probe) : 'n/a', "\n";
if ($probe) vio_destroy($probe);
foreach ($results as $name => $r) echo str_pad($name, 28), " ", $r, "\n";
echo "output: $outdir\n";
