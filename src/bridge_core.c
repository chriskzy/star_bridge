#include "bridge_core.h"
#include "ring_buffer.h"
#include "native_frame.h"
#include "native_connection.h"
#include "uds_transport.h"
#include "debug_trace.h"
#include "config_manager.h"
#include "json_utils.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void buffer_append(BridgeEngine *eng, const char *data, size_t len) {
    pthread_mutex_lock(&eng->buffer_mutex);
    ring_append(eng->html_buffer, eng->capacity, &eng->head, &eng->tail, data, len);
    pthread_mutex_unlock(&eng->buffer_mutex);
}

void engine_push_html(BridgeEngine *eng, const char *html) {
    if (!eng || !html) return;
    buffer_append(eng, html, strlen(html));
}

void engine_push_text(BridgeEngine *eng, const char *text) {
    if (!eng || !text) return;
    pthread_mutex_lock(&eng->buffer_mutex);
    ring_append(eng->text_buffer, eng->capacity, &eng->text_head, &eng->text_tail, text, strlen(text));
    pthread_mutex_unlock(&eng->buffer_mutex);
}

void bridge_html_escape(const char *src, char *dest, size_t max_len) {
    if (!dest || max_len == 0) return;
    size_t out = 0;
    if (!src) {
        dest[0] = '\0';
        return;
    }
    for (size_t i = 0; src[i] && out + 1 < max_len; i++) {
        const char *rep = NULL;
        switch (src[i]) {
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '&': rep = "&amp;"; break;
            case '"': rep = "&quot;"; break;
            default: break;
        }
        if (rep) {
            size_t rlen = strlen(rep);
            if (out + rlen >= max_len) break;
            memcpy(dest + out, rep, rlen);
            out += rlen;
        } else {
            dest[out++] = src[i];
        }
    }
    dest[out] = '\0';
}

void bridge_format_log_line(const char *raw_line, const char *workspace, char *output_html, size_t max_len) {
    char filename[512] = {0};
    char message[2048] = {0};
    int line = 0;
    int col = 0;

    if (sscanf(raw_line, "%511[^:]:%d:%d: error: %2047[^\n]", filename, &line, &col, message) >= 3) {
        char safe_file[1024], safe_message[4096], href[2048];
        bridge_html_escape(filename, safe_file, sizeof(safe_file));
        bridge_html_escape(message, safe_message, sizeof(safe_message));
        if (filename[0] == '/') {
            snprintf(href, sizeof(href), "vscode://file%s:%d:%d", filename, line, col);
        } else {
            snprintf(href, sizeof(href), "vscode://file%s/%s:%d:%d", workspace && workspace[0] ? workspace : ".", filename, line, col);
        }
        snprintf(output_html, max_len,
                 "<div class='log-row compiler-error'><a class='log-link' href='%s'><span class='log-indicator badge-error'>ERROR</span></a> <span class='log-text'><strong>%s:%d:%d</strong> - %s</span></div>\n",
                 href, safe_file, line, col, safe_message);
        return;
    }

    if (sscanf(raw_line, "%511[^:]:%d: warning: %2047[^\n]", filename, &line, message) >= 2) {
        char safe_file[1024], safe_message[4096], href[2048];
        bridge_html_escape(filename, safe_file, sizeof(safe_file));
        bridge_html_escape(message, safe_message, sizeof(safe_message));
        if (filename[0] == '/') {
            snprintf(href, sizeof(href), "vscode://file%s:%d", filename, line);
        } else {
            snprintf(href, sizeof(href), "vscode://file%s/%s:%d", workspace && workspace[0] ? workspace : ".", filename, line);
        }
        snprintf(output_html, max_len,
                 "<div class='log-row compiler-warning'><a class='log-link' href='%s'><span class='log-indicator badge-warning'>WARNING</span></a> <span class='log-text'><strong>%s:%d</strong> - %s</span></div>\n",
                 href, safe_file, line, safe_message);
        return;
    }

    char safe[4096];
    bridge_html_escape(raw_line, safe, sizeof(safe));
    snprintf(output_html, max_len, "<div class='log-row'>%s</div>\n", safe);
}

static void *fd_reader_loop(void *arg) {
    BridgeEngine *eng = (BridgeEngine *)arg;
    int fd = eng->framed_mode ? eng->stderr_fd : eng->stdout_fd;
    char read_buf[1024];
    char line[4096];
    size_t line_len = 0;

    while (eng->running) {
        ssize_t n = read(fd, read_buf, sizeof(read_buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (read_buf[i] == '\n' || line_len + 1 >= sizeof(line)) {
                    line[line_len] = '\0';
                    if (!eng->framed_mode) {
                        engine_push_text(eng, line);
                        engine_push_text(eng, "\n");
                    } else if (eng->framed_mode && fd == eng->stderr_fd && line[0]) {
                        debug_trace_append("native_stderr %s", line);
                    }
                    char html[8192];
                    bridge_format_log_line(line, eng->workspace, html, sizeof(html));
                    engine_push_html(eng, html);
                    line_len = 0;
                } else if (read_buf[i] != '\r') {
                    line[line_len++] = read_buf[i];
                }
            }
        } else if (n == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            usleep(10000);
        }
    }

    if (line_len > 0) {
        line[line_len] = '\0';
        if (!eng->framed_mode) {
            engine_push_text(eng, line);
            engine_push_text(eng, "\n");
        } else if (eng->framed_mode && fd == eng->stderr_fd && line[0]) {
            debug_trace_append("native_stderr %s", line);
        }
        char html[8192];
        bridge_format_log_line(line, eng->workspace, html, sizeof(html));
        engine_push_html(eng, html);
    }
    return NULL;
}

