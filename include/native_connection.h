#ifndef NATIVE_CONNECTION_H
#define NATIVE_CONNECTION_H

#include <sys/types.h>
#include <stdbool.h>
#include <stddef.h>

/* Transport types */
typedef enum {
    TRANSPORT_NONE = 0,
    TRANSPORT_STDIO_FRAMED,
    TRANSPORT_UDS
} TransportType;

/* Connection states */
typedef enum {
    CONN_DISCONNECTED = 0,
    CONN_CONNECTING,
    CONN_CONNECTED,
    CONN_HANDSHAKING,
    CONN_READY,
    CONN_UNHEALTHY,
    CONN_CLOSED
} ConnectionState;

/* NativeConnection struct — all fields required by Task 12 */
typedef struct {
    TransportType   transport;          /* Which transport is active */
    int             read_fd;            /* Read fd (stdout pipe or UDS fd) */
    int             write_fd;           /* Write fd (stdin pipe or UDS fd) */
    int             socket_fd;          /* UDS socket fd (-1 if not UDS) */
    pid_t           child_pid;          /* Child PID (0 if connect_existing) */
    ConnectionState state;              /* Current connection state */
    int             protocol_version;   /* Negotiated protocol version (0 = not negotiated) */
    int             reconnect_attempts; /* Number of reconnect attempts so far */
    int             reconnect_max;      /* Max reconnect attempts (0 = disabled) */
    int             reconnect_delay_ms; /* Base delay between retries (exponential backoff) */
} NativeConnection;

/* Initialize a NativeConnection to default values */
void nc_init(NativeConnection *nc);

/* Set up stdio_framed transport using existing pipe fds */
void nc_set_stdio(NativeConnection *nc, int stdin_fd, int stdout_fd, pid_t child_pid);

/* Set up UDS transport using a connected socket fd */
void nc_set_uds(NativeConnection *nc, int sock_fd, pid_t child_pid);

/* Close write fd (shutdown write side) */
void nc_shutdown_write(NativeConnection *nc);

/* Close all fds and reset connection to DISCONNECTED */
void nc_close(NativeConnection *nc);

/* State transition helper */
void nc_set_state(NativeConnection *nc, ConnectionState new_state);

/* Read a length-prefixed frame from the connection.
 * Uses the appropriate read fd. Returns NULL on EOF/error.
 * Caller must free buffer. */
char *nc_read_frame(NativeConnection *nc, size_t *out_len);

/* Read a frame with timeout (ms). -1 means no timeout (blocking).
 * If error_out is non-NULL, *error_out is set to a static error string on failure
 * (e.g. "timeout", "payload_too_large", "read_error"), or NULL on success. */
char *nc_read_frame_timeout(NativeConnection *nc, size_t *out_len, int timeout_ms, const char **error_out);

/* Write a length-prefixed frame to the connection.
 * Uses the appropriate write fd. Returns true on success. */
bool nc_write_frame(NativeConnection *nc, const char *json, size_t len);

/* Attempt to reconnect the UDS connection.
 * Connects to the given socket path with bounded retries and exponential backoff.
 * Returns true if reconnection succeeded.
 * Does nothing if transport is not UDS or if reconnect_max <= 0. */
bool nc_reconnect(NativeConnection *nc, const char *socket_path, int timeout_ms);

#endif /* NATIVE_CONNECTION_H */
