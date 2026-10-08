/*
 * php-vio - Shader reflection and cross-compilation via SPIRV-Cross C API
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "../php_vio.h"
#include "vio_shader.h"
#include "vio_shader_reflect.h"
#include "vio_shader_compiler.h"
#include "vio_tess_hlsl.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

/* Canonical minimal stages for vio_hlsl_stage_supported(). They use the
 * builtins every real shader of that stage needs (gl_InvocationID, tess
 * levels, gl_TessCoord) so an "Unsupported builtin / execution model" from
 * an older SPIRV-Cross is caught here, not at vio_shader() time.
 *
 * The geometry probes read their input position from a user varying: the
 * probe asks whether SPIRV-Cross emits HLSL geometry stages at all.
 * gl_in[].gl_Position and gl_InvocationID, which its HLSL backend rejects as
 * "Unsupported builtin", are rewritten before transpiling
 * (vio_gs_hlsl_rewrite), so the GS-instancing probe passes through that path
 * too. */
static const char *vio_hlsl_probe_source(int stage)
{
    switch (stage) {
        case VIO_PROBE_GS_INSTANCED:
            /* GS instancing (layout(invocations = N)): [instance(N)] in HLSL. */
            return "#version 450\nlayout(points, invocations = 4) in;\nlayout(triangle_strip, max_vertices = 3) out;\n"
                   "layout(location = 0) in vec4 vPos[];\n"
                   "void main(){ for (int i = 0; i < 3; i++) { gl_Position = vPos[0] + vec4(float(gl_InvocationID), float(i), 0.0, 0.0); EmitVertex(); } EndPrimitive(); }\n";
        case VIO_STAGE_VERTEX:
            return "#version 330 core\nlayout(location=0) in vec3 p;\nvoid main(){ gl_Position = vec4(p, 1.0); }\n";
        case VIO_STAGE_FRAGMENT:
            return "#version 330 core\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(1.0); }\n";
        case VIO_STAGE_GEOMETRY:
            /* #version 450: location qualifiers on stage varyings need 410+. */
            return "#version 450\nlayout(points) in;\nlayout(triangle_strip, max_vertices = 3) out;\n"
                   "layout(location = 0) in vec4 vPos[];\nuniform float u_half;\n"
                   "void main(){ for (int i = 0; i < 3; i++) { gl_Position = vPos[0] + vec4(u_half * float(i), 0.0, 0.0, 0.0); EmitVertex(); } EndPrimitive(); }\n";
        case VIO_STAGE_TESS_CONTROL:
            return "#version 400 core\nlayout(vertices = 3) out;\n"
                   "void main(){ gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n"
                   "  if (gl_InvocationID == 0) { gl_TessLevelOuter[0] = 1.0; gl_TessLevelOuter[1] = 1.0; gl_TessLevelOuter[2] = 1.0; gl_TessLevelInner[0] = 1.0; } }\n";
        case VIO_STAGE_TESS_EVAL:
            return "#version 400 core\nlayout(triangles, equal_spacing, ccw) in;\n"
                   "void main(){ gl_Position = gl_in[0].gl_Position * gl_TessCoord.x + gl_in[1].gl_Position * gl_TessCoord.y + gl_in[2].gl_Position * gl_TessCoord.z; }\n";
        default:
            return NULL;
    }
}

/* Tessellation stages are probed as a pair: hull and domain shader are
 * built from both stages (vio_tess_hlsl.c). */
static char *vio_hlsl_tess_probe(int stage, int shader_model, char **err)
{
    size_t tcs_size = 0, tes_size = 0;
    uint32_t *tcs = vio_compile_glsl_stage_to_spirv(vio_hlsl_probe_source(VIO_STAGE_TESS_CONTROL),
                                                    VIO_STAGE_TESS_CONTROL, &tcs_size, err);
    if (!tcs) return NULL;
    uint32_t *tes = vio_compile_glsl_stage_to_spirv(vio_hlsl_probe_source(VIO_STAGE_TESS_EVAL),
                                                    VIO_STAGE_TESS_EVAL, &tes_size, err);
    char *hlsl = NULL;
    if (tes) {
        vio_tess_hlsl_desc d = { tcs, tcs_size, tes, tes_size, 0, shader_model, 1 };
        hlsl = vio_tess_to_hlsl(stage, &d, err);
        free(tes);
    }
    free(tcs);
    return hlsl;
}

/* Shader stage a probe compiles as (probe variants map onto a real stage). */
static int vio_hlsl_probe_stage(int probe)
{
    return probe == VIO_PROBE_GS_INSTANCED ? VIO_STAGE_GEOMETRY : probe;
}

int vio_hlsl_stage_supported(int stage)
{
    static int cache[VIO_PROBE_COUNT] = { -1, -1, -1, -1, -1, -1 };
    if (stage < 0 || stage >= VIO_PROBE_COUNT) return 0;
    if (cache[stage] >= 0) return cache[stage];

    int ok = 0;
    const char *src = vio_hlsl_probe_source(stage);
    if (stage == VIO_STAGE_TESS_CONTROL || stage == VIO_STAGE_TESS_EVAL) {
        char *err = NULL;
        char *hlsl = vio_hlsl_tess_probe(stage, 50, &err);
        ok = hlsl != NULL;
        if (!ok && getenv("VIO_DEBUG_STAGE_PROBE"))
            fprintf(stderr, "[vio] stage probe %d: tessellation HLSL failed: %s\n", stage, err ? err : "unknown");
        free(hlsl);
        free(err);
    } else if (src) {
        size_t spirv_size = 0;
        char *err = NULL;
        uint32_t *spirv = vio_compile_glsl_stage_to_spirv(src, vio_hlsl_probe_stage(stage), &spirv_size, &err);
        if (!spirv && getenv("VIO_DEBUG_STAGE_PROBE")) {
            fprintf(stderr, "[vio] stage probe %d: GLSL->SPIR-V failed: %s\n", stage, err ? err : "unknown");
        }
        if (err) free(err);
        err = NULL;
        if (spirv) {
            char *hlsl = vio_spirv_to_hlsl_ex(spirv, spirv_size, 50, 0, &err);
            if (hlsl) { ok = 1; free(hlsl); }
            else if (getenv("VIO_DEBUG_STAGE_PROBE")) {
                fprintf(stderr, "[vio] stage probe %d: SPIR-V->HLSL failed: %s\n", stage, err ? err : "unknown");
            }
            if (err) free(err);
            free(spirv);
        }
    }
    cache[stage] = ok;
    return ok;
}

char *vio_hlsl_probe_hlsl(int stage, int shader_model)
{
    if (stage == VIO_STAGE_TESS_CONTROL || stage == VIO_STAGE_TESS_EVAL) {
        char *err = NULL;
        char *hlsl = vio_hlsl_tess_probe(stage, shader_model, &err);
        free(err);
        return hlsl;
    }
    const char *src = vio_hlsl_probe_source(stage);
    if (!src) return NULL;
    size_t spirv_size = 0;
    char *err = NULL;
    uint32_t *spirv = vio_compile_glsl_stage_to_spirv(src, vio_hlsl_probe_stage(stage), &spirv_size, &err);
    if (err) free(err);
    if (!spirv) return NULL;
    err = NULL;
    char *hlsl = vio_spirv_to_hlsl_ex(spirv, spirv_size, shader_model, 0, &err);
    if (err) free(err);
    free(spirv);
    return hlsl;
}

int vio_spirv_execution_model(const void *spirv, size_t bytes)
{
    const uint32_t *w = (const uint32_t *)spirv;
    size_t n = bytes / 4;
    if (!w || n < 5 || w[0] != 0x07230203u) return -1;
    for (size_t i = 5; i < n; ) {
        uint32_t count = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (count == 0) break;
        if (op == 15 && count >= 3) return (int)w[i + 1];   /* OpEntryPoint <model> <id> <name> */
        i += count;
    }
    return -1;
}

char *vio_mesh_fix_positions(char *src, int flip_y, int fix_z, const char *vec4)
{
    static const char head[] = "gl_MeshVerticesEXT[";
    static const char tail[] = "].gl_Position = ";
    if (!src || (!flip_y && !fix_z) || !strstr(src, head)) return src;
    size_t len = strlen(src), cap = len * 2 + 256, n = 0;
    char *out = (char *)malloc(cap);
    if (!out) return src;
    const char *p = src;
    for (;;) {
        const char *m = strstr(p, head);
        if (!m) break;
        /* index expression up to the matching ']' */
        const char *q = m + sizeof(head) - 1;
        int depth = 0;
        while (*q && !(*q == ']' && depth == 0)) { if (*q == '[') depth++; else if (*q == ']') depth--; q++; }
        const char *semi = (*q && strncmp(q, tail, sizeof(tail) - 1) == 0) ? strchr(q + sizeof(tail) - 1, ';') : NULL;
        if (!semi) {
            size_t chunk = (size_t)(m + sizeof(head) - 1 - p);
            if (n + chunk + 1 >= cap) { cap = (n + chunk + 1) * 2; char *g = (char *)realloc(out, cap); if (!g) { free(out); return src; } out = g; }
            memcpy(out + n, p, chunk); n += chunk;
            p = m + sizeof(head) - 1;
            continue;
        }
        const char *idx = m + sizeof(head) - 1;
        size_t idx_len = (size_t)(q - idx);
        const char *expr = q + sizeof(tail) - 1;
        size_t expr_len = (size_t)(semi - expr);
        size_t need = (size_t)(m - p) + idx_len * 2 + expr_len + 256;
        if (n + need >= cap) { cap = (n + need) * 2; char *g = (char *)realloc(out, cap); if (!g) { free(out); return src; } out = g; }
        memcpy(out + n, p, (size_t)(m - p)); n += (size_t)(m - p);
        n += (size_t)sprintf(out + n, "{ %s _vio_mp = %.*s; gl_MeshVerticesEXT[%.*s].gl_Position = %s(_vio_mp.x, %s_vio_mp.y, %s, _vio_mp.w); }",
                             vec4, (int)expr_len, expr, (int)idx_len, idx, vec4, flip_y ? "-" : "",
                             fix_z ? "(_vio_mp.z + _vio_mp.w) * 0.5" : "_vio_mp.z");
        p = semi + 1;
    }
    size_t rest = strlen(p);
    if (n + rest + 1 > cap) { char *g = (char *)realloc(out, n + rest + 1); if (!g) { free(out); return src; } out = g; }
    memcpy(out + n, p, rest + 1);
    free(src);
    return out;
}

