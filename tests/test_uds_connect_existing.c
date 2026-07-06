/*
 * UDS unit test for successful connect_existing connection.
 * Connects to a running fake UDS agent, exchanges length-prefixed frames,
 * and verifies responses.
 *
 * Usage: test_uds_connect_existing <socket_path>
 */

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
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
    if (w < 0) { perror("write_len"); return -1; }
    if ((size_t)w != 4) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, json + off, len - off);
        if (n > 0) off += (size_t)n;
        else if (errno == EINTR) continue;
        else { perror("write_body"); return -1; }
    }
    return 0;
}

/* Connect to a UDS socket */
static int uds_connect(const char *path) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t path_len = strlen(path);
    if (path_len >= sizeof(addr.sun_path)) return -1;
    memcpy(addr.sun_path, path, path_len + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

/* Check if JSON string contains a key-value pair */
static int json_contains(const char *json, const char *key, const char *value) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "\"%s\":\"%s\"", key, value);
    return strstr(json, buf) != NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: test_uds_connect_existing <socket_path>\n");
        return 1;
    }

    const char *socket_path = argv[1];

    printf("Connecting to fake UDS agent at %s...\n", socket_path);

    int fd = uds_connect(socket_path);
    if (fd < 0) {
        fprintf(stderr, "FAIL: could not connect to UDS socket\n");
        return 1;
    }
    printf("Connected (fd=%d)\n", fd);

    /* Test 1: Send health frame and verify response */
    printf("Test 1: health frame... ");
    fflush(stdout);

    const char *health = "{\"type\":\"health\"}";
    fprintf(stderr, "DEBUG: write %zu bytes to fd %d\n", strlen(health), fd);
    int rc = write_frame(fd, health, strlen(health));
    fprintf(stderr, "DEBUG: write_frame returned %d\n", rc);
    if (rc != 0) {
        printf("FAIL: write health frame\n");
        close(fd);
        return 1;
    }

    size_t resp_len = 0;
    char *resp = read_frame(fd, &resp_len);
    if (!resp) {
        printf("FAIL: no health response\n");
        close(fd);
        return 1;
    }

    if (!json_contains(resp, "status", "ok")) {
        printf("FAIL: status not ok (got: %s)\n", resp);
        free(resp);
        close(fd);
        return 1;
    }
    if (!json_contains(resp, "agent", "fake-uds-agent")) {
        printf("FAIL: agent not fake-uds-agent (got: %s)\n", resp);
        free(resp);
        close(fd);
        return 1;
    }
    printf("PASS (response: %s)\n", resp);
    free(resp);

    /* Test 2: Send request frame and verify echo response */
    printf("Test 2: request frame... ");
    fflush(stdout);

    const char *request = "{\"type\":\"request\",\"input\":\"hello from connect_existing test\"}";
    if (write_frame(fd, request, strlen(request)) != 0) {
        printf("FAIL: write request frame\n");
        close(fd);
        return 1;
    }

    /* Read frames until we get a non-ack response */
    resp = NULL;
    for (int i = 0; i < 5; i++) {
        char *tmp = read_frame(fd, &resp_len);
        if (!tmp) {
            printf("FAIL: no request response\n");
            close(fd);
            return 1;
        }
        /* Skip ack frames */
        if (strstr(tmp, "\"ack\"")) {
            free(tmp);
            continue;
        }
        resp = tmp;
        break;
    }
    if (!resp) {
        printf("FAIL: no non-ack request response\n");
        close(fd);
        return 1;
    }

    if (!json_contains(resp, "status", "completed")) {
        printf("FAIL: status not completed (got: %s)\n", resp);
        free(resp);
        close(fd);
        return 1;
    }
    if (!strstr(resp, "hello from connect_existing test")) {
        printf("FAIL: input not echoed (got: %s)\n", resp);
        free(resp);
        close(fd);
        return 1;
    }
    printf("PASS (response: %s)\n", resp);
    free(resp);

    /* Test 3: Send unknown frame and verify response */
    printf("Test 3: unknown frame... ");
    fflush(stdout);

    const char *unknown = "{\"type\":\"unknown\"}";
    if (write_frame(fd, unknown, strlen(unknown)) != 0) {
        printf("FAIL: write unknown frame\n");
        close(fd);
        return 1;
    }

    resp = read_frame(fd, &resp_len);
    if (!resp) {
        printf("FAIL: no unknown frame response\n");
        close(fd);
        return 1;
    }

    if (!json_contains(resp, "status", "completed")) {
        printf("FAIL: status not completed (got: %s)\n", resp);
        free(resp);
        close(fd);
        return 1;
    }
    printf("PASS (response: %s)\n", resp);
    free(resp);

    close(fd);
    printf("\nAll connect_existing tests passed.\n");
    return 0;
}
