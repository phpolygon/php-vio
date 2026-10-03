--TEST--
HLSL stage overrides: vio_shader(['hlsl' => ['tess_control' | 'tess_eval' | 'geometry' => src]]) give D3D tessellation and GS instancing; GL / Vulkan keep the GLSL stages; a cbuffer that does not match the GLSL uniforms warns (VIO_FEATURE_HLSL_STAGE_OVERRIDE)
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_TESSELLATION) || vio_supports_feature($c, VIO_FEATURE_HLSL_STAGE_OVERRIDE)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with tessellation or HLSL stage overrides");
?>
--FILE--
<?php
/* GEOMETRY-STAGES-PLAN Phase 3. One shader description for every backend:
 * GLSL stages for OpenGL / Vulkan, and on D3D (VIO_FEATURE_HLSL_STAGE_OVERRIDE)
 * hand-written hull / domain / geometry HLSL, because SPIRV-Cross emits no
 * hull / domain shaders and no gl_InvocationID for an HLSL GS.
 *   A. the tessellated disc of test 110 - edge count follows u_level (HS
 *      cbuffer), radius from u_radius (DS cbuffer)
 *   B. an instanced GS: four invocations draw four quadrants
 *   C. an HLSL cbuffer that does not match the GLSL stage's uniforms warns
 *   D. contract: an override without its GLSL stage, an unknown stage key
 * The HLSL domain shader outputs D3D clip space (z in [0, w]); vio adds no
 * depth fixup to override sources. */
$W = 64; $R = 0.8;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b): bool { return abs($a[0]-$b[0]) <= 3 && abs($a[1]-$b[1]) <= 3 && abs($a[2]-$b[2]) <= 3; }
function polar(float $r, float $deg): array { global $W; $a = deg2rad($deg); return [(int)round($W/2 + cos($a) * $r * $W/2), (int)round($W/2 - sin($a) * $r * $W/2)]; }

$TVS = "#version 450\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$TCS = "#version 450\nlayout(vertices = 4) out;\nuniform float u_level;\n"
     . "void main(){ gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n"
     . "  if (gl_InvocationID == 0) { gl_TessLevelOuter[0] = u_level; gl_TessLevelOuter[1] = u_level;\n"
     . "    gl_TessLevelOuter[2] = u_level; gl_TessLevelOuter[3] = u_level;\n"
     . "    gl_TessLevelInner[0] = u_level; gl_TessLevelInner[1] = u_level; } }";
$TES = "#version 450\nlayout(quads, equal_spacing, ccw) in;\nuniform float u_radius;\n"
     . "void main(){ float a = gl_TessCoord.x * 6.28318530718; float r = gl_TessCoord.y * u_radius;\n"
     . "  gl_Position = vec4(cos(a) * r, sin(a) * r, 0.0, 1.0); }";
$FS  = "#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0.0, 1.0, 0.0, 1.0); }";
$HS = <<<'HLSL'
cbuffer HullParams : register(b0) { float u_level; };
struct VSOut { float4 pos : SV_Position; };
struct CP { float4 pos : POSITION; };
struct PC { float edges[4] : SV_TessFactor; float inside[2] : SV_InsideTessFactor; };
PC patch_const(InputPatch<VSOut, 4> ip)
{
    PC o;
    o.edges[0] = u_level; o.edges[1] = u_level; o.edges[2] = u_level; o.edges[3] = u_level;
    o.inside[0] = u_level; o.inside[1] = u_level;
    return o;
}
[domain("quad")]
[partitioning("integer")]
[outputtopology("triangle_ccw")]
[outputcontrolpoints(4)]
[patchconstantfunc("patch_const")]
CP main(InputPatch<VSOut, 4> ip, uint id : SV_OutputControlPointID)
{
    CP o;
    o.pos = ip[id].pos;
    return o;
}
HLSL;
$DS = <<<'HLSL'
cbuffer DomainParams : register(b0) { float u_radius; };
struct CP { float4 pos : POSITION; };
struct PC { float edges[4] : SV_TessFactor; float inside[2] : SV_InsideTessFactor; };
struct DSOut { float4 pos : SV_Position; };
[domain("quad")]
DSOut main(PC pc, float2 uv : SV_DomainLocation, const OutputPatch<CP, 4> cp)
{
    float a = uv.x * 6.28318530718;
    float r = uv.y * u_radius;
    DSOut o;
    o.pos = float4(cos(a) * r, sin(a) * r, 0.5, 1.0);
    return o;
}
HLSL;