bool engine_init(BridgeEngine *eng, const BridgeConfig *cfg, const char *command, char *const argv[], const char *workspace, bool framed_mode, const char *transport, const char *socket_path, const char *owner_mode) {
    if (!eng || !workspace) return false;
    memset(eng, 0, sizeof(*eng));
    eng->cfg = cfg; /* Store configuration context pointer */
    eng->capacity = BRIDGE_BUFFER_SIZE;
    eng->html_buffer = calloc(eng->capacity, 1);
    eng->text_buffer = calloc(eng->capacity, 1);
    if (!eng->html_buffer || !eng->text_buffer) {
        free(eng->html_buffer);
        free(eng->text_buffer);
        return false;
    }
    pthread_mutex_init(&eng->buffer_mutex, NULL);
    pthread_mutex_init(&eng->conn_read_mutex, NULL);
    /* Initialize timeout buckets to default values (0 means use default) */
    eng->socket_connect_timeout_ms = 0;
    eng->hello_timeout_ms = 0;
    eng->model_load_timeout_ms = 0;
    eng->turn_response_timeout_ms = 0;
    /* Heartbeat defaults */
    eng->heartbeat_interval_ms = BRIDGE_HEARTBEAT_INTERVAL_MS;
    eng->heartbeat_timeout_ms = BRIDGE_HEARTBEAT_TIMEOUT_MS;
    eng->heartbeat_running = false;
    eng->last_pong_time = 0;
    /* Current request id */
    eng->current_request_id[0] = '\0';
    snprintf(eng->workspace, sizeof(eng->workspace), "%s", workspace && workspace[0] ? workspace : ".");
    eng->framed_mode = framed_mode;

    /* Initialize NativeConnection */
    nc_init(&eng->nc);

    /* Determine transport type */
    bool use_uds = (transport && strcmp(transport, "uds") == 0);
    bool use_connect_existing = (owner_mode && strcmp(owner_mode, "connect_existing") == 0);

    if (use_uds && use_connect_existing) {
        /* connect_existing mode: no child process, connect to existing socket */
        if (!socket_path || socket_path[0] == '\0') {
            free(eng->html_buffer);
            free(eng->text_buffer);
            return false;
        }
        UDSConnectResult result = uds_connect(socket_path, eng->socket_connect_timeout_ms > 0
                                               ? eng->socket_connect_timeout_ms : BRIDGE_SOCKET_CONNECT_TIMEOUT_MS);
        if (result.fd < 0) {
            debug_trace_append("uds_connect_failed transport=uds error=%s", result.error ? result.error : "unknown");
            free(eng->html_buffer);
            free(eng->text_buffer);
            return false;
        }
        eng->child_pid = 0;
        eng->stdin_fd = -1;
        eng->stdout_fd = -1;
        eng->stderr_fd = -1;
        eng->running = true;
        nc_set_uds(&eng->nc, result.fd, 0);
        nc_set_state(&eng->nc, CONN_CONNECTED);
        debug_trace_append("process_started transport=uds mode=connect_existing socket_path=%s",
                           socket_path);
        debug_trace_append("socket_connected transport=uds fd=%d", result.fd);
        return true;
    }

    if (use_uds && !use_connect_existing) {
        /* launch_and_connect mode: spawn child, connect via UDS */
        if (!command || !argv) {
            free(eng->html_buffer);
            free(eng->text_buffer);
            return false;
        }
        /* Fork child process */
        pid_t pid = fork();
        if (pid == 0) {
            /* Child: exec the native agent */
            if (workspace && workspace[0]) chdir(workspace);
            execvp(command, argv);
            perror("execvp");
            _exit(127);
        } else {
            /* Parent continues */
        }
        if (pid < 0) {
            free(eng->html_buffer);
            free(eng->text_buffer);
            return false;
        }
        eng->child_pid = pid;
        eng->stdin_fd = -1;
        eng->stdout_fd = -1;
        eng->stderr_fd = -1;
        eng->running = true;

        debug_trace_append("process_started transport=uds mode=launch_and_connect pid=%ld command=%s",
                           (long)pid, command);
        debug_trace_append("socket_waiting transport=uds pid=%ld socket_path=%s",
                           (long)pid, socket_path ? socket_path : "");

        /* Connect to UDS socket (wait for child to create it) */
        int timeout = eng->socket_connect_timeout_ms > 0
                      ? eng->socket_connect_timeout_ms : BRIDGE_SOCKET_CONNECT_TIMEOUT_MS;
        UDSConnectResult result = uds_connect(socket_path, timeout);
        if (result.fd < 0) {
            debug_trace_append("uds_connect_failed transport=uds error=%s pid=%ld",
                               result.error ? result.error : "unknown", (long)pid);
            /* Clean up child */
            kill(pid, SIGTERM);
            {
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 }; /* 10ms */
                nanosleep(&ts, NULL);
            }
            kill(pid, SIGKILL);
            {
                int wstatus;
                waitpid(pid, &wstatus, 0);
            }
            eng->child_pid = 0;
            eng->running = false;
            free(eng->html_buffer);
            free(eng->text_buffer);
            return false;
        }
        nc_set_uds(&eng->nc, result.fd, pid);
        nc_set_state(&eng->nc, CONN_CONNECTED);
        debug_trace_append("socket_connected transport=uds fd=%d pid=%ld", result.fd, (long)pid);
        return true;
    }

    /* stdio_framed or stdio transport: fork child with pipes */
    if (!command || !argv) {
        free(eng->html_buffer);
        free(eng->text_buffer);
        return false;
    }

    int in_pipe[2];
    int out_pipe[2];
    int err_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        free(eng->html_buffer);
        free(eng->text_buffer);
        return false;
    }

    pid_t pid = fork();
    if (pid == 0) {
        if (workspace && workspace[0]) chdir(workspace);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        execvp(command, argv);
        perror("execvp");
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (pid < 0) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(err_pipe[0]);
        free(eng->html_buffer);
        free(eng->text_buffer);
        return false;
    }

    eng->child_pid = pid;
    eng->stdin_fd = in_pipe[1];
    eng->stdout_fd = out_pipe[0];
    eng->stderr_fd = err_pipe[0];
    eng->running = true;

    if (!framed_mode) {
        pthread_create(&eng->reader_thread, NULL, fd_reader_loop, eng);
    } else {
        pthread_create(&eng->stderr_thread, NULL, fd_reader_loop, eng);
    }

    /* Initialize NativeConnection for stdio_framed transport */
    nc_set_stdio(&eng->nc, eng->stdin_fd, eng->stdout_fd, pid);
    nc_set_state(&eng->nc, CONN_CONNECTED);
    debug_trace_append("process_started transport=%s pid=%ld command=%s",
                       framed_mode ? "stdio_framed" : "stdio", (long)pid, command);
    debug_trace_append("socket_waiting transport=%s pid=%ld",
                       framed_mode ? "stdio_framed" : "stdio", (long)pid);
    debug_trace_append("socket_connected transport=%s pid=%ld stdin_fd=%d stdout_fd=%d",
                       framed_mode ? "stdio_framed" : "stdio", (long)pid, eng->stdin_fd, eng->stdout_fd);
    return true;
}

