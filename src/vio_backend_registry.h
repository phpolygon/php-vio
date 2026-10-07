/*
 * php-vio - Backend registry internals
 */

#ifndef VIO_BACKEND_REGISTRY_H
#define VIO_BACKEND_REGISTRY_H

#include "../include/vio_backend.h"

void vio_backend_registry_init(void);
void vio_backend_registry_shutdown(void);

/* 'auto' with prefer / require (OPEN-ITEMS-PLAN A7, BACKEND-SELECTION-PLAN
 * Phase 1): one candidate per backend with its preferred adapter. */
enum { VIO_PREFER_PERFORMANCE = 0, VIO_PREFER_QUALITY = 1, VIO_PREFER_COMPAT = 2 };
enum { VIO_PLATFORM_WINDOWS = 0, VIO_PLATFORM_MACOS = 1, VIO_PLATFORM_LINUX = 2 };

typedef struct _vio_select_candidate {
    char               backend[24];
    const vio_backend *be;          /* NULL: simulated (VIO_TEST_ADAPTERS), not registered */
    vio_adapter_info   adapter;
    int                has_adapter; /* 0: the backend cannot tell before a context (OpenGL) */
    int                score;
    int                eligible;
    char               reason[48];  /* why not eligible */
} vio_select_candidate;

int vio_select_host_platform(void);
/* Score, filter by `require` (VIO_FEATURE_BIT mask) and sort: eligible first,
 * then by score, ties in the given order. */
void vio_select_rank(vio_select_candidate *c, int n, int platform, int prefer, uint64_t require);

#endif /* VIO_BACKEND_REGISTRY_H */
