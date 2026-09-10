/*
 * php-vio - 2D embedded shaders
 * OpenGL 3.3 Core Profile GLSL 330 (works on every supported context)
 */

#ifndef VIO_SHADERS_2D_H
#define VIO_SHADERS_2D_H

/* Shape shader: position + color, with orthographic projection uniform */
static const char *vio_2d_vertex_shader =
    "#version 330 core\n"
    "layout(location = 0) in vec2 aPosition;\n"
    "layout(location = 1) in vec2 aTexCoord;\n"
    "layout(location = 2) in vec4 aColor;\n"
    "uniform mat4 uProjection;\n"
    "out vec2 vTexCoord;\n"
    "out vec4 vColor;\n"
    "void main() {\n"
    "    gl_Position = uProjection * vec4(aPosition, 0.0, 1.0);\n"
    "    vTexCoord = aTexCoord;\n"
    "    vColor = aColor;\n"
    "}\n";

/* Shape fragment shader: just outputs vertex color */
static const char *vio_2d_fragment_shader_shapes =
    "#version 330 core\n"
    "in vec2 vTexCoord;\n"
    "in vec4 vColor;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    FragColor = vColor;\n"
    "}\n";

/* Sprite/text fragment shader: samples texture * vertex color */
static const char *vio_2d_fragment_shader_sprites =
    "#version 330 core\n"
    "in vec2 vTexCoord;\n"
    "in vec4 vColor;\n"
    "out vec4 FragColor;\n"
    "uniform sampler2D uTexture;\n"
    "void main() {\n"
    "    FragColor = texture(uTexture, vTexCoord) * vColor;\n"
    "}\n";

/* ── HLSL versions (D3D11 SM 5.0 / D3D12 SM 5.1) ──────────────── */

/* Shared 2D constant buffer: projection + output control. uOutput.x = 1 when the
 * backbuffer is HDR10 (the pixel shaders PQ-encode their display-referred
 * colour), uOutput.y = paper-white luminance in nits (GAP-PHASE5 Block 6). */
static const char *vio_2d_hlsl_cb =
    "cbuffer CB : register(b0) { float4x4 uProjection; float4 uOutput; };\n"
    "float3 vio_pq(float3 srgb, float nits) {\n"
    "    float3 lin = pow(max(srgb, 0.0), 2.2);\n"
    "    float3 bt2020 = float3(dot(lin, float3(0.6274, 0.3293, 0.0433)), dot(lin, float3(0.0691, 0.9195, 0.0114)), dot(lin, float3(0.0164, 0.0880, 0.8956)));\n"
    "    float3 y = pow(max(bt2020 * (nits / 10000.0), 0.0), 0.1593017578125);\n"
    "    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);\n"
    "}\n"
    "float4 vio_out(float4 c) { if (uOutput.x > 0.5) c.rgb = vio_pq(c.rgb, uOutput.y > 0.0 ? uOutput.y : 200.0); return c; }\n";

static const char *vio_2d_hlsl_vs =
    "cbuffer CB : register(b0) { float4x4 uProjection; float4 uOutput; };\n"
    "struct VS_IN  { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
    "struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
    "VS_OUT main(VS_IN i) {\n"
    "    VS_OUT o;\n"
    "    o.pos = mul(uProjection, float4(i.pos, 0.0, 1.0));\n"
    "    o.uv  = i.uv;\n"
    "    o.col = i.col;\n"
    "    return o;\n"
    "}\n";

static const char *vio_2d_hlsl_ps_shapes =
    "struct PS_IN { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
    "float4 main(PS_IN i) : SV_TARGET { return vio_out(i.col); }\n";

static const char *vio_2d_hlsl_ps_sprites =
    "Texture2D    uTexture : register(t0);\n"
    "SamplerState uSampler : register(s0);\n"
    "struct PS_IN { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
    "float4 main(PS_IN i) : SV_TARGET {\n"
    "    return vio_out(uTexture.Sample(uSampler, i.uv) * i.col);\n"
    "}\n";

/* Glyph atlas is an R8 coverage texture (single channel). Sample .r as alpha
 * and take RGB straight from the vertex colour — the white-RGB/coverage-alpha
 * RGBA8 expansion is no longer needed, so the atlas uploads at 1/4 the size. */
static const char *vio_2d_hlsl_ps_text =
    "Texture2D    uTexture : register(t0);\n"
    "SamplerState uSampler : register(s0);\n"
    "struct PS_IN { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
    "float4 main(PS_IN i) : SV_TARGET {\n"
    "    float a = uTexture.Sample(uSampler, i.uv).r;\n"
    "    return vio_out(float4(i.col.rgb, i.col.a * a));\n"
    "}\n";

/* ── Vulkan GLSL variants (explicit bindings → SPIR-V) ───────────────
 *
 * The shared GLSL above relies on the glslang AUTO_MAP_BINDINGS +
 * VULKAN_RULES_RELAXED path, which assigns binding numbers implicitly. For
 * Vulkan that is fragile: a binding mismatch is a SILENT black screen, not a
 * validation error. These variants pin the projection to a vertex-stage
 * push constant (64-byte mat4) and the sprite sampler to set=0, binding=0 so
 * the pipeline layout is deterministic and shared by both pipelines.
 *
 * NOTE: SPIR-V's clip space has Y pointing down (opposite of OpenGL). The
 * orthographic projection produced by vio_2d_ortho already maps screen-space
 * Y-down to NDC the same way it does for D3D, so no Y flip is needed here. */

