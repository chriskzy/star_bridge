#include "tool_policy.h"

#include <stdio.h>
#include <string.h>

static int expect(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        return 1;
    }
    return 0;
}

int main(void) {
    int failed = 0;
    failed |= expect(tool_policy_is_denied("shell"), "shell denied");
    failed |= expect(!tool_policy_is_denied("google_search"), "google_search allowed");

    /* strict-args: valid cases */
    failed |= expect(tool_policy_args_valid("google_search", "{\"query\":\"x\"}"), "search args valid");
    failed |= expect(!tool_policy_args_valid("google_search", "{\"url\":\"x\"}"), "search args wrong key");
    failed |= expect(tool_policy_args_valid("browse_url", "{\"url\":\"x\"}"), "browse args valid");
    failed |= expect(!tool_policy_args_valid("browse_url", "{\"query\":\"x\"}"), "browse args wrong key");
    failed |= expect(tool_policy_args_valid("shell_command", "{\"command\":\"printf ok\"}"), "shell args valid");
    failed |= expect(!tool_policy_args_valid("shell_command", "{\"query\":\"printf ok\"}"), "shell args wrong key");

    /* strict-args: empty values */
    failed |= expect(!tool_policy_args_valid("google_search", "{\"query\":\"\"}"), "search empty string");
    failed |= expect(!tool_policy_args_valid("browse_url", "{\"url\":\"\"}"), "browse empty string");
    failed |= expect(!tool_policy_args_valid("shell_command", "{\"command\":\"\"}"), "shell empty string");

    /* strict-args: non-string values */
    failed |= expect(!tool_policy_args_valid("google_search", "{\"query\":123}"), "search number not string");
    failed |= expect(!tool_policy_args_valid("browse_url", "{\"url\":true}"), "browse bool not string");
    failed |= expect(!tool_policy_args_valid("shell_command", "{\"command\":null}"), "shell null not string");

    /* strict-args: extra fields */
    failed |= expect(!tool_policy_args_valid("google_search", "{\"query\":\"x\",\"extra\":\"y\"}"), "search extra field");
    failed |= expect(!tool_policy_args_valid("browse_url", "{\"url\":\"x\",\"extra\":\"y\"}"), "browse extra field");
    failed |= expect(!tool_policy_args_valid("shell_command", "{\"command\":\"x\",\"extra\":\"y\"}"), "shell extra field");

    /* strict-args: missing fields */
    failed |= expect(!tool_policy_args_valid("google_search", "{\"notquery\":\"x\"}"), "search missing query");
    failed |= expect(!tool_policy_args_valid("browse_url", "{\"noturl\":\"x\"}"), "browse missing url");

    /* strict-args: malformed JSON */
    failed |= expect(!tool_policy_args_valid("google_search", ""), "search empty json");
    failed |= expect(!tool_policy_args_valid("google_search", "{\"query"), "search truncated json");
    failed |= expect(!tool_policy_args_valid("google_search", "null"), "search not object");
    failed |= expect(!tool_policy_args_valid("browse_url", "[]"), "browse not object");

    /* strict-args: unknown tool falls through */
    failed |= expect(tool_policy_args_valid("unknown_tool", "{}"), "unknown tool passes");

    failed |= expect(!tool_policy_requires_permission("google_search"), "google_search auto allowed");
    failed |= expect(!tool_policy_requires_permission("browse_url"), "browse_url auto allowed");
    failed |= expect(tool_policy_requires_permission("shell_command"), "shell_command requires permission");

    failed |= expect(strcmp(tool_policy_error_message(TOOL_ERROR_DENIED), "tool denied by bridge policy") == 0, "deny message");
    failed |= expect(strcmp(tool_policy_error_message(TOOL_ERROR_MALFORMED_ARGS), "malformed tool arguments") == 0, "malformed message");
    failed |= expect(strcmp(tool_policy_error_message(TOOL_ERROR_TIMEOUT), "tool execution timeout") == 0, "timeout message");

    char summary[256];
    tool_policy_build_error_summary("google_search", TOOL_ERROR_MALFORMED_ARGS, "{\"query\":\"x\"}", summary, sizeof(summary));
    failed |= expect(strstr(summary, "TOOL_ERROR google_search malformed tool arguments") != NULL, "summary malformed");

    return failed ? 1 : 0;
}