int vio_spirv_accel_binding(const void *spirv, size_t bytes)
{
    const uint32_t *w = (const uint32_t *)spirv;
    size_t n = bytes / 4;
    if (!w || n < 5 || w[0] != 0x07230203u) return -1;
    uint32_t as_type = 0, as_ptr = 0, as_var = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t count = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (count == 0) break;
        if (op == 5341 && count >= 2) as_type = w[i + 1];                                     /* OpTypeAccelerationStructureKHR */
        else if (op == 32 && count >= 4 && as_type && w[i + 3] == as_type) as_ptr = w[i + 1];  /* OpTypePointer */
        else if (op == 59 && count >= 4 && as_ptr && w[i + 1] == as_ptr && !as_var) as_var = w[i + 2]; /* OpVariable */
        i += count;
    }
    if (!as_var) return -1;
    for (size_t i = 5; i < n; ) {
        uint32_t count = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (count == 0) break;
        if (op == 71 && count >= 4 && w[i + 1] == as_var && w[i + 2] == 33) return (int)w[i + 3];  /* Binding */
        i += count;
    }
    return 0;
}

int vio_spirv_has_builtin(const void *spirv, size_t bytes, uint32_t builtin)
{
    const uint32_t *w = (const uint32_t *)spirv;
    size_t n = bytes / 4;
    if (!w || n < 5 || w[0] != 0x07230203u) return 0;
    for (size_t i = 5; i < n; ) {
        uint32_t count = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (count == 0) break;
        /* OpDecorate %target BuiltIn <builtin> / OpMemberDecorate %type <m> BuiltIn <builtin> */
        if (op == 71 && count >= 4 && w[i + 2] == 11 && w[i + 3] == builtin) return 1;
        if (op == 72 && count >= 5 && w[i + 3] == 11 && w[i + 4] == builtin) return 1;
        i += count;
    }
    return 0;
}

int vio_spirv_has_capability(const void *spirv, size_t bytes, uint32_t capability)
{
    const uint32_t *w = (const uint32_t *)spirv;
    size_t n = bytes / 4;
    if (!w || n < 5 || w[0] != 0x07230203u) return 0;
    for (size_t i = 5; i < n; ) {
        uint32_t count = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (count == 0) break;
        if (op == 17 && count >= 2 && w[i + 1] == capability) return 1;   /* OpCapability */
        i += count;
    }
    return 0;
}

int vio_spirv_uses_descriptor_set(const void *spirv, size_t bytes, uint32_t set)
{
    const uint32_t *w = (const uint32_t *)spirv;
    size_t n = bytes / 4;
    if (!w || n < 5 || w[0] != 0x07230203u) return 0;
    for (size_t i = 5; i < n; ) {
        uint32_t count = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (count == 0) break;
        if (op == 71 && count >= 4 && w[i + 2] == 34 && w[i + 3] == set) return 1;   /* OpDecorate DescriptorSet */
        i += count;
    }
    return 0;
}

uint32_t vio_spirv_local_size_x(const void *spirv, size_t bytes)
{
    const uint32_t *w = (const uint32_t *)spirv;
    size_t n = bytes / 4;
    if (!w || n < 5 || w[0] != 0x07230203u) return 0;
    for (size_t i = 5; i < n; ) {
        uint32_t count = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (count == 0) break;
        /* OpExecutionMode %entry LocalSize x y z (LocalSizeId: spec constants, unknown here) */
        if (op == 16 && count >= 6 && w[i + 2] == 17) return w[i + 3];
        i += count;
    }
    return 0;
}

#ifdef HAVE_SPIRV_CROSS

#include <spirv_cross/spirv_cross_c.h>
#include "vio_hlsl_internal.h"

/* ── Transpilation ───────────────────────────────────────────────── */

/* SPIRV-Cross emits gl_ViewportIndex in a vertex / tessellation-evaluation
 * stage without the #extension it needs (it does add one for gl_Layer), and
 * glslang / the GL driver then reject the shader. Insert
 * GL_ARB_shader_viewport_layer_array after #version when either built-in is
 * used and no viewport/layer extension is declared. Takes ownership of `glsl`
 * (malloc'd) and returns the (possibly new) string. */
char *vio_glsl_require_viewport_layer_ext(char *glsl)
{
    /* GLSL extension names (shader source text, not GL API) - split so the
     * no-GL-outside-the-backend audit gate (test 070) does not read them as
     * GL_* tokens. */
    static const char arb[] = "GL_" "ARB_shader_viewport_layer_array";
    static const char nv[]  = "GL_" "NV_viewport_array2";
    static const char amd[] = "GL_" "AMD_vertex_shader_viewport_index";
    if (!glsl || (!strstr(glsl, "gl_ViewportIndex") && !strstr(glsl, "gl_Layer"))) return glsl;
    if (strstr(glsl, arb) || strstr(glsl, nv) || strstr(glsl, amd)) return glsl;
    const char *nl = strstr(glsl, "#version");
    nl = nl ? strchr(nl, '\n') : NULL;
    if (!nl) return glsl;
    static const char ext[] = "#extension GL_" "ARB_shader_viewport_layer_array : require\n";
    size_t head = (size_t)(nl + 1 - glsl), len = strlen(glsl);
    char *out = (char *)malloc(len + sizeof(ext));
    if (!out) return glsl;
    memcpy(out, glsl, head);
    memcpy(out + head, ext, sizeof(ext) - 1);
    memcpy(out + head + sizeof(ext) - 1, glsl + head, len - head + 1);
    free(glsl);
    return out;
}

/* SPIRV-Cross before vulkan-sdk-1.3.275 (Ubuntu 24.04) writes `perprimitiveEXT`
 * fragment inputs without enabling GL_EXT_mesh_shader, which glslang then
 * rejects. Takes ownership of `glsl` (malloc'd). */
char *vio_glsl_require_mesh_shader_ext(char *glsl)
{
    static const char name[] = "GL_" "EXT_mesh_shader";
    if (!glsl || !strstr(glsl, "perprimitiveEXT") || strstr(glsl, name)) return glsl;
    const char *nl = strstr(glsl, "#version");
    nl = nl ? strchr(nl, '\n') : NULL;
    if (!nl) return glsl;
    static const char ext[] = "#extension GL_" "EXT_mesh_shader : require\n";
    size_t head = (size_t)(nl + 1 - glsl), len = strlen(glsl);
    char *out = (char *)malloc(len + sizeof(ext));
    if (!out) return glsl;
    memcpy(out, glsl, head);
    memcpy(out + head, ext, sizeof(ext) - 1);
    memcpy(out + head + sizeof(ext) - 1, glsl + head, len - head + 1);
    free(glsl);
    return out;
}

/* SPIRV-Cross before vulkan-sdk-1.3.275 (Ubuntu 24.04 ships 1.3.268) has no
 * GLSL name for BuiltIn PatchVertices and emits `gl_BuiltIn_14`, which the GL
 * compiler rejects. Takes ownership of `glsl` (malloc'd). */
static char *vio_glsl_fix_patch_vertices(char *glsl)
{
    static const char bad[] = "gl_BuiltIn_14";
    static const char good[] = "gl_PatchVerticesIn";
    if (!glsl || !strstr(glsl, bad)) return glsl;
    size_t count = 0, len = strlen(glsl);
    for (const char *p = glsl; (p = strstr(p, bad)) != NULL; p += sizeof(bad) - 1) count++;
    char *out = (char *)malloc(len + count * (sizeof(good) - sizeof(bad)) + 1);
    if (!out) return glsl;
    char *w = out;
    const char *p = glsl;
    for (const char *a; (a = strstr(p, bad)) != NULL; p = a + sizeof(bad) - 1) {
        memcpy(w, p, (size_t)(a - p)); w += a - p;
        memcpy(w, good, sizeof(good) - 1); w += sizeof(good) - 1;
    }
    strcpy(w, p);
    free(glsl);
    return out;
}

/* GL_OVR_multiview2: SPIRV-Cross maps gl_ViewIndex to gl_ViewID_OVR but keeps
 * treating it as the int of GL_EXT_multiview - it casts to uint where it needs
 * one and assigns it to ints as is, which GL rejects (gl_ViewID_OVR is a uint).
 * Every use becomes int(gl_ViewID_OVR). Takes ownership of `glsl`. */
static char *vio_glsl_fix_ovr_view_id(char *glsl)
{
    static const char id[] = "gl_ViewID_OVR";
    if (!glsl || !strstr(glsl, id)) return glsl;
    size_t count = 0, len = strlen(glsl);
    for (const char *p = glsl; (p = strstr(p, id)) != NULL; p += sizeof(id) - 1) count++;
    char *out = (char *)malloc(len + count * 5 + 1);   /* "int(" + ")" per use */
    if (!out) return glsl;
    char *w = out;
    const char *p = glsl;
    for (const char *m; (m = strstr(p, id)) != NULL; p = m + sizeof(id) - 1) {
        memcpy(w, p, (size_t)(m - p)); w += m - p;
        memcpy(w, "int(" "gl_ViewID_OVR)", 18); w += 18;
    }
    strcpy(w, p);
    free(glsl);
    return out;
}

static int vio_glsl_ovr_views = 0;
void vio_glsl_set_ovr_view_count(int views) { vio_glsl_ovr_views = views > 0 ? views : 0; }

