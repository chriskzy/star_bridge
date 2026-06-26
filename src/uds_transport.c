#include "uds_transport.h"
#include "config_manager.h"
#include "native_frame.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* Connect to a Unix-domain socket at the given path.
 * Uses AF_UNIX / SOCK_STREAM with blocking connect plus bounded timeout.
 * Timeout is in milliseconds; 0 means use blocking connect (no timeout).
 * Sets FD_CLOEXEC on the resulting fd.
 */
UDSConnectResult uds_connect(const char *path, int timeout_ms) {
    UDSConnectResult result = { -1, -1, NULL };

    if (!path || path[0] == '\0') {
        result.error = "native_socket_path_empty";
        return result;
    }

    /* Create the socket */
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        result.error = "native_socket_create_failed";
        return result;
    }

    /* Set FD_CLOEXEC so child processes don't inherit the fd */
    int flags = fcntl(sock, F_GETFD, 0);
    if (flags >= 0) {
        fcntl(sock, F_SETFD, flags | FD_CLOEXEC);
    }

    /* Prepare the socket address */
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    /* Check path length against sun_path limit */
    size_t path_len = strlen(path);
    if (path_len >= sizeof(addr.sun_path)) {
        close(sock);
        result.error = "native_socket_path_too_long";
        return result;
    }
    memcpy(addr.sun_path, path, path_len + 1);

    /* If timeout is 0, use blocking connect directly */
    if (timeout_ms <= 0) {
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            result.fd = sock;
            return result;
        }
        /* Differentiate ECONNREFUSED for stale socket handling */
        if (errno == ECONNREFUSED) {
            result.error = "native_socket_connection_refused";
        } else {
            result.error = "native_socket_connect_failed";
        }
        close(sock);
        return result;
    }

    /* Use non-blocking connect + poll for bounded timeout */
    /* Set socket to non-blocking */
    int old_flags = fcntl(sock, F_GETFL, 0);
    if (old_flags >= 0) {
        fcntl(sock, F_SETFL, old_flags | O_NONBLOCK);
    }

    int connect_result = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (connect_result == 0) {
        /* Connected immediately */
        /* Restore blocking mode */
        if (old_flags >= 0) {
            fcntl(sock, F_SETFL, old_flags);
        }
        result.fd = sock;
        return result;
    }

    if (connect_result < 0 && errno != EINPROGRESS) {
        /* Real error (not just in progress) */
        if (errno == ECONNREFUSED) {
            /* Stale socket — check if we should clean up */
            /* Store error for caller to handle */
            result.error = "native_socket_connection_refused";
            close(sock);
            return result;
        }
        if (errno == ENOENT) {
            /* Socket file does not exist yet — wait for it to appear */
            /* Poll with stat() in a loop bounded by timeout_ms */
            int waited = 0;
            struct stat st;
            while (waited < timeout_ms) {
                /* Check if socket file exists */
                if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode)) {
                    /* Socket exists now — retry connect */
                    int retry = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
                    if (retry == 0) {
                        /* Restore blocking mode */
                        if (old_flags >= 0) {
                            fcntl(sock, F_SETFL, old_flags);
                        }
                        result.fd = sock;
                        return result;
                    }
                    if (retry < 0 && errno != EINPROGRESS) {
                        /* Still not connectable */
                        if (errno == ECONNREFUSED) {
                            result.error = "native_socket_connection_refused";
                        } else {
                            result.error = "native_socket_connect_failed";
                        }
                        close(sock);
                        return result;
                    }
                    /* Now in progress — poll for completion */
                    struct pollfd pfd;
                    pfd.fd = sock;
                    pfd.events = POLLOUT;
                    int remaining = timeout_ms - waited;
                    if (remaining < 0) remaining = 0;
                    int poll_result = poll(&pfd, 1, remaining);
                    if (poll_result <= 0) {
                        result.error = poll_result == 0 ? "native_socket_connect_timeout" : "native_socket_poll_failed";
                        close(sock);
                        return result;
                    }
                    /* Check connection success */
                    int err = 0;
                    socklen_t errlen = sizeof(err);
                    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &errlen) == 0 && err == 0) {
                        /* Restore blocking mode */
                        if (old_flags >= 0) {
                            fcntl(sock, F_SETFL, old_flags);
                        }
                        result.fd = sock;
                        return result;
                    }
                    result.error = "native_socket_connect_failed";
                    close(sock);
                    return result;
                }
                /* Sleep 10ms and continue waiting */
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
                nanosleep(&ts, NULL);
                waited += 10;
            }
            /* Timeout waiting for socket to appear */
            result.error = "native_socket_connect_timeout";
            close(sock);
            return result;
        }
        result.error = "native_socket_connect_failed";
        close(sock);
        return result;
    }

    /* Connection in progress — poll for completion */
    struct pollfd pfd;
    pfd.fd = sock;
    pfd.events = POLLOUT;

    int poll_result = poll(&pfd, 1, timeout_ms);
    if (poll_result <= 0) {
        /* Timeout or error */
        if (poll_result == 0) {
            result.error = "native_socket_connect_timeout";
        } else {
            result.error = "native_socket_poll_failed";
        }
        close(sock);
        return result;
    }

    /* Check if connection succeeded */
    int so_error = 0;
    socklen_t errlen = sizeof(so_error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &errlen) == 0 && so_error == 0) {
        /* Connected successfully */
        /* Restore blocking mode */
        if (old_flags >= 0) {
            fcntl(sock, F_SETFL, old_flags);
        }
        result.fd = sock;
        return result;
    }

    /* Connection failed */
    if (so_error == ECONNREFUSED) {
        result.error = "native_socket_connection_refused";
    } else if (so_error == ETIMEDOUT) {
        result.error = "native_socket_connect_timeout";
    } else {
        result.error = "native_socket_connect_failed";
    }
    close(sock);
    return result;
}

