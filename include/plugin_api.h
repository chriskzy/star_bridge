#ifndef PLUGIN_API_H
#define PLUGIN_API_H

#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/*  Plugin API for external harness integration                        */
/* ------------------------------------------------------------------ *
 *  Plugins allow external harnesses to register themselves with the
 *  bridge, providing lifecycle hooks (init, run, cleanup) and optional
 *  tool execution callbacks.
 *
 *  Usage:
 *    1. Define a PluginInstance with your hooks.
 *    2. Call plugin_register() to register it with a unique name.
 *    3. The bridge will invoke hooks at the appropriate lifecycle
 *       points (startup, per-request, shutdown).
 *
 *  Example plugin in src/plugin_example.c
 */

/* Maximum plugin name length */
#define PLUGIN_NAME_MAX  64

/* Maximum number of concurrently registered plugins */
#define PLUGIN_MAX       16

/* ------------------------------------------------------------------ */
/*  Plugin version / capability flags                                  */
/* ------------------------------------------------------------------ */
typedef enum {
    PLUGIN_CAP_NONE         = 0,
    PLUGIN_CAP_TOOL_EXEC    = (1 << 0),  /* plugin can handle tool execution */
    PLUGIN_CAP_HARNESS      = (1 << 1),  /* plugin acts as a harness adapter */
    PLUGIN_CAP_FILTER       = (1 << 2),  /* plugin can filter/modify requests */
} PluginCapability;

/* ------------------------------------------------------------------ */
/*  Plugin lifecycle hook return codes                                 */
/* ------------------------------------------------------------------ */
typedef enum {
    PLUGIN_OK    = 0,
    PLUGIN_SKIP  = 1,   /* hook declined (caller should proceed as normal) */
    PLUGIN_ERR   = 2,   /* hook failed unrecoverably */
} PluginStatus;

/* ------------------------------------------------------------------ */
/*  Plugin instance — filled in by the plugin author                   */
/* ------------------------------------------------------------------ */
typedef struct {
    char     name[PLUGIN_NAME_MAX];
    unsigned version;              /* plugin version number (increment on changes) */
    PluginCapability capabilities; /* bitmask of PLUGIN_CAP_* flags */

    /* Lifecycle hooks */

    /* Called once at bridge startup after config is loaded.
     * Return PLUGIN_OK to continue, PLUGIN_ERR to abort startup. */
    PluginStatus (*on_init)(void);

    /* Called before each request is processed.
     * Return PLUGIN_OK to proceed, PLUGIN_SKIP to skip plugin processing,
     * PLUGIN_ERR to fail the request. */
    PluginStatus (*on_request)(const char *request_id, const char *input_text);

    /* Called after each request completes.
     * Return PLUGIN_OK / PLUGIN_SKIP / PLUGIN_ERR. */
    PluginStatus (*on_response)(const char *request_id, const char *response_text);

    /* Called on bridge shutdown for cleanup.
     * Return PLUGIN_OK / PLUGIN_ERR. */
    PluginStatus (*on_cleanup)(void);

    /* Optional: tool execution callback.
     * If PLUGIN_CAP_TOOL_EXEC is set, the bridge will call this when
     * a tool execution is requested.
     * Return true if handled, false to fall back to default tool runner. */
    bool (*on_tool_exec)(const char *tool_name,
                         const char *argument_json,
                         char *result_json,
                         size_t result_json_max);

    /* Opaque user data pointer (not inspected by the bridge) */
    void *user_data;
} PluginInstance;

/* ------------------------------------------------------------------ */
/*  Registration API                                                   */
/* ------------------------------------------------------------------ */

/* Register a plugin. name must be unique and non-empty.
 * Returns true on success, false if name already registered or limit reached. */
bool plugin_register(const PluginInstance *plugin);

/* Unregister a plugin by name. Returns true if found and removed. */
bool plugin_unregister(const char *name);

/* Look up a registered plugin by name. Returns NULL if not found. */
PluginInstance *plugin_lookup(const char *name);

/* ------------------------------------------------------------------ */
/*  Lifecycle dispatch — called by the bridge                          */
/* ------------------------------------------------------------------ */

/* Call on_init for all registered plugins. Returns PLUGIN_ERR if any fail. */
PluginStatus plugin_dispatch_init(void);

/* Call on_request for all registered plugins (in registration order). */
PluginStatus plugin_dispatch_request(const char *request_id, const char *input_text);

/* Call on_response for all registered plugins (in registration order). */
PluginStatus plugin_dispatch_response(const char *request_id, const char *response_text);

/* Call on_cleanup for all registered plugins (in reverse order). */
PluginStatus plugin_dispatch_cleanup(void);

/* Try to dispatch a tool execution to plugins with PLUGIN_CAP_TOOL_EXEC.
 * Returns true if a plugin handled it, false to fall back to default runner. */
bool plugin_dispatch_tool_exec(const char *tool_name,
                               const char *argument_json,
                               char *result_json,
                               size_t result_json_max);

/* ------------------------------------------------------------------ */
/*  Plugin enumeration                                                 */
/* ------------------------------------------------------------------ */

/* Return the number of registered plugins */
int plugin_count(void);

/* Return the plugin at index i (0 .. plugin_count()-1). Returns NULL if out of range. */
PluginInstance *plugin_at(int index);

#endif /* PLUGIN_API_H */
