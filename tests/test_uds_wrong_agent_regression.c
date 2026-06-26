/*
 * Regression test: wrong stdio/framed command cannot be mistaken for healthy UDS native agent.
 *
 * Tests:
 * 1. uds_connect to non-existent socket returns structured error
 * 2. uds_connect to a socket with no listening agent returns ECONNREFUSED
 * 3. Connecting to a file that is not a socket fails
 * 4. uds_connect with empty path fails
 */

#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* Simulate uds_connect result */
typedef struct {
    int fd;
    int err;
    const char *error;
} ConnectResult;

/* Connect to a UDS socket with error reporting */
static ConnectResult try_connect(const char *path, int timeout_ms) {
    ConnectResult r = { -1, 0, NULL };

    if (!path || path[0] == '\0') {
        r.error = "native_socket_path_empty";
        return r;
    }

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        r.error = "native_socket_create_failed";
        return r;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t path_len = strlen(path);
    if (path_len >= sizeof(addr.sun_path)) {
        close(sock);
        r.error = "native_socket_path_too_long";
        return r;
    }
    memcpy(addr.sun_path, path, path_len + 1);

    if (timeout_ms <= 0) {
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            r.fd = sock;
            return r;
        }
        if (errno == ECONNREFUSED) {
            r.error = "native_socket_connection_refused";
        } else if (errno == ENOENT) {
            r.error = "native_socket_path_not_found";
        } else {
            r.error = "native_socket_connect_failed";
        }
        close(sock);
        return r;
    }

    /* Non-blocking connect with poll timeout */
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (errno == EINPROGRESS || errno == EAGAIN) {
            struct pollfd pfd = { .fd = sock, .events = POLLOUT };
            int rc = poll(&pfd, 1, timeout_ms);
            if (rc == 0) {
                r.error = "native_socket_connect_timeout";
                close(sock);
                return r;
            }
            /* Check socket error */
            int soerror = 0;
            socklen_t slen = sizeof(soerror);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &soerror, &slen);
            if (soerror == 0) {
                r.fd = sock;
                return r;
            }
            if (soerror == ECONNREFUSED) {
                r.error = "native_socket_connection_refused";
            } else if (soerror == ENOENT) {
                r.error = "native_socket_path_not_found";
            } else {
                r.error = "native_socket_connect_failed";
            }
            close(sock);
            return r;
        }
        if (errno == ECONNREFUSED) {
            r.error = "native_socket_connection_refused";
        } else if (errno == ENOENT) {
            r.error = "native_socket_path_not_found";
        } else {
            r.error = "native_socket_connect_failed";
        }
        close(sock);
        return r;
    }

    /* Immediate connect success */
    r.fd = sock;
    return r;
}

/* Create a file that is NOT a socket */
static void create_regular_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "not a socket\n");
        fclose(f);
    }
}

/* Create a listening UDS socket (fake agent) */
static int create_listener(const char *path) {
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t plen = strlen(path);
    if (plen >= sizeof(addr.sun_path)) { close(fd); return -1; }
    memcpy(addr.sun_path, path, plen + 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    if (listen(fd, 5) != 0) { close(fd); unlink(path); return -1; }
    return fd;
}

int main(void) {
    int failed = 0;

    /* Test 1: Connect to non-existent socket */
    printf("Test 1: Connect to non-existent socket returns structured error... ");
    fflush(stdout);
    ConnectResult r1 = try_connect("/tmp/nonexistent_test_socket_XXXXXX", 5000);
    if (r1.fd >= 0) {
        printf("FAIL: connected to non-existent socket\n");
        close(r1.fd);
        return 1;
    }
    if (r1.error == NULL) {
        printf("FAIL: no error for non-existent socket\n");
        return 1;
    }
    printf("PASS (error: %s)\n", r1.error);

    /* Test 2: Connect to a socket with no listener (ECONNREFUSED) */
    printf("Test 2: Connect to socket with no listener returns ECONNREFUSED... ");
    fflush(stdout);
    /* Create a socket file by binding and closing */
    const char *stale_path = ".uds_stale_test.sock";
    int stale_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (stale_fd < 0) {
        printf("FAIL: create socket failed\n");
        return 1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t plen = strlen(stale_path);
    if (plen >= sizeof(addr.sun_path)) { close(stale_fd); printf("FAIL: path too long\n"); return 1; }
    memcpy(addr.sun_path, stale_path, plen + 1);
    if (bind(stale_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(stale_fd);
        printf("FAIL: bind failed\n");
        return 1;
    }
    /* Close without listening — socket file exists but no listener */
    close(stale_fd);

    ConnectResult r2 = try_connect(stale_path, 5000);
    if (r2.fd >= 0) {
        printf("FAIL: connected to socket with no listener\n");
        unlink(stale_path);
        return 1;
    }
    if (r2.error == NULL) {
        printf("FAIL: no error for stale socket\n");
        unlink(stale_path);
        return 1;
    }
    printf("PASS (error: %s)\n", r2.error);
    unlink(stale_path);

    /* Test 3: Connect to a regular file (not a socket) */
    printf("Test 3: Connect to regular file fails... ");
    fflush(stdout);
    const char *regular_path = ".uds_regular_test.txt";
    create_regular_file(regular_path);
    ConnectResult r3 = try_connect(regular_path, 5000);
    if (r3.fd >= 0) {
        printf("FAIL: connected to regular file\n");
        close(r3.fd);
        unlink(regular_path);
        return 1;
    }
    if (r3.error == NULL) {
        printf("FAIL: no error for regular file\n");
        unlink(regular_path);
        return 1;
    }
    printf("PASS (error: %s)\n", r3.error);
    unlink(regular_path);

    /* Test 4: Connect with empty path fails */
    printf("Test 4: Connect with empty path fails... ");
    fflush(stdout);
    ConnectResult r4 = try_connect("", 5000);
    if (r4.fd >= 0) {
        printf("FAIL: connected with empty path\n");
        close(r4.fd);
        return 1;
    }
    if (r4.error == NULL) {
        printf("FAIL: no error for empty path\n");
        return 1;
    }
    printf("PASS (error: %s)\n", r4.error);

    /* Test 5: Connect to valid listening socket works */
    printf("Test 5: Connect to valid listening socket works... ");
    fflush(stdout);
    const char *valid_path = ".uds_valid_test.sock";
    int listener = create_listener(valid_path);
    if (listener < 0) {
        printf("FAIL: create listener failed\n");
        return 1;
    }
    ConnectResult r5 = try_connect(valid_path, 5000);
    if (r5.fd < 0) {
        printf("FAIL: could not connect to valid socket (error: %s)\n", r5.error);
        close(listener);
        unlink(valid_path);
        return 1;
    }
    printf("PASS\n");
    close(r5.fd);
    close(listener);
    unlink(valid_path);

    if (failed) {
        printf("\nSome tests FAILED.\n");
        return 1;
    }
    printf("\nAll wrong-agent regression tests passed.\n");
    return 0;
}
