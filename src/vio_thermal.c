/*
 * vio_thermal.c - cross-platform thermal-state bridge.
 *
 * Exposes a single state string ("nominal" / "fair" / "serious" / "critical" /
 * "unknown") consumed by the engine's ThermalMonitor (ThermalSourceOs).
 *
 *   - macOS/iOS: NSProcessInfo.thermalState via the ObjC runtime (a real,
 *     OS-computed whole-system heat-pressure level).
 *   - Linux: the hottest /sys/class/thermal/thermal_zoneN/temp sensor, mapped
 *     to a pressure level by temperature thresholds.
 *   - Windows: WMI MSAcpi_ThermalZoneTemperature (the only vendor-neutral path
 *     without a kernel sensor driver). Best-effort: many laptops don't expose
 *     it or gate it behind admin, in which case we return "unknown" and the
 *     ThermalMonitor falls back to its frametime guard.
 *   - Anything else: "unknown".
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_thermal.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_OSX || TARGET_OS_IOS
#define VIO_THERMAL_APPLE 1
#endif
#endif

/* ── Temperature → pressure-level mapping (Linux + Windows) ───────────────
 * Heuristic thresholds tuned for laptop CPU/GPU sensors, which routinely sit
 * at 70-90 °C under sustained load. "critical" is genuine throttling territory.
 */
#if defined(__linux__) || defined(_WIN32)
static const char *vio_temp_to_state(double celsius)
{
    if (celsius >= 95.0) return "critical";
    if (celsius >= 87.0) return "serious";
    if (celsius >= 78.0) return "fair";
    return "nominal";
}
#endif

#ifdef VIO_THERMAL_APPLE
/* ── macOS / iOS ──────────────────────────────────────────────────────── */
#include <objc/runtime.h>
#include <objc/message.h>

const char *vio_get_thermal_state_str(void)
{
    Class processInfoClass = objc_getClass("NSProcessInfo");
    if (!processInfoClass) {
        return "unknown";
    }

    SEL processInfoSel = sel_registerName("processInfo");
    id (*sendInstance)(Class, SEL) = (id (*)(Class, SEL)) objc_msgSend;
    id info = sendInstance(processInfoClass, processInfoSel);
    if (!info) {
        return "unknown";
    }

    SEL thermalStateSel = sel_registerName("thermalState");
    long (*sendThermal)(id, SEL) = (long (*)(id, SEL)) objc_msgSend;
    long state = sendThermal(info, thermalStateSel);

    /* NSProcessInfoThermalState: 0=Nominal, 1=Fair, 2=Serious, 3=Critical */
    switch (state) {
        case 0:  return "nominal";
        case 1:  return "fair";
        case 2:  return "serious";
        case 3:  return "critical";
        default: return "unknown";
    }
}

#elif defined(__linux__)
/* ── Linux ────────────────────────────────────────────────────────────── */
#include <stdio.h>

const char *vio_get_thermal_state_str(void)
{
    /* Scan the ACPI/hwmon thermal zones and keep the hottest plausible reading.
     * Values are in millidegrees Celsius. Zones are sparse, so we probe a fixed
     * range and simply skip the ones that aren't present. */
    double maxC = -1000.0;
    char path[80];

    for (int i = 0; i < 32; i++) {
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", i);
        FILE *f = fopen(path, "r");
        if (!f) {
            continue;
        }
        long milli = 0;
        if (fscanf(f, "%ld", &milli) == 1) {
            double c = (double) milli / 1000.0;
            /* Sanity-bound: ignore obviously bogus sensors (e.g. 0 or >200 °C). */
            if (c > 0.0 && c < 200.0 && c > maxC) {
                maxC = c;
            }
        }
        fclose(f);
    }

    if (maxC <= -1000.0) {
        return "unknown";
    }
    return vio_temp_to_state(maxC);
}

#elif defined(_WIN32)
/* ── Windows ──────────────────────────────────────────────────────────── */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <wbemidl.h>
#include <objbase.h>
#include <oleauto.h>
#include <string.h>

/* ---- NVIDIA NVAPI GPU temperature (real GPU-core sensor) ----------------
 * nvapi64.dll ships with every NVIDIA driver (System32), so we dynamically
 * load it and resolve functions through the single exported entry point
 * nvapi_QueryInterface by their well-known interface ids — no NVAPI SDK or
 * import library required. Returns the hottest GPU core temp in °C, or -1000
 * when no NVIDIA GPU / driver is present. Pointers are cached after the first
 * successful init (polled ~1 Hz from the single render thread). */
typedef void *(__cdecl *vio_nvapi_query_t)(unsigned int);
typedef int (__cdecl *vio_nvapi_init_t)(void);
typedef int (__cdecl *vio_nvapi_enum_t)(void *handles[64], unsigned int *count);

#define VIO_NVAPI_MAX_THERMAL_SENSORS 3
typedef struct {
    unsigned int version;
    unsigned int count;
    struct {
        int controller;
        int defaultMinTemp;
        int defaultMaxTemp;
        int currentTemp;
        int target;
    } sensor[VIO_NVAPI_MAX_THERMAL_SENSORS];
} vio_nv_thermal_t;
typedef int (__cdecl *vio_nvapi_thermal_t)(void *gpu, unsigned int sensorIndex, vio_nv_thermal_t *settings);