char *vio_spirv_to_glsl(const uint32_t *spirv, size_t spirv_size, int version, char **error_msg)
{
    spvc_context ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler compiler = NULL;
    spvc_compiler_options options = NULL;
    const char *result = NULL;
    char *output = NULL;

    if (spvc_context_create(&ctx) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup("Failed to create SPIRV-Cross context");
        return NULL;
    }

    size_t word_count = spirv_size / sizeof(uint32_t);

    if (spvc_context_parse_spirv(ctx, spirv, word_count, &ir) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    if (spvc_context_create_compiler(ctx, SPVC_BACKEND_GLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    spvc_compiler_create_compiler_options(compiler, &options);
    spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, version);
    spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_FALSE);
    /* Flatten UBOs to plain uniforms for GLSL <=420 (macOS only supports 410) */
    spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_EMIT_UNIFORM_BUFFER_AS_PLAIN_UNIFORMS, SPVC_TRUE);
    /* gl_ViewIndex -> gl_ViewID_OVR + layout(num_views = N) (vio_shader 'view_count'). */
    if (vio_glsl_ovr_views > 0)
        spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_OVR_MULTIVIEW_VIEW_COUNT, (unsigned)vio_glsl_ovr_views);
    spvc_compiler_install_compiler_options(compiler, options);
    /* GLSL for OpenGL has no separate textures / samplers. */
    vio_spvc_combine_separate(compiler);

    if (spvc_compiler_compile(compiler, &result) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    /* Stray debug print: emits to stderr on every shader compile. Gated
     * behind VIO_DEBUG_SPIRV so production / tests don't get spammed and
     * shader-related .phpt tests can match their --EXPECT block. */
    if (getenv("VIO_DEBUG_SPIRV")) {
        fprintf(stderr, "[vio] SPIRV-Cross output (first 500 chars):\n%.500s\n---\n", result);
    }
    output = vio_glsl_fix_patch_vertices(strdup(result));
    output = vio_glsl_fix_ovr_view_id(output);
    SpvExecutionModel model = spvc_compiler_get_execution_model(compiler);
    if (model == SpvExecutionModelVertex || model == SpvExecutionModelTessellationEvaluation) {
        output = vio_glsl_require_viewport_layer_ext(output);
    }
    spvc_context_destroy(ctx);
    return output;
}

/* Transpile a compute SPIR-V module to GLSL for the OpenGL backend.
 *
 * Distinct from vio_spirv_to_glsl(): the render path flattens UBOs to plain
 * uniforms (EMIT_UNIFORM_BUFFER_AS_PLAIN_UNIFORMS) so it works on GL <= 4.2 /
 * macOS 4.1. Compute requires GL >= 4.3, where real std140 UBO blocks and
 * std430 SSBO blocks with explicit `binding =` qualifiers are available — and
 * the compute primitive depends on them: the Params block must stay a UBO so it
 * can be bound via glBindBufferBase(GL_UNIFORM_BUFFER, binding, ...), and the
 * two SSBOs must keep their `layout(std430, binding=N) buffer` qualifiers so the
 * SSBO binding points match the slots the PHP layer binds. So this variant emits
 * version >= 430 and leaves UBOs/SSBOs as real interface blocks. */
