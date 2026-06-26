#include "plugin_api.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Plugin registry — simple fixed-size array                          */
/* ------------------------------------------------------------------ */

static PluginInstance s_plugins[PLUGIN_MAX];
static int s_plugin_count = 0;

/* ------------------------------------------------------------------ */
/*  Registration                                                       */
/* ------------------------------------------------------------------ */

bool plugin_register(const PluginInstance *plugin) {
    if (!plugin || !plugin->name[0]) return false;

    /* Check for duplicate name */
    for (int i = 0; i < s_plugin_count; i++) {
        if (strcmp(s_plugins[i].name, plugin->name) == 0)
            return false;
    }

    /* Check capacity */
    if (s_plugin_count >= PLUGIN_MAX) return false;

    /* Copy into registry */
    s_plugins[s_plugin_count] = *plugin;
    /* Ensure name is null-terminated */
    s_plugins[s_plugin_count].name[PLUGIN_NAME_MAX - 1] = '\0';
    s_plugin_count++;
    return true;
}

bool plugin_unregister(const char *name) {
    if (!name || !name[0]) return false;
    for (int i = 0; i < s_plugin_count; i++) {
        if (strcmp(s_plugins[i].name, name) == 0) {
            /* Shift remaining plugins down */
            for (int j = i; j < s_plugin_count - 1; j++)
                s_plugins[j] = s_plugins[j + 1];
            s_plugin_count--;
            return true;
        }
    }
    return false;
}

PluginInstance *plugin_lookup(const char *name) {
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < s_plugin_count; i++) {
        if (strcmp(s_plugins[i].name, name) == 0)
            return &s_plugins[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Lifecycle dispatch                                                 */
/* ------------------------------------------------------------------ */

PluginStatus plugin_dispatch_init(void) {
    for (int i = 0; i < s_plugin_count; i++) {
        if (s_plugins[i].on_init) {
            PluginStatus st = s_plugins[i].on_init();
            if (st == PLUGIN_ERR) return PLUGIN_ERR;
        }
    }
    return PLUGIN_OK;
}

PluginStatus plugin_dispatch_request(const char *request_id, const char *input_text) {
    for (int i = 0; i < s_plugin_count; i++) {
        if (s_plugins[i].on_request) {
            PluginStatus st = s_plugins[i].on_request(request_id, input_text);
            if (st == PLUGIN_ERR) return PLUGIN_ERR;
            /* PLUGIN_SKIP means skip remaining plugins for this hook */
            if (st == PLUGIN_SKIP) break;
        }
    }
    return PLUGIN_OK;
}

PluginStatus plugin_dispatch_response(const char *request_id, const char *response_text) {
    for (int i = 0; i < s_plugin_count; i++) {
        if (s_plugins[i].on_response) {
            PluginStatus st = s_plugins[i].on_response(request_id, response_text);
            if (st == PLUGIN_ERR) return PLUGIN_ERR;
            if (st == PLUGIN_SKIP) break;
        }
    }
    return PLUGIN_OK;
}

PluginStatus plugin_dispatch_cleanup(void) {
    /* Dispatch in reverse order */
    for (int i = s_plugin_count - 1; i >= 0; i--) {
        if (s_plugins[i].on_cleanup) {
            PluginStatus st = s_plugins[i].on_cleanup();
            if (st == PLUGIN_ERR) return PLUGIN_ERR;
        }
    }
    return PLUGIN_OK;
}

bool plugin_dispatch_tool_exec(const char *tool_name,
                               const char *argument_json,
                               char *result_json,
                               size_t result_json_max) {
    for (int i = 0; i < s_plugin_count; i++) {
        if (s_plugins[i].capabilities & PLUGIN_CAP_TOOL_EXEC) {
            if (s_plugins[i].on_tool_exec) {
                bool handled = s_plugins[i].on_tool_exec(tool_name, argument_json,
                                                          result_json, result_json_max);
                if (handled) return true;
            }
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Enumeration                                                        */
/* ------------------------------------------------------------------ */

int plugin_count(void) {
    return s_plugin_count;
}

PluginInstance *plugin_at(int index) {
    if (index < 0 || index >= s_plugin_count) return NULL;
    return &s_plugins[index];
}
