/*
 * php-vio - GLSL tessellation stages as HLSL hull / domain shaders.
 */

#ifndef VIO_TESS_HLSL_H
#define VIO_TESS_HLSL_H

#include <stddef.h>
#include <stdint.h>

typedef struct vio_tess_hlsl_desc {
    const void *tcs;            /* SPIR-V of the tessellation control stage */
    size_t      tcs_size;       /* bytes */
    const void *tes;            /* SPIR-V of the tessellation evaluation stage */
    size_t      tes_size;
    uint32_t    input_points;   /* control points per input patch (the draw's patch size);
                                   0 = the control stage's output vertices */
    int         shader_model;   /* SPIRV-Cross HLSL target: 50 / 51 / 60..69 */
    int         fixup_depth;    /* domain shader: GL -> D3D clip-space depth fixup, set when
                                   the domain shader is the last stage writing gl_Position */
} vio_tess_hlsl_desc;

/* Translate one stage of a tessellation pair to HLSL: VIO_STAGE_TESS_CONTROL
 * gives a hull shader, VIO_STAGE_TESS_EVAL a domain shader. Both stages are
 * needed for either: the hull shader takes domain / spacing / winding from the
 * evaluation stage, and hull and domain shader share one control-point and one
 * patch-constant layout, built from the control stage's outputs.
 *
 * OpenGL conventions are kept: gl_TessCoord is passed through unchanged and
 * the output winding is reversed (D3D's tessellator has the same domain
 * coordinates and factor edges as OpenGL, with the opposite winding).
 *
 * Returns malloc'd HLSL (caller frees) or NULL with *error_msg set. */
char *vio_tess_to_hlsl(int stage, const vio_tess_hlsl_desc *desc, char **error_msg);

/* layout(vertices = N) of a tessellation control stage; 0 if not found. */
uint32_t vio_tess_output_vertices(const void *tcs, size_t tcs_size);

/* A stage given as GLSL source or SPIR-V (vio_shader data) as a malloc'd
 * SPIR-V copy; *out_size in bytes. NULL with *error_msg set on failure. */
uint32_t *vio_tess_stage_spirv(const void *data, size_t size, int stage, size_t *out_size, char **error_msg);

#endif /* VIO_TESS_HLSL_H */
