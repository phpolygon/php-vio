/*
 * php-vio - Shader management implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_shader.h"
#include <stdlib.h>

#ifdef HAVE_GLFW
#include <glad/glad.h>
#endif

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

static void vio_shader_free_object(zend_object *obj)
{
    vio_shader_object *shader = vio_shader_from_obj(obj);

#ifdef HAVE_GLFW
    if (shader->program) {
        glDeleteProgram(shader->program);
        shader->program = 0;
    }
#endif

    if (shader->vert_spirv) {
        free(shader->vert_spirv);
        shader->vert_spirv = NULL;
    }
    if (shader->frag_spirv) {
        free(shader->frag_spirv);
        shader->frag_spirv = NULL;
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
