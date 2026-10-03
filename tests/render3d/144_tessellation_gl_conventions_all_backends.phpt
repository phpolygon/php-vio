--TEST--
Tessellation keeps OpenGL conventions on every backend: the edge each outer level subdivides, the winding of generated triangles (back-face culling), patch varyings with uniforms in both stages, an input patch larger than the output patch (gl_PatchVerticesIn), isolines
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_TESSELLATION)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with tessellation");
?>
--FILE--
<?php
/* SPIRV-CROSS-HLSL-TESS-PLAN 5f. Pure GLSL tessellation stages (no HLSL
 * override): on D3D11 / D3D12 they go through vio's hull / domain generator
 * (or a SPIRV-Cross with hull / domain shaders).
 *   A. quad domain: outer[1] = 6 subdivides the v = 0 edge only; ccw triangles
 *      survive VIO_CULL_BACK
 *   B. triangle domain: the same for outer[1]
 *   C. a patch varying written by the control stage from a uniform, scaled by
 *      a uniform of the evaluation stage
 *   D. layout(vertices = 3) fed with 4-point patches: the control stage reads
 *      the 4th input point and gl_PatchVerticesIn == 4
 *   E. isolines: lines at v = 0, 1/4, 1/2, 3/4 - on D3D only on hardware: WARP
 *      (vio's headless D3D device, the Windows CI) loses the line primitives of
 *      tessellated isolines (point output is fine), with any HLSL
 *   F. A once more on D3D12 with shader_model 6 (DXC), when available
 * Every check is orientation independent: colours carry the domain
 * coordinates (R = marker, G = v), so the readback's row order does not
 * matter. */
$W = 64;
$VS = "#version 450\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$FS = "#version 450\nlayout(location=0) in vec3 c;\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(c, 1.0); }";
function tcs(int $n, string $levels, string $extra = '', string $body = ''): string {
    return "#version 450\nlayout(vertices = $n) out;\n$extra"
         . "void main(){ gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n$body"
         . "  if (gl_InvocationID == 0) { $levels } }";
}
$QUAD_TCS = tcs(4, "gl_TessLevelOuter[0] = 1.0; gl_TessLevelOuter[1] = 6.0; gl_TessLevelOuter[2] = 1.0; gl_TessLevelOuter[3] = 1.0; gl_TessLevelInner[0] = 1.0; gl_TessLevelInner[1] = 1.0;");
$QUAD_TES = "#version 450\nlayout(quads, equal_spacing, ccw) in;\nlayout(location=0) out vec3 c;\n"
          . "void main(){ vec2 uv = gl_TessCoord.xy; float m = (uv.y == 0.0 && uv.x > 0.0 && uv.x < 1.0) ? 1.0 : 0.0;\n"
          . "  c = vec3(m, uv.y, 0.0); gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0); }";
$TRI_TCS = tcs(3, "gl_TessLevelOuter[0] = 1.0; gl_TessLevelOuter[1] = 6.0; gl_TessLevelOuter[2] = 1.0; gl_TessLevelInner[0] = 1.0;");
$TRI_TES = "#version 450\nlayout(triangles, equal_spacing, ccw) in;\nlayout(location=0) out vec3 c;\n"
         . "void main(){ vec3 t = gl_TessCoord; float m = (t.y == 0.0 && t.x > 0.0 && t.z > 0.0) ? 1.0 : 0.0;\n"
         . "  c = vec3(m, t.y, 0.0); gl_Position = vec4(vec2(-1.0, -1.0) * t.x + vec2(1.0, -1.0) * t.y + vec2(0.0, 1.0) * t.z, 0.0, 1.0); }";
$PATCH_TCS = tcs(4, "gl_TessLevelOuter[0] = 2.0; gl_TessLevelOuter[1] = 2.0; gl_TessLevelOuter[2] = 2.0; gl_TessLevelOuter[3] = 2.0; gl_TessLevelInner[0] = 2.0; gl_TessLevelInner[1] = 2.0; pTint = u_tint;",
                 "uniform vec4 u_tint;\nlayout(location = 1) patch out vec4 pTint;\n");
$PATCH_TES = "#version 450\nlayout(quads, equal_spacing, ccw) in;\nuniform float u_scale;\nlayout(location = 1) patch in vec4 pTint;\nlayout(location=0) out vec3 c;\n"
           . "void main(){ c = pTint.rgb * u_scale; gl_Position = vec4(gl_TessCoord.xy * 2.0 - 1.0, 0.0, 1.0); }";
