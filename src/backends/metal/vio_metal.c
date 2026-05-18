/*
 * Build-system shim — see Issue #2.
 *
 * PHP's PHP_ADD_SOURCES_X macro only recognises .c/.cpp/.s/.S sources,
 * not Objective-C .m. Routing the Metal backend through a .c file lets
 * us reuse the standard PHP build machinery so the object lands in
 * PHP_GLOBAL_OBJS for static builds and shared_objects_vio for shared
 * builds — without the Makefile.frag bypass that orphans the object
 * in static-php-cli setups.
 *
 * config.m4 passes "-x objective-c -fobjc-arc" as per-source flags,
 * which makes Clang compile this file as Objective-C with ARC even
 * though its filename says .c. The actual implementation stays in
 * vio_metal.m so editors keep recognising it as ObjC.
 */
#include "vio_metal.m"