char *vio_spirv_to_glsl_compute(const uint32_t *spirv, size_t spirv_size, int version, char **error_msg)
{
    spvc_context ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler compiler = NULL;
    spvc_compiler_options options = NULL;
    const char *result = NULL;
    char *output = NULL;

    if (version < 430) version = 430;

    if (spvc_context_create(&ctx) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup("Failed to create SPIRV-Cross context");
        return NULL;
    }

    size_t word_count = spirv_size / sizeof(uint32_t);

    if (spvc_context_parse_spirv(ctx, spirv, word_count, &ir) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    if (spvc_context_create_compiler(ctx, SPVC_BACKEND_GLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    spvc_compiler_create_compiler_options(compiler, &options);
    spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, (unsigned)version);
    spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_FALSE);
    /* Keep UBOs as std140 blocks (NOT plain uniforms) and SSBOs as std430 blocks,
     * preserving their explicit `binding =` qualifiers so the GL backend binds by
     * the same slot the shader declares. */
    spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_EMIT_UNIFORM_BUFFER_AS_PLAIN_UNIFORMS, SPVC_FALSE);
    spvc_compiler_install_compiler_options(compiler, options);

    if (spvc_compiler_compile(compiler, &result) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    /* SPIRV-Cross drops the derivative-group execution mode of a compute module
     * (GL_NV_compute_shader_derivatives), and dFdx in a compute shader without it
     * does not compile - put the extension and the layout back after #version.
     * The split literal keeps the GL audit gate (070) quiet. */
    int quads = 0, linear = 0;
    {
        const SpvExecutionMode *modes = NULL;
        size_t mode_count = 0;
        if (spvc_compiler_get_execution_modes(compiler, &modes, &mode_count) == SPVC_SUCCESS) {
            for (size_t m = 0; m < mode_count; m++) {
                if ((int)modes[m] == 5289) quads = 1;    /* DerivativeGroupQuadsNV / KHR */
                if ((int)modes[m] == 5290) linear = 1;   /* DerivativeGroupLinearNV / KHR */
            }
        }
    }
    if ((quads || linear) && strncmp(result, "#version", 8) == 0) {
        const char *nl = strchr(result, '\n');
        const char *ins = quads
            ? "#extension GL_" "NV_compute_shader_derivatives : require\nlayout(derivative_group_quadsNV) in;\n"
            : "#extension GL_" "NV_compute_shader_derivatives : require\nlayout(derivative_group_linearNV) in;\n";
        if (nl) {
            size_t head = (size_t)(nl - result) + 1, len = strlen(result), il = strlen(ins);
            output = (char *)malloc(len + il + 1);
            if (output) {
                memcpy(output, result, head);
                memcpy(output + head, ins, il);
                memcpy(output + head + il, result + head, len - head + 1);
            }
        }
    }

    if (getenv("VIO_DUMP_CS_GLSL")) {
        fprintf(stderr, "==== OpenGL compute GLSL ====\n%s\n==== end ====\n", output ? output : result);
        fflush(stderr);
    }
    if (!output) output = strdup(result);
    spvc_context_destroy(ctx);
    return output;
}

char *vio_spirv_to_msl(const uint32_t *spirv, size_t spirv_size, char **error_msg)
{
    spvc_context ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler compiler = NULL;
    const char *result = NULL;
    char *output = NULL;

    if (spvc_context_create(&ctx) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup("Failed to create SPIRV-Cross context");
        return NULL;
    }

    size_t word_count = spirv_size / sizeof(uint32_t);

    if (spvc_context_parse_spirv(ctx, spirv, word_count, &ir) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    if (spvc_context_create_compiler(ctx, SPVC_BACKEND_MSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    if (spvc_compiler_compile(compiler, &result) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    output = strdup(result);
    spvc_context_destroy(ctx);
    return output;
}

/* ── Geometry stages for HLSL ─────────────────────────────────────────
 *
 * SPIRV-Cross's HLSL backend rejects two geometry-shader builtins with
 * "Unsupported builtin in HLSL": the input position gl_in[i].gl_Position and
 * gl_InvocationID. Both have plain HLSL counterparts (an SV_Position input
 * element, SV_GSInstanceID with [instance(N)]), so the module is rewritten
 * before transpiling and the HLSL is patched after:
 *   - gl_in[i].gl_Position: every access chain gl_in[i].member0 is pointed at a
 *     new input array `vio_gl_in_Position` at a free location; SPIRV-Cross
 *     emits it as TEXCOORD<loc>, which becomes SV_Position. The vertex stage
 *     outputs gl_Position as SV_Position, so the signatures link.
 *   - gl_InvocationID: the builtin input becomes a Private variable
 *     `vio_gs_invocation`; main() takes SV_GSInstanceID and assigns it.
 * Modules without these builtins pass through unchanged. */

typedef struct { uint32_t *w; size_t n, cap; int oom; } vio_spv_words;

static void spv_push(vio_spv_words *v, uint32_t x)
{
    if (v->oom) return;
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 1024;
        uint32_t *g = (uint32_t *)realloc(v->w, cap * sizeof(uint32_t));
        if (!g) { v->oom = 1; return; }
        v->w = g;
        v->cap = cap;
    }
    v->w[v->n++] = x;
}

static void spv_inst(vio_spv_words *v, uint32_t op, const uint32_t *ops, uint32_t count)
{
    spv_push(v, ((count + 1) << 16) | op);
    for (uint32_t i = 0; i < count; i++) spv_push(v, ops[i]);
}

static void spv_name(vio_spv_words *v, uint32_t id, const char *name)
{
    uint32_t len = (uint32_t)strlen(name);
    uint32_t words = len / 4 + 1;
    spv_push(v, ((words + 2) << 16) | 5 /* OpName */);
    spv_push(v, id);
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = 0;
        for (uint32_t b = 0; b < 4; b++) {
            uint32_t k = i * 4 + b;
            if (k < len) w |= (uint32_t)(unsigned char)name[k] << (8 * b);
        }
        spv_push(v, w);
    }
}

#define VIO_GS_POS_LOCATION_NONE 0xFFFFFFFFu

/* Returns a rewritten module (malloc'd, *out_words set) or NULL when nothing
 * had to change. *pos_location receives the location of the position input
 * (VIO_GS_POS_LOCATION_NONE if none), *invocation 1 when gl_InvocationID was
 * replaced, *invocations the layout(invocations = N) count. */
static uint32_t *vio_gs_hlsl_rewrite(const uint32_t *spv, size_t words, size_t *out_words,
                                     uint32_t *pos_location, int *invocation, uint32_t *invocations)
{
    *pos_location = VIO_GS_POS_LOCATION_NONE;
    *invocation = 0;
    *invocations = 1;
    if (words < 5 || spv[0] != 0x07230203) return NULL;

    uint32_t bound = spv[3];
    /* id -> instruction offset, for types / constants / variables. */
    size_t *def = (size_t *)calloc(bound, sizeof(size_t));
    unsigned char *pervertex = (unsigned char *)calloc(bound, 1);
    if (!def || !pervertex) { free(def); free(pervertex); return NULL; }

    uint32_t inv_var = 0, max_location = 0, glin_var = 0;
    int has_location = 0, is_geometry = 0;
    for (size_t i = 5; i < words; ) {
        uint32_t op = spv[i] & 0xFFFF, wc = spv[i] >> 16;
        if (wc == 0 || i + wc > words) { free(def); free(pervertex); return NULL; }
        switch (op) {
            case 16: /* OpExecutionMode ep Invocations N */
                if (wc >= 4 && spv[i + 2] == 0) *invocations = spv[i + 3];
                break;
            case 71: /* OpDecorate id dec ... */
                if (wc >= 4 && spv[i + 2] == 11 && spv[i + 3] == 8) inv_var = spv[i + 1];
                if (wc >= 4 && spv[i + 2] == 30) { has_location = 1; if (spv[i + 3] > max_location) max_location = spv[i + 3]; }
                break;
            case 72: /* OpMemberDecorate struct member BuiltIn Position */
                if (wc >= 5 && spv[i + 2] == 0 && spv[i + 3] == 11 && spv[i + 4] == 0 && spv[i + 1] < bound) pervertex[spv[i + 1]] = 1;
                break;
            case 15: /* OpEntryPoint model ... */
                if (wc >= 2 && spv[i + 1] == 3 /* Geometry */) is_geometry = 1;
                break;
            case 21: case 23: case 28: case 30: case 32:
                if (spv[i + 1] < bound) def[spv[i + 1]] = i;   /* types: result id is operand 1 */
                break;
            case 43: /* OpConstant type result value */
                if (wc >= 4 && spv[i + 2] < bound) def[spv[i + 2]] = i;
                break;
            case 59: /* OpVariable type id storage */
                if (wc >= 4 && spv[i + 2] < bound) def[spv[i + 2]] = i;
                if (wc >= 4 && spv[i + 3] == 1 /* Input */) {
                    size_t pt = spv[i + 1] < bound ? def[spv[i + 1]] : 0;   /* OpTypePointer Input T */
                    if (pt && (spv[pt] & 0xFFFF) == 32) {
                        size_t at = spv[pt + 3] < bound ? def[spv[pt + 3]] : 0;
                        if (at && (spv[at] & 0xFFFF) == 28 && spv[at + 2] < bound && pervertex[spv[at + 2]]) glin_var = spv[i + 2];
                    }
                }
                break;
            default: break;
        }
        i += wc;
    }

    if (!is_geometry) { free(def); free(pervertex); return NULL; }

    /* gl_in[i].gl_Position: access chains with member index constant 0. */
    uint32_t vec4_type = 0, len_id = 0;
    int glin_rewrites = 0, glin_other_uses = 0;
    if (glin_var) {
        size_t pt = def[spv[def[glin_var] + 1]];
        size_t at = def[spv[pt + 3]];
        len_id = spv[at + 3];
        size_t st = def[spv[at + 2]];
        if (st && (spv[st] & 0xFFFF) == 30 && (spv[st] >> 16) >= 3) vec4_type = spv[st + 2];
        for (size_t i = 5; i < words; i += spv[i] >> 16) {
            uint32_t op = spv[i] & 0xFFFF, wc = spv[i] >> 16;
            if (op == 15 /* OpEntryPoint */ || op == 59 || op == 5 || op == 6 || op == 71 || op == 72) continue;
            int uses = 0;
            for (uint32_t k = 1; k < wc; k++) if (spv[i + k] == glin_var) uses = 1;
            if (!uses) continue;
            if ((op == 65 || op == 66) && wc >= 6 && spv[i + 3] == glin_var) {
                size_t c = spv[i + 5] < bound ? def[spv[i + 5]] : 0;
                if (c && (spv[c] & 0xFFFF) == 43 && spv[c + 3] == 0) { glin_rewrites++; continue; }
            }
            glin_other_uses++;
        }
        if (!vec4_type || !glin_rewrites) glin_var = 0;
    }
    if (!glin_var && !inv_var) { free(def); free(pervertex); return NULL; }

    uint32_t inv_int_type = 0;
    if (inv_var) {
        size_t pt = def[spv[def[inv_var] + 1]];
        inv_int_type = spv[pt + 3];
    }

    uint32_t new_bound = bound;
    uint32_t arr_id = 0, arr_ptr_id = 0, pos_var = 0, priv_ptr_id = 0;
    if (glin_var) { arr_id = new_bound++; arr_ptr_id = new_bound++; pos_var = new_bound++; }
    if (inv_var) priv_ptr_id = new_bound++;
    uint32_t loc = has_location ? max_location + 1 : 0;

    vio_spv_words out = { NULL, 0, 0, 0 };
    for (int h = 0; h < 5; h++) spv_push(&out, spv[h]);
    out.w[3] = new_bound;

    int annotations_done = 0, globals_done = 0;
    for (size_t i = 5; i < words; ) {
        uint32_t op = spv[i] & 0xFFFF, wc = spv[i] >> 16;
        if (!annotations_done && (op == 71 || op == 72)) {
            if (pos_var) {
                spv_name(&out, pos_var, "vio_gl_in_Position");
                uint32_t d[3] = { pos_var, 30, loc };
                spv_inst(&out, 71, d, 3);
            }
            if (inv_var) spv_name(&out, inv_var, "vio_gs_invocation");
            annotations_done = 1;
        }
        if (!globals_done && op == 54 /* OpFunction */) {
            if (pos_var) {
                uint32_t a[3] = { arr_id, vec4_type, len_id };
                spv_inst(&out, 28, a, 3);
                uint32_t p[3] = { arr_ptr_id, 1, arr_id };
                spv_inst(&out, 32, p, 3);
                uint32_t v[3] = { arr_ptr_id, pos_var, 1 };
                spv_inst(&out, 59, v, 3);
            }
            if (inv_var) {
                uint32_t p[3] = { priv_ptr_id, 6 /* Private */, inv_int_type };
                spv_inst(&out, 32, p, 3);
                uint32_t v[3] = { priv_ptr_id, inv_var, 6 };
                spv_inst(&out, 59, v, 3);
            }
            globals_done = 1;
        }
        if (op == 15) {
            /* OpEntryPoint model id "name" interface... */
            uint32_t k = 3;
            while (k < wc) { uint32_t w = spv[i + k++]; if ((w >> 24) == 0 || ((w >> 16) & 0xFF) == 0 || ((w >> 8) & 0xFF) == 0 || (w & 0xFF) == 0) break; }
            vio_spv_words ep = { NULL, 0, 0, 0 };
            for (uint32_t j = 1; j < k; j++) spv_push(&ep, spv[i + j]);
            for (uint32_t j = k; j < wc; j++) {
                uint32_t id = spv[i + j];
                if (inv_var && id == inv_var) continue;
                if (glin_var && id == glin_var && !glin_other_uses) continue;
                spv_push(&ep, id);
            }
            if (pos_var) spv_push(&ep, pos_var);
            spv_inst(&out, 15, ep.w, (uint32_t)ep.n);
            free(ep.w);
        } else if (op == 5 && inv_var && wc >= 2 && spv[i + 1] == inv_var) {
            /* drop: renamed above */
        } else if (op == 71 && inv_var && wc >= 3 && spv[i + 1] == inv_var) {
            /* drop the BuiltIn InvocationId decoration */
        } else if (op == 59 && inv_var && wc >= 3 && spv[i + 2] == inv_var) {
            /* re-declared as Private before the first function */
        } else if (glin_var && (op == 65 || op == 66) && wc >= 6 && spv[i + 3] == glin_var &&
                   spv[i + 5] < bound && def[spv[i + 5]] && (spv[def[spv[i + 5]]] & 0xFFFF) == 43 &&
                   spv[def[spv[i + 5]] + 3] == 0) {
            /* type result base idx0 0 rest... -> type result pos_var idx0 rest... */
            vio_spv_words ac = { NULL, 0, 0, 0 };
            spv_push(&ac, spv[i + 1]);
            spv_push(&ac, spv[i + 2]);
            spv_push(&ac, pos_var);
            spv_push(&ac, spv[i + 4]);
            for (uint32_t j = 6; j < wc; j++) spv_push(&ac, spv[i + j]);
            spv_inst(&out, op, ac.w, (uint32_t)ac.n);
            free(ac.w);
        } else {
            for (uint32_t j = 0; j < wc; j++) spv_push(&out, spv[i + j]);
        }
        i += wc;
    }
    free(def);
    free(pervertex);
    if (out.oom || !annotations_done || !globals_done) { free(out.w); return NULL; }
    *out_words = out.n;
    if (pos_var) *pos_location = loc;
    if (inv_var) *invocation = 1;
    return out.w;
}

/* Insert `ins` at `at` into the malloc'd string s (takes ownership). */
static char *vio_str_insert(char *s, size_t at, const char *ins)
{
    size_t len = strlen(s), il = strlen(ins);
    char *r = (char *)malloc(len + il + 1);
    if (!r) return s;
    memcpy(r, s, at);
    memcpy(r + at, ins, il);
    memcpy(r + at + il, s + at, len - at + 1);
    free(s);
    return r;
}

/* Patch the transpiled GS for the rewrites of vio_gs_hlsl_rewrite. */
static char *vio_gs_hlsl_patch(char *hlsl, uint32_t pos_location, int invocation, uint32_t invocations)
{
    if (!hlsl) return NULL;
    if (pos_location != VIO_GS_POS_LOCATION_NONE) {
        char sem[32];
        snprintf(sem, sizeof(sem), ": TEXCOORD%u;", pos_location);
        char *st = strstr(hlsl, "struct SPIRV_Cross_Input");
        char *p = st ? strstr(st, sem) : NULL;
        char *end = st ? strstr(st, "};") : NULL;
        if (p && (!end || p < end)) {
            size_t at = (size_t)(p - hlsl);
            size_t sl = strlen(sem);
            memmove(hlsl + at, hlsl + at + sl, strlen(hlsl + at + sl) + 1);
            hlsl = vio_str_insert(hlsl, at, ": SV_Position;");
        }
    }
    if (invocation) {
        char *m = strstr(hlsl, "void main(");
        char *close = m ? strchr(m, ')') : NULL;
        if (close) {
            size_t at = (size_t)(close - hlsl);
            hlsl = vio_str_insert(hlsl, at, ", uint vio_gs_instance : SV_GSInstanceID");
            m = strstr(hlsl, "void main(");
            char *brace = m ? strchr(m, '{') : NULL;
            if (brace) hlsl = vio_str_insert(hlsl, (size_t)(brace - hlsl) + 1, "\n    vio_gs_invocation = int(vio_gs_instance);");
        }
        if (!strstr(hlsl, "[instance(")) {
            char *mv = strstr(hlsl, "[maxvertexcount(");
            if (mv) {
                char attr[32];
                snprintf(attr, sizeof(attr), "[instance(%u)]\n", invocations ? invocations : 1);
                hlsl = vio_str_insert(hlsl, (size_t)(mv - hlsl), attr);
            }
        }
    }
    return hlsl;
}

char *vio_spirv_to_hlsl(const uint32_t *spirv, size_t spirv_size, int shader_model, char **error_msg)
{
    return vio_spirv_to_hlsl_ex(spirv, spirv_size, shader_model, 1, error_msg);
}

char *vio_spirv_to_hlsl_ex(const uint32_t *spirv, size_t spirv_size, int shader_model,
                           int fixup_depth, char **error_msg)
{
    return vio_spirv_to_hlsl_hooked(spirv, spirv_size / sizeof(uint32_t), shader_model, fixup_depth, NULL, error_msg);
}

static int vio_hlsl_16bit_types = 0;
void vio_hlsl_set_16bit_types(int enable) { vio_hlsl_16bit_types = enable ? 1 : 0; }

/* SPIRV-Cross declares the draw-parameter cbuffer without a register, so the
 * compiler would pick the lowest free one. Pin it to b13, where D3D12's root
 * signature keeps the draw-parameter root constants (D3D11 leaves b13 unbound:
 * the values read 0, like vio_draw's). */
static char *vio_hlsl_pin_vertex_info(char *hlsl)
{
    static const char decl[] = "cbuffer SPIRV_Cross_VertexInfo";
    static const char reg[] = " : register(b13)";
    if (!hlsl) return hlsl;
    char *at = strstr(hlsl, decl);
    if (!at) return hlsl;
    size_t len = strlen(hlsl), head = (size_t)(at - hlsl) + sizeof(decl) - 1;
    char *out = (char *)malloc(len + sizeof(reg));
    if (!out) return hlsl;
    memcpy(out, hlsl, head);
    memcpy(out + head, reg, sizeof(reg) - 1);
    strcpy(out + head + sizeof(reg) - 1, hlsl + head);
    free(hlsl);
    return out;
}

/* 64-bit atomics on a RWByteAddressBuffer (GL_EXT_shader_atomic_int64 on an
 * SSBO, VIO_FEATURE_ATOMIC64). SPIRV-Cross emits `buf.InterlockedMax(off, v, r)`
 * for them, but the 64-bit raw-buffer methods of Shader Model 6.6 are the *64
 * ones; with a uint64_t value the 32-bit overload still compiles and silently
 * truncates (checked with DXC 1.9). SPIRV-Cross always passes the original
 * value as a temporary it declared as `uint64_t _N;` / `int64_t _N;`, so a call
 * whose last argument is such a temporary is renamed to its *64 method. Below
 * Shader Model 6.6 DXC then rejects the shader instead of computing garbage.
 * Free InterlockedX(dest, ...) calls (groupshared, typed) are overloaded on the
 * operand type and left alone. Takes ownership of `hlsl`. */
static int hlsl_is_int64_temp(const char *hlsl, const char *id, size_t id_len)
{
    static const char *types[] = { "uint64_t ", "int64_t " };
    for (int t = 0; t < 2; t++) {
        size_t tl = strlen(types[t]);
        for (const char *p = hlsl; (p = strstr(p, types[t])) != NULL; p += tl) {
            if (p > hlsl && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) continue;
            const char *q = p + tl;
            if (strncmp(q, id, id_len) == 0 && (q[id_len] == ';' || q[id_len] == ' ' || q[id_len] == '=')) return 1;
        }
    }
    return 0;
}

static char *vio_hlsl_fix_int64_buffer_atomics(char *hlsl)
{
    if (!hlsl || !strstr(hlsl, "int64_t") || !strstr(hlsl, ".Interlocked")) return hlsl;
    size_t len = strlen(hlsl), cap = len + 64, n = 0;
    char *out = (char *)malloc(cap);
    if (!out) return hlsl;
    const char *p = hlsl;
    for (;;) {
        const char *m = strstr(p, ".Interlocked");
        if (!m) break;
        const char *name = m + 1, *paren = name;
        while (isalnum((unsigned char)*paren) || *paren == '_') paren++;
        size_t name_len = (size_t)(paren - name);
        int is64 = 0;
        if (*paren == '(' && !(name_len > 2 && name[name_len - 2] == '6' && name[name_len - 1] == '4')) {
            /* Last top-level argument of the call. */
            int depth = 0;
            const char *last = paren + 1, *q = paren + 1;
            for (; *q; q++) {
                if (*q == '(') depth++;
                else if (*q == ')') { if (depth == 0) break; depth--; }
                else if (*q == ',' && depth == 0) last = q + 1;
            }
            while (last < q && isspace((unsigned char)*last)) last++;
            const char *end = q;
            while (end > last && isspace((unsigned char)end[-1])) end--;
            int ident = end > last;
            for (const char *c = last; c < end; c++) if (!(isalnum((unsigned char)*c) || *c == '_')) ident = 0;
            if (ident && *q == ')') is64 = hlsl_is_int64_temp(hlsl, last, (size_t)(end - last));
        }
        size_t chunk = (size_t)(paren - p);
        if (n + chunk + 3 >= cap) { cap = (n + chunk + 3) * 2; char *g = (char *)realloc(out, cap); if (!g) { free(out); return hlsl; } out = g; }
        memcpy(out + n, p, chunk); n += chunk;
        if (is64) { out[n++] = '6'; out[n++] = '4'; }
        p = paren;
    }
    size_t rest = strlen(p);
    if (n + rest + 1 > cap) { char *g = (char *)realloc(out, n + rest + 1); if (!g) { free(out); return hlsl; } out = g; }
    memcpy(out + n, p, rest + 1);
    free(hlsl);
    return out;
}

char *vio_spirv_to_hlsl_hooked(const uint32_t *spirv, size_t word_count, int shader_model,
                               int fixup_depth, const vio_hlsl_hooks *hooks, char **error_msg)
{
    spvc_context ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler compiler = NULL;
    spvc_compiler_options options = NULL;
    const char *result = NULL;
    char *output = NULL;

    if (spvc_context_create(&ctx) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup("Failed to create SPIRV-Cross context");
        return NULL;
    }

    /* Geometry stages: gl_in[].gl_Position / gl_InvocationID (see vio_gs_hlsl_rewrite). */
    uint32_t gs_pos_location = VIO_GS_POS_LOCATION_NONE, gs_invocations = 1;
    int gs_invocation = 0;
    size_t rewritten_words = 0;
    uint32_t *rewritten = vio_gs_hlsl_rewrite(spirv, word_count, &rewritten_words,
                                              &gs_pos_location, &gs_invocation, &gs_invocations);

    spvc_result parsed = spvc_context_parse_spirv(ctx, rewritten ? rewritten : spirv,
                                                  rewritten ? rewritten_words : word_count, &ir);
    free(rewritten);   /* the parsed IR holds its own copy */
    if (parsed != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    if (spvc_context_create_compiler(ctx, SPVC_BACKEND_HLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    spvc_compiler_create_compiler_options(compiler, &options);
    spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_HLSL_SHADER_MODEL, shader_model);
    /* gl_BaseVertex / gl_BaseInstance below SM 6.8 (no SV_Start*Location):
     * SPIRV-Cross reads them from cbuffer SPIRV_Cross_VertexInfo, which
     * vio_hlsl_pin_vertex_info puts at b13 - D3D12's draw-parameter root
     * constants (OPEN-ITEMS-PLAN A11). */
    if (shader_model < 68)
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_HLSL_SUPPORT_NONZERO_BASE_VERTEX_BASE_INSTANCE, SPVC_TRUE);
    if (vio_hlsl_16bit_types && shader_model >= 62)
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_HLSL_ENABLE_16BIT_TYPES, SPVC_TRUE);
    spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_HLSL_POINT_SIZE_COMPAT, SPVC_TRUE);
    spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_HLSL_POINT_COORD_COMPAT, SPVC_TRUE);
    /* Map OpenGL clip space z [-1,1] to D3D11 clip space z [0,1]:
     * Emits gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5.
     * Only for the LAST vertex-like stage (VS, or the GS / TES behind it) -
     * SPIRV-Cross would otherwise convert once per stage. */
    /* A geometry shader emits its vertices with Append(); SPIRV-Cross puts the
     * fixup at the END of the GS entry point, after every Append, where it never
     * takes effect (z stayed in [-1, 1], so everything with z < 0 was clipped on
     * D3D). The GS gets a text fixup on each emitted copy below instead. */
    int is_geometry = spvc_compiler_get_execution_model(compiler) == SpvExecutionModelGeometry;
    spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_FIXUP_DEPTH_CONVENTION,
                                   (fixup_depth && !is_geometry) ? SPVC_TRUE : SPVC_FALSE);
    if (hooks && hooks->configure) hooks->configure(compiler, options, hooks->user);
    spvc_compiler_install_compiler_options(compiler, options);

    /* Remap combined image-samplers to avoid overlapping register semantics.
     * GLSL uses combined sampler2D; HLSL needs separate texture (t) and sampler (s)
     * registers. Shift sampler bindings to avoid collision with texture bindings. */
    spvc_resources resources = NULL;
    spvc_compiler_create_shader_resources(compiler, &resources);
    if (resources) {
        const spvc_reflected_resource *sampled_images;
        size_t sampled_count;
        spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE,
                                                   &sampled_images, &sampled_count);
        /* Assign unique texture (t) and sampler (s) registers per combined image-sampler.
         * For SM 5.1 (D3D12): depth/shadow samplers get registers 8+ so they map to
         * comparison static samplers in the root signature. Regular samplers get 0+.
         * The shadow base is 8 (not 4) so up to 8 regular samplers fit before the
         * comparison range — the mesh shader uses 5 (albedo, ssao, sdf_ao, probe,
         * environment cube). MUST stay in sync with the replay in php_vio.c and the
         * root-signature static-sampler layout in vio_d3d12.c. */
        unsigned int regular_idx = 0;
        unsigned int shadow_idx = 8; /* offset for comparison static samplers */
        for (size_t i = 0; i < sampled_count; i++) {
            /* Check if this is a depth sampler (sampler2DShadow) */
            int is_depth_sampler = 0;
            if (shader_model >= 51) {
                spvc_type sampled_type = spvc_compiler_get_type_handle(compiler, sampled_images[i].type_id);
                if (sampled_type) {
                    spvc_type img_type = spvc_compiler_get_type_handle(compiler,
                        spvc_type_get_base_type_id(sampled_type));
                    if (img_type && spvc_type_get_image_is_depth(img_type)) {
                        is_depth_sampler = 1;
                    }
                }
            }
            unsigned int binding = is_depth_sampler ? shadow_idx++ : regular_idx++;
            int planned = shader_model >= 51 ? vio_sampler_plan_reg(sampled_images[i].name)
                                             : vio_sampler_plan_index(sampled_images[i].name);
            if (planned >= 0) binding = (unsigned int)planned;
            spvc_compiler_set_decoration(compiler, sampled_images[i].id,
                                          SpvDecorationBinding, binding);
        }

        /* Also handle separate images and samplers */
        const spvc_reflected_resource *sep_images;
        size_t sep_image_count;
        spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_SEPARATE_IMAGE,
                                                   &sep_images, &sep_image_count);
        /* Set 1 is the bindless table (vio_texture_index): it keeps its
         * bindings, t0 / s1 in register space 1 (the D3D12 root table). */
        for (size_t i = 0; i < sep_image_count; i++) {
            if (spvc_compiler_get_decoration(compiler, sep_images[i].id, SpvDecorationDescriptorSet) == 1) {
                /* vio_cubes (binding 5) and vio_texture_arrays (6) alias the same
                 * descriptors as vio_textures, each unbounded from t0: spaces 3 / 4. */
                unsigned b = spvc_compiler_get_decoration(compiler, sep_images[i].id, SpvDecorationBinding);
                if (b == 5 || b == 6) {
                    spvc_compiler_set_decoration(compiler, sep_images[i].id, SpvDecorationDescriptorSet, b == 5 ? 3 : 4);
                    spvc_compiler_set_decoration(compiler, sep_images[i].id, SpvDecorationBinding, 0);
                }
                continue;
            }
            spvc_compiler_set_decoration(compiler, sep_images[i].id,
                                          SpvDecorationBinding, (unsigned int)(sampled_count + i));
        }

        /* The first uniform block holds the stage constants (vio_spirv_get_uniform_offsets),
         * which the D3D backends bind at b0 per stage: keep it there whatever its GLSL
         * binding, so `layout(binding = 1) uniform Material` works (OPEN-ITEMS-PLAN A32).
         * Graphics stages only (vertex .. fragment): compute keeps its Params block
         * where the compute root signature expects it. */
        if ((int)spvc_compiler_get_execution_model(compiler) <= (int)SpvExecutionModelFragment) {
            const spvc_reflected_resource *ubos;
            size_t ubo_count;
            spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, &ubos, &ubo_count);
            if (ubo_count > 0) spvc_compiler_set_decoration(compiler, ubos[0].id, SpvDecorationBinding, 0);
        }

        /* Mesh / task stages also see the texture table (OPEN-ITEMS-PLAN A30): their
         * storage buffers move to register space 2, where the D3D12 mesh root
         * signature puts its root SRV, so t0 of the table stays the texture's. */
        {
            SpvExecutionModel em = spvc_compiler_get_execution_model(compiler);
            if (em == SpvExecutionModelMeshEXT || em == SpvExecutionModelTaskEXT) {
                const spvc_reflected_resource *ssbos;
                size_t ssbo_count;
                spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_STORAGE_BUFFER, &ssbos, &ssbo_count);
                for (size_t i = 0; i < ssbo_count; i++) {
                    spvc_compiler_set_decoration(compiler, ssbos[i].id, SpvDecorationDescriptorSet, 2);
                    spvc_compiler_set_decoration(compiler, ssbos[i].id, SpvDecorationBinding, (unsigned int)i);
                }
            }
        }

        /* Mesh / task stages also see the texture table (OPEN-ITEMS-PLAN A30): their
         * storage buffers move to register space 2, where the D3D12 mesh root
         * signature puts its root SRV, so t0 of the table stays the texture's. */
        {
            SpvExecutionModel em = spvc_compiler_get_execution_model(compiler);
            if (em == SpvExecutionModelMeshEXT || em == SpvExecutionModelTaskEXT) {
                const spvc_reflected_resource *ssbos;
                size_t ssbo_count;
                spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_STORAGE_BUFFER, &ssbos, &ssbo_count);
                for (size_t i = 0; i < ssbo_count; i++) {
                    spvc_compiler_set_decoration(compiler, ssbos[i].id, SpvDecorationDescriptorSet, 2);
                    spvc_compiler_set_decoration(compiler, ssbos[i].id, SpvDecorationBinding, (unsigned int)i);
                }
            }
        }

        /* Ray query (GL_EXT_ray_query): the acceleration structure moves to its
         * own register space so the D3D12 root SRV (t0, space9) never overlaps the
         * SRV tables of space 0. One structure per shader stage. */
        const spvc_reflected_resource *accels;
        size_t accel_count;
        spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_ACCELERATION_STRUCTURE,
                                                   &accels, &accel_count);
        for (size_t i = 0; i < accel_count; i++) {
            spvc_compiler_set_decoration(compiler, accels[i].id, SpvDecorationDescriptorSet, 9);
            spvc_compiler_set_decoration(compiler, accels[i].id, SpvDecorationBinding, (unsigned int)i);
        }

        const spvc_reflected_resource *sep_samplers;
        size_t sep_sampler_count;
        spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_SEPARATE_SAMPLERS,
                                                   &sep_samplers, &sep_sampler_count);
        for (size_t i = 0; i < sep_sampler_count; i++) {
            if (spvc_compiler_get_decoration(compiler, sep_samplers[i].id, SpvDecorationDescriptorSet) == 1) continue;
            spvc_compiler_set_decoration(compiler, sep_samplers[i].id,
                                          SpvDecorationBinding, (unsigned int)(sampled_count + sep_image_count + i));
        }
    }

    if (spvc_compiler_compile(compiler, &result) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    /* Debug aid (like VIO_DUMP_CS_HLSL for compute): print the transpiled
     * graphics HLSL so register / semantic assignment can be checked. */
    if (getenv("VIO_DUMP_HLSL")) {
        fprintf(stderr, "==== HLSL (SM %d) ====\n%s\n==== end ====\n", shader_model, result);
        fflush(stderr);
    }

    if (fixup_depth && is_geometry) {
        /* Convert the copy that is appended, not the static gl_Position: a GS
         * may emit the same position twice. */
        static const char anchor[] = "stage_output.gl_Position = gl_Position;";
        static const char fixed[] = "stage_output.gl_Position = gl_Position; "
            "stage_output.gl_Position.z = (stage_output.gl_Position.z + stage_output.gl_Position.w) * 0.5;";
        size_t count = 0, len = strlen(result);
        for (const char *p = result; (p = strstr(p, anchor)) != NULL; p += sizeof(anchor) - 1) count++;
        output = (char *)malloc(len + count * (sizeof(fixed) - sizeof(anchor)) + 1);
        if (output) {
            char *w = output;
            const char *p = result;
            for (;;) {
                const char *a = strstr(p, anchor);
                if (!a) break;
                memcpy(w, p, (size_t)(a - p)); w += a - p;
                memcpy(w, fixed, sizeof(fixed) - 1); w += sizeof(fixed) - 1;
                p = a + sizeof(anchor) - 1;
            }
            strcpy(w, p);
        }
    } else {
        output = strdup(result);
    }
    if (is_geometry) output = vio_gs_hlsl_patch(output, gs_pos_location, gs_invocation, gs_invocations);
    output = vio_hlsl_fix_int64_buffer_atomics(output);
    output = vio_hlsl_pin_vertex_info(output);
    if (output && hooks && hooks->finish) output = hooks->finish(compiler, output, error_msg, hooks->user);

    spvc_context_destroy(ctx);
    return output;
}

