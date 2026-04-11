/*
 * php-vio - Backend registry implementation
 */

#include "vio_backend_registry.h"
#include <string.h>

static const vio_backend *backends[VIO_MAX_BACKENDS];
static int backend_count = 0;

void vio_backend_registry_init(void)
{
    memset(backends, 0, sizeof(backends));
    backend_count = 0;
}

void vio_backend_registry_shutdown(void)
{
    backend_count = 0;
}

int vio_register_backend(const vio_backend *backend)
{
    if (!backend || !backend->name) {
        return -1;
    }

    if (backend->api_version != VIO_BACKEND_API_VERSION) {
        return -1;
    }

    if (backend_count >= VIO_MAX_BACKENDS) {
        return -1;
    }

    /* Check for duplicate registration */
    for (int i = 0; i < backend_count; i++) {
        if (strcmp(backends[i]->name, backend->name) == 0) {
            return -1;
        }
    }

    backends[backend_count++] = backend;
    return 0;
}

const vio_backend *vio_find_backend(const char *name)
{
    if (!name) {
        return NULL;
    }

    for (int i = 0; i < backend_count; i++) {
        if (strcmp(backends[i]->name, name) == 0) {
            return backends[i];
        }
    }

    return NULL;
}

const vio_backend *vio_get_auto_backend(void)
{
    /* Priority: vulkan > metal > opengl */
    const char *priority[] = {"vulkan", "metal", "opengl"};
    for (int p = 0; p < 3; p++) {
        const vio_backend *b = vio_find_backend(priority[p]);
        if (b) {
            return b;
        }
    }

    /* Fall back to first registered backend */
    if (backend_count > 0) {
        return backends[0];
    }

    return NULL;
}

int vio_backend_count(void)
{
    return backend_count;
}

const char *vio_get_backend_name(int index)
{
    if (index < 0 || index >= backend_count) return NULL;
    return backends[index]->name;
}
