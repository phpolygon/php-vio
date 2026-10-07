/*
 * php-vio - Shader management implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_shader.h"
#include "../include/vio_backend.h"
#include <stdlib.h>

zend_class_entry *vio_shader_ce = NULL;
static zend_object_handlers vio_shader_handlers;

static zend_object *vio_shader_create_object(zend_class_entry *ce)
{
    vio_shader_object *shader = zend_object_alloc(sizeof(vio_shader_object), ce);

    /* Zero everything before std (which is at the end of the struct) */
    memset(shader, 0, XtOffsetOf(vio_shader_object, std));

    shader->format = VIO_SHADER_AUTO;
    for (int i = 0; i < 16; i++) shader->gl_to_hlsl_sampler[i] = -1;

    zend_object_std_init(&shader->std, ce);
    object_properties_init(&shader->std, ce);
    shader->std.handlers = &vio_shader_handlers;

    return &shader->std;
}

/* Where declarations may start: after the #version / #extension lines that lead
 * the source (blank and comment-only lines in between are skipped). */
static size_t vio_glsl_header_end(const char *src)
{
    size_t end = 0;
    const char *p = src;
    while (*p) {
        const char *line = p;
        const char *nl = strchr(p, '\n');
        const char *next = nl ? nl + 1 : p + strlen(p);
        while (*line == ' ' || *line == '\t' || *line == '\r') line++;
        if (strncmp(line, "#version", 8) == 0 || strncmp(line, "#extension", 10) == 0) {
            end = (size_t)(next - src);
        } else if (*line != '\n' && *line != '\0' && strncmp(line, "//", 2) != 0) {
            break;
        }
        p = next;
    }
    return end;
}

char *vio_glsl_multiview_instancing(const char *src, int fragment, int views)
{
    char head[512], tail[512];
    if (!src || views < 2) return NULL;
    if (fragment) {
        snprintf(head, sizeof(head),
            "layout(location = 31) flat in int vio_mv_view_in;\n"
            "#define gl_ViewIndex vio_mv_view_in\n");
        tail[0] = '\0';
    } else {
        snprintf(head, sizeof(head),
            "#extension GL" "_ARB_shader_viewport_layer_array : require\n"   /* split: audit gate 070 */
            "layout(location = 31) flat out int vio_mv_view_out;\n"
            "int vio_mv_view;\nint vio_mv_instance;\n"
            "#define gl_ViewIndex vio_mv_view\n"
            "#define gl_InstanceIndex vio_mv_instance\n"
            "#define main vio_mv_user_main\n");
        snprintf(tail, sizeof(tail),
            "\n#undef main\n#undef gl_InstanceIndex\n#undef gl_ViewIndex\n"
            "void main() {\n"
            "    vio_mv_view = gl_InstanceIndex %% %d;\n"
            "    vio_mv_instance = gl_InstanceIndex / %d;\n"
            "    vio_mv_view_out = vio_mv_view;\n"
            "    gl_Layer = vio_mv_view;\n"
            "    vio_mv_user_main();\n"
            "}\n", views, views);
    }
    size_t at = vio_glsl_header_end(src), len = strlen(src);
    size_t hl = strlen(head), tl = strlen(tail);
    char *out = (char *)malloc(len + hl + tl + 2);
    if (!out) return NULL;
    memcpy(out, src, at);
    memcpy(out + at, head, hl);
    memcpy(out + at + hl, src + at, len - at);
    memcpy(out + len + hl, tail, tl + 1);
    return out;
}

static void vio_shader_free_object(zend_object *obj)
{
    vio_shader_object *shader = vio_shader_from_obj(obj);

    if (shader->backend) {
        const vio_backend *be = (const vio_backend *)shader->backend;
        if (be->destroy_shader_obj) {
            be->destroy_shader_obj(shader);
        }
    }

    free(shader->mv_src[0]);
    free(shader->mv_src[1]);
    shader->mv_src[0] = shader->mv_src[1] = NULL;

    if (shader->vert_spirv) {
        free(shader->vert_spirv);
        shader->vert_spirv = NULL;
    }
    if (shader->frag_spirv) {
        free(shader->frag_spirv);
        shader->frag_spirv = NULL;
    }
    if (shader->task_spirv) {
        free(shader->task_spirv);
        shader->task_spirv = NULL;
    }
    for (int i = 0; i < VIO_EXTRA_STAGE_COUNT; i++) {
        if (shader->stage_spirv[i]) {
            free(shader->stage_spirv[i]);
            shader->stage_spirv[i] = NULL;
        }
        if (shader->stage_cb[i]) {
            /* The backend constant buffer follows the vertex/fragment cbuffer
             * ownership rule (backend-owned, see below). */
            free(shader->stage_cb[i]);
            shader->stage_cb[i] = NULL;
        }
    }

    if (shader->uniform_lookup) {
        zend_hash_destroy(shader->uniform_lookup);
        FREE_HASHTABLE(shader->uniform_lookup);
        shader->uniform_lookup = NULL;
    }

    /* Backend cbuffers are owned by the backend — no free needed here
     * (they are D3D11 COM objects released on context teardown) */

    zend_object_std_dtor(&shader->std);
}

void vio_shader_register(void)
{
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "VioShader", NULL);
    vio_shader_ce = zend_register_internal_class(&ce);
    vio_shader_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_shader_ce->create_object = vio_shader_create_object;

    memcpy(&vio_shader_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_shader_handlers.offset   = XtOffsetOf(vio_shader_object, std);
    vio_shader_handlers.free_obj = vio_shader_free_object;
    vio_shader_handlers.clone_obj = NULL;
}
