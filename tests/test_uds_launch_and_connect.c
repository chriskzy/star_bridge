/*
 * UDS integration test for bridge-launched fake agent in launch_and_connect mode.
 *
 * This program:
 * 1. Forks a child that creates a UDS socket and acts as a fake native agent
 * 2. The parent waits for the socket to appear, connects, and exchanges frames
 * 3. Verifies health and request frames work over UDS
 *
 * This simulates the bridge-launched-agent pattern where the bridge
 * forks the agent, the agent creates the socket, and the bridge connects.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* Read exactly n bytes from fd */
static int read_exact(int fd, char *buf, size_t count) {
    size_t off = 0;
    while (off < count) {
        ssize_t n = read(fd, buf + off, count - off);
        if (n > 0) off += (size_t)n;
        else if (n == 0) return -1;
        else if (errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

/* Read a length-prefixed frame */
static char *read_frame(int fd, size_t *out_len) {
    uint32_t len = 0;
    if (read_exact(fd, (char *)&len, 4) != 0) return NULL;
    if (len == 0) return strdup("");
    if (len > 16777216u) return NULL;
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

/* Write a length-prefixed frame */
static int write_frame(int fd, const char *json, size_t len) {
    uint32_t nlen = (uint32_t)len;
    ssize_t w = write(fd, &nlen, 4);
    if (w < 0) return -1;
    if ((size_t)w != 4) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, json + off, len - off);
        if (n > 0) off += (size_t)n;
        else if (errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

/* Child process: create UDS socket, listen, act as fake agent */
static void child_main(const char *socket_path) {
    /* Remove stale socket if present */
    unlink(socket_path);

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("child: socket");
        _exit(1);
    }

    int flags = fcntl(listen_fd, F_GETFD, 0);
    if (flags >= 0) fcntl(listen_fd, F_SETFD, flags | FD_CLOEXEC);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    size_t path_len = strlen(socket_path);
    if (path_len >= sizeof(addr.sun_path)) {
        fprintf(stderr, "child: socket path too long\n");
        _exit(1);
    }
    memcpy(addr.sun_path, socket_path, path_len + 1);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("child: bind");
        _exit(1);
    }

    if (listen(listen_fd, 5) != 0) {
        perror("child: listen");
        _exit(1);
    }

    /* Signal parent that socket is ready */
    fprintf(stderr, "child: listening on %s\n", socket_path);

    /* Accept one connection */
    struct sockaddr_un peer;
    socklen_t peer_len = sizeof(peer);
    int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
    if (client_fd < 0) {
        perror("child: accept");
        _exit(1);
    }

    flags = fcntl(client_fd, F_GETFD, 0);
    if (flags >= 0) fcntl(client_fd, F_SETFD, flags | FD_CLOEXEC);

    /* Process frames until EOF */
    while (1) {
        size_t frame_len = 0;
        char *frame = read_frame(client_fd, &frame_len);
        if (!frame) break;

        if (strstr(frame, "\"type\":\"health\"")) {
            /* Extract health id if present */
            char id_buf[128] = "";
            const char *id_key = strstr(frame, "\"id\":\"");
            if (id_key) {
                id_key += 5;
                const char *end = strchr(id_key, '"');
                if (!end) end = id_key + strlen(id_key);
                size_t max = (size_t)(end - id_key);
                if (max > sizeof(id_buf) - 1) max = sizeof(id_buf) - 1;
                memcpy(id_buf, id_key, max);
                id_buf[max] = '\0';
            }
            char resp[512];
            snprintf(resp, sizeof(resp),
                     "{\"type\":\"health\",\"id\":\"%s\",\"status\":\"ok\"}", id_buf);
            write_frame(client_fd, resp, strlen(resp));
        } else if (strstr(frame, "\"type\":\"request\"")) {
            const char *input_ptr = strstr(frame, "\"input\":\"");
            char input_buf[4096];
            if (input_ptr) {
                input_ptr += 9;
                const char *end = strchr(input_ptr, '"');
                if (!end) end = input_ptr + strlen(input_ptr);
                size_t max = (size_t)(end - input_ptr);
                if (max > sizeof(input_buf) - 1) max = sizeof(input_buf) - 1;
                memcpy(input_buf, input_ptr, max);
                input_buf[max] = '\0';
                char resp[8192];
                snprintf(resp, sizeof(resp),
                         "{\"type\":\"response\",\"id\":\"req-1\",\"status\":\"completed\","
                         "\"output\":\"Launch test agent received: %s\"}", input_buf);
                write_frame(client_fd, resp, strlen(resp));
            } else {
                const char *resp = "{\"type\":\"response\",\"status\":\"completed\",\"output\":\"Launch test agent received input\"}";
                write_frame(client_fd, resp, strlen(resp));
            }
        } else {
            const char *resp = "{\"type\":\"response\",\"status\":\"completed\",\"output\":\"Launch test agent: unknown frame\"}";
            write_frame(client_fd, resp, strlen(resp));
        }
        free(frame);
    }

    close(client_fd);
    close(listen_fd);
    unlink(socket_path);
    _exit(0);
}

/* Check if JSON string contains a key-value pair */
static int json_contains(const char *json, const char *key, const char *value) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "\"%s\":\"%s\"", key, value);
    return strstr(json, buf) != NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: test_uds_launch_and_connect <socket_path>\n");
        return 1;
    }

    const char *socket_path = argv[1];

    printf("Launch and connect test...\n");
    printf("Socket path: %s\n", socket_path);

    /* Fork child that creates UDS socket */
    pid_t child_pid = fork();
    if (child_pid < 0) {
        perror("fork");
        return 1;
    }

    if (child_pid == 0) {
        /* Child: create socket and act as agent */
        child_main(socket_path);
        /* child_main does not return */
    }

    /* Parent: wait for socket to appear, then connect */
    printf("Waiting for socket to appear...\n");
    int waited = 0;
    while (waited < 200) {
        struct stat st;
        if (stat(socket_path, &st) == 0 && S_ISSOCK(st.st_mode)) {
            break;
        }
        usleep(10000); /* 10ms */
        waited++;
    }
    if (waited >= 200) {
        printf("FAIL: timeout waiting for socket\n");
        kill(child_pid, SIGTERM);
        waitpid(child_pid, NULL, 0);
        unlink(socket_path);
        return 1;
    }
    printf("Socket appeared after %dms\n", waited * 10);

    /* Connect to the socket */
    int fd = -1;
    {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        size_t path_len = strlen(socket_path);
        if (path_len >= sizeof(addr.sun_path)) {
            printf("FAIL: socket path too long\n");
            kill(child_pid, SIGTERM);
            waitpid(child_pid, NULL, 0);
            unlink(socket_path);
            return 1;
        }
        memcpy(addr.sun_path, socket_path, path_len + 1);

        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            perror("socket");
            kill(child_pid, SIGTERM);
            waitpid(child_pid, NULL, 0);
            unlink(socket_path);
            return 1;
        }

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            perror("connect");
            close(fd);
            kill(child_pid, SIGTERM);
            waitpid(child_pid, NULL, 0);
            unlink(socket_path);
            return 1;
        }
    }
    printf("Connected (fd=%d)\n", fd);

    /* Test 1: Send health frame and verify response */
    printf("Test 1: health frame... ");
    fflush(stdout);

    const char *health = "{\"type\":\"health\"}";
    if (write_frame(fd, health, strlen(health)) != 0) {
        printf("FAIL: write health frame\n");
        close(fd);
        kill(child_pid, SIGTERM);
        waitpid(child_pid, NULL, 0);
        unlink(socket_path);
        return 1;
    }

    size_t resp_len = 0;
    char *resp = read_frame(fd, &resp_len);
    if (!resp) {
        printf("FAIL: no health response\n");
        close(fd);
        kill(child_pid, SIGTERM);
        waitpid(child_pid, NULL, 0);
        unlink(socket_path);
        return 1;
    }

    if (!json_contains(resp, "status", "ok")) {
        printf("FAIL: status not ok (got: %s)\n", resp);
        free(resp);
        close(fd);
        kill(child_pid, SIGTERM);
        waitpid(child_pid, NULL, 0);
        unlink(socket_path);
        return 1;
    }
    printf("PASS (response: %s)\n", resp);
    free(resp);

    /* Test 2: Send request frame and verify echo response */
    printf("Test 2: request frame... ");
    fflush(stdout);

    const char *request = "{\"type\":\"request\",\"input\":\"hello from launch_and_connect test\"}";
    if (write_frame(fd, request, strlen(request)) != 0) {
        printf("FAIL: write request frame\n");
        close(fd);
        kill(child_pid, SIGTERM);
        waitpid(child_pid, NULL, 0);
        unlink(socket_path);
        return 1;
    }

    resp = read_frame(fd, &resp_len);
    if (!resp) {
        printf("FAIL: no request response\n");
        close(fd);
        kill(child_pid, SIGTERM);
        waitpid(child_pid, NULL, 0);
        unlink(socket_path);
        return 1;
    }

    if (!json_contains(resp, "status", "completed")) {
        printf("FAIL: status not completed (got: %s)\n", resp);
        free(resp);
        close(fd);
        kill(child_pid, SIGTERM);
        waitpid(child_pid, NULL, 0);
        unlink(socket_path);
        return 1;
    }
    if (!strstr(resp, "hello from launch_and_connect test")) {
        printf("FAIL: input not echoed (got: %s)\n", resp);
        free(resp);
        close(fd);
        kill(child_pid, SIGTERM);
        waitpid(child_pid, NULL, 0);
        unlink(socket_path);
        return 1;
    }
    printf("PASS (response: %s)\n", resp);
    free(resp);

    /* Test 3: Send unknown frame and verify response */
    printf("Test 3: unknown frame... ");
    fflush(stdout);

    const char *unknown = "{\"type\":\"ping\"}";
    if (write_frame(fd, unknown, strlen(unknown)) != 0) {
        printf("FAIL: write unknown frame\n");
        close(fd);
        kill(child_pid, SIGTERM);
        waitpid(child_pid, NULL, 0);
        unlink(socket_path);
        return 1;
    }

    resp = read_frame(fd, &resp_len);
    if (!resp) {
        printf("FAIL: no unknown frame response\n");
        close(fd);
        kill(child_pid, SIGTERM);
        waitpid(child_pid, NULL, 0);
        unlink(socket_path);
        return 1;
    }

    if (!json_contains(resp, "status", "completed")) {
        printf("FAIL: status not completed (got: %s)\n", resp);
        free(resp);
        close(fd);
        kill(child_pid, SIGTERM);
        waitpid(child_pid, NULL, 0);
        unlink(socket_path);
        return 1;
    }
    printf("PASS (response: %s)\n", resp);
    free(resp);

    /* Clean up: close connection, wait for child */
    close(fd);
    printf("Closing connection, waiting for child...\n");

    int status;
    waitpid(child_pid, &status, 0);
    unlink(socket_path);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL: child exited with status %d\n", status);
        return 1;
    }

    printf("\nAll launch_and_connect tests passed.\n");
    return 0;
}