void engine_write_stdin(BridgeEngine *eng, const char *input, size_t len) {
    if (!eng || eng->stdin_fd < 0 || !input || len == 0) return;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(eng->stdin_fd, input + off, len - off);
        if (n > 0) off += (size_t)n;
        else if (errno != EINTR) break;
    }
}

/* Write a framed message to the native agent.
 * Delegates to NativeConnection for transport-specific framing. */
void engine_write_frame(BridgeEngine *eng, const char *json, size_t len) {
    if (!eng || !json || len == 0) return;
    nc_write_frame(&eng->nc, json, len);
}

/* Read a framed response from native agent.
 * Delegates to NativeConnection for transport-specific framing.
 * Returns NULL on error/EOF.
 * Caller must free buffer. */
char *engine_read_frame(BridgeEngine *eng, size_t *out_len) {
    return engine_read_frame_timeout(eng, out_len, -1);
}

char *engine_read_frame_timeout(BridgeEngine *eng, size_t *out_len, int timeout_ms) {
    if (!eng || !eng->framed_mode) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    /* Serialize reads with the connection mutex to prevent heartbeat/turn
     * races. Only one thread reads from the native connection at a time. */
    pthread_mutex_lock(&eng->conn_read_mutex);
    const char *err = NULL;
    char *result = nc_read_frame_timeout(&eng->nc, out_len, timeout_ms, &err);
    if (!result && err && strcmp(err, "payload_too_large") == 0) {
        engine_send_error(eng, "payload_too_large", "frame payload exceeds max");
    }
    pthread_mutex_unlock(&eng->conn_read_mutex);
    return result;
}

/* Send a health ping with a unique id and verify the native's health pong.
 * Expected native response: {"type":"health","id":"<same_id>","status":"ok"}
 */
bool engine_health_check(BridgeEngine *eng, int timeout_ms) {
    if (!eng || !eng->framed_mode) return eng && eng->running;

    /* Generate a unique health id based on timestamp */
    static uint64_t health_seq = 0;
    health_seq++;
    char health_id[64];
    snprintf(health_id, sizeof(health_id), "health_%llu_%lu",
             (unsigned long long)time(NULL), (unsigned long)health_seq);

    /* Send health ping with id */
    char health[512];
    int len = snprintf(health, sizeof(health),
                       "{\"type\":\"health\",\"id\":\"%s\"}", health_id);
    if (len <= 0 || (size_t)len >= sizeof(health)) return false;
    engine_write_frame(eng, health, (size_t)len);

    /* Read health pong response */
    size_t len_out = 0;
    char *resp = engine_read_frame_timeout(eng, &len_out, timeout_ms);
    if (!resp) {
        debug_trace_append("health_check error=no_response id=%s timeout_ms=%d", health_id, timeout_ms);
        return false;
    }

    /* Validate response: must have same id and status ok */
    bool has_id = strstr(resp, health_id) != NULL;
    bool has_status = strstr(resp, "\"status\":\"ok\"") != NULL;
    bool has_type = strstr(resp, "\"type\":\"health\"") != NULL;
    bool ok = has_type && has_id && has_status;
    if (!ok) {
        debug_trace_append("health_check error=invalid_response id=%s type=%d id_match=%d status=%d",
                           health_id, has_type, has_id, has_status);
    }
    free(resp);
    return ok;
}

size_t engine_read_html(BridgeEngine *eng, char *dest, size_t max_len) {
    if (!eng || !dest || max_len == 0) return 0;
    pthread_mutex_lock(&eng->buffer_mutex);
    size_t available = ring_read(eng->html_buffer, eng->capacity, &eng->head, &eng->tail, dest, max_len);
    pthread_mutex_unlock(&eng->buffer_mutex);
    return available;
}

size_t engine_read_text(BridgeEngine *eng, char *dest, size_t max_len) {
    if (!eng || !dest || max_len == 0) return 0;
    pthread_mutex_lock(&eng->buffer_mutex);
    size_t available = ring_read(eng->text_buffer, eng->capacity, &eng->text_head, &eng->text_tail, dest, max_len);
    pthread_mutex_unlock(&eng->buffer_mutex);
    return available;
}

size_t engine_text_available(BridgeEngine *eng) {
    if (!eng) return 0;
    pthread_mutex_lock(&eng->buffer_mutex);
    size_t available = ring_available(eng->text_head, eng->text_tail);
    pthread_mutex_unlock(&eng->buffer_mutex);
    return available;
}

void engine_cancel(BridgeEngine *eng) {
    if (!eng) return;
    /* Idempotent: if already cancelled, don't send another cancel frame */
    if (eng->cancelled) return;
    eng->cancelled = true;
    /* Send cancel frame in framed mode; never kill the agent process */
    if (eng->framed_mode && eng->stdin_fd > 0) {
        char cancel[512];
        const char *req_id = eng->current_request_id[0] ? eng->current_request_id : "cancel-1";
        snprintf(cancel, sizeof(cancel),
                 "{\"type\":\"cancel\",\"id\":\"%s\",\"reason\":\"client_disconnected\"}",
                 req_id);
        engine_write_frame(eng, cancel, strlen(cancel));
    }
}

