/*
 * vio_thermal.c - cross-platform NSProcessInfo.thermalState bridge.
 *
 * Implemented in plain C so it compiles unchanged on Linux/Windows hosts
 * where the symbol is still needed (returns "unknown") but no Objective-C
 * compiler is available. On Apple platforms we drive the ObjC runtime
 * directly through objc_msgSend, which avoids the .m / -fobjc-arc build
 * shim used by the Metal backend.
 *
 * Cocoa (or UIKit on iOS) is already linked unconditionally on macOS/iOS,
 * which pulls Foundation in, so NSProcessInfo is reachable without an
 * extra PHP_ADD_FRAMEWORK call from config.m4.
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

#ifdef VIO_THERMAL_APPLE
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

#else /* non-Apple */

const char *vio_get_thermal_state_str(void)
{
    return "unknown";
}

#endif
