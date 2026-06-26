#ifndef TOOL_RUNNER_H
#define TOOL_RUNNER_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char tool_name[64];
    char result_json[65536];
    char stderr_text[8192];
    int exit_code;
    bool timed_out;
    bool denied;
    bool truncated;     /* stdout or stderr was truncated to fit buffer */
    bool ok;
} ToolRunResult;

typedef bool (*ToolAppCallback)(const char *tool_name,
                                const char *argument_json,
                                char *result_json,
                                size_t result_json_max,
                                char *stderr_text,
                                size_t stderr_text_max);

void tool_runner_register_app_callback(ToolAppCallback callback);

/* Resolve node path: try configured path, then PATH lookup */
const char *tool_runner_resolve_node_path(const char *configured_path);

bool tool_runner_run(const char *tool_name,
                     const char *argument_json,
                     const char *workspace,
                     const char *agent_env,
                     const char *extra_native_args,
                     const char *browser_state_dir,
                     const char *preferred_browser,
                     bool shell_command_enabled,
                     int timeout_ms,
                     ToolRunResult *out);

void tool_runner_redact_secrets(char *text);

#endif /* TOOL_RUNNER_H */