$VS = "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=0) out vec4 vPos;\nvoid main(){ vPos = vec4(aPos, 1.0); gl_Position = vPos; }";
$GS = "#version 450\nlayout(points, invocations = 4) in;\nlayout(triangle_strip, max_vertices = 4) out;\n"
    . "layout(location=0) in vec4 vPos[];\nlayout(location=0) out vec3 gColor;\n"
    . "const vec3 PAL[4] = vec3[4](vec3(1,0,0), vec3(0,1,0), vec3(0,0,1), vec3(1,1,0));\n"
    . "void main(){ vec2 o = vec2(gl_InvocationID % 2 == 0 ? -0.5 : 0.5, gl_InvocationID < 2 ? -0.5 : 0.5);\n"
    . "  for (int i = 0; i < 4; i++) { vec2 c = vec2(i % 2 == 0 ? -0.5 : 0.5, i < 2 ? -0.5 : 0.5);\n"
    . "    gColor = PAL[gl_InvocationID]; gl_Position = vec4(vPos[0].xy + o + c * 0.9, 0.0, 1.0); EmitVertex(); }\n"
    . "  EndPrimitive(); }";
$GSH = <<<'HLSL'
struct VSOut { float4 vPos : TEXCOORD0; float4 pos : SV_Position; };
struct GSOut { float3 color : TEXCOORD0; float4 pos : SV_Position; };
static const float3 PAL[4] = { float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1), float3(1, 1, 0) };
[instance(4)]
[maxvertexcount(4)]
void main(point VSOut ip[1], uint inst : SV_GSInstanceID, inout TriangleStream<GSOut> stream)
{
    float2 o = float2((inst % 2 == 0) ? -0.5 : 0.5, (inst < 2) ? -0.5 : 0.5);
    for (int i = 0; i < 4; i++) {
        float2 c = float2((i % 2 == 0) ? -0.5 : 0.5, (i < 2) ? -0.5 : 0.5);
        GSOut v;
        v.color = PAL[inst];
        v.pos = float4(ip[0].vPos.xy + o + c * 0.9, 0.5, 1.0);
        stream.Append(v);
    }
    stream.RestartStrip();
}
HLSL;
$FSC = "#version 450\nlayout(location=0) in vec3 gColor;\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(gColor, 1.0); }";

