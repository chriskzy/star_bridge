#include "config_manager.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int expect(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        return 1;
    }
    return 0;
}

int main(void) {
    char path[] = "/tmp/phase5_tool_config_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }
    const char *json =
        "{"
        "\"google_search_timeout_ms\":1200,"
        "\"browse_url_timeout_ms\":3400,"
        "\"shell_command_timeout_ms\":5600,"
        "\"shell_command_enabled\":true"
        "}";
    write(fd, json, strlen(json));
    close(fd);

    init_global_config(9033);
    int failed = 0;
    failed |= expect(global_config.trace, "trace default enabled");
    failed |= expect(global_config.debug_log_enabled, "debug log default enabled");
    failed |= expect(strcmp(global_config.debug_log_path, ".codex-bridge-debug.log") == 0, "debug log default path");
    failed |= expect(!global_config.bridge_workspace_monitor_enabled, "workspace monitor default disabled");
    failed |= expect(load_config_from_file(path), "load config");
    failed |= expect(global_config.google_search_timeout_ms == 1200, "google timeout");
    failed |= expect(global_config.browse_url_timeout_ms == 3400, "browse timeout");
    failed |= expect(global_config.shell_command_timeout_ms == 5600, "shell timeout");
    failed |= expect(global_config.shell_command_enabled, "shell enabled");

    unlink(path);
    return failed ? 1 : 0;
}
