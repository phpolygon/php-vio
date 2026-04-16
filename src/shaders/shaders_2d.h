/*
 * php-vio - 2D embedded shaders
 * OpenGL 4.1 Core Profile GLSL 410
 */

#ifndef VIO_SHADERS_2D_H
#define VIO_SHADERS_2D_H

/* Shape shader: position + color, with orthographic projection uniform */
static const char *vio_2d_vertex_shader =
    "#version 410 core\n"
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
    "#version 410 core\n"
    "in vec2 vTexCoord;\n"
    "in vec4 vColor;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    FragColor = vColor;\n"
    "}\n";

/* Sprite/text fragment shader: samples texture * vertex color */
static const char *vio_2d_fragment_shader_sprites =
    "#version 410 core\n"
    "in vec2 vTexCoord;\n"
    "in vec4 vColor;\n"
    "out vec4 FragColor;\n"
    "uniform sampler2D uTexture;\n"
    "void main() {\n"
    "    FragColor = texture(uTexture, vTexCoord) * vColor;\n"
    "}\n";

/* ── HLSL versions (D3D11 SM 5.0 / D3D12 SM 5.1) ──────────────── */

static const char *vio_2d_hlsl_vs =
    "cbuffer CB : register(b0) { float4x4 uProjection; };\n"
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
    "float4 main(PS_IN i) : SV_TARGET { return i.col; }\n";

static const char *vio_2d_hlsl_ps_sprites =
    "Texture2D    uTexture : register(t0);\n"
    "SamplerState uSampler : register(s0);\n"
    "struct PS_IN { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
    "float4 main(PS_IN i) : SV_TARGET {\n"
    "    return uTexture.Sample(uSampler, i.uv) * i.col;\n"
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
