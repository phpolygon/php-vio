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
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
int  vio_dxc_available(void);
void vio_dxc_set_dir(const char *dir);
int  vio_dxc_compile(const char *hlsl, const char *entry, const char *profile, int debug,
                     int enable_16bit, void **out_bytes, size_t *out_len, char **out_error);
int  vio_dxc_highest_minor(int max_minor);
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
    /* A module of the same base name that is already loaded wins over the
     * path; this only matters if dxc_dir changes within the process. */
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
    std::string next = dir ? dir : "";
    if (next == g_dir) return;
    g_dir = next;
    /* Re-probe with the new directory; drop what the old one loaded. */
    if (g_dxcompiler) { FreeLibrary(g_dxcompiler); g_dxcompiler = nullptr; }
    if (g_dxil) { FreeLibrary(g_dxil); g_dxil = nullptr; }
    g_create = nullptr;
    g_state = 0;
}

extern "C" int vio_dxc_available(void)
{
    return ensure_loaded() ? 1 : 0;
}

extern "C" int vio_dxc_compile(const char *hlsl, const char *entry, const char *profile, int debug,
                               int enable_16bit, void **out_bytes, size_t *out_len, char **out_error)
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
    /* A library (lib_6_x, ray tracing) has exports instead of one entry point. */
    if (!profile || std::strncmp(profile, "lib_", 4) != 0) {
        args.push_back(L"-E"); args.push_back(wentry.c_str());
    } else {
        /* From lib_6_7 on DXC requires [raypayload] + read/write qualifiers on
         * every payload struct; neither SPIRV-Cross nor GLSL ray payloads carry
         * them, so keep the pre-6.7 payload rules. */
        args.push_back(L"-disable-payload-qualifiers");
    }
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
    /* `half` / int16_t are real 16-bit types (SM 6.2+, VIO_FEATURE_SHADER_FLOAT16). */
    if (enable_16bit) args.push_back(L"-enable-16bit-types");

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

/* Highest Shader Model 6 minor version (0..max_minor) this DXC + dxil.dll pair
 * compiles and validates: a newer profile than the validator knows fails to
 * sign, so the trivial compute shader goes through the full compile. -1 when
 * not even cs_6_0 works. */
extern "C" int vio_dxc_highest_minor(int max_minor)
{
    static const char *probe = "[numthreads(1, 1, 1)] void main() {}\n";
    for (int minor = max_minor; minor >= 0; minor--) {
        char profile[16];
        std::snprintf(profile, sizeof(profile), "cs_6_%d", minor);
        void *bytes = nullptr; size_t len = 0; char *err = nullptr;
        int rc = vio_dxc_compile(probe, "main", profile, 0, 0, &bytes, &len, &err);
        std::free(bytes);
        std::free(err);
        if (rc == 0) return minor;
    }
    return -1;
}
