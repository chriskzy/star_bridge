#ifndef TOOL_POLICY_H
#define TOOL_POLICY_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TOOL_ERROR_DENIED = 0,
    TOOL_ERROR_MALFORMED_ARGS,
    TOOL_ERROR_TIMEOUT,
    TOOL_ERROR_UNKNOWN
} ToolErrorKind;

bool tool_policy_is_denied(const char *tool_name);
bool tool_policy_is_safe_tool(const char *tool_name);
bool tool_policy_requires_permission(const char *tool_name);
bool tool_policy_args_valid(const char *tool_name, const char *tool_args_json);
const char *tool_policy_error_message(ToolErrorKind kind);
void tool_policy_build_error_summary(const char *tool_name,
                                     ToolErrorKind kind,
                                     const char *detail,
                                     char *dest,
                                     size_t max_len);

#endif /* TOOL_POLICY_H */
