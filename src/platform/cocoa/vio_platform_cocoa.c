/*
 * Build-system shim, as src/backends/metal/vio_metal.c: PHP_ADD_SOURCES_X
 * knows no .m sources, so config.m4 compiles this file with
 * "-x objective-c -fobjc-arc" and the implementation stays in
 * vio_platform_cocoa.m.
 */
#include "vio_platform_cocoa.m"