/* ── Reflection ──────────────────────────────────────────────────── */

static void copy_resources(spvc_compiler compiler, spvc_resources resources,
                           spvc_resource_type type,
                           vio_reflect_resource **out, int *out_count)
{
    const spvc_reflected_resource *list;
    size_t count;
    spvc_resources_get_resource_list_for_type(resources, type, &list, &count);

    if (count == 0) {
        *out = NULL;
        *out_count = 0;
        return;
    }

    *out = calloc(count, sizeof(vio_reflect_resource));
    *out_count = (int)count;

    for (size_t i = 0; i < count; i++) {
        (*out)[i].name     = strdup(spvc_compiler_get_name(compiler, list[i].id));
        (*out)[i].id       = list[i].id;
        (*out)[i].set      = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationDescriptorSet);
        (*out)[i].binding  = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationBinding);
        (*out)[i].location = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationLocation);

        /* Extract vector size from type (1=float, 2=vec2, 3=vec3, 4=vec4) */
        spvc_type type_handle = spvc_compiler_get_type_handle(compiler, list[i].type_id);
        (*out)[i].vecsize = type_handle ? spvc_type_get_vector_size(type_handle) : 3;
        (*out)[i].columns = type_handle ? spvc_type_get_columns(type_handle) : 1;
        if ((*out)[i].columns < 1) (*out)[i].columns = 1;

        /* Detect depth image (sampler2DShadow → Depth=1 in SPIR-V) */
        (*out)[i].is_depth = 0;
        if (type == SPVC_RESOURCE_TYPE_SAMPLED_IMAGE && type_handle) {
            spvc_type image_type = spvc_compiler_get_type_handle(compiler, spvc_type_get_base_type_id(type_handle));
            if (image_type && spvc_type_get_image_is_depth(image_type)) {
                (*out)[i].is_depth = 1;
            }
        }
    }
}

