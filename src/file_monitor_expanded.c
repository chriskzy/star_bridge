#include "file_monitor_expanded.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void init_monitor_registry(MonitorRegistry *reg, BridgeEngine *eng) {
    memset(reg, 0, sizeof(*reg));
    reg->engine = eng;
}

bool register_workspace_target(MonitorRegistry *reg, const char *workspace) {
    if (!reg || !workspace) return false;
    snprintf(reg->workspace, sizeof(reg->workspace), "%s", workspace);
    reg->running = true;
    return true;
}

static void push_snapshot(MonitorRegistry *reg) {
    DIR *dir = opendir(reg->workspace[0] ? reg->workspace : ".");
    if (!dir) return;
    engine_push_html(reg->engine, "<div class='event-node' data-action='CREATED'><span class='event-badge badge-created'>WORKSPACE</span>monitor started</div>\n");
    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(dir)) && count < 64) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char row[1200], safe[900];
        bridge_html_escape(ent->d_name, safe, sizeof(safe));
        snprintf(row, sizeof(row), "<div class='tree-row'>%s</div>\n", safe);
        engine_push_html(reg->engine, row);
        count++;
    }
    closedir(dir);
}

void *expanded_kqueue_loop(void *arg) {
    MonitorRegistry *reg = (MonitorRegistry *)arg;
    if (!reg || !reg->engine) return NULL;
    push_snapshot(reg);
    while (reg->running) {
        sleep(2);
    }
    return NULL;
}

void clear_monitor_registry(MonitorRegistry *reg) {
    if (reg) reg->running = false;
}
