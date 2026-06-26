#ifndef FILE_MONITOR_EXPANDED_H
#define FILE_MONITOR_EXPANDED_H

#include "bridge_core.h"
#include <stdbool.h>

typedef struct {
    BridgeEngine *engine;
    char workspace[1024];
    bool running;
} MonitorRegistry;

void init_monitor_registry(MonitorRegistry *reg, BridgeEngine *eng);
bool register_workspace_target(MonitorRegistry *reg, const char *workspace);
void *expanded_kqueue_loop(void *arg);
void clear_monitor_registry(MonitorRegistry *reg);

#endif
