/*
 * Build-system shim - see Metal backend for the same pattern.
 *
 * PHP's PHP_ADD_SOURCES_X macro only recognises .c/.cpp/.s/.S sources,
 * not Objective-C .m. Routing the iOS backend through a .c file lets us
 * reuse the standard PHP build machinery so the object lands in
 * PHP_GLOBAL_OBJS for static builds (the iOS target) and stays out of
 * shared builds (the macOS dev workflow).
 *
 * config.m4 passes "-x objective-c -fobjc-arc" as per-source flags,
 * which makes Clang compile this file as Objective-C with ARC even
 * though its filename says .c. The implementation lives in vio_ios.m
 * so editors keep recognising it as Objective-C.
 */
#include "vio_ios.m"
