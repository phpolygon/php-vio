/*
 * php-vio - HLSL stage override checks shared by the D3D11 and D3D12 backends.
 * Include after <d3dcompiler.h> (D3DReflect, ID3D11ShaderReflection).
 */

#ifndef VIO_D3D_SHADER_CHECK_H
#define VIO_D3D_SHADER_CHECK_H

#if defined(HAVE_D3D11) || defined(HAVE_D3D12)

#include "../vio_shader.h"
#include "../vio_shader_reflect.h"


/* HLSL stage override ('hlsl' => [stage => source], VIO_FEATURE_HLSL_STAGE_OVERRIDE):
 * the uniforms vio_set_uniform writes for the stage come from the GLSL stage's
 * reflection (name, byte offset), and the generic path copies them into the
 * stage's b0. Check that the HLSL cbuffer at register b0 has every one of them
 * at the same offset and warn otherwise - a silent mismatch would read
 * garbage. DXIL (shader model 6) has no D3DReflect; it is skipped. */
/* ID3D11ShaderReflection has no COBJMACROS wrappers in C: calls go through lpVtbl. */
static inline void vio_d3d_check_override_cbuffer(ID3DBlob *blob, const void *glsl_spirv, size_t spirv_size,
                                                  const char *backend, const char *label)
{
    if (!blob || !glsl_spirv || spirv_size < 20) return;
    vio_uniform_entry entries[VIO_MAX_UNIFORMS];
    int total = 0;
    int count = vio_spirv_get_uniform_offsets((const uint32_t *)glsl_spirv, spirv_size, entries, VIO_MAX_UNIFORMS, &total);
    if (count <= 0) return;
    ID3D11ShaderReflection *refl = NULL;
    if (FAILED(D3DReflect(ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                          &IID_ID3D11ShaderReflection, (void **)&refl)) || !refl) return;
    ID3D11ShaderReflectionConstantBuffer *cb = NULL;
    D3D11_SHADER_DESC sd;
    if (SUCCEEDED(refl->lpVtbl->GetDesc(refl, &sd))) {
        for (UINT r = 0; r < sd.BoundResources && !cb; r++) {
            D3D11_SHADER_INPUT_BIND_DESC bd;
            if (SUCCEEDED(refl->lpVtbl->GetResourceBindingDesc(refl, r, &bd)) &&
                bd.Type == D3D_SIT_CBUFFER && bd.BindPoint == 0) {
                cb = refl->lpVtbl->GetConstantBufferByName(refl, bd.Name);
            }
        }
    }
    for (int i = 0; i < count; i++) {
        D3D11_SHADER_VARIABLE_DESC vd;
        ID3D11ShaderReflectionVariable *var = cb ? cb->lpVtbl->GetVariableByName(cb, entries[i].name) : NULL;
        if (!var || FAILED(var->lpVtbl->GetDesc(var, &vd))) {
            php_error_docref(NULL, E_WARNING, "%s: HLSL %s override has no '%s' in its register(b0) cbuffer "
                             "(the GLSL stage puts it at offset %d)", backend, label, entries[i].name, entries[i].offset);
        } else if ((int)vd.StartOffset != entries[i].offset) {
            php_error_docref(NULL, E_WARNING, "%s: HLSL %s override has '%s' at cbuffer offset %u, the GLSL stage at %d - "
                             "declare the uniforms in the same order and packing", backend, label, entries[i].name,
                             vd.StartOffset, entries[i].offset);
        }
    }
    refl->lpVtbl->Release(refl);
}

#endif

#endif /* VIO_D3D_SHADER_CHECK_H */