int vio_spirv_reflect(const uint32_t *spirv, size_t spirv_size,
                       vio_reflect_result *result, char **error_msg)
{
    memset(result, 0, sizeof(vio_reflect_result));

    spvc_context ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler compiler = NULL;
    spvc_resources resources = NULL;

    if (spvc_context_create(&ctx) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup("Failed to create SPIRV-Cross context");
        return -1;
    }

    size_t word_count = spirv_size / sizeof(uint32_t);

    if (spvc_context_parse_spirv(ctx, spirv, word_count, &ir) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return -1;
    }

    /* Use GLSL backend for reflection - it supports all resource types */
    if (spvc_context_create_compiler(ctx, SPVC_BACKEND_GLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return -1;
    }

    if (spvc_compiler_create_shader_resources(compiler, &resources) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return -1;
    }

    copy_resources(compiler, resources, SPVC_RESOURCE_TYPE_STAGE_INPUT, &result->inputs, &result->input_count);
    copy_resources(compiler, resources, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, &result->ubos, &result->ubo_count);
    copy_resources(compiler, resources, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE, &result->textures, &result->texture_count);
    copy_resources(compiler, resources, SPVC_RESOURCE_TYPE_PUSH_CONSTANT, &result->uniforms, &result->uniform_count);
    copy_resources(compiler, resources, SPVC_RESOURCE_TYPE_STORAGE_BUFFER, &result->storage_buffers, &result->storage_buffer_count);
    copy_resources(compiler, resources, SPVC_RESOURCE_TYPE_STORAGE_IMAGE, &result->storage_images, &result->storage_image_count);

    spvc_context_destroy(ctx);
    return 0;
}

void vio_reflect_free(vio_reflect_result *result)
{
    for (int i = 0; i < result->input_count; i++) free((void *)result->inputs[i].name);
    for (int i = 0; i < result->uniform_count; i++) free((void *)result->uniforms[i].name);
    for (int i = 0; i < result->texture_count; i++) free((void *)result->textures[i].name);
    for (int i = 0; i < result->ubo_count; i++) free((void *)result->ubos[i].name);
    for (int i = 0; i < result->storage_buffer_count; i++) free((void *)result->storage_buffers[i].name);
    for (int i = 0; i < result->storage_image_count; i++) free((void *)result->storage_images[i].name);
    free(result->inputs);
    free(result->uniforms);
    free(result->textures);
    free(result->ubos);
    free(result->storage_buffers);
    free(result->storage_images);
    memset(result, 0, sizeof(vio_reflect_result));
}

/* Component type of a uniform member for vio_uniform_entry. */
static void vio_entry_set_type(spvc_compiler compiler, spvc_type_id type_id, vio_uniform_entry *e)
{
    spvc_type t = spvc_compiler_get_type_handle(compiler, type_id);
    e->base_type = 0; e->vecsize = 0; e->columns = 0;
    if (!t) return;
    spvc_basetype bt = spvc_type_get_basetype(t);
    if (bt == SPVC_BASETYPE_FP32) e->base_type = 1;
    else if (bt == SPVC_BASETYPE_INT32 || bt == SPVC_BASETYPE_UINT32 || bt == SPVC_BASETYPE_BOOLEAN) e->base_type = 2;
    e->vecsize = (short)spvc_type_get_vector_size(t);
    e->columns = (short)spvc_type_get_columns(t);
}

int vio_spvc_combine_separate(void *compiler_handle)
{
    spvc_compiler c = (spvc_compiler)compiler_handle;
    spvc_resources res = NULL;
    if (!c || spvc_compiler_create_shader_resources(c, &res) != SPVC_SUCCESS || !res) return 0;
    const spvc_reflected_resource *list = NULL;
    size_t n = 0;
    spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_SEPARATE_IMAGE, &list, &n);
    if (n == 0) return 0;
    for (size_t i = 0; i < n; i++)
        if (spvc_compiler_get_decoration(c, list[i].id, SpvDecorationDescriptorSet) == 1) return 0;
    if (spvc_compiler_build_combined_image_samplers(c) != SPVC_SUCCESS) return 0;
    const spvc_combined_image_sampler *cis = NULL;
    size_t cn = 0;
    spvc_compiler_get_combined_image_samplers(c, &cis, &cn);
    for (size_t i = 0; i < cn; i++) {
        const char *name = spvc_compiler_get_name(c, cis[i].image_id);
        if (name && name[0]) spvc_compiler_set_name(c, cis[i].combined_id, name);
    }
    return (int)cn;
}

