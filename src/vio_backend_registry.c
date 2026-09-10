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
    /* Platform-specific priority:
     * macOS:   metal > opengl (Vulkan via MoltenVK is opt-in, not auto)
     * Windows: d3d12 > d3d11 > vulkan > opengl
     * Linux:   vulkan > opengl
     */
#ifdef __APPLE__
    const char *priority[] = {"metal", "opengl"};
    int priority_count = 2;
#elif defined(_WIN32)
    const char *priority[] = {"d3d12", "d3d11", "vulkan", "opengl"};
    int priority_count = 4;
#else
    const char *priority[] = {"vulkan", "opengl"};
    int priority_count = 2;
#endif
    /* First pass: the highest-priority backend that can actually draw 3D.
     * A registered backend whose supports_feature() reports no
     * VIO_FEATURE_3D_PIPELINE (today: Vulkan — vulkan_create_pipeline returns
     * NULL, so vio_pipeline()/vio_draw() would silently render nothing) must not
     * win "auto" over one that can, or a Linux box with a Vulkan ICD would get a
     * 2D-only context for a 3D game. supports_feature is a static table on every
     * backend, so this costs nothing and needs no device. Once a backend gains a
     * 3D pipeline it automatically becomes eligible again. */
    /* Pass 0 (GAP-PHASE5 Block 10): prefer a backend with the full 3D feature set an
     * engine renderer needs (HDR / depth / cube targets, MRT, cubemaps, instancing),
     * so a backend whose 3D path is still growing (Vulkan while Block 10 lands in
     * steps) never wins 'auto' over a complete one. */
    for (int p = 0; p < priority_count; p++) {
        const vio_backend *b = vio_find_backend(priority[p]);
        if (b && b->supports_feature && b->supports_feature(VIO_FEATURE_3D_PIPELINE)
            && b->supports_feature(VIO_FEATURE_INSTANCED_DRAW) && b->supports_feature(VIO_FEATURE_RENDER_TARGET_HDR)
            && b->supports_feature(VIO_FEATURE_RENDER_TARGET_DEPTH) && b->supports_feature(VIO_FEATURE_RENDER_TARGET_CUBE)
            && b->supports_feature(VIO_FEATURE_MRT) && b->supports_feature(VIO_FEATURE_CUBEMAP)) {
            return b;
        }
    }
    for (int p = 0; p < priority_count; p++) {
        const vio_backend *b = vio_find_backend(priority[p]);
        if (b && b->supports_feature && b->supports_feature(VIO_FEATURE_3D_PIPELINE)) {
            return b;
        }
    }

    /* Second pass: plain priority order (2D-only deployments, e.g. a build with
     * only the Vulkan backend). */
    for (int p = 0; p < priority_count; p++) {
        const vio_backend *b = vio_find_backend(priority[p]);
        if (b) {
            return b;
        }
    }

    /* Fall back to the first registered backend that has a 3D pipeline, then
     * to the first registered backend at all. */
    for (int i = 0; i < backend_count; i++) {
        const vio_backend *b = backends[i];
        if (b->supports_feature && b->supports_feature(VIO_FEATURE_3D_PIPELINE)) {
            return b;
        }
    }
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