/* Clean up a stale socket file before connecting.
 * Only unlinks in launch_and_connect mode.
 * Returns true if cleanup was performed.
 */
bool uds_cleanup_stale(const char *path) {
    if (!path || path[0] == '\0') return false;

    /* Check owner mode — only clean up in launch_and_connect */
    if (strcmp(global_config.uds_owner_mode, "launch_and_connect") != 0) {
        return false;
    }

    fprintf(stdout, "Cleaning up stale socket: %s\n", path);
    unlink(path);
    return true;
}

/* -------------------------------------------------------------------
 *  Frame I/O over UDS — reuses the same length-prefixed protocol
 * ------------------------------------------------------------------- */

/* Read a length-prefixed JSON frame from a UDS fd.
 * Delegates to frame_read() from native_frame.c which uses the
 * same 4-byte LE length-prefixed protocol as stdio_framed.
 * Enforces FRAME_MAX_PAYLOAD on the read path.
 * deadline_ms: -1 means blocking, 0 means non-blocking, positive means timeout ms.
 */
char *uds_frame_read(int fd, size_t *out_len, int deadline_ms, const char **error_out) {
    return frame_read(fd, out_len, deadline_ms, error_out);
}

/* Write a length-prefixed JSON frame to a UDS fd.
 * Delegates to frame_write() from native_frame.c which uses the
 * same 4-byte LE length-prefixed protocol as stdio_framed.
 * Enforces FRAME_MAX_PAYLOAD on the write path.
 */
bool uds_frame_write(int fd, const char *json, size_t len) {
    return frame_write(fd, json, len);
}

/* Write a typed frame (e.g. {"type":"hello","role":"bridge",...}) to a UDS fd.
 * Combines a type field with a JSON body into a single frame.
 */
bool uds_frame_write_typed(int fd, const char *type, const char *body_json, size_t body_len) {
    return frame_write_typed(fd, type, body_json, body_len);
}

/* -------------------------------------------------------------------
 *  Partial read/write helpers (shared by stdio_framed and UDS)
 * ------------------------------------------------------------------- */

/* Attempt to read up to n bytes from fd. Handles EINTR and EAGAIN/EWOULDBLOCK.
 * Returns bytes read, 0 on EOF, -1 on error.
 * This is a partial read — it may return fewer than count bytes if
 * fewer are available. It does not spin waiting for the full count.
 */
ssize_t uds_read_partial(int fd, char *buf, size_t count) {
    ssize_t n;
    do {
        n = read(fd, buf, count);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        /* No data available yet — not an error for non-blocking fd */
        return 0;
    }
    return n;
}

/* Attempt to write up to n bytes to fd. Handles EINTR and partial writes.
 * Returns bytes written, -1 on error.
 */
ssize_t uds_write_partial(int fd, const char *buf, size_t count) {
    size_t off = 0;
    while (off < count) {
        ssize_t n = write(fd, buf + off, count - off);
        if (n > 0) {
            off += (size_t)n;
        } else if (n == 0) {
            return (ssize_t)off;
        } else if (errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return (ssize_t)off;
}