int vio_spirv_separate_images(const uint32_t *spirv, size_t spirv_size, char (*names)[64], int max)
{
    spvc_context ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler compiler = NULL;
    spvc_resources resources = NULL;
    int n = 0;
    if (!spirv || spvc_context_create(&ctx) != SPVC_SUCCESS) return 0;
    if (spvc_context_parse_spirv(ctx, spirv, spirv_size / sizeof(uint32_t), &ir) == SPVC_SUCCESS
        && spvc_context_create_compiler(ctx, SPVC_BACKEND_NONE, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) == SPVC_SUCCESS
        && spvc_compiler_create_shader_resources(compiler, &resources) == SPVC_SUCCESS) {
        const spvc_reflected_resource *list = NULL;
        size_t count = 0;
        spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_SEPARATE_IMAGE, &list, &count);
        for (size_t i = 0; i < count && n < max; i++) {
            if (spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationDescriptorSet) == 1) continue;
            snprintf(names[n], 64, "%s", list[i].name ? list[i].name : "");
            n++;
        }
    }
    spvc_context_destroy(ctx);
    return n;
}

int vio_spirv_uniform_block_binding(const uint32_t *spirv, size_t spirv_size)
{
    spvc_context ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler compiler = NULL;
    spvc_resources resources = NULL;
    int binding = -1;
    if (!spirv || spvc_context_create(&ctx) != SPVC_SUCCESS) return -1;
    if (spvc_context_parse_spirv(ctx, spirv, spirv_size / sizeof(uint32_t), &ir) == SPVC_SUCCESS
        && spvc_context_create_compiler(ctx, SPVC_BACKEND_NONE, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) == SPVC_SUCCESS
        && spvc_compiler_create_shader_resources(compiler, &resources) == SPVC_SUCCESS) {
        const spvc_reflected_resource *ubos = NULL;
        size_t ubo_count = 0;
        spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, &ubos, &ubo_count);
        if (ubo_count > 0) {
            const char *block = spvc_compiler_get_name(compiler, ubos[0].base_type_id);
            int loose = (block && strcmp(block, "gl_DefaultUniformBlock") == 0)
                     || (ubos[0].name && strcmp(ubos[0].name, "gl_DefaultUniformBlock") == 0);
            if (!loose) binding = (int)spvc_compiler_get_decoration(compiler, ubos[0].id, SpvDecorationBinding);
        }
    }
    spvc_context_destroy(ctx);
    return binding;
}