static int vio_nvapi_state = 0; /* 0=untried, 1=ready, -1=unavailable */
static vio_nvapi_enum_t vio_nvapi_enum = NULL;
static vio_nvapi_thermal_t vio_nvapi_thermal = NULL;

static int vio_nvapi_gpu_temp_c(void)
{
    if (vio_nvapi_state == 0) {
        vio_nvapi_state = -1; /* assume failure until proven otherwise */
        HMODULE lib = LoadLibraryA("nvapi64.dll");
        if (lib) {
            vio_nvapi_query_t query = (vio_nvapi_query_t) GetProcAddress(lib, "nvapi_QueryInterface");
            if (query) {
                vio_nvapi_init_t init = (vio_nvapi_init_t) query(0x0150E828u);   /* NvAPI_Initialize */
                vio_nvapi_enum = (vio_nvapi_enum_t) query(0xE5AC921Fu);          /* EnumPhysicalGPUs */
                vio_nvapi_thermal = (vio_nvapi_thermal_t) query(0xE3640A56u);    /* GPU_GetThermalSettings */
                if (init && vio_nvapi_enum && vio_nvapi_thermal && init() == 0) {
                    vio_nvapi_state = 1;
                }
            }
        }
    }
    if (vio_nvapi_state != 1) {
        return -1000;
    }

    void *gpus[64];
    unsigned int n = 0;
    if (vio_nvapi_enum(gpus, &n) != 0 || n == 0) {
        return -1000;
    }

    int maxTemp = -1000;
    for (unsigned int i = 0; i < n; i++) {
        vio_nv_thermal_t ts;
        memset(&ts, 0, sizeof(ts));
        ts.version = (unsigned int) sizeof(vio_nv_thermal_t) | (1u << 16); /* V1 */
        /* NVAPI_THERMAL_TARGET_ALL = 15 */
        if (vio_nvapi_thermal(gpus[i], 15u, &ts) == 0 && ts.count > 0) {
            int t = ts.sensor[0].currentTemp;
            if (t > 0 && t < 200 && t > maxTemp) {
                maxTemp = t;
            }
        }
    }
    return maxTemp;
}

/* ---- WMI ACPI thermal zone (fallback; rarely exposed on consumer HW) ---- */
static double vio_wmi_thermal_zone_c(void)
{
    double maxC = -1000.0;

    HRESULT hrInit = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int weInitialised = (hrInit == S_OK || hrInit == S_FALSE);
    if (FAILED(hrInit) && hrInit != RPC_E_CHANGED_MODE) {
        return -1000.0;
    }

    IWbemLocator *locator = NULL;
    IWbemServices *services = NULL;
    IEnumWbemClassObject *enumerator = NULL;

    HRESULT hr = CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IWbemLocator, (void **) &locator);
    if (FAILED(hr) || locator == NULL) {
        goto cleanup;
    }

    BSTR ns = SysAllocString(L"ROOT\\WMI");
    hr = IWbemLocator_ConnectServer(locator, ns, NULL, NULL, NULL, 0, NULL, NULL, &services);
    SysFreeString(ns);
    if (FAILED(hr) || services == NULL) {
        goto cleanup;
    }

    CoSetProxyBlanket((IUnknown *) services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);

    BSTR lang = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature");
    hr = IWbemServices_ExecQuery(services, lang, query,
                                 WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                 NULL, &enumerator);
    SysFreeString(lang);
    SysFreeString(query);
    if (FAILED(hr) || enumerator == NULL) {
        goto cleanup;
    }

    for (;;) {
        IWbemClassObject *obj = NULL;
        ULONG returned = 0;
        HRESULT hrNext = IEnumWbemClassObject_Next(enumerator, WBEM_INFINITE, 1, &obj, &returned);
        if (hrNext != S_OK || returned == 0 || obj == NULL) {
            break;
        }

        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(IWbemClassObject_Get(obj, L"CurrentTemperature", 0, &v, NULL, NULL))
            && (v.vt == VT_I4 || v.vt == VT_UI4)) {
            long tenthsKelvin = (v.vt == VT_I4) ? v.lVal : (long) v.ulVal;
            double c = ((double) tenthsKelvin / 10.0) - 273.15;
            if (c > 0.0 && c < 200.0 && c > maxC) {
                maxC = c;
            }
        }
        VariantClear(&v);
        IWbemClassObject_Release(obj);
    }

cleanup:
    if (enumerator) IEnumWbemClassObject_Release(enumerator);
    if (services) IWbemServices_Release(services);
    if (locator) IWbemLocator_Release(locator);
    if (weInitialised) CoUninitialize();

    return maxC;
}

const char *vio_get_thermal_state_str(void)
{
    /* Prefer a real GPU-core sensor (NVIDIA NVAPI), then the WMI ACPI thermal
     * zone, then give up so the ThermalMonitor's frametime guard takes over. */
    int gpuTemp = vio_nvapi_gpu_temp_c();
    if (gpuTemp > -1000) {
        return vio_temp_to_state((double) gpuTemp);
    }
    double wmiC = vio_wmi_thermal_zone_c();
    if (wmiC > -1000.0) {
        return vio_temp_to_state(wmiC);
    }
    return "unknown";
}

#else
/* ── Unsupported platform ─────────────────────────────────────────────── */
const char *vio_get_thermal_state_str(void)
{
    return "unknown";
}

#endif
