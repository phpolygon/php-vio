/*
 * php-vio - Platform registry (NATIVE-PLATFORM-PLAN, OPEN-ITEMS A1)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"   /* the build's HAVE_* switches (config.w32.h / php_config.h) */
#include <stdlib.h>
#include <string.h>
#include "../include/vio_platform.h"
#include "vio_window.h"

#define VIO_MAX_PLATFORMS 8

static const vio_platform *vio_platforms[VIO_MAX_PLATFORMS];
static int                 vio_platform_count = 0;
static const vio_platform *vio_platform_current = NULL;

int vio_register_platform(const vio_platform *p)
{
    if (!p || !p->name || vio_platform_count >= VIO_MAX_PLATFORMS) return -1;
    for (int i = 0; i < vio_platform_count; i++) {
        if (strcmp(vio_platforms[i]->name, p->name) == 0) return 0;
    }
    vio_platforms[vio_platform_count++] = p;
    return 0;
}

const vio_platform *vio_find_platform(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < vio_platform_count; i++) {
        if (strcmp(vio_platforms[i]->name, name) == 0) return vio_platforms[i];
    }
    return NULL;
}

/* VIO_PLATFORM=<name> picks one explicitly; otherwise the native platform of
 * the OS when it is built in, then GLFW, then null. */
const vio_platform *vio_platform_active(void)
{
    if (vio_platform_current) return vio_platform_current;
    const char *env = getenv("VIO_PLATFORM");
    const vio_platform *p = (env && *env) ? vio_find_platform(env) : NULL;
    static const char *order[] = { "win32", "cocoa", "x11", "glfw", "null" };
    for (size_t i = 0; !p && i < sizeof(order) / sizeof(order[0]); i++) p = vio_find_platform(order[i]);
    if (!p) {
        vio_platform_null_register();
        p = vio_find_platform("null");
    }
    vio_platform_current = p;
    return p;
}

void vio_platform_set_active(const vio_platform *p)
{
    vio_platform_current = p;
}

/* MINIT / MSHUTDOWN (declared in src/vio_window.h): the built-in platforms
 * register, the active one starts. Without any window system the null
 * platform is active and init succeeds. */
int vio_window_init(void)
{
    vio_platform_null_register();
#ifdef HAVE_GLFW
    vio_platform_glfw_register();
#endif
#ifdef _WIN32
    vio_platform_win32_register();
#endif
#ifdef HAVE_X11
    vio_platform_x11_register();
#endif
#if defined(HAVE_COCOA) && !defined(HAVE_IOS)
    vio_platform_cocoa_register();
#endif
    vio_platform_current = NULL;   /* choose among everything registered now */
    return vio_platform_active()->init();
}

void vio_window_shutdown(void)
{
    vio_platform_active()->shutdown();
}