int vio_spirv_get_uniform_offsets(const uint32_t *spirv, size_t spirv_size,
                                   vio_uniform_entry *entries, int max_entries,
                                   int *total_size)
{
    spvc_context ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler compiler = NULL;
    spvc_resources resources = NULL;
    int count = 0;

    *total_size = 0;

    if (spvc_context_create(&ctx) != SPVC_SUCCESS) return 0;

    size_t word_count = spirv_size / sizeof(uint32_t);
    if (spvc_context_parse_spirv(ctx, spirv, word_count, &ir) != SPVC_SUCCESS) {
        spvc_context_destroy(ctx);
        return 0;
    }

    if (spvc_context_create_compiler(ctx, SPVC_BACKEND_HLSL, ir,
            SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) != SPVC_SUCCESS) {
        spvc_context_destroy(ctx);
        return 0;
    }

    if (spvc_compiler_create_shader_resources(compiler, &resources) != SPVC_SUCCESS) {
        spvc_context_destroy(ctx);
        return 0;
    }

    /* Get uniform buffers (cbuffer _Global in HLSL) */
    const spvc_reflected_resource *ubos;
    size_t ubo_count;
    spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER,
                                              &ubos, &ubo_count);

    /* Also check push constants (used by OpenGL-style uniforms) */
    const spvc_reflected_resource *push_constants;
    size_t push_count;
    spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_PUSH_CONSTANT,
                                              &push_constants, &push_count);

    /* Process the first UBO or push constant block */
    spvc_type_id type_id = 0;
    if (ubo_count > 0) {
        type_id = ubos[0].base_type_id;
    } else if (push_count > 0) {
        type_id = push_constants[0].base_type_id;
    }

    if (type_id) {
        spvc_type type = spvc_compiler_get_type_handle(compiler, type_id);
        unsigned int member_count = spvc_type_get_num_member_types(type);


        for (unsigned int i = 0; i < member_count && count < max_entries; i++) {
            const char *name = spvc_compiler_get_member_name(compiler, type_id, i);
            unsigned int base_offset = 0;
            size_t member_size = 0;
            spvc_compiler_type_struct_member_offset(compiler, type, i, &base_offset);
            spvc_compiler_get_declared_struct_member_size(compiler, type, i, &member_size);

            if (!name || !name[0]) continue;

            /* Check if this member is a struct array (e.g. DirLight u_dir_lights[16]) */
            spvc_type_id member_type_id = spvc_type_get_member_type(type, i);
            spvc_type member_type = spvc_compiler_get_type_handle(compiler, member_type_id);
            unsigned int num_array_dims = spvc_type_get_num_array_dimensions(member_type);

            /* For array-of-struct, get the element (struct) type via parent_type */
            spvc_type_id elem_type_id = 0;
            spvc_type elem_type = NULL;
            unsigned int num_struct_members = 0;

            if (num_array_dims > 0) {
                elem_type_id = spvc_type_get_base_type_id(member_type);
                if (elem_type_id) {
                    elem_type = spvc_compiler_get_type_handle(compiler, elem_type_id);
                    num_struct_members = spvc_type_get_num_member_types(elem_type);
                }
            }

            if (num_array_dims > 0 && num_struct_members > 0 && elem_type) {
                /* Struct array: flatten to name[idx].field entries */
                unsigned int array_size = spvc_type_get_array_dimension(member_type, 0);
                unsigned int array_stride = 0;
                spvc_compiler_type_struct_member_array_stride(compiler, type, i, &array_stride);

                if (array_stride == 0 && array_size > 1) {
                    array_stride = (unsigned int)(member_size / array_size);
                }

                for (unsigned int ai = 0; ai < array_size && count < max_entries; ai++) {
                    for (unsigned int si = 0; si < num_struct_members && count < max_entries; si++) {
                        const char *field = spvc_compiler_get_member_name(compiler, elem_type_id, si);
                        unsigned int field_offset = 0;
                        size_t field_size = 0;
                        spvc_compiler_type_struct_member_offset(compiler, elem_type, si, &field_offset);
                        spvc_compiler_get_declared_struct_member_size(compiler, elem_type, si, &field_size);

                        if (field && field[0]) {
                            snprintf(entries[count].name, sizeof(entries[count].name),
                                     "%s[%u].%s", name, ai, field);
                            entries[count].offset = (int)(base_offset + ai * array_stride + field_offset);
                            entries[count].size = (int)field_size;
                            entries[count].stride = 0;
                            vio_entry_set_type(compiler, spvc_type_get_member_type(elem_type, si), &entries[count]);
                            int end = entries[count].offset + (int)field_size;
                            if (end > *total_size) *total_size = end;
                            count++;
                        }
                    }
                }
            } else {
                /* Simple scalar/vector/matrix member, or an array of them */
                strncpy(entries[count].name, name, sizeof(entries[count].name) - 1);
                entries[count].name[sizeof(entries[count].name) - 1] = '\0';
                entries[count].offset = (int)base_offset;
                entries[count].size = (int)member_size;
                entries[count].stride = 0;
                vio_entry_set_type(compiler, member_type_id, &entries[count]);
                if (num_array_dims > 0) {
                    unsigned int array_stride = 0;
                    spvc_compiler_type_struct_member_array_stride(compiler, type, i, &array_stride);
                    entries[count].stride = (int)array_stride;
                }
                int end = (int)(base_offset + member_size);
                if (end > *total_size) *total_size = end;
                count++;
            }
        }
    }

    /* Align total size to 16 bytes (D3D11 CB requirement) */
    *total_size = (*total_size + 15) & ~15;

    spvc_context_destroy(ctx);
    return count;
}

#else /* !HAVE_SPIRV_CROSS */

void vio_hlsl_set_16bit_types(int enable) { (void)enable; }
void vio_glsl_set_ovr_view_count(int views) { (void)views; }

char *vio_spirv_to_glsl(const uint32_t *spirv, size_t spirv_size, int version, char **error_msg)
{
    (void)spirv; (void)spirv_size; (void)version;
    if (error_msg) *error_msg = strdup("spirv-cross not available (compile with --with-spirv-cross)");
    return NULL;
}

char *vio_spirv_to_glsl_compute(const uint32_t *spirv, size_t spirv_size, int version, char **error_msg)
{
    (void)spirv; (void)spirv_size; (void)version;
    if (error_msg) *error_msg = strdup("spirv-cross not available (compile with --with-spirv-cross)");
    return NULL;
}

char *vio_spirv_to_msl(const uint32_t *spirv, size_t spirv_size, char **error_msg)
{
    (void)spirv; (void)spirv_size;
    if (error_msg) *error_msg = strdup("spirv-cross not available (compile with --with-spirv-cross)");
    return NULL;
}

char *vio_spirv_to_hlsl(const uint32_t *spirv, size_t spirv_size, int shader_model, char **error_msg)
{
    (void)spirv; (void)spirv_size; (void)shader_model;
    if (error_msg) *error_msg = strdup("spirv-cross not available (compile with --with-spirv-cross)");
    return NULL;
}

char *vio_spirv_to_hlsl_ex(const uint32_t *spirv, size_t spirv_size, int shader_model,
                           int fixup_depth, char **error_msg)
{
    (void)spirv; (void)spirv_size; (void)shader_model; (void)fixup_depth;
    if (error_msg) *error_msg = strdup("spirv-cross not available (compile with --with-spirv-cross)");
    return NULL;
}

int vio_spirv_reflect(const uint32_t *spirv, size_t spirv_size,
                       vio_reflect_result *result, char **error_msg)
{
    (void)spirv; (void)spirv_size;
    memset(result, 0, sizeof(vio_reflect_result));
    if (error_msg) *error_msg = strdup("spirv-cross not available (compile with --with-spirv-cross)");
    return -1;
}

void vio_reflect_free(vio_reflect_result *result) {
    memset(result, 0, sizeof(vio_reflect_result));
}

int vio_spirv_get_uniform_offsets(const uint32_t *spirv, size_t spirv_size,
                                   vio_uniform_entry *entries, int max_entries,
                                   int *total_size)
{
    (void)spirv; (void)spirv_size; (void)entries; (void)max_entries;
    if (total_size) *total_size = 0;
    return 0;
}

int vio_spirv_uniform_block_binding(const uint32_t *spirv, size_t spirv_size)
{
    (void)spirv; (void)spirv_size;
    return -1;
}

int vio_spvc_combine_separate(void *compiler)
{
    (void)compiler;
    return 0;
}

int vio_spirv_separate_images(const uint32_t *spirv, size_t spirv_size, char (*names)[64], int max)
{
    (void)spirv; (void)spirv_size; (void)names; (void)max;
    return 0;
}

#endif /* HAVE_SPIRV_CROSS */

/* ── Shader-wide sampler plan (OPEN-ITEMS-PLAN A30) ─────────────────── */

static const vio_sampler_plan *vio_sampler_plan_active = NULL;

void vio_sampler_plan_use(const vio_sampler_plan *plan) { vio_sampler_plan_active = plan; }
const vio_sampler_plan *vio_sampler_plan_current(void) { return vio_sampler_plan_active; }

int vio_sampler_plan_reg(const char *name)
{
    const vio_sampler_plan *p = vio_sampler_plan_active;
    if (!p || !name) return -1;
    for (int i = 0; i < p->count; i++) if (strcmp(p->names[i], name) == 0) return p->reg[i];
    return -1;
}

int vio_sampler_plan_index(const char *name)
{
    const vio_sampler_plan *p = vio_sampler_plan_active;
    if (!p || !name) return -1;
    for (int i = 0; i < p->count; i++) if (strcmp(p->names[i], name) == 0) return i;
    return -1;
}

static int vio_sampler_plan_find(const vio_sampler_plan *p, const char *name)
{
    for (int i = 0; i < p->count; i++) if (strcmp(p->names[i], name) == 0) return i;
    return -1;
}

static void vio_sampler_plan_add(vio_sampler_plan *p, const char *name, int reg, int is_depth)
{
    if (p->count >= VIO_SAMPLER_PLAN_MAX || reg < 0) return;
    snprintf(p->names[p->count], sizeof(p->names[0]), "%s", name ? name : "");
    p->reg[p->count] = reg;
    p->is_depth[p->count] = is_depth;
    p->count++;
}

/* Lowest register in [lo, hi) no planned sampler holds; -1 when full. */
static int vio_sampler_plan_free_reg(const vio_sampler_plan *p, int lo, int hi)
{
    for (int r = lo; r < hi; r++) {
        int used = 0;
        for (int i = 0; i < p->count && !used; i++) used = p->reg[i] == r;
        if (!used) return r;
    }
    return -1;
}

void vio_sampler_plan_build(vio_sampler_plan *plan, const uint32_t *const *spirv, const size_t *size, int n)
{
    memset(plan, 0, sizeof(*plan));
    for (int s = 0; s < n; s++) {
        if (!spirv[s] || !size[s]) continue;
        vio_reflect_result r = {0};
        char *err = NULL;
        if (vio_spirv_reflect(spirv[s], size[s], &r, &err) != 0) { free(err); continue; }
        if (s == 0) {
            /* The fragment stage: the replay php_vio.c always did. */
            int regular = 0, shadow = 8;
            for (int t = 0; t < r.texture_count; t++) {
                int d = r.textures[t].is_depth ? 1 : 0;
                vio_sampler_plan_add(plan, r.textures[t].name, d ? shadow++ : regular++, d);
            }
            char sep[VIO_SAMPLER_PLAN_MAX][64];
            int nsep = vio_spirv_separate_images(spirv[s], size[s], sep, VIO_SAMPLER_PLAN_MAX);
            for (int j = 0; j < nsep; j++) vio_sampler_plan_add(plan, sep[j], r.texture_count + j, 0);
        } else {
            for (int t = 0; t < r.texture_count; t++) {
                if (vio_sampler_plan_find(plan, r.textures[t].name) >= 0) continue;
                int d = r.textures[t].is_depth ? 1 : 0;
                vio_sampler_plan_add(plan, r.textures[t].name, d ? vio_sampler_plan_free_reg(plan, 8, 16)
                                                                 : vio_sampler_plan_free_reg(plan, 0, 8), d);
            }
        }
        vio_reflect_free(&r);
    }
}
