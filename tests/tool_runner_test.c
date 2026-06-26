#include "tool_runner.h"

#include <stdio.h>
#include <string.h>

static int expect(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        return 1;
    }
    return 0;
}

static bool app_callback(const char *tool_name,
                         const char *argument_json,
                         char *result_json,
                         size_t result_json_max,
                         char *stderr_text,
                         size_t stderr_text_max) {
    snprintf(result_json, result_json_max, "{\"tool\":\"%s\",\"args\":%s}", tool_name, argument_json ? argument_json : "{}");
    snprintf(stderr_text, stderr_text_max, "app callback handled");
    return true;
}

int main(void) {
    ToolRunResult run;
    int failed = 0;

    /* google_search is not supported by bridge — returns false with structured error */
    failed |= expect(!tool_runner_run("google_search", "{\"query\":\"codex bridge phase5\"}", ".", "", "", "", "brave", false, 5000, &run), "google_search run");
    failed |= expect(strcmp(run.tool_name, "google_search") == 0, "tool name");
    failed |= expect(strstr(run.stderr_text, "not supported by bridge") != NULL, "google search fallback");
    failed |= expect(!run.denied, "not denied");

    memset(&run, 0, sizeof(run));
    failed |= expect(!tool_runner_run("shell_command", "{\"command\":\"rm -rf /\"}", ".", "", "", "", "brave", false, 5000, &run), "denied shell");
    failed |= expect(run.denied, "denied flag");

    memset(&run, 0, sizeof(run));
    /* browse_url is not supported by bridge — returns false with structured error */
    failed |= expect(!tool_runner_run("browse_url", "{\"url\":\"https://example.test\"}", ".", "", "", "/tmp/browser-state", "brave", false, 5000, &run), "browse url run");
    failed |= expect(strstr(run.stderr_text, "not supported by bridge") != NULL, "browse diagnostic");

    memset(&run, 0, sizeof(run));
    char redact_buf[256];
    snprintf(redact_buf, sizeof(redact_buf), "GOOGLE_SEARCH_API_KEY=abc123 GOOGLE_SEARCH_CX=def456");
    tool_runner_redact_secrets(redact_buf);
    failed |= expect(strstr(redact_buf, "abc123") == NULL, "api key redacted");
    failed |= expect(strstr(redact_buf, "def456") == NULL, "cx redacted");

    char redact_log[256];
    snprintf(redact_log, sizeof(redact_log), "tool=google_search stderr=GOOGLE_SEARCH_API_KEY=abc123 GOOGLE_SEARCH_CX=def456");
    tool_runner_redact_secrets(redact_log);
    failed |= expect(strstr(redact_log, "abc123") == NULL, "log api key redacted");
    failed |= expect(strstr(redact_log, "def456") == NULL, "log cx redacted");

    memset(&run, 0, sizeof(run));
    failed |= expect(tool_runner_run("shell_command", "{\"command\":\"printf shell-ok\"}", ".", "", "", "", "brave", true, 5000, &run), "shell command enabled");
    failed |= expect(strstr(run.result_json, "shell-ok") != NULL, "shell command output");

    memset(&run, 0, sizeof(run));
    /* google_search with bad args: still hits not-supported early return, returns false, not denied */
    failed |= expect(!tool_runner_run("google_search", "{\"url\":\"x\"}", ".", "", "", "", "brave", false, 5000, &run), "malformed args");
    failed |= expect(!run.denied, "malformed not denied");

    memset(&run, 0, sizeof(run));
    failed |= expect(!tool_runner_run("shell_command", "{\"query\":\"printf shell-ok\"}", ".", "", "", "", "brave", true, 5000, &run), "malformed shell args");
    failed |= expect(!run.denied, "shell malformed not denied");

    tool_runner_register_app_callback(app_callback);
    memset(&run, 0, sizeof(run));
    failed |= expect(tool_runner_run("app_echo", "{\"value\":\"x\"}", ".", "", "", "", "brave", false, 5000, &run), "app callback run");
    failed |= expect(strstr(run.result_json, "\"tool\":\"app_echo\"") != NULL, "app callback output");
    failed |= expect(strstr(run.stderr_text, "app callback handled") != NULL, "app callback stderr");
    tool_runner_register_app_callback(NULL);

    return failed ? 1 : 0;
}