static const char *vio_2d_vk_vs =
    "#version 450\n"
    "layout(location = 0) in vec2 aPosition;\n"
    "layout(location = 1) in vec2 aTexCoord;\n"
    "layout(location = 2) in vec4 aColor;\n"
    "layout(push_constant) uniform PushConstants { mat4 uProjection; vec4 uOutput; } pc;\n"
    "layout(location = 0) out vec2 vTexCoord;\n"
    "layout(location = 1) out vec4 vColor;\n"
    "void main() {\n"
    "    gl_Position = pc.uProjection * vec4(aPosition, 0.0, 1.0);\n"
    "    gl_Position.y = -gl_Position.y; // Vulkan clip-space Y points down (NDC y=-1 at top); flip to match the GL/D3D-oriented 2D projection. CULL_MODE_NONE, so the winding flip is irrelevant.\n"
    "    vTexCoord = aTexCoord;\n"
    "    vColor = aColor;\n"
    "}\n";

/* uOutput.x = 1 when the swapchain is HDR10 (PQ-encode, GAP-PHASE5 Block 10d), uOutput.y = paper white in nits. */
#define VIO_2D_VK_PQ \
    "layout(push_constant) uniform PushConstants { mat4 uProjection; vec4 uOutput; } pc;\n" \
    "vec3 vio_pq(vec3 srgb, float nits) {\n" \
    "    vec3 lin = pow(max(srgb, 0.0), vec3(2.2));\n" \
    "    vec3 bt2020 = vec3(dot(lin, vec3(0.6274, 0.3293, 0.0433)), dot(lin, vec3(0.0691, 0.9195, 0.0114)), dot(lin, vec3(0.0164, 0.0880, 0.8956)));\n" \
    "    vec3 y = pow(max(bt2020 * (nits / 10000.0), 0.0), vec3(0.1593017578125));\n" \
    "    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));\n" \
    "}\n" \
    "vec4 vio_out(vec4 c) { if (pc.uOutput.x > 0.5) c.rgb = vio_pq(c.rgb, pc.uOutput.y > 0.0 ? pc.uOutput.y : 200.0); return c; }\n"

static const char *vio_2d_vk_fs_shapes =
    "#version 450\n"
    "layout(location = 0) in vec2 vTexCoord;\n"
    "layout(location = 1) in vec4 vColor;\n"
    "layout(location = 0) out vec4 FragColor;\n"
    VIO_2D_VK_PQ
    "void main() {\n"
    "    FragColor = vio_out(vColor);\n"
    "}\n";

static const char *vio_2d_vk_fs_sprites =
    "#version 450\n"
    "layout(location = 0) in vec2 vTexCoord;\n"
    "layout(location = 1) in vec4 vColor;\n"
    "layout(location = 0) out vec4 FragColor;\n"
    "layout(set = 0, binding = 0) uniform sampler2D uTexture;\n"
    VIO_2D_VK_PQ
    "void main() {\n"
    "    FragColor = vio_out(texture(uTexture, vTexCoord) * vColor);\n"
    "}\n";

/* ── Metal Shading Language (MSL) equivalents ────────────────────── */

#ifdef HAVE_METAL

static const char *vio_2d_metal_shader_source =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct VertexIn {\n"
    "    float2 position [[attribute(0)]];\n"
    "    float2 texcoord [[attribute(1)]];\n"
    "    float4 color    [[attribute(2)]];\n"
    "};\n"
    "\n"
    "struct VertexOut {\n"
    "    float4 position [[position]];\n"
    "    float2 texcoord;\n"
    "    float4 color;\n"
    "};\n"
    "\n"
    "struct Uniforms {\n"
    "    float4x4 projection;\n"
    "};\n"
    "\n"
    "vertex VertexOut vio_2d_vertex_main(\n"
    "    VertexIn in [[stage_in]],\n"
    "    constant Uniforms &uniforms [[buffer(1)]])\n"
    "{\n"
    "    VertexOut out;\n"
    "    out.position = uniforms.projection * float4(in.position, 0.0, 1.0);\n"
    "    out.texcoord = in.texcoord;\n"
    "    out.color = in.color;\n"
    "    return out;\n"
    "}\n"
    "\n"
    "fragment float4 vio_2d_fragment_shapes(VertexOut in [[stage_in]])\n"
    "{\n"
    "    return in.color;\n"
    "}\n"
    "\n"
    "fragment float4 vio_2d_fragment_sprites(\n"
    "    VertexOut in [[stage_in]],\n"
    "    texture2d<float> tex [[texture(0)]],\n"
    "    sampler s [[sampler(0)]])\n"
    "{\n"
    "    return tex.sample(s, in.texcoord) * in.color;\n"
    "}\n";

#endif /* HAVE_METAL */

#endif /* VIO_SHADERS_2D_H */