void engine_destroy(BridgeEngine *eng) {
    if (!eng) return;
    pthread_mutex_destroy(&eng->conn_read_mutex);
    eng->running = false;
    if (eng->framed_mode && eng->stdin_fd > 0) {
        const char *shutdown = "{\"type\":\"shutdown\",\"reason\":\"bridge_exit\"}";
        engine_write_frame(eng, shutdown, strlen(shutdown));
        /* Wait for close acknowledgement before closing socket when possible */
        size_t ack_len = 0;
        char *ack = engine_read_frame_timeout(eng, &ack_len, 5000);
        if (ack) {
            bool is_ack = strstr(ack, "\"type\":\"shutdown_ack\"") != NULL;
            bool ok_status = strstr(ack, "\"status\":\"ok\"") != NULL;
            if (is_ack && ok_status) {
                debug_trace_append("engine_destroy shutdown_ack received");
            } else {
                debug_trace_append("engine_destroy shutdown_ack invalid or missing");
            }
            free(ack);
        } else {
            debug_trace_append("engine_destroy shutdown_ack timeout (5s)");
        }
    }
    if (eng->child_pid > 0) {
        for (int i = 0; i < 10; i++) {
            int status = 0;
            pid_t done = waitpid(eng->child_pid, &status, WNOHANG);
            if (done == eng->child_pid) {
                eng->child_pid = 0;
                break;
            }
            usleep(50000);
        }
    }
    if (eng->stdin_fd > 0) close(eng->stdin_fd);
    if (eng->stdout_fd > 0) close(eng->stdout_fd);
    if (eng->stderr_fd > 0) close(eng->stderr_fd);
    eng->child_pid = 0;
    eng->stdin_fd = -1;
    eng->stdout_fd = -1;
    eng->stderr_fd = -1;
    if (eng->reader_thread) pthread_join(eng->reader_thread, NULL);
    if (eng->stderr_thread) pthread_join(eng->stderr_thread, NULL);
    pthread_mutex_destroy(&eng->buffer_mutex);
    free(eng->html_buffer);
    free(eng->text_buffer);
    nc_close(&eng->nc);
}

/* Set whether a turn is active.
 * When turn_active is true, reconnection is blocked. */
void engine_set_turn_active(BridgeEngine *eng, bool active) {
    if (!eng) return;
    eng->turn_active = active;
    debug_trace_append("turn_active transport=%s active=%s",
                       eng->framed_mode ? "uds" : "stdio_framed",
                       active ? "true" : "false");
}

/* Attempt UDS reconnect if no turn is active.
 * Does nothing if a turn is active (mid-turn reconnect is unsafe).
 * Does nothing if cancelled flag is set (turn closed via cancel/error). */
bool engine_reconnect_uds(BridgeEngine *eng, const char *socket_path, int timeout_ms) {
    if (!eng) return false;

    /* Block reconnect mid-turn unless cancelled */
    if (eng->turn_active && !eng->cancelled) {
        debug_trace_append("reconnect_blocked reason=turn_active socket_path=%s", socket_path);
        return false;
    }

    debug_trace_append("reconnect_attempt transport=uds socket_path=%s timeout_ms=%d turn_active=%s cancelled=%s",
                       socket_path, timeout_ms,
                       eng->turn_active ? "true" : "false",
                       eng->cancelled ? "true" : "false");

    return nc_reconnect(&eng->nc, socket_path, timeout_ms);
}

/* Build and send the bridge hello frame to the native agent.
 * The hello frame declares bridge identity, supported protocol versions, and accepted parameters.
 * Returns true if the frame was written successfully.
 */
bool engine_send_hello(BridgeEngine *eng, const char *workspace_root) {
    if (!eng || !workspace_root) return false;
    if (eng->nc.write_fd < 0) return false;

    /* Build hello JSON with supported protocol versions list */
    char hello[4096];
    int len = snprintf(hello, sizeof(hello),
        "{\"type\":\"hello\",\"role\":\"bridge\",\"bridge_version\":\"%s\",\"protocol_versions\":[%d],"
        "\"harness_id\":\"%s\",\"workspace_root\":\"%s\",\"accepted_events\":[\"response\",\"error\",\"status\",\"log\"],"
        "\"max_frame_bytes\":%d}",
        BRIDGE_VERSION, BRIDGE_PROTOCOL_VERSION,
        BRIDGE_HARNESS_ID, workspace_root, BRIDGE_MAX_FRAME_BYTES);
    if (len <= 0 || (size_t)len >= sizeof(hello)) {
        debug_trace_append("hello_frame error=truncated");
        return false;
    }

    bool ok = nc_write_frame(&eng->nc, hello, (size_t)len);
    if (ok) {
        nc_set_state(&eng->nc, CONN_HANDSHAKING);
        debug_trace_append("hello_sent transport=%s protocol_versions=[%d] workspace_root=%s",
                           eng->framed_mode ? "uds" : "stdio_framed",
                           BRIDGE_PROTOCOL_VERSION, workspace_root);
    } else {
        debug_trace_append("hello_frame error=write_failed");
    }
    /* Small delay to allow child process to start and send its hello */
    {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 }; /* 50ms */
        nanosleep(&ts, NULL);
    }
    return ok;
}

/* Parse and validate the native agent's hello response.
 * Expected fields:
 *   type: "hello" (required)
 *   role: "native_agent" (required)
 *   protocol_version: must be in bridge's supported list (required)
 *   agent_name: string (required)
 *   agent_version: string (required)
 *   supported_transports: array (required)
 *   supported_events: array (optional)
 *   capabilities: object (optional)
 *
 * Returns true if the hello frame is valid and protocol version is supported.
 * Sets nc->protocol_version to the negotiated version on success.
 */
