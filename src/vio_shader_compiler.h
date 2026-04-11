/*
 * php-vio - Shader compiler (GLSL -> SPIR-V via glslang)
 */

#ifndef VIO_SHADER_COMPILER_H
#define VIO_SHADER_COMPILER_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stddef.h>
#include <stdint.h>

/* Initialize glslang (call once in MINIT) */
int  vio_shader_compiler_init(void);

/* Shutdown glslang (call in MSHUTDOWN) */
void vio_shader_compiler_shutdown(void);

/* Compile GLSL source to SPIR-V binary.
 * Returns malloc'd buffer (caller frees) and sets out_size.
 * Returns NULL on failure, sets error_msg if non-NULL. */
uint32_t *vio_compile_glsl_to_spirv(const char *source, int is_fragment,
                                     size_t *out_size, char **error_msg);

#endif /* VIO_SHADER_COMPILER_H */
