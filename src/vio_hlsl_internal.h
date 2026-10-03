/*
 * php-vio - internal SPIR-V -> HLSL helpers shared by vio_shader_reflect.c and
 * vio_tess_hlsl.c (not part of the backend-facing API).
 */

#ifndef VIO_HLSL_INTERNAL_H
#define VIO_HLSL_INTERNAL_H

#ifdef HAVE_SPIRV_CROSS

#include <stddef.h>
#include <stdint.h>
#include <spirv_cross/spirv_cross_c.h>

/* Hooks around vio's SPIR-V -> HLSL translation: `configure` runs after vio's
 * own options are set and before they are installed (execution modes, names,
 * extra options), `finish` gets the transpiled HLSL (malloc'd, ownership
 * passes) and returns the final source or NULL with *error_msg set. */
typedef struct vio_hlsl_hooks {
    void (*configure)(spvc_compiler compiler, spvc_compiler_options options, void *user);
    char *(*finish)(spvc_compiler compiler, char *hlsl, char **error_msg, void *user);
    void *user;
} vio_hlsl_hooks;

/* vio_spirv_to_hlsl_ex with hooks: same options, register assignment and
 * geometry-stage rewrites. `words` is a SPIR-V word count. */
char *vio_spirv_to_hlsl_hooked(const uint32_t *spirv, size_t words, int shader_model,
                               int fixup_depth, const vio_hlsl_hooks *hooks, char **error_msg);

#endif /* HAVE_SPIRV_CROSS */

#endif /* VIO_HLSL_INTERNAL_H */