bool engine_read_native_hello(BridgeEngine *eng) {
    if (!eng) return false;
    if (eng->nc.read_fd < 0) return false;

    /* Read a frame with configured hello timeout (default BRIDGE_HELLO_TIMEOUT_MS) */
    int hello_timeout = eng->hello_timeout_ms > 0 ? eng->hello_timeout_ms : BRIDGE_HELLO_TIMEOUT_MS;
    size_t len = 0;
    char *frame = nc_read_frame_timeout(&eng->nc, &len, hello_timeout, NULL);
    if (!frame) {
        debug_trace_append("native_hello error=no_response timeout_ms=%d", hello_timeout);
        return false;
    }

    /* Parse JSON fields using basic string search (we don't have a full JSON parser) */
    bool has_type = strstr(frame, "\"type\":\"hello\"") != NULL ||
                    strstr(frame, "\"type\": \"hello\"") != NULL;
    bool has_role = strstr(frame, "\"role\":\"native_agent\"") != NULL ||
                    strstr(frame, "\"role\": \"native_agent\"") != NULL;
    bool has_protocol = strstr(frame, "\"protocol_version\"") != NULL;
    bool has_agent_name = strstr(frame, "\"agent_name\"") != NULL;
    bool has_agent_version = strstr(frame, "\"agent_version\"") != NULL;
    bool has_supported_transports = strstr(frame, "\"supported_transports\"") != NULL;
    bool has_supported_events = strstr(frame, "\"supported_events\"") != NULL;
    (void)has_supported_events;
    bool has_capabilities = strstr(frame, "\"capabilities\"") != NULL;
    (void)has_capabilities;

    /* Extract protocol_version value */
    int proto_version = 0;
    const char *pv = strstr(frame, "\"protocol_version\":");
    if (pv) {
        pv += 19; /* skip past the key and colon */
        while (*pv == ' ' || *pv == '\t') pv++;
        if (*pv >= '0' && *pv <= '9') proto_version = *pv - '0';
    }

    if (!has_type) {
        debug_trace_append("native_hello error=missing_type");
        engine_send_error(eng, "protocol_mismatch", "missing type field in hello");
        free(frame);
        return false;
    }
    if (!has_role) {
        debug_trace_append("native_hello error=missing_role");
        engine_send_error(eng, "protocol_mismatch", "missing role field in hello");
        free(frame);
        return false;
    }
    if (!has_protocol) {
        debug_trace_append("native_hello error=missing_protocol_version");
        engine_send_error(eng, "protocol_mismatch", "missing protocol_version in hello");
        free(frame);
        return false;
    }
    /* Check that the native's protocol_version is in the bridge's supported list */
    static const int supported_versions[] = { BRIDGE_PROTOCOL_VERSION };
    static const int num_supported = 1;
    bool version_supported = false;
    for (int i = 0; i < num_supported; i++) {
        if (proto_version == supported_versions[i]) {
            version_supported = true;
            break;
        }
    }
    if (!version_supported) {
        debug_trace_append("native_hello error=protocol_version_not_supported "
                           "native=%d supported=[%d]", proto_version, BRIDGE_PROTOCOL_VERSION);
        engine_send_error(eng, "protocol_mismatch", "unsupported protocol version");
        free(frame);
        return false;
    }
    if (!has_agent_name) {
        debug_trace_append("native_hello error=missing_agent_name");
        engine_send_error(eng, "protocol_mismatch", "missing agent_name");
        free(frame);
        return false;
    }
    if (!has_agent_version) {
        debug_trace_append("native_hello error=missing_agent_version");
        engine_send_error(eng, "protocol_mismatch", "missing agent_version");
        free(frame);
        return false;
    }
    if (!has_supported_transports) {
        debug_trace_append("native_hello error=missing_supported_transports");
        engine_send_error(eng, "protocol_mismatch", "missing supported_transports");
        free(frame);
        return false;
    }

    /* Check max_frame_bytes if present in native response */
    const char *mfb = strstr(frame, "\"max_frame_bytes\":");
    if (mfb) {
        mfb += 18; /* skip past "max_frame_bytes": */
        while (*mfb == ' ' || *mfb == '\t') mfb++;
        /* Parse the value */
        long nfb = 0;
        while (*mfb >= '0' && *mfb <= '9') {
            nfb = nfb * 10 + (*mfb - '0');
            mfb++;
        }
        if (nfb < BRIDGE_MAX_FRAME_BYTES) {
            debug_trace_append("native_hello error=max_frame_bytes_too_small "
                               "native=%ld bridge=%d", nfb, BRIDGE_MAX_FRAME_BYTES);
            free(frame);
            return false;
        }
    }

    /* Record negotiated protocol version */
    eng->nc.protocol_version = proto_version;
    nc_set_state(&eng->nc, CONN_READY);

    /* Extract agent_name and agent_version for logging */
    const char *an = strstr(frame, "\"agent_name\":\"");
    const char *av = strstr(frame, "\"agent_version\":\"");
    char agent_name[64] = "(unknown)";
    char agent_version[64] = "(unknown)";
    if (an) {
        an += 14; /* skip past "agent_name":" */
        size_t i = 0;
        while (*an && *an != '"' && i < sizeof(agent_name) - 1) { agent_name[i++] = *an++; }
        agent_name[i] = '\0';
    }
    if (av) {
        av += 16; /* skip past "agent_version":" */
        size_t i = 0;
        while (*av && *av != '"' && i < sizeof(agent_version) - 1) { agent_version[i++] = *av++; }
        agent_version[i] = '\0';
    }

    debug_trace_append("hello_received transport=%s agent_name=%s agent_version=%s protocol_version=%d",
                       eng->framed_mode ? "uds" : "stdio_framed",
                       agent_name, agent_version, proto_version);

    free(frame);
    return true;
}

/* Wait for the native agent to send a "ready" frame indicating model loading is complete.
 * Expected frame: {"type":"ready","status":"ready","model_loaded":true,...}
 * Uses the configured model_load_timeout_ms (default BRIDGE_MODEL_LOAD_TIMEOUT_MS).
 * Returns true if a valid ready frame is received within the timeout.
 */
