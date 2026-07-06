/*
 * Fake UDS native agent test fixture.
 * Creates a Unix-domain socket and responds with length-prefixed JSON frames
 * using the same protocol as stdio_framed.
 *
 * Usage: fake_uds_agent <socket_path>
 *
 * Responds to:
 *   health -> {"type":"health","id":"<echo>","status":"ok"}
 *   request -> {"type":"response","status":"completed","output":"Fake UDS agent received: <input>"}
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Read exactly n bytes from fd (blocking, handles EINTR) */
static int read_exact(int fd, char *buf, size_t count) {
    size_t off = 0;
    while (off < count) {
        ssize_t n = read(fd, buf + off, count - off);
        if (n > 0) off += (size_t)n;
        else if (n == 0) return -1; /* EOF */
        else if (errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

/* Read a length-prefixed JSON frame from fd */
static char *read_frame(int fd, size_t *out_len) {
    uint32_t len = 0;
    if (read_exact(fd, (char *)&len, 4) != 0) return NULL;
    if (len == 0) return strdup(""); /* empty frame */
    if (len > 16777216u) return NULL; /* sanity limit */
    char *buf = malloc((size_t)len + 1);
    if (!buf) return NULL;
    if (read_exact(fd, buf, (size_t)len) != 0) {
        free(buf);
        return NULL;
    }
    buf[(size_t)len] = '\0';
    if (out_len) *out_len = (size_t)len;
    return buf;
}

/* Write a length-prefixed JSON frame to fd */
static int write_frame(int fd, const char *json, size_t len) {
    uint32_t nlen = (uint32_t)len;
    if (write(fd, &nlen, 4) != 4) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, json + off, len - off);
        if (n > 0) off += (size_t)n;
        else if (errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

/* Simple JSON value extraction */
static const char *json_string_value(const char *json, const char *key) {
    const char *p = json;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    p = strstr(p, search);
    if (!p) return NULL;
    p += strlen(search);
    /* skip colon, whitespace, opening quote */
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    if (*p != '"') return NULL;
    p++;
    return p; /* caller must handle terminating " */
}

static volatile int keep_running = 1;
static void handle_signal(int sig) {
    (void)sig;
    keep_running = 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: fake_uds_agent <socket_path>\n");
        return 1;
    }

    const char *socket_path = argv[1];

    /* Remove stale socket if present */
    unlink(socket_path);

    /* Create listening socket */
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    /* Set FD_CLOEXEC */
    int flags = fcntl(listen_fd, F_GETFD, 0);
    if (flags >= 0) fcntl(listen_fd, F_SETFD, flags | FD_CLOEXEC);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    size_t path_len = strlen(socket_path);
    if (path_len >= sizeof(addr.sun_path)) {
        fprintf(stderr, "socket path too long\n");
        close(listen_fd);
        return 1;
    }
    memcpy(addr.sun_path, socket_path, path_len + 1);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 5) != 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    /* Allow the parent to connect immediately */
    fprintf(stderr, "fake_uds_agent: listening on %s\n", socket_path);

    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    while (keep_running) {
        struct sockaddr_un peer;
        socklen_t peer_len = sizeof(peer);
        int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        flags = fcntl(client_fd, F_GETFD, 0);
        if (flags >= 0) fcntl(client_fd, F_SETFD, flags | FD_CLOEXEC);

        fprintf(stderr, "fake_uds_agent: client connected\n");

        /* Process frames until EOF */
        while (keep_running) {
            size_t frame_len = 0;
            char *frame = read_frame(client_fd, &frame_len);
            if (!frame) {
                fprintf(stderr, "fake_uds_agent: EOF or error\n");
                break;
            }

            fprintf(stderr, "fake_uds_agent: received frame (len=%zu): %s\n",
                    frame_len, frame);

            /* Determine response based on frame type */
            if (strstr(frame, "\"type\":\"hello\"") || strstr(frame, "\"type\": \"hello\"")) {
                /* Respond with native agent hello */
                const char *resp = "{\"type\":\"hello\",\"role\":\"native_agent\",\"protocol_version\":1,"
                    "\"agent_name\":\"fake-uds-agent\",\"agent_version\":\"1.0.0\","
                    "\"supported_transports\":[\"uds\"],\"supported_events\":[\"response\",\"error\"],"
                    "\"capabilities\":{\"session_state\":true}}";
                write_frame(client_fd, resp, strlen(resp));
                fprintf(stderr, "fake_uds_agent: sent hello response\n");
                /* Send ready frame to indicate agent is loaded */
                const char *ready = "{\"type\":\"ready\",\"status\":\"ready\",\"model_loaded\":true,\"agent\":\"fake-uds-agent\"}";
                write_frame(client_fd, ready, strlen(ready));
                fprintf(stderr, "fake_uds_agent: sent ready frame\n");
            } else if (strstr(frame, "\"type\":\"health\"") || strstr(frame, "\"type\": \"health\"") ||
                strstr(frame, "\"type\":\"shutdown\"")) {
                /* Extract health id if present */
                const char *id_ptr = json_string_value(frame, "id");
                char id_buf[128] = "";
                if (id_ptr) {
                    const char *end = strchr(id_ptr, '"');
                    if (!end) end = id_ptr + strlen(id_ptr);
                    size_t max = (size_t)(end - id_ptr);
                    if (max > sizeof(id_buf) - 1) max = sizeof(id_buf) - 1;
                    memcpy(id_buf, id_ptr, max);
                    id_buf[max] = '\0';
                }
                /* Respond with health pong */
                char resp[512];
                snprintf(resp, sizeof(resp),
                         "{\"type\":\"health\",\"id\":\"%s\",\"status\":\"ok\",\"agent\":\"fake-uds-agent\"}", id_buf);
                write_frame(client_fd, resp, strlen(resp));
                fprintf(stderr, "fake_uds_agent: sent health response (id=%s)\n", id_buf);
            } else if (strstr(frame, "\"type\":\"ping\"") || strstr(frame, "\"type\": \"ping\"")) {
                /* Extract ping id if present */
                const char *ping_id_ptr = json_string_value(frame, "id");
                char ping_id_buf[128] = "";
                if (ping_id_ptr) {
                    const char *end = strchr(ping_id_ptr, '"');
                    if (!end) end = ping_id_ptr + strlen(ping_id_ptr);
                    size_t max = (size_t)(end - ping_id_ptr);
                    if (max > sizeof(ping_id_buf) - 1) max = sizeof(ping_id_buf) - 1;
                    memcpy(ping_id_buf, ping_id_ptr, max);
                    ping_id_buf[max] = '\0';
                }
                /* Respond with pong */
                char resp[512];
                snprintf(resp, sizeof(resp),
                         "{\"type\":\"pong\",\"id\":\"%s\",\"status\":\"ok\"}", ping_id_buf);
                write_frame(client_fd, resp, strlen(resp));
                fprintf(stderr, "fake_uds_agent: sent pong response (id=%s)\n", ping_id_buf);
            } else if (strstr(frame, "\"type\":\"request\"")) {
                const char *req_id_ptr = json_string_value(frame, "id");
                char req_id_buf[128] = "req-1";
                if (req_id_ptr) {
                    const char *end = strchr(req_id_ptr, '"');
                    if (!end) end = req_id_ptr + strlen(req_id_ptr);
                    size_t max = (size_t)(end - req_id_ptr);
                    if (max > sizeof(req_id_buf) - 1) max = sizeof(req_id_buf) - 1;
                    memcpy(req_id_buf, req_id_ptr, max);
                    req_id_buf[max] = '\0';
                }

                /* Send turn-level ack first */
                char ack[256];
                snprintf(ack, sizeof(ack),
                         "{\"type\":\"ack\",\"id\":\"%s\",\"status\":\"accepted\"}", req_id_buf);
                write_frame(client_fd, ack, strlen(ack));

                /* Extract input text for echo */
                const char *input_ptr = json_string_value(frame, "input");
                char input_buf[4096];
                if (input_ptr) {
                    const char *end = strchr(input_ptr, '"');
                    if (!end) end = input_ptr + strlen(input_ptr);
                    size_t max = (size_t)(end - input_ptr);
                    if (max > sizeof(input_buf) - 1) max = sizeof(input_buf) - 1;
                    memcpy(input_buf, input_ptr, max);
                    input_buf[max] = '\0';
                    /* Build echo response */
                    char resp[8192];
                    snprintf(resp, sizeof(resp),
                             "{\"type\":\"response\",\"id\":\"%s\",\"status\":\"completed\","
                             "\"output\":\"Fake UDS agent received: %s\"}", req_id_buf, input_buf);
                    write_frame(client_fd, resp, strlen(resp));
                    fprintf(stderr, "fake_uds_agent: sent request response\n");
                } else {
                    const char *resp = "{\"type\":\"response\",\"status\":\"completed\",\"output\":\"Fake UDS agent received input\"}";
                    write_frame(client_fd, resp, strlen(resp));
                }
            } else if (strstr(frame, "\"type\":\"create_state\"")) {
                /* Accept create_state and respond with ack containing state_id */
                const char *key_ptr = json_string_value(frame, "key");
                char key_buf[256] = "";
                if (key_ptr) {
                    const char *end = strchr(key_ptr, '"');
                    if (!end) end = key_ptr + strlen(key_ptr);
                    size_t max = (size_t)(end - key_ptr);
                    if (max > sizeof(key_buf) - 1) max = sizeof(key_buf) - 1;
                    memcpy(key_buf, key_ptr, max);
                    key_buf[max] = '\0';
                }
                /* Generate a deterministic state_id based on key hash */
                unsigned long h = 0;
                for (const char *p = key_buf; *p; p++) h = h * 131 + (unsigned char)*p;
                char state_id[64];
                snprintf(state_id, sizeof(state_id), "state_%lx", h);
                char ack[512];
                snprintf(ack, sizeof(ack),
                         "{\"type\":\"ack\",\"status\":\"accepted\",\"state_id\":\"%s\"}", state_id);
                write_frame(client_fd, ack, strlen(ack));
                fprintf(stderr, "fake_uds_agent: created state key=%s state_id=%s\n", key_buf, state_id);
            } else if (strstr(frame, "\"type\":\"load_state\"") || strstr(frame, "\"type\":\"save_state\"")) {
                /* Accept load/save_state and respond with ack */
                const char *ack = "{\"type\":\"ack\",\"status\":\"accepted\"}";
                write_frame(client_fd, ack, strlen(ack));
                fprintf(stderr, "fake_uds_agent: acknowledged state operation\n");
            } else if (strstr(frame, "\"type\":\"switch_state\"")) {
                /* Accept switch_state and respond with ack */
                const char *ack = "{\"type\":\"ack\",\"status\":\"accepted\"}";
                write_frame(client_fd, ack, strlen(ack));
                fprintf(stderr, "fake_uds_agent: acknowledged state switch\n");
            } else if (strstr(frame, "\"type\":\"error\"")) {
                /* Ignore error frames silently */
                fprintf(stderr, "fake_uds_agent: ignoring error frame\n");
            } else {
                /* Default response */
                const char *resp = "{\"type\":\"response\",\"status\":\"completed\",\"output\":\"Fake UDS agent: unknown frame type\"}";
                write_frame(client_fd, resp, strlen(resp));
            }

            free(frame);
        }

        close(client_fd);
        fprintf(stderr, "fake_uds_agent: client disconnected\n");
    }

    close(listen_fd);
    unlink(socket_path);
    fprintf(stderr, "fake_uds_agent: shutting down\n");
    return 0;
}