/* 4 input points, 3 output points: the third output point is half the fourth input. */
$BIG_TCS = "#version 450\nlayout(vertices = 3) out;\nlayout(location = 0) patch out vec4 pInfo;\n"
         . "void main(){ int i = gl_InvocationID; gl_out[gl_InvocationID].gl_Position = i == 2 ? vec4(gl_in[3].gl_Position.xy * 0.5, 0.0, 1.0) : gl_in[i].gl_Position;\n"
         . "  if (i == 0) { pInfo = vec4(float(gl_PatchVerticesIn) / 8.0, 0.0, 0.0, 1.0);\n"
         . "    gl_TessLevelOuter[0] = 1.0; gl_TessLevelOuter[1] = 1.0; gl_TessLevelOuter[2] = 1.0; gl_TessLevelInner[0] = 1.0; } }";
$BIG_TES = "#version 450\nlayout(triangles, equal_spacing, ccw) in;\nlayout(location = 0) patch in vec4 pInfo;\nlayout(location=0) out vec3 c;\n"
         . "void main(){ c = pInfo.rgb; vec3 t = gl_TessCoord;\n"
         . "  gl_Position = gl_in[0].gl_Position * t.x + gl_in[1].gl_Position * t.y + gl_in[2].gl_Position * t.z; }";
$ISO_TCS = tcs(2, "gl_TessLevelOuter[0] = 4.0; gl_TessLevelOuter[1] = 1.0;");
$ISO_TES = "#version 450\nlayout(isolines, equal_spacing) in;\nlayout(location=0) out vec3 c;\n"
         . "void main(){ vec2 uv = gl_TessCoord.xy; c = vec3(1.0, uv.y, 0.0); gl_Position = vec4(uv.x * 1.8 - 0.9, uv.y * 1.8 - 0.9, 0.0, 1.0); }";

/* lit pixels, and the green value of the reddest pixel */
function scan(string $p, int $w = 64): array {
    $lit = 0; $best = -1; $g = -1; $greens = [];
    for ($i = 0; $i < $w * $w; $i++) {
        $r = ord($p[$i * 4]); $gg = ord($p[$i * 4 + 1]); $b = ord($p[$i * 4 + 2]);
        if ($r || $gg || $b) $lit++;
        if ($r > $best) { $best = $r; $g = $gg; }
        if ($r) $greens[$gg] = true;
    }
    ksort($greens);
    return [$lit, $best, $g, array_keys($greens)];
}

function check_isolines(?string $p, int $w = 64): ?string {
    if ($p === null) return "E: shader not created";
    $want = [0, 64, 128, 191];
    $got = scan($p, $w)[3];
    $ok = count($got) === 4;
    foreach ($want as $i => $v) if (!$ok || abs($got[$i] - $v) > 1) $ok = false;
    return $ok ? null : "E: isolines at green " . json_encode($got) . ", want ~" . json_encode($want);
}

/* E on D3D hardware: a windowed context, when it reports a real adapter. */
function iso_hardware(string $backend): string {
    global $VS, $FS, $ISO_TCS, $ISO_TES;
    $ctx = @vio_create($backend, ["width" => 256, "height" => 256, "headless" => false, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    $gpu = vio_gpu_info()['name'] ?? '';
    if ($gpu === '' || stripos($gpu, 'Basic Render') !== false) { vio_destroy($ctx); return "skip (WARP)"; }
    if (vio_framebuffer_size($ctx) !== [256, 256]) { vio_destroy($ctx); return "skip (window is not 256x256)"; }
    $sh = vio_shader($ctx, ['vertex' => $VS, 'tess_control' => $ISO_TCS, 'tess_eval' => $ISO_TES, 'fragment' => $FS]);
    $pipe = $sh ? vio_pipeline($ctx, ['shader' => $sh, 'patch_vertices' => 2, 'depth_test' => false, 'cull_mode' => VIO_CULL_BACK]) : null;
    $line = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0], 'layout' => [VIO_FLOAT3]]);
    $p = null;
    if ($pipe) for ($f = 0; $f < 2; $f++) {   /* the first frame of a fresh window may be empty */
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx); vio_bind_pipeline($ctx, $pipe); vio_draw($ctx, $line); vio_end($ctx);
        $p = vio_read_pixels($ctx);
    }
    $e = check_isolines($p, 256);
    vio_destroy($ctx);
    return $e ? "FAIL\n  $e" : "OK";
}

