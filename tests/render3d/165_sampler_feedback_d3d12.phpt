--TEST--
Sampler feedback (VIO_FEATURE_SAMPLER_FEEDBACK): a fragment stage that calls WriteSamplerFeedback records the minimum mip it sampled per region of a texture; vio_sampler_feedback_read decodes it, vio_sampler_feedback_clear resets it
--EXTENSIONS--
vio
--FILE--
<?php
/* D3D12 only (SM 6.5 + SamplerFeedbackTier 0.9): GLSL has no sampler feedback,
 * so the fragment stage is an HLSL override ('hlsl' => ['fragment' => ...]) that
 * declares the contract register
 *   FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> vio_feedback : register(u0, space2);
 * and vio binds the map of the texture given to vio_sampler_feedback_bind().
 * A 256x256 texture with a full mip chain drawn onto a 32x32 target samples
 * mip 3 (256 / 32 = 8 texels per pixel); every region of the map must report
 * a mip near 3, and after a clear none may report anything. Backends without
 * the feature refuse the three functions (false + warning).
 * VIO_REQUIRE_SAMPLER_FEEDBACK=d3d12 makes the backend mandatory. */
$W = 32;
$VS = "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=0) out vec2 uv;\n"
    . "void main(){ uv = aPos.xy * 0.5 + 0.5; gl_Position = vec4(aPos, 1.0); }";
$FS = "#version 450\nlayout(location=0) in vec2 uv;\nlayout(location=0) out vec4 o;\nuniform sampler2D u_tex;\n"
    . "void main(){ o = texture(u_tex, uv); }";
/* Same interface as the GLSL stage: t0 / s0 (vio's register scheme), TEXCOORD0. */
$PS_HLSL = "Texture2D<float4> u_tex : register(t0);\n"
    . "SamplerState _u_tex_sampler : register(s0);\n"
    . "FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> vio_feedback : register(u0, space2);\n"
    . "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    . "float4 main(PSIn i) : SV_Target0 {\n"
    . "  vio_feedback.WriteSamplerFeedback(u_tex, _u_tex_sampler, i.uv);\n"
    . "  return u_tex.Sample(_u_tex_sampler, i.uv);\n"
    . "}\n";

$opts = ["width" => $W, "height" => $W, "headless" => true, "vsync" => false, "shader_model" => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_SAMPLER_FEEDBACK') ?: ''));

function run_backend(string $name): string {
    global $W, $VS, $FS, $PS_HLSL, $opts, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    $tex = vio_texture($ctx, ['data' => str_repeat("\x80\x40\x20\xFF", 256 * 256), 'width' => 256, 'height' => 256, 'mipmaps' => true]);
    if (!vio_supports_feature($ctx, VIO_FEATURE_SAMPLER_FEEDBACK)) {
        $fail = [];
        if (@vio_sampler_feedback_bind($ctx, $tex) !== false) $fail[] = "bind succeeded without the feature";
        if (@vio_sampler_feedback_read($ctx, $tex) !== false) $fail[] = "read succeeded without the feature";
        if (@vio_sampler_feedback_clear($ctx, $tex) !== false) $fail[] = "clear succeeded without the feature";
        vio_destroy($ctx);
        if ($fail) return "FAIL\n  " . implode("\n  ", $fail);
        return $req ? "FAIL\n  required but VIO_FEATURE_SAMPLER_FEEDBACK is 0" : "skip (no sampler feedback)";
    }
    $fail = [];
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS, 'hlsl' => ['fragment' => $PS_HLSL]]);
    if (!$sh) { vio_destroy($ctx); return "FAIL\n  shader with the feedback override not created"; }
    $pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);

    if (!vio_sampler_feedback_clear($ctx, $tex)) $fail[] = "initial clear failed";
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    if (!vio_sampler_feedback_bind($ctx, $tex)) $fail[] = "bind failed";
    vio_bind_pipeline($ctx, $pipe);
    vio_bind_texture($ctx, $tex, 0);
    vio_set_uniform($ctx, 'u_tex', 0);
    vio_draw($ctx, $quad);
    vio_sampler_feedback_bind($ctx, null);
    vio_end($ctx);

    $fb = vio_sampler_feedback_read($ctx, $tex);
    if (!is_array($fb) || !isset($fb['min_mip'], $fb['regions_x'], $fb['regions_y'], $fb['region'])) {
        $fail[] = "read returned " . json_encode($fb);
    } else {
        if (count($fb['min_mip']) !== $fb['regions_x'] * $fb['regions_y']) $fail[] = "min_mip has " . count($fb['min_mip']) . " entries for {$fb['regions_x']}x{$fb['regions_y']} regions";
        if ($fb['regions_x'] * $fb['region'] < 256 || $fb['regions_y'] * $fb['region'] < 256) $fail[] = "regions do not cover the texture";
        $bad = array_filter($fb['min_mip'], fn($m) => $m === null || $m < 2 || $m > 4);
        if ($bad) $fail[] = count($bad) . " regions not at mip 3 (first: " . json_encode(array_values($bad)[0]) . ")";
    }
    if (!vio_sampler_feedback_clear($ctx, $tex)) $fail[] = "clear failed";
    $fb = vio_sampler_feedback_read($ctx, $tex);
    if (!is_array($fb) || array_filter($fb['min_mip'], fn($m) => $m !== null)) $fail[] = "regions still report a mip after the clear";
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