function run_backend(string $name): string {
    global $W, $R, $TVS, $TCS, $TES, $FS, $HS, $DS, $VS, $GS, $GSH, $FSC;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    $override = vio_supports_feature($ctx, VIO_FEATURE_HLSL_STAGE_OVERRIDE);
    $tess = vio_supports_feature($ctx, VIO_FEATURE_TESSELLATION) || $override;
    $gsInst = vio_supports_feature($ctx, VIO_FEATURE_GEOMETRY_INSTANCING) || ($override && vio_supports_feature($ctx, VIO_FEATURE_GEOMETRY));
    if (!$tess && !$gsInst) { vio_destroy($ctx); return "skip (neither tessellation nor HLSL overrides)"; }
    $fail = [];
    $base = ['depth_test' => false, 'cull_mode' => VIO_CULL_NONE];
    $frame = function (callable $draw) use ($ctx): string {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx); $draw(); vio_end($ctx);
        return vio_read_pixels($ctx);
    };

    /* ---- A: tessellation ------------------------------------------------- */
    if ($tess) {
        $sh = vio_shader($ctx, ['vertex' => $TVS, 'tess_control' => $TCS, 'tess_eval' => $TES, 'fragment' => $FS,
                                'hlsl' => ['tess_control' => $HS, 'tess_eval' => $DS]]);
        if (!$sh) { $fail[] = "A: tessellation shader not created"; }
        else {
            $pipe = vio_pipeline($ctx, ['shader' => $sh, 'patch_vertices' => 4] + $base);
            $patch = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'layout' => [VIO_FLOAT3]]);
            foreach ([4 => false, 16 => true] as $level => $lit45) {
                $p = $frame(function () use ($ctx, $pipe, $patch, $level, $R) {
                    vio_bind_pipeline($ctx, $pipe);
                    vio_set_uniform($ctx, 'u_level', (float)$level);
                    vio_set_uniform($ctx, 'u_radius', $R);
                    vio_draw($ctx, $patch);
                });
                [$x, $y] = polar(0.5 * $R, 45.0);
                if (!near(px($p, $x, $y, $W), [0, 255, 0])) $fail[] = "A: level $level: 0.5R not green " . json_encode(px($p, $x, $y, $W));
                [$x, $y] = polar(1.15 * $R, 45.0);
                if (!near(px($p, $x, $y, $W), [0, 0, 0])) $fail[] = "A: level $level: outside R not black " . json_encode(px($p, $x, $y, $W));
                [$x, $y] = polar(0.9 * $R, 45.0);
                $want = $lit45 ? [0, 255, 0] : [0, 0, 0];
                if (!near(px($p, $x, $y, $W), $want)) $fail[] = "A: level $level: 45deg/0.9R " . json_encode(px($p, $x, $y, $W)) . " want " . json_encode($want);
            }
        }
    }

    /* ---- B: instanced geometry stage ------------------------------------- */
    if ($gsInst) {
        $sh = vio_shader($ctx, ['vertex' => $VS, 'geometry' => $GS, 'fragment' => $FSC, 'hlsl' => ['geometry' => $GSH]]);
        if (!$sh) { $fail[] = "B: instanced geometry shader not created"; }
        else {
            $p = vio_pipeline($ctx, ['shader' => $sh, 'topology' => VIO_POINTS] + $base);
            $point = vio_mesh($ctx, ['vertices' => [0, 0, 0], 'layout' => [VIO_FLOAT3]]);
            $img = $frame(function () use ($ctx, $p, $point) { vio_bind_pipeline($ctx, $p); vio_draw($ctx, $point); });
            $got = [];
            foreach ([[16, 16], [48, 16], [16, 48], [48, 48]] as [$x, $y]) $got[] = json_encode(px($img, $x, $y, $W));
            sort($got);
            $want = [json_encode([0, 0, 255]), json_encode([0, 255, 0]), json_encode([255, 0, 0]), json_encode([255, 255, 0])];
            sort($want);
            if ($got !== $want) $fail[] = "B: quadrant colours " . implode(' ', $got);
        }
    }

    /* ---- C: cbuffer layout check (D3D only) ----------------------------- */
    if ($override && $tess) {
        $badHS = str_replace('float u_level;', 'float u_tess_level;', $HS);
        $warn = '';
        set_error_handler(function ($no, $msg) use (&$warn) { $warn .= $msg . "\n"; return true; });
        $sh = vio_shader($ctx, ['vertex' => $TVS, 'tess_control' => $TCS, 'tess_eval' => $TES, 'fragment' => $FS,
                                'hlsl' => ['tess_control' => $badHS, 'tess_eval' => $DS]]);
        restore_error_handler();
        if (strpos($warn, "'u_level'") === false) $fail[] = "C: a cbuffer without u_level did not warn: " . trim($warn);
    }

    /* ---- D: contract ------------------------------------------------------ */
    if (@vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FSC, 'hlsl' => ['geometry' => $GSH]]) !== false) $fail[] = "D: an HLSL override without its GLSL stage was accepted";
    if (@vio_shader($ctx, ['vertex' => $VS, 'geometry' => $GS, 'fragment' => $FSC, 'hlsl' => ['pixel' => 'x']]) !== false) $fail[] = "D: an unknown 'hlsl' stage key was accepted";

    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %s
d3d11: %s
d3d12: %s
vulkan: %s
metal: %s
DONE
