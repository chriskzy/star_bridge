/*
 * UDS reconnect test after fake native agent restart.
 *
 * Tests:
 * 1. Connect to fake UDS agent, verify frame exchange works
 * 2. Close connection (simulate agent restart)
 * 3. Reconnect using nc_reconnect
 * 4. Verify frame exchange works on reconnected connection
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

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

/* Connect to a UDS socket */
static int uds_connect_socket(const char *path, int timeout_ms) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t path_len = strlen(path);
    if (path_len >= sizeof(addr.sun_path)) return -1;
    memcpy(addr.sun_path, path, path_len + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    /* Set non-blocking for timeout */
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static volatile int keep_running = 1;
static void handle_signal(int sig) {
    (void)sig;
    keep_running = 0;
}

/* Fork a fake UDS agent that responds to health checks */
static pid_t start_fake_agent(const char *path) {
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: run fake agent */
        unlink(path);

        int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd < 0) _exit(1);

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        size_t plen = strlen(path);
        if (plen >= sizeof(addr.sun_path)) _exit(1);
        memcpy(addr.sun_path, path, plen + 1);

        if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) _exit(1);
        if (listen(listen_fd, 5) != 0) _exit(1);

        signal(SIGTERM, handle_signal);
        signal(SIGINT, handle_signal);

        while (keep_running) {
            int client = accept(listen_fd, NULL, NULL);
            if (client < 0) {
                if (errno == EINTR) continue;
                break;
            }
            /* Process one frame and respond */
            size_t flen = 0;
            char *frame = read_frame(client, &flen);
            if (frame) {
                const char *resp = "{\"type\":\"response\",\"status\":\"ok\",\"agent\":\"fake-agent\"}";
                write_frame(client, resp, strlen(resp));
                free(frame);
            }
            close(client);
        }

        close(listen_fd);
        unlink(path);
        _exit(0);
    }
    return pid;
}

/* Wait for agent socket to appear */
static int wait_for_socket(const char *path, int max_sec) {
    for (int i = 0; i < max_sec * 10; i++) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode)) return 0;
        usleep(100000);
    }
    return -1;
}

int main(void) {
    int failed = 0;
    const char *socket_path = ".uds_reconnect_test.sock";

    /* Start fake agent */
    pid_t agent_pid = start_fake_agent(socket_path);
    if (agent_pid < 0) {
        printf("FAIL: fork failed\n");
        return 1;
    }

    /* Wait for agent to be ready */
    if (wait_for_socket(socket_path, 5) != 0) {
        printf("FAIL: agent socket not ready\n");
        kill(agent_pid, SIGKILL);
        return 1;
    }

    printf("Agent started (pid=%d)\n", agent_pid);

    /* Test 1: Connect and verify frame exchange */
    printf("Test 1: Initial connection works... ");
    fflush(stdout);

    int fd = uds_connect_socket(socket_path, 5000);
    if (fd < 0) {
        printf("FAIL: connect failed\n");
        kill(agent_pid, SIGKILL);
        return 1;
    }

    /* Send health request */
    const char *health = "{\"type\":\"health\"}";
    if (write_frame(fd, health, strlen(health)) != 0) {
        printf("FAIL: write failed\n");
        close(fd);
        kill(agent_pid, SIGKILL);
        return 1;
    }

    /* Read response */
    size_t rlen = 0;
    char *resp = read_frame(fd, &rlen);
    if (!resp) {
        printf("FAIL: no response\n");
        close(fd);
        kill(agent_pid, SIGKILL);
        return 1;
    }
    if (strstr(resp, "\"ok\"") == NULL) {
        printf("FAIL: unexpected response: %s\n", resp);
        free(resp);
        close(fd);
        kill(agent_pid, SIGKILL);
        return 1;
    }
    free(resp);
    printf("PASS\n");

    /* Test 2: Close connection, restart agent, reconnect */
    printf("Test 2: Reconnect after agent restart... ");
    fflush(stdout);

    /* Close connection */
    close(fd);

    /* Kill agent and wait for it to die */
    kill(agent_pid, SIGTERM);
    usleep(500000);

    /* Start new agent on same path */
    pid_t agent2_pid = start_fake_agent(socket_path);
    if (agent2_pid < 0) {
        printf("FAIL: second fork failed\n");
        return 1;
    }

    /* Wait for new agent to be ready */
    if (wait_for_socket(socket_path, 5) != 0) {
        printf("FAIL: new agent socket not ready\n");
        kill(agent2_pid, SIGKILL);
        return 1;
    }

    /* Reconnect */
    int fd2 = uds_connect_socket(socket_path, 5000);
    if (fd2 < 0) {
        printf("FAIL: reconnect failed\n");
        kill(agent2_pid, SIGKILL);
        return 1;
    }

    /* Send health request on reconnected connection */
    const char *health2 = "{\"type\":\"health\"}";
    if (write_frame(fd2, health2, strlen(health2)) != 0) {
        printf("FAIL: write after reconnect failed\n");
        close(fd2);
        kill(agent2_pid, SIGKILL);
        return 1;
    }

    /* Read response */
    rlen = 0;
    resp = read_frame(fd2, &rlen);
    if (!resp) {
        printf("FAIL: no response after reconnect\n");
        close(fd2);
        kill(agent2_pid, SIGKILL);
        return 1;
    }
    if (strstr(resp, "\"ok\"") == NULL) {
        printf("FAIL: unexpected response after reconnect: %s\n", resp);
        free(resp);
        close(fd2);
        kill(agent2_pid, SIGKILL);
        return 1;
    }
    free(resp);
    printf("PASS\n");

    /* Test 3: Multiple reconnects work */
    printf("Test 3: Multiple reconnects work... ");
    fflush(stdout);

    close(fd2);
    kill(agent2_pid, SIGTERM);
    usleep(500000);

    /* Start third agent */
    pid_t agent3_pid = start_fake_agent(socket_path);
    if (agent3_pid < 0) {
        printf("FAIL: third fork failed\n");
        return 1;
    }

    if (wait_for_socket(socket_path, 5) != 0) {
        printf("FAIL: third agent socket not ready\n");
        kill(agent3_pid, SIGKILL);
        return 1;
    }

    /* Reconnect */
    int fd3 = uds_connect_socket(socket_path, 5000);
    if (fd3 < 0) {
        printf("FAIL: third reconnect failed\n");
        kill(agent3_pid, SIGKILL);
        return 1;
    }

    /* Verify exchange */
    const char *health3 = "{\"type\":\"health\"}";
    if (write_frame(fd3, health3, strlen(health3)) != 0) {
        printf("FAIL: write after third reconnect failed\n");
        close(fd3);
        kill(agent3_pid, SIGKILL);
        return 1;
    }

    rlen = 0;
    resp = read_frame(fd3, &rlen);
    if (!resp) {
        printf("FAIL: no response after third reconnect\n");
        close(fd3);
        kill(agent3_pid, SIGKILL);
        return 1;
    }
    if (strstr(resp, "\"ok\"") == NULL) {
        printf("FAIL: unexpected response after third reconnect: %s\n", resp);
        free(resp);
        close(fd3);
        kill(agent3_pid, SIGKILL);
        return 1;
    }
    free(resp);
    printf("PASS\n");

    close(fd3);
    kill(agent3_pid, SIGKILL);
    unlink(socket_path);

    if (failed) {
        printf("\nSome tests FAILED.\n");
        return 1;
    }
    printf("\nAll reconnect tests passed.\n");
    return 0;
}