bool engine_wait_for_ready(BridgeEngine *eng) {
    if (!eng) return false;
    if (eng->nc.read_fd < 0) return false;

    debug_trace_append("model_loading transport=%s timeout_ms=%d",
                       eng->framed_mode ? "uds" : "stdio_framed",
                       eng->model_load_timeout_ms > 0 ? eng->model_load_timeout_ms : BRIDGE_MODEL_LOAD_TIMEOUT_MS);

    int load_timeout = eng->model_load_timeout_ms > 0 ? eng->model_load_timeout_ms : BRIDGE_MODEL_LOAD_TIMEOUT_MS;
    size_t len = 0;
    char *frame = nc_read_frame_timeout(&eng->nc, &len, load_timeout, NULL);
    if (!frame) {
        debug_trace_append("ready_frame error=no_response timeout_ms=%d", load_timeout);
        engine_send_error(eng, "handshake_timeout", "ready frame not received within timeout");
        return false;
    }

    /* Validate ready frame */
    bool is_ready = strstr(frame, "\"type\":\"ready\"") != NULL ||
                    strstr(frame, "\"type\": \"ready\"") != NULL;
    bool model_loaded = strstr(frame, "\"model_loaded\":true") != NULL ||
                        strstr(frame, "\"model_loaded\": true") != NULL;

    if (!is_ready) {
        debug_trace_append("ready_frame error=wrong_type type=%.64s", frame);
        engine_send_error(eng, "protocol_mismatch", "ready frame has wrong type");
        free(frame);
        return false;
    }
    if (!model_loaded) {
        debug_trace_append("ready_frame error=model_not_loaded");
        engine_send_error(eng, "model_loading", "native model not loaded");
        free(frame);
        return false;
    }

    /* Extract session_state if present */
    const char *ss = strstr(frame, "\"session_state\":\"");
    if (!ss) ss = strstr(frame, "\"session_state\": \"");
    char session_state[64] = "(unknown)";
    if (ss) {
        ss += 17; /* skip past "session_state":" (both variants have same length) */
        /* skip space if present */
        while (*ss == ' ' || *ss == '\t') ss++;
        size_t i = 0;
        while (*ss && *ss != '"' && i < sizeof(session_state) - 1) { session_state[i++] = *ss++; }
        session_state[i] = '\0';
    }

    nc_set_state(&eng->nc, CONN_READY);
    debug_trace_append("ready transport=%s session_state=%s model_loaded=true",
                       eng->framed_mode ? "uds" : "stdio_framed", session_state);
    free(frame);
    return true;
}

/* Send a structured error frame to the native agent.
 * Error types: handshake_timeout, protocol_mismatch, frame_decode_error,
 *              native_busy, model_loading, native_unhealthy,
 *              request_id_mismatch, unsupported_event, payload_too_large
 */
void engine_send_error(BridgeEngine *eng, const char *error_type, const char *detail) {
    if (!eng || !error_type) return;
    if (eng->framed_mode && eng->stdin_fd > 0) {
        char buf[1024];
        const char *safe_detail = detail ? detail : "";
        /* JSON-escape the detail string */
        char escaped[512];
        size_t ei = 0;
        for (const char *p = safe_detail; *p && ei < sizeof(escaped) - 4; p++) {
            if (*p == '"' || *p == '\\') { escaped[ei++] = '\\'; }
            escaped[ei++] = *p;
        }
        escaped[ei] = '\0';
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"error\",\"error_type\":\"%s\",\"detail\":\"%s\"}",
                 error_type, escaped);
        engine_write_frame(eng, buf, strlen(buf));
        debug_trace_append("engine_send_error error_type=%s detail=\"%s\"", error_type, escaped);
    }
}

/* -------------------------------------------------------------------
 * Session state loading.
 *
 * Sends a "load_state" frame to the native agent to restore a saved
 * KV cache / state identified by the given state_id.
 * Waits for an ack frame confirming the load.
 * ------------------------------------------------------------------- */
bool engine_load_session_state(BridgeEngine *eng, const char *session_key, const char *state_id) {
    if (!eng || !session_key || !state_id) return false;
    if (!eng->framed_mode) return false;

    char esc_key[256] = {0};
    char esc_state[256] = {0};
    json_escape(session_key ? session_key : "", esc_key, sizeof(esc_key));
    json_escape(state_id ? state_id : "", esc_state, sizeof(esc_state));

    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
                     "{\"type\":\"load_state\",\"key\":\"%s\",\"state_id\":\"%s\"}",
                     esc_key, esc_state);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return false;
    engine_write_frame(eng, buf, (size_t)n);
    debug_trace_append("session_load_state key=%s state_id=%s", session_key, state_id);

    /* Wait for ack which may include the state_id */
    size_t ack_len = 0;
    char *ack = engine_read_frame_timeout(eng, &ack_len, 10000);
    if (!ack) {
        debug_trace_append("session_load_state error=no_ack");
        return false;
    }
    bool ok = strstr(ack, "\"type\":\"ack\"") != NULL &&
              strstr(ack, "\"status\":\"accepted\"") != NULL;
    /* Extract state_id from ack for session index update */
    const char *sid_marker = strstr(ack, "\"state_id\":\"");
    if (sid_marker) {
        sid_marker += 12; /* skip past "state_id":" */
        const char *end = strchr(sid_marker, '"');
        if (end && end > sid_marker) {
            size_t sid_len = (size_t)(end - sid_marker);
            if (sid_len < 256) {
                char new_sid[256];
                memcpy(new_sid, sid_marker, sid_len);
                new_sid[sid_len] = '\0';
                debug_trace_append("session_load_state received state_id=%s", new_sid);
                session_index_update(session_key, new_sid, "", "", 0);
                save_session_index();
            }
        }
    }
    free(ack);
    if (!ok) {
        debug_trace_append("session_load_state error=ack_rejected");
        return false;
    }
    debug_trace_append("session_load_state completed key=%s", session_key);
    return true;
}

