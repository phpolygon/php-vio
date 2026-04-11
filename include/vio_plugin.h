/*
 * php-vio - PHP Video Input Output
 * Plugin system types and registration API
 */

#ifndef VIO_PLUGIN_H
#define VIO_PLUGIN_H

#include <stddef.h>

#define VIO_PLUGIN_API_VERSION 1
#define VIO_MAX_PLUGINS 32

/* ── Plugin type flags ───────────────────────────────────────────── */

#define VIO_PLUGIN_TYPE_GENERIC  0
#define VIO_PLUGIN_TYPE_OUTPUT   1  /* Can receive frames (recording, streaming, etc.) */
#define VIO_PLUGIN_TYPE_INPUT    2  /* Can inject input events */
#define VIO_PLUGIN_TYPE_FILTER   4  /* Can process frames in pipeline */

/* ── Plugin struct ───────────────────────────────────────────────── */

typedef struct _vio_plugin {
    const char *name;
    const char *description;
    const char *version;
    int         api_version;    /* Must match VIO_PLUGIN_API_VERSION */
    int         type;           /* Bitmask of VIO_PLUGIN_TYPE_* */

    /* Lifecycle */
    int   (*init)(void);
    void  (*shutdown)(void);

    /* Output plugin callbacks (type & VIO_PLUGIN_TYPE_OUTPUT) */
    int   (*output_start)(const char *target, int width, int height, int fps);
    int   (*output_push_frame)(const unsigned char *rgba, int width, int height);
    void  (*output_stop)(void);

    /* Input plugin callbacks (type & VIO_PLUGIN_TYPE_INPUT) */
    int   (*input_poll)(void *events, int max_events);

    /* Filter plugin callbacks (type & VIO_PLUGIN_TYPE_FILTER) */
    int   (*filter_process)(unsigned char *rgba, int width, int height);

    /* User data */
    void *user_data;
} vio_plugin;

/* ── Registry API (C-level, for native plugins) ──────────────────── */

int  vio_plugin_registry_init(void);
void vio_plugin_registry_shutdown(void);
int  vio_register_plugin(const vio_plugin *plugin);
const vio_plugin *vio_find_plugin(const char *name);
int  vio_plugin_count(void);
const char *vio_get_plugin_name(int index);
const vio_plugin *vio_get_plugin(int index);

#endif /* VIO_PLUGIN_H */