function run_backend(string $name, array $opts = []): string {
    global $W, $VS, $FS, $QUAD_TCS, $QUAD_TES, $TRI_TCS, $TRI_TES, $PATCH_TCS, $PATCH_TES, $BIG_TCS, $BIG_TES, $ISO_TCS, $ISO_TES;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false] + $opts);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_TESSELLATION)) { vio_destroy($ctx); return "skip (no tessellation)"; }
    if (isset($opts['shader_model']) && (vio_swapchain_info($ctx)['shader_model'] ?? 5) < 6) { vio_destroy($ctx); return "skip (no DXC)"; }
    $d3d = in_array(vio_backend_name($ctx), ['d3d11', 'd3d12'], true);
    $metal = vio_backend_name($ctx) === 'metal';   /* no isolines in Metal's tessellator */
    $fail = [];
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'layout' => [VIO_FLOAT3]]);
    $tri  = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 0,1,0], 'layout' => [VIO_FLOAT3]]);
    $line = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0], 'layout' => [VIO_FLOAT3]]);
    $draw = function (string $tcs, string $tes, int $n, $mesh, array $uniforms = []) use ($ctx, $VS, $FS): ?string {
        $sh = vio_shader($ctx, ['vertex' => $VS, 'tess_control' => $tcs, 'tess_eval' => $tes, 'fragment' => $FS]);
        if (!$sh) return null;
        $pipe = vio_pipeline($ctx, ['shader' => $sh, 'patch_vertices' => $n, 'depth_test' => false, 'cull_mode' => VIO_CULL_BACK]);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        foreach ($uniforms as $k => $v) vio_set_uniform($ctx, $k, $v);
        vio_draw($ctx, $mesh);
        vio_end($ctx);
        return vio_read_pixels($ctx);
    };

    foreach (['A' => [$QUAD_TCS, $QUAD_TES, 4, $quad, 4096], 'B' => [$TRI_TCS, $TRI_TES, 3, $tri, 2048]] as $k => [$tcs, $tes, $n, $mesh, $area]) {
        $p = $draw($tcs, $tes, $n, $mesh);
        if ($p === null) { $fail[] = "$k: shader not created"; continue; }
        [$lit, $red, $g] = scan($p);
        if (abs($lit - $area) > $area / 16) $fail[] = "$k: $lit px lit, want ~$area (winding culled?)";
        elseif ($red < 128 || $g > 40) $fail[] = "$k: subdivided edge at v=" . round($g / 255, 2) . " (red $red), want v=0";
    }
    if (isset($opts['shader_model'])) { vio_destroy($ctx); return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK"; }

    $p = $draw($PATCH_TCS, $PATCH_TES, 4, $quad, ['u_tint' => [0.2, 0.4, 0.8, 1.0], 'u_scale' => 0.5]);
    if ($p === null) $fail[] = "C: shader not created";
    else {
        $o = (32 * 64 + 32) * 4;
        $px = [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])];
        if (abs($px[0] - 26) > 3 || abs($px[1] - 51) > 3 || abs($px[2] - 102) > 3) $fail[] = "C: patch varying " . json_encode($px) . ", want [26,51,102]";
    }

    $p = $draw($BIG_TCS, $BIG_TES, 4, $quad);
    if ($p === null) $fail[] = "D: shader not created";
    else {
        [$lit] = scan($p);
        /* (-1,-1), (1,-1), (-0.5, 0.5): 1.5 of the 4 NDC units -> 1536 px */
        if (abs($lit - 1536) > 96) $fail[] = "D: $lit px lit, want ~1536 (4th input point)";
        $red = 0;
        for ($i = 0; $i < 64 * 64; $i++) $red = max($red, ord($p[$i * 4]));
        if (abs($red - 128) > 2) $fail[] = "D: gl_PatchVerticesIn / 8 = " . round($red / 255, 3) . ", want 0.5";
    }

    /* Headless D3D is WARP, which cannot draw tessellated isolines; see
     * iso_hardware() below for D3D. */
    if (!$d3d && !$metal && ($e = check_isolines($draw($ISO_TCS, $ISO_TES, 2, $line)))) $fail[] = $e;

    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
/* DXC as in test 119: VIO_DXC_DIR, else the Windows SDK, else the search path. */
$sm6 = ['shader_model' => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $sm6['dxc_dir'] = $dxc;
echo "d3d12 sm6: ", run_backend('d3d12', $sm6), "\n";
echo "d3d11 hardware isolines: ", iso_hardware('d3d11'), "\n";
echo "d3d12 hardware isolines: ", iso_hardware('d3d12'), "\n";
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
d3d12 sm6: %r(OK|skip \(.*\))%r
d3d11 hardware isolines: %r(OK|skip \(.*\))%r
d3d12 hardware isolines: %r(OK|skip \(.*\))%r
DONE
