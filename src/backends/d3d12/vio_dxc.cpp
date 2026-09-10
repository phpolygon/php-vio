/*
 * php-vio - DXC (Shader Model 6) front end for the D3D12 backend (GAP-PHASE5 Block 7)
 *
 * dxcapi.h is a C++ header, so this is the one C++ translation unit of the
 * D3D12 backend. It loads dxcompiler.dll (and requires dxil.dll next to it,
 * which signs the DXIL so the runtime accepts it) at first use and compiles
 * HLSL to DXIL with IDxcCompiler3. Everything else in the backend stays C and
 * talks to this file through the extern "C" functions below.
 */

/* Windows-only translation unit (config.w32 lists it; config.m4 does not), so it
 * needs no HAVE_D3D12 guard: without the D3D12 backend nothing references it. */
#include <windows.h>
#include <dxcapi.h>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" {
int  vio_dxc_available(void);
void vio_dxc_set_dir(const char *dir);
int  vio_dxc_compile(const char *hlsl, const char *entry, const char *profile, int debug,
                     void **out_bytes, size_t *out_len, char **out_error);
}

namespace {

HMODULE g_dxcompiler = nullptr;
HMODULE g_dxil = nullptr;
DxcCreateInstanceProc g_create = nullptr;
int g_state = 0;   /* 0 = untried, 1 = available, -1 = unavailable */
std::string g_dir;

HMODULE load_from(const std::string &dir, const char *name)
{
    if (dir.empty()) return LoadLibraryA(name);
    std::string path = dir;
    if (path.back() != '\\' && path.back() != '/') path += '\\';
    path += name;
    return LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

bool ensure_loaded()
{
    if (g_state != 0) return g_state == 1;
    g_state = -1;
    /* dxil.dll first: DXC picks it up from the same directory to sign the
     * DXIL; without it the runtime rejects the shader as unsigned. */
    g_dxil = load_from(g_dir, "dxil.dll");
    if (!g_dxil) return false;
    g_dxcompiler = load_from(g_dir, "dxcompiler.dll");
    if (!g_dxcompiler) return false;
    g_create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(g_dxcompiler, "DxcCreateInstance"));
    if (!g_create) return false;
    g_state = 1;
    return true;
}

std::wstring widen(const char *s)
{
    std::wstring out;
    if (!s) return out;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return out;
    out.resize(static_cast<size_t>(n - 1));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    return out;
}

} // namespace

extern "C" void vio_dxc_set_dir(const char *dir)
{
    g_dir = dir ? dir : "";
    g_state = 0;   /* re-probe with the new directory */
}

extern "C" int vio_dxc_available(void)
{
    return ensure_loaded() ? 1 : 0;
}

extern "C" int vio_dxc_compile(const char *hlsl, const char *entry, const char *profile, int debug,
                               void **out_bytes, size_t *out_len, char **out_error)
{
    if (out_bytes) *out_bytes = nullptr;
    if (out_len) *out_len = 0;
    if (out_error) *out_error = nullptr;
    if (!hlsl || !ensure_loaded()) return -1;

    IDxcUtils *utils = nullptr;
    IDxcCompiler3 *compiler = nullptr;
    if (FAILED(g_create(CLSID_DxcUtils, __uuidof(IDxcUtils), reinterpret_cast<void **>(&utils))) || !utils) return -1;
    if (FAILED(g_create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), reinterpret_cast<void **>(&compiler))) || !compiler) {
        utils->Release();
        return -1;
    }

    std::wstring wentry = widen(entry ? entry : "main");
    std::wstring wprofile = widen(profile ? profile : "vs_6_0");
    std::vector<LPCWSTR> args;
    args.push_back(L"-E"); args.push_back(wentry.c_str());
    args.push_back(L"-T"); args.push_back(wprofile.c_str());
    if (debug) {
        args.push_back(L"-Zi");
        args.push_back(L"-Od");
        args.push_back(L"-Qembed_debug");
    } else {
        args.push_back(L"-O3");
    }
    /* SPIRV-Cross emits SM 5.1 style HLSL; keep DXC's stricter defaults but
     * allow the legacy resource binding shape it produces. */
    args.push_back(L"-HV"); args.push_back(L"2018");

    DxcBuffer src = {};
    src.Ptr = hlsl;
    src.Size = std::strlen(hlsl);
    src.Encoding = DXC_CP_UTF8;

    IDxcResult *result = nullptr;
    HRESULT hr = compiler->Compile(&src, args.data(), static_cast<UINT32>(args.size()), nullptr,
                                   __uuidof(IDxcResult), reinterpret_cast<void **>(&result));
    int rc = -1;
    if (SUCCEEDED(hr) && result) {
        HRESULT status = E_FAIL;
        result->GetStatus(&status);
        IDxcBlobUtf8 *errors = nullptr;
        if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), reinterpret_cast<void **>(&errors), nullptr))
            && errors && errors->GetStringLength() > 0 && out_error) {
            size_t n = errors->GetStringLength();
            *out_error = static_cast<char *>(std::malloc(n + 1));
            if (*out_error) { std::memcpy(*out_error, errors->GetStringPointer(), n); (*out_error)[n] = '\0'; }
        }
        if (errors) errors->Release();
        if (SUCCEEDED(status)) {
            IDxcBlob *object = nullptr;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), reinterpret_cast<void **>(&object), nullptr))
                && object && object->GetBufferSize() > 0) {
                size_t n = object->GetBufferSize();
                void *buf = std::malloc(n);
                if (buf) {
                    std::memcpy(buf, object->GetBufferPointer(), n);
                    if (out_bytes) *out_bytes = buf; else std::free(buf);
                    if (out_len) *out_len = n;
                    rc = 0;
                }
            }
            if (object) object->Release();
        }
        result->Release();
    }
    compiler->Release();
    utils->Release();
    return rc;
}