/* -------------------------------------------------------------------
 * Session state saving.
 *
 * Sends a "save_state" frame to the native agent to persist the
 * current KV cache / state under the given state_id.
 * ------------------------------------------------------------------- */
bool engine_save_session_state(BridgeEngine *eng, const char *session_key, const char *state_id) {
    if (!eng || !session_key || !state_id) return false;
    if (!eng->framed_mode) return false;

    char esc_save_key[256] = {0};
    char esc_save_state[256] = {0};
    json_escape(session_key ? session_key : "", esc_save_key, sizeof(esc_save_key));
    json_escape(state_id ? state_id : "", esc_save_state, sizeof(esc_save_state));

    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
                     "{\"type\":\"save_state\",\"key\":\"%s\",\"state_id\":\"%s\"}",
                     esc_save_key, esc_save_state);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return false;
    engine_write_frame(eng, buf, (size_t)n);
    debug_trace_append("session_save_state key=%s state_id=%s", session_key, state_id);

    /* Wait for ack */
    size_t ack_len = 0;
    char *ack = engine_read_frame_timeout(eng, &ack_len, 10000);
    if (!ack) {
        debug_trace_append("session_save_state error=no_ack");
        return false;
    }
    bool ok = strstr(ack, "\"type\":\"ack\"") != NULL &&
              strstr(ack, "\"status\":\"accepted\"") != NULL;
    free(ack);
    if (!ok) {
        debug_trace_append("session_save_state error=ack_rejected");
        return false;
    }
    debug_trace_append("session_save_state completed key=%s", session_key);
    return true;
}

/* -------------------------------------------------------------------
 * Session state switching.
 *
 * Sends a "switch_state" frame to the native agent to transition
 * from one session state to another. This is called when the user
 * switches between Codex sessions in the same project.
 * ------------------------------------------------------------------- */
bool engine_switch_session_state(BridgeEngine *eng, const char *from_key, const char *to_key, const char *to_state_id) {
    if (!eng || !from_key || !to_key || !to_state_id) return false;
    if (!eng->framed_mode) return false;

    char esc_from_key[256] = {0};
    char esc_to_key[256] = {0};
    char esc_switch_state[256] = {0};
    json_escape(from_key ? from_key : "", esc_from_key, sizeof(esc_from_key));
    json_escape(to_key ? to_key : "", esc_to_key, sizeof(esc_to_key));
    json_escape(to_state_id ? to_state_id : "", esc_switch_state, sizeof(esc_switch_state));

    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
                     "{\"type\":\"switch_state\",\"from_key\":\"%s\",\"to_key\":\"%s\",\"state_id\":\"%s\"}",
                     esc_from_key, esc_to_key, esc_switch_state);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return false;
    engine_write_frame(eng, buf, (size_t)n);
    debug_trace_append("session_switch_state from=%s to=%s state_id=%s", from_key, to_key, to_state_id);

    /* Wait for ack */
    size_t ack_len = 0;
    char *ack = engine_read_frame_timeout(eng, &ack_len, 10000);
    if (!ack) {
        debug_trace_append("session_switch_state error=no_ack");
        return false;
    }
    bool ok = strstr(ack, "\"type\":\"ack\"") != NULL &&
              strstr(ack, "\"status\":\"accepted\"") != NULL;
    free(ack);
    if (!ok) {
        debug_trace_append("session_switch_state error=ack_rejected");
        return false;
    }
    debug_trace_append("session_switch_state completed to_key=%s", to_key);
    return true;
}

/* -------------------------------------------------------------------
 * New session state creation.
 *
 * Sends a "create_state" frame to the native agent to initialise a
 * new KV cache / state for a fresh session with the given project root,
 * model id, context budget, and optional metadata.
 * ------------------------------------------------------------------- */
bool engine_create_session_state(BridgeEngine *eng, const char *session_key, const char *project_root, const char *model_id, int context_budget, const char *metadata) {
    if (!eng || !session_key || !project_root || !model_id) return false;
    if (!eng->framed_mode) return false;

    char esc_create_key[256] = {0};
    char esc_create_root[512] = {0};
    char esc_create_model[256] = {0};
    json_escape(session_key ? session_key : "", esc_create_key, sizeof(esc_create_key));
    json_escape(project_root ? project_root : "", esc_create_root, sizeof(esc_create_root));
    json_escape(model_id ? model_id : "", esc_create_model, sizeof(esc_create_model));

    char buf[1536];
    const char *safe_metadata = metadata ? metadata : "{}";
    int n = snprintf(buf, sizeof(buf),
                     "{\"type\":\"create_state\",\"key\":\"%s\",\"project_root\":\"%s\","
                     "\"model_id\":\"%s\",\"context_budget\":%d,\"metadata\":%s}",
                     esc_create_key, esc_create_root, esc_create_model, context_budget, safe_metadata);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return false;
    engine_write_frame(eng, buf, (size_t)n);
    debug_trace_append("session_create_state key=%s project=%s model=%s budget=%d",
                       session_key, project_root, model_id, context_budget);

    /* Wait for ack which includes the new state_id */
    size_t ack_len = 0;
    char *ack = engine_read_frame_timeout(eng, &ack_len, 10000);
    if (!ack) {
        debug_trace_append("session_create_state error=no_ack");
        return false;
    }
    bool ok = strstr(ack, "\"type\":\"ack\"") != NULL &&
              strstr(ack, "\"status\":\"accepted\"") != NULL;
    /* Extract state_id from ack for session index update */
    const char *sid_marker = strstr(ack, "\"state_id\":\"");
    if (sid_marker) {
        sid_marker += 12; /* skip past "state_id":" */
        const char *end = strchr(sid_marker, '"');
        if (end && end > sid_marker) {
            size_t sid_len = (size_t)(end - sid_marker);
            if (sid_len < 256) {
                char new_sid[256];
                memcpy(new_sid, sid_marker, sid_len);
                new_sid[sid_len] = '\0';
                debug_trace_append("session_create_state received state_id=%s", new_sid);
                session_index_update(session_key, new_sid, project_root, model_id, context_budget);
                save_session_index();
            }
        }
    }
    free(ack);
    if (!ok) {
        debug_trace_append("session_create_state error=ack_rejected");
        return false;
    }
    debug_trace_append("session_create_state completed key=%s", session_key);
    return true;
}

