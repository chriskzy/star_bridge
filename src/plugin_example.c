/* ------------------------------------------------------------------ *
 *  Example plugin demonstrating the Plugin API.
 *  This plugin logs lifecycle events and provides a simple
 *  "echo" tool execution handler.
 * ------------------------------------------------------------------ */

#include "plugin_api.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Lifecycle hooks                                                    */
/* ------------------------------------------------------------------ */

static PluginStatus example_on_init(void) {
    fprintf(stderr, "[plugin_example] init OK\n");
    return PLUGIN_OK;
}

static PluginStatus example_on_request(const char *request_id, const char *input_text) {
    fprintf(stderr, "[plugin_example] request %s: %zu bytes\n",
            request_id, input_text ? strlen(input_text) : 0);
    return PLUGIN_OK;
}

static PluginStatus example_on_response(const char *request_id, const char *response_text) {
    fprintf(stderr, "[plugin_example] response %s: %zu bytes\n",
            request_id, response_text ? strlen(response_text) : 0);
    return PLUGIN_OK;
}

static PluginStatus example_on_cleanup(void) {
    fprintf(stderr, "[plugin_example] cleanup OK\n");
    return PLUGIN_OK;
}

/* ------------------------------------------------------------------ */
/*  Tool execution hook — echo tool                                    */
/* ------------------------------------------------------------------ */

static bool example_on_tool_exec(const char *tool_name,
                                 const char *argument_json,
                                 char *result_json,
                                 size_t result_json_max) {
    if (!tool_name || strcmp(tool_name, "echo") != 0)
        return false; /* not handled, fall back to default runner */

    /* Simple echo: return the arguments back */
    snprintf(result_json, result_json_max,
             "{\"status\":\"ok\",\"tool\":\"%s\",\"echoed\":%s}",
             tool_name, argument_json ? argument_json : "{}");
    return true;
}

/* ------------------------------------------------------------------ */
/*  Plugin instance definition                                         */
/* ------------------------------------------------------------------ */

static PluginInstance g_example_plugin = {
    .name         = "example",
    .version      = 1,
    .capabilities = PLUGIN_CAP_TOOL_EXEC,
    .on_init      = example_on_init,
    .on_request   = example_on_request,
    .on_response  = example_on_response,
    .on_cleanup   = example_on_cleanup,
    .on_tool_exec = example_on_tool_exec,
    .user_data    = NULL,
};

/* ------------------------------------------------------------------ */
/*  Registration helper                                                */
/* ------------------------------------------------------------------ */

void plugin_example_register(void) {
    bool ok = plugin_register(&g_example_plugin);
    if (ok)
        fprintf(stderr, "[plugin_example] registered as '%s'\n", g_example_plugin.name);
    else
        fprintf(stderr, "[plugin_example] registration FAILED\n");
}
