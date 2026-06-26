#ifndef BRIDGE_CORE_H
#define BRIDGE_CORE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include "native_connection.h"
#include "config_manager.h"

#define BRIDGE_BUFFER_SIZE 131072

#define BRIDGE_VERSION "1.0.0"
#define BRIDGE_PROTOCOL_VERSION 1
#define BRIDGE_HARNESS_ID "codex.responses"
#define BRIDGE_MAX_FRAME_BYTES 16777216

/* Startup timeout buckets (milliseconds) */
#define BRIDGE_SOCKET_CONNECT_TIMEOUT_MS  30000
#define BRIDGE_HELLO_TIMEOUT_MS           10000
#define BRIDGE_MODEL_LOAD_TIMEOUT_MS      120000
#define BRIDGE_TURN_RESPONSE_TIMEOUT_MS   60000
#define BRIDGE_HEARTBEAT_INTERVAL_MS       30000
#define BRIDGE_HEARTBEAT_TIMEOUT_MS        60000

typedef struct {
    pid_t child_pid;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
    char *html_buffer;
    char *text_buffer;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t text_head;
    size_t text_tail;
    bool running;
    bool framed_mode;
    volatile bool cancelled;
    volatile bool turn_active;
    /* Set when the just-finished framed turn hit max_turn_events and returned
     * partial output; consumed by the non-stream response writer to emit
     * incomplete_details. Reset each time it is read. */
    bool turn_event_limited;
    /* Timeout buckets (milliseconds, 0 = use default) */
    int socket_connect_timeout_ms;
    int hello_timeout_ms;
    int model_load_timeout_ms;
    int turn_response_timeout_ms;
    char workspace[1024];
    pthread_mutex_t buffer_mutex;
    pthread_t reader_thread;
    pthread_t stderr_thread;
    NativeConnection nc;
    /* Heartbeat/ping-pong */
    volatile bool heartbeat_running;
    int heartbeat_interval_ms;
    int heartbeat_timeout_ms;
    pthread_t heartbeat_thread;
    volatile time_t last_pong_time;
    /* Current request id for cancellation */
    char current_request_id[64];
    /* Session tracking */
    char last_session_id[256];
    /* Connection read synchronization */
    pthread_mutex_t conn_read_mutex;
    /* Configuration context (non-owning pointer) */
    const BridgeConfig *cfg;
} BridgeEngine;

bool engine_init(BridgeEngine *eng, const BridgeConfig *cfg, const char *command, char *const argv[], const char *workspace, bool framed_mode, const char *transport, const char *socket_path, const char *owner_mode);
void engine_write_stdin(BridgeEngine *eng, const char *input, size_t len);
void engine_write_frame(BridgeEngine *eng, const char *json, size_t len);
char *engine_read_frame(BridgeEngine *eng, size_t *out_len);
char *engine_read_frame_timeout(BridgeEngine *eng, size_t *out_len, int timeout_ms);
bool engine_health_check(BridgeEngine *eng, int timeout_ms);
size_t engine_read_html(BridgeEngine *eng, char *dest, size_t max_len);
void engine_push_html(BridgeEngine *eng, const char *html);
size_t engine_read_text(BridgeEngine *eng, char *dest, size_t max_len);
size_t engine_text_available(BridgeEngine *eng);
void engine_push_text(BridgeEngine *eng, const char *text);
void engine_cancel(BridgeEngine *eng);
void engine_destroy(BridgeEngine *eng);
void engine_set_turn_active(BridgeEngine *eng, bool active);
bool engine_send_hello(BridgeEngine *eng, const char *workspace_root);
bool engine_read_native_hello(BridgeEngine *eng);
bool engine_reconnect_uds(BridgeEngine *eng, const char *socket_path, int timeout_ms);
bool engine_wait_for_ready(BridgeEngine *eng);
void engine_send_error(BridgeEngine *eng, const char *error_type, const char *detail);
bool engine_load_session_state(BridgeEngine *eng, const char *session_key, const char *state_id);
bool engine_save_session_state(BridgeEngine *eng, const char *session_key, const char *state_id);
bool engine_switch_session_state(BridgeEngine *eng, const char *from_key, const char *to_key, const char *to_state_id);
bool engine_create_session_state(BridgeEngine *eng, const char *session_key, const char *project_root, const char *model_id, int context_budget, const char *metadata);

/* Heartbeat/ping-pong */
bool engine_start_heartbeat(BridgeEngine *eng);
void engine_stop_heartbeat(BridgeEngine *eng);
bool engine_send_ping(BridgeEngine *eng);
bool engine_wait_pong(BridgeEngine *eng, int timeout_ms);

void bridge_html_escape(const char *src, char *dest, size_t max_len);
void bridge_format_log_line(const char *raw_line, const char *workspace, char *output_html, size_t max_len);

#endif /* BRIDGE_CORE_H */