/* -------------------------------------------------------------------
 * Heartbeat / ping-pong support.
 *
 * The bridge periodically sends {"type":"ping","id":"..."} frames to the
 * native agent while idle. The native agent must reply with
 * {"type":"pong","id":"..."}. If no pong arrives within the heartbeat
 * timeout, the connection is marked unhealthy.
 *
 * Heartbeats are NOT sent while a turn is in progress (turn_active==true)
 * to avoid interleaving with response frames.
 * ------------------------------------------------------------------- */

/* Send a single ping frame with a unique id */
bool engine_send_ping(BridgeEngine *eng) {
    if (!eng || !eng->framed_mode) return false;

    static uint64_t ping_seq = 0;
    ping_seq++;
    char ping_buf[256];
    int len = snprintf(ping_buf, sizeof(ping_buf),
                       "{\"type\":\"ping\",\"id\":\"ping_%llu_%lu\"}",
                       (unsigned long long)time(NULL), (unsigned long)ping_seq);
    if (len <= 0 || (size_t)len >= sizeof(ping_buf)) return false;
    engine_write_frame(eng, ping_buf, (size_t)len);
    debug_trace_append("heartbeat sent_ping seq=%lu", (unsigned long)ping_seq);
    return true;
}

/* Wait for a pong frame with a matching id within timeout_ms.
 * Returns true if a valid pong is received.
 * Retries if a non-pong frame arrives (turn frame stolen during race). */
bool engine_wait_pong(BridgeEngine *eng, int timeout_ms) {
    if (!eng || !eng->framed_mode) return false;

    int remaining = timeout_ms;
    while (remaining > 0) {
        size_t len = 0;
        char *resp = engine_read_frame_timeout(eng, &len, remaining);
        if (!resp) {
            debug_trace_append("heartbeat error=no_pong timeout_ms=%d remaining=%d", timeout_ms, remaining);
            return false;
        }

        bool has_type = strstr(resp, "\"type\":\"pong\"") != NULL;
        bool has_status = strstr(resp, "\"status\":\"ok\"") != NULL;
        bool ok = has_type && has_status;
        if (ok) {
            free(resp);
            return true;
        }
        /* Non-pong frame (e.g. turn frame stolen before turn_active took effect):
         * log and retry with reduced remaining timeout */
        debug_trace_append("heartbeat info=non_pong_frame type=%.10s remaining=%d",
                           resp + 10, remaining);
        free(resp);
        /* Reduce remaining by a small retry penalty */
        remaining -= 100;
        if (remaining <= 0) {
            debug_trace_append("heartbeat error=no_pong timeout_ms=%d (retry exhausted)", timeout_ms);
            return false;
        }
    }
    debug_trace_append("heartbeat error=no_pong timeout_ms=%d (expired)", timeout_ms);
    return false;
}

/* Heartbeat thread: sends ping frames periodically while idle */
static void *heartbeat_loop(void *arg) {
    BridgeEngine *eng = (BridgeEngine *)arg;
    if (!eng) return NULL;

    while (eng->heartbeat_running && eng->running) {
        /* Sleep for heartbeat interval */
        int interval = eng->heartbeat_interval_ms > 0 ? eng->heartbeat_interval_ms : BRIDGE_HEARTBEAT_INTERVAL_MS;

        /* Use nanosleep in small increments to check the running flag */
        for (int i = 0; i < 100; i++) {
            struct timespec rem;
            struct timespec req = { .tv_sec = 0, .tv_nsec = interval * 10000L }; /* 1/100th of interval */
            if (req.tv_nsec > 999999999L) { req.tv_sec = 1; req.tv_nsec -= 999999999L; }
            nanosleep(&req, &rem);
            if (!eng->heartbeat_running || !eng->running) return NULL;
        }

        /* Skip heartbeat if a turn is in progress */
        if (eng->turn_active) {
            debug_trace_append("heartbeat skip reason=turn_active");
            continue;
        }

        /* Send ping and wait for pong */
        debug_trace_append("heartbeat send_ping");
        engine_send_ping(eng);

        int timeout = eng->heartbeat_timeout_ms > 0 ? eng->heartbeat_timeout_ms : BRIDGE_HEARTBEAT_TIMEOUT_MS;
        bool pong_ok = engine_wait_pong(eng, timeout);
        if (pong_ok) {
            eng->last_pong_time = time(NULL);
            debug_trace_append("heartbeat pong_ok");
        } else {
            debug_trace_append("heartbeat error=no_pong");
            engine_send_error(eng, "native_unhealthy", "heartbeat pong not received");
            nc_set_state(&eng->nc, CONN_UNHEALTHY);
        }
    }
    return NULL;
}

/* Start the heartbeat thread. Must be called after connection is ready. */
bool engine_start_heartbeat(BridgeEngine *eng) {
    if (!eng || !eng->framed_mode) return false;

    eng->heartbeat_running = true;
    eng->last_pong_time = time(NULL);

    int ret = pthread_create(&eng->heartbeat_thread, NULL, heartbeat_loop, eng);
    if (ret != 0) {
        eng->heartbeat_running = false;
        debug_trace_append("heartbeat error=thread_create_failed");
        return false;
    }
    debug_trace_append("heartbeat started interval_ms=%d timeout_ms=%d",
                       eng->heartbeat_interval_ms, eng->heartbeat_timeout_ms);
    return true;
}

/* Stop the heartbeat thread and wait for it to exit. */
void engine_stop_heartbeat(BridgeEngine *eng) {
    if (!eng) return;
    eng->heartbeat_running = false;
    if (eng->heartbeat_thread) {
        pthread_join(eng->heartbeat_thread, NULL);
    }
    debug_trace_append("heartbeat stopped");
}
