/*
 * php-vio - PHP Video Input Output
 * Plugin registry implementation
 */

#include "include/vio_plugin.h"
#include <string.h>
#include <stdio.h>

static const vio_plugin *registered_plugins[VIO_MAX_PLUGINS];
static int plugin_count = 0;
static int registry_initialized = 0;

int vio_plugin_registry_init(void)
{
    if (registry_initialized) {
        return 0;
    }
    memset(registered_plugins, 0, sizeof(registered_plugins));
    plugin_count = 0;
    registry_initialized = 1;
    return 0;
}

void vio_plugin_registry_shutdown(void)
{
    if (!registry_initialized) {
        return;
    }
    for (int i = 0; i < plugin_count; i++) {
        if (registered_plugins[i] && registered_plugins[i]->shutdown) {
            registered_plugins[i]->shutdown();
        }
        registered_plugins[i] = NULL;
    }
    plugin_count = 0;
    registry_initialized = 0;
}

int vio_register_plugin(const vio_plugin *plugin)
{
    if (!registry_initialized) {
        return -1;
    }
    if (!plugin || !plugin->name) {
        return -1;
    }
    if (plugin->api_version != VIO_PLUGIN_API_VERSION) {
        fprintf(stderr, "vio: plugin '%s' has API version %d, expected %d\n",
                plugin->name, plugin->api_version, VIO_PLUGIN_API_VERSION);
        return -1;
    }
    if (plugin_count >= VIO_MAX_PLUGINS) {
        fprintf(stderr, "vio: maximum plugin count (%d) reached\n", VIO_MAX_PLUGINS);
        return -1;
    }
    /* Check for duplicate name */
    for (int i = 0; i < plugin_count; i++) {
        if (strcmp(registered_plugins[i]->name, plugin->name) == 0) {
            fprintf(stderr, "vio: plugin '%s' already registered\n", plugin->name);
            return -1;
        }
    }
    registered_plugins[plugin_count] = plugin;
    plugin_count++;

    /* Call init if provided */
    if (plugin->init) {
        int ret = plugin->init();
        if (ret != 0) {
            /* Roll back registration on init failure */
            plugin_count--;
            registered_plugins[plugin_count] = NULL;
            return -1;
        }
    }

    return 0;
}

const vio_plugin *vio_find_plugin(const char *name)
{
    if (!name || !registry_initialized) {
        return NULL;
    }
    for (int i = 0; i < plugin_count; i++) {
        if (strcmp(registered_plugins[i]->name, name) == 0) {
            return registered_plugins[i];
        }
    }
    return NULL;
}

int vio_plugin_count(void)
{
    return plugin_count;
}

const char *vio_get_plugin_name(int index)
{
    if (index < 0 || index >= plugin_count) {
        return NULL;
    }
    return registered_plugins[index]->name;
}

const vio_plugin *vio_get_plugin(int index)
{
    if (index < 0 || index >= plugin_count) {
        return NULL;
    }
    return registered_plugins[index];
}
