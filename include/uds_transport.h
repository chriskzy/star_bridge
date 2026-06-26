#ifndef UDS_TRANSPORT_H
#define UDS_TRANSPORT_H

#include <stdbool.h>
#include <sys/types.h>
#include <stddef.h>

/* UDS connection result */
typedef struct {
    int fd;              /* Socket fd, or -1 on error */
    int error_fd;        /* -1 unless error occurred */
    const char *error;   /* NULL if successful, static error string otherwise */
} UDSConnectResult;

/* Connect to a Unix-domain socket at the given path.
 * Uses AF_UNIX / SOCK_STREAM with blocking connect plus bounded timeout.
 * Timeout is in milliseconds; 0 means no timeout (use blocking connect).
 * Sets FD_CLOEXEC on the resulting fd.
 * On success: fd >= 0, error == NULL.
 * On failure: fd == -1, error points to a static string.
 */
UDSConnectResult uds_connect(const char *path, int timeout_ms);

/* Clean up a stale socket file before connecting.
 * Only unlinks in launch_and_connect mode.
 * Returns true if cleanup was performed.
 */
bool uds_cleanup_stale(const char *path);

/* Read a length-prefixed JSON frame from a UDS fd.
 * Reuses the same 4-byte LE length-prefixed protocol as stdio_framed.
 * Returns NULL on EOF/error. Caller must free the buffer.
 * If error_out is non-NULL, *error_out is set to a static error string on failure
 * (e.g. "payload_too_large", "read_error", "truncated_header", "truncated_payload",
 *  "timeout", "out_of_memory"), or NULL on success.
 * Enforces FRAME_MAX_PAYLOAD.
 * deadline_ms: -1 means blocking (no timeout), 0 means non-blocking (poll with 0),
 *              positive means timeout in milliseconds for the entire frame read.
 */
char *uds_frame_read(int fd, size_t *out_len, int deadline_ms, const char **error_out);

/* Write a length-prefixed JSON frame to a UDS fd.
 * Reuses the same 4-byte LE length-prefixed protocol as stdio_framed.
 * Enforces FRAME_MAX_PAYLOAD on the write path.
 * Returns true on success.
 */
bool uds_frame_write(int fd, const char *json, size_t len);

/* Write a typed frame (e.g. {"type":"hello","role":"bridge",...}) to a UDS fd.
 * Combines a type field with a JSON body into a single frame.
 */
bool uds_frame_write_typed(int fd, const char *type, const char *body_json, size_t body_len);

/* Partial read helper: attempt to read up to n bytes from fd.
 * Handles EINTR. Returns bytes read, 0 on EOF, -1 on error.
 */
ssize_t uds_read_partial(int fd, char *buf, size_t count);

/* Partial write helper: attempt to write up to n bytes to fd.
 * Handles EINTR and partial writes. Returns bytes written, -1 on error.
 */
ssize_t uds_write_partial(int fd, const char *buf, size_t count);

#endif /* UDS_TRANSPORT_H */
