/*
 * vio_thermal.h - host thermal state lookup
 *
 * Returns the OS-reported thermal pressure as a stable string token so PHP
 * land can map it onto its own enum without speaking ObjC. macOS / iOS
 * read NSProcessInfo.thermalState; every other host returns "unknown".
 */
#ifndef VIO_THERMAL_H
#define VIO_THERMAL_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One of "nominal", "fair", "serious", "critical", "unknown". The pointer
 * refers to a static string literal and must not be freed by the caller.
 */
const char *vio_get_thermal_state_str(void);

#ifdef __cplusplus
}
#endif

#endif /* VIO_THERMAL_H */
