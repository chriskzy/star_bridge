#include "native_connection.h"
#include "native_frame.h"
#include "uds_transport.h"
#include "debug_trace.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>

/* Initialize a NativeConnection to default values */
void nc_init(NativeConnection *nc) {
    if (!nc) return;
    memset(nc, 0, sizeof(*nc));
    nc->read_fd = -1;
    nc->write_fd = -1;
    nc->socket_fd = -1;
    nc->child_pid = 0;
    nc->transport = TRANSPORT_NONE;
    nc->state = CONN_DISCONNECTED;
    nc->protocol_version = 0;
    nc->reconnect_attempts = 0;
    nc->reconnect_max = 3;          /* Default max 3 attempts */
    nc->reconnect_delay_ms = 1000;  /* Default base delay 1 second */
}

/* Set up stdio_framed transport using existing pipe fds */
void nc_set_stdio(NativeConnection *nc, int stdin_fd, int stdout_fd, pid_t child_pid) {
    if (!nc) return;
    nc->transport = TRANSPORT_STDIO_FRAMED;
    nc->write_fd = stdin_fd;
    nc->read_fd = stdout_fd;
    nc->socket_fd = -1;
    nc->child_pid = child_pid;
    nc->state = CONN_CONNECTED;
}

/* Set up UDS transport using a connected socket fd */
void nc_set_uds(NativeConnection *nc, int sock_fd, pid_t child_pid) {
    if (!nc) return;
    nc->transport = TRANSPORT_UDS;
    nc->read_fd = sock_fd;
    nc->write_fd = sock_fd;
    nc->socket_fd = sock_fd;
    nc->child_pid = child_pid;
    nc->state = CONN_CONNECTED;
}

/* Close write fd (shutdown write side) */
void nc_shutdown_write(NativeConnection *nc) {
    if (!nc) return;
    if (nc->write_fd >= 0) {
        shutdown(nc->write_fd, SHUT_WR);
    }
}

/* Close all fds and reset connection to DISCONNECTED */
void nc_close(NativeConnection *nc) {
    if (!nc) return;
    if (nc->read_fd >= 0 && nc->read_fd != nc->write_fd) {
        close(nc->read_fd);
    }
    if (nc->write_fd >= 0) {
        close(nc->write_fd);
    }
    if (nc->socket_fd >= 0 && nc->socket_fd != nc->read_fd && nc->socket_fd != nc->write_fd) {
        close(nc->socket_fd);
    }
    nc->read_fd = -1;
    nc->write_fd = -1;
    nc->socket_fd = -1;
    nc->state = CONN_DISCONNECTED;
}

/* State transition helper */
void nc_set_state(NativeConnection *nc, ConnectionState new_state) {
    if (!nc) return;
    ConnectionState old = nc->state;
    nc->state = new_state;
    /* Log state transitions for handshake debugging */
    const char *names[] = {
        "disconnected", "connecting", "connected",
        "handshaking", "ready", "unhealthy", "closed"
    };
    const char *old_name = (old >= 0 && old < 7) ? names[old] : "?";
    const char *new_name = (new_state >= 0 && new_state < 7) ? names[new_state] : "?";
    debug_trace_append("connection_state transport=%s state=%s->%s",
                       nc->transport == TRANSPORT_UDS ? "uds" : "stdio_framed",
                       old_name, new_name);
}

/* Read a frame with optional timeout.
 * timeout_ms is passed as deadline_ms to the underlying frame reader:
 *   -1 means blocking (no timeout),
 *    0 means non-blocking,
 *    positive means timeout in milliseconds.
 */
char *nc_read_frame_timeout(NativeConnection *nc, size_t *out_len, int timeout_ms, const char **error_out) {
    if (!nc || nc->read_fd < 0) {
        if (out_len) *out_len = 0;
        if (error_out) *error_out = "no_connection";
        return NULL;
    }

    /* Use the appropriate frame reader based on transport.
     * Both uds_frame_read and frame_read now accept deadline_ms internally,
     * so we pass timeout_ms directly as the deadline. */
    switch (nc->transport) {
        case TRANSPORT_UDS:
            return uds_frame_read(nc->read_fd, out_len, timeout_ms, error_out);
        case TRANSPORT_STDIO_FRAMED:
            return frame_read(nc->read_fd, out_len, timeout_ms, error_out);
        default:
            if (out_len) *out_len = 0;
            if (error_out) *error_out = "unknown_transport";
            return NULL;
    }
}

/* Read a frame without timeout (blocking) */
char *nc_read_frame(NativeConnection *nc, size_t *out_len) {
    return nc_read_frame_timeout(nc, out_len, -1, NULL);
}

/* Write a length-prefixed frame to the connection */
bool nc_write_frame(NativeConnection *nc, const char *json, size_t len) {
    if (!nc || nc->write_fd < 0 || !json || len == 0) return false;

    switch (nc->transport) {
        case TRANSPORT_UDS:
            return uds_frame_write(nc->write_fd, json, len);
        case TRANSPORT_STDIO_FRAMED:
            return frame_write(nc->write_fd, json, len);
        default:
            return false;
    }
}

/* Attempt to reconnect the UDS connection with bounded retries and exponential backoff. */
bool nc_reconnect(NativeConnection *nc, const char *socket_path, int timeout_ms) {
    if (!nc) return false;
    if (nc->transport != TRANSPORT_UDS) return false;
    if (nc->reconnect_max <= 0) return false;
    if (!socket_path || socket_path[0] == '\0') return false;

    /* If already connected, close old connection first */
    if (nc->state > CONN_DISCONNECTED) {
        nc_close(nc);
    }

    nc_set_state(nc, CONN_CONNECTING);
    nc->reconnect_attempts = 0;

    int delay = nc->reconnect_delay_ms;
    while (nc->reconnect_attempts < nc->reconnect_max) {
        debug_trace_append("reconnect_attempt transport=uds attempt=%d max=%d socket_path=%s timeout_ms=%d",
                           nc->reconnect_attempts + 1, nc->reconnect_max,
                           socket_path, timeout_ms);

        UDSConnectResult result = uds_connect(socket_path, timeout_ms);
        if (result.fd >= 0) {
            /* Connected successfully */
            nc->read_fd = result.fd;
            nc->write_fd = result.fd;
            nc->socket_fd = result.fd;
            nc->state = CONN_CONNECTED;
            nc->reconnect_attempts = 0;
            debug_trace_append("reconnect_result transport=uds status=connected fd=%d attempts=%d",
                               result.fd, nc->reconnect_attempts);
            return true;
        }

        nc->reconnect_attempts++;
        debug_trace_append("reconnect_result transport=uds status=failed error=%s attempt=%d",
                           result.error ? result.error : "unknown", nc->reconnect_attempts);

        /* Exponential backoff: double delay each retry, cap at 30 seconds */
        if (nc->reconnect_attempts < nc->reconnect_max) {
            int sleep_ms = delay;
            if (sleep_ms > 30000) sleep_ms = 30000;
            usleep((useconds_t)sleep_ms * 1000);
            delay *= 2;
        }
    }

    nc_set_state(nc, CONN_UNHEALTHY);
    debug_trace_append("reconnect_result transport=uds status=exhausted attempts=%d max=%d",
                       nc->reconnect_attempts, nc->reconnect_max);
    return false;
}
