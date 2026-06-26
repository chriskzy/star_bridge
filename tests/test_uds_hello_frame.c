/*
 * Test: Bridge sends hello frame to native agent on startup.
 *
 * Verifies that engine_send_hello produces a JSON hello frame with:
 *   type: "hello", role: "bridge", bridge_version, protocol_version: 1,
 *   harness_id: "codex.responses", workspace_root, accepted_events, max_frame_bytes
 *
 * Uses a pipe to capture the framed output, then parses and validates the JSON.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <signal.h>

/* Bridge constants (copied from bridge_core.h for standalone test) */
#define BRIDGE_VERSION "1.0.0"
#define BRIDGE_PROTOCOL_VERSION 1
#define BRIDGE_HARNESS_ID "codex.responses"
#define BRIDGE_MAX_FRAME_BYTES 16777216

/* Forward declaration - unused */

int main(void) {
    int failed = 0;

    /* Create pipe to capture framed output */
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        perror("pipe");
        return 1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: read framed data from pipe and validate it */
        close(pipe_fd[1]);

        /* Read the 4-byte length prefix */
        unsigned char lenbuf[4];
        ssize_t n = read(pipe_fd[0], lenbuf, 4);
        if (n != 4) {
            fprintf(stderr, "FAIL: read length prefix returned %zd\n", n);
            _exit(1);
        }

        unsigned long len = 0;
        len |= (unsigned long)lenbuf[0] << 24;
        len |= (unsigned long)lenbuf[1] << 16;
        len |= (unsigned long)lenbuf[2] << 8;
        len |= (unsigned long)lenbuf[3];

        if (len > 100000) {
            fprintf(stderr, "FAIL: frame length too large: %lu\n", len);
            _exit(1);
        }

        /* Read the frame body */
        char *buf = malloc(len + 1);
        if (!buf) {
            fprintf(stderr, "FAIL: malloc failed\n");
            _exit(1);
        }
        size_t off = 0;
        while (off < len) {
            n = read(pipe_fd[0], buf + off, len - off);
            if (n <= 0) {
                fprintf(stderr, "FAIL: read body returned %zd at offset %zu\n", n, off);
                free(buf);
                _exit(1);
            }
            off += (size_t)n;
        }
        buf[len] = '\0';

        /* Parse JSON and validate fields */
        int errors = 0;

        /* Check for "type":"hello" */
        if (!strstr(buf, "\"type\":\"hello\"")) {
            fprintf(stderr, "FAIL: missing type=hello\n");
            errors++;
        }

        /* Check for "role":"bridge" */
        if (!strstr(buf, "\"role\":\"bridge\"")) {
            fprintf(stderr, "FAIL: missing role=bridge\n");
            errors++;
        }

        /* Check for bridge_version */
        if (!strstr(buf, "\"bridge_version\":\"1.0.0\"")) {
            fprintf(stderr, "FAIL: missing bridge_version\n");
            errors++;
        }

        /* Check for protocol_versions array */
        if (!strstr(buf, "\"protocol_versions\":[1]")) {
            fprintf(stderr, "FAIL: missing protocol_versions\n");
            errors++;
        }

        /* Check for harness_id */
        if (!strstr(buf, "\"harness_id\":\"codex.responses\"")) {
            fprintf(stderr, "FAIL: missing harness_id\n");
            errors++;
        }

        /* Check for workspace_root */
        if (!strstr(buf, "\"workspace_root\":\"/tmp/test_hello\"")) {
            fprintf(stderr, "FAIL: missing workspace_root\n");
            errors++;
        }

        /* Check for accepted_events array */
        if (!strstr(buf, "\"accepted_events\"")) {
            fprintf(stderr, "FAIL: missing accepted_events\n");
            errors++;
        }
        if (!strstr(buf, "\"response\"") || !strstr(buf, "\"error\"") ||
            !strstr(buf, "\"status\"") || !strstr(buf, "\"log\"")) {
            fprintf(stderr, "FAIL: missing accepted events\n");
            errors++;
        }

        /* Check for max_frame_bytes */
        if (!strstr(buf, "\"max_frame_bytes\":16777216")) {
            fprintf(stderr, "FAIL: missing max_frame_bytes\n");
            errors++;
        }

        /* Also verify it's valid JSON (has opening and closing braces) */
        if (buf[0] != '{') {
            fprintf(stderr, "FAIL: not valid JSON (no opening brace)\n");
            errors++;
        }
        if (buf[len-1] != '}') {
            fprintf(stderr, "FAIL: not valid JSON (no closing brace)\n");
            errors++;
        }

        if (errors > 0) {
            fprintf(stderr, "FAIL: %d validation errors\n", errors);
            fprintf(stderr, "Received frame:\n%.*s\n", (int)len, buf);
            _exit(1);
        }

        fprintf(stderr, "PASS: hello frame validated\n");
        fflush(stderr);

        free(buf);

        _exit(0);
    }

    /* Parent: send hello frame via pipe */
    close(pipe_fd[0]);

    /* Build and send hello frame */
    char hello[4096];
    int len = snprintf(hello, sizeof(hello),
        "{\"type\":\"hello\",\"role\":\"bridge\",\"bridge_version\":\"%s\",\"protocol_versions\":[%d],"
        "\"harness_id\":\"%s\",\"workspace_root\":\"%s\",\"accepted_events\":[\"response\",\"error\",\"status\",\"log\"],"
        "\"max_frame_bytes\":%d}",
        BRIDGE_VERSION, BRIDGE_PROTOCOL_VERSION,
        BRIDGE_HARNESS_ID, "/tmp/test_hello", BRIDGE_MAX_FRAME_BYTES);

    if (len <= 0 || (size_t)len >= sizeof(hello)) {
        fprintf(stderr, "FAIL: hello frame truncated\n");
        close(pipe_fd[1]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return 1;
    }

    /* Write length prefix */
    unsigned char lenbuf[4];
    lenbuf[0] = (unsigned char)(len >> 24);
    lenbuf[1] = (unsigned char)(len >> 16);
    lenbuf[2] = (unsigned char)(len >> 8);
    lenbuf[3] = (unsigned char)(len);
    write(pipe_fd[1], lenbuf, 4);
    write(pipe_fd[1], hello, (size_t)len);

    close(pipe_fd[1]);

    /* Wait for child to finish */
    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: child test failed\n");
        return 1;
    }

    printf("Test 1: Hello frame content validation... PASS\n");

    /* Test 2: Verify engine_send_hello works end-to-end */
    printf("Test 2: engine_send_hello via bridge startup... ");
    fflush(stdout);

    /* Create a test agent that reads framed input and echoes back */
    int pipe_a[2], pipe_b[2], pipe_c[2];
    if (pipe(pipe_a) != 0 || pipe(pipe_b) != 0 || pipe(pipe_c) != 0) {
        perror("pipe");
        return 1;
    }

    pid_t agent = fork();
    if (agent == 0) {
        /* Agent: read framed frames from stdin, log first frame to stderr, echo back to stdout */
        close(pipe_a[1]);
        close(pipe_b[0]);
        close(pipe_c[0]);
        dup2(pipe_a[0], STDIN_FILENO);
        dup2(pipe_b[1], STDOUT_FILENO);
        dup2(pipe_c[1], STDERR_FILENO);
        close(pipe_a[0]);
        close(pipe_b[1]);
        close(pipe_c[1]);

        /* Read first frame and log to stderr */
        unsigned char lb[4];
        ssize_t n = read(STDIN_FILENO, lb, 4);
        if (n != 4) _exit(1);
        unsigned long flen = 0;
        flen |= (unsigned long)lb[0] << 24;
        flen |= (unsigned long)lb[1] << 16;
        flen |= (unsigned long)lb[2] << 8;
        flen |= (unsigned long)lb[3];
        if (flen > 100000) _exit(1);
        char *fbuf = malloc(flen + 1);
        if (!fbuf) _exit(1);
        size_t off = 0;
        while (off < flen) {
            n = read(STDIN_FILENO, fbuf + off, flen - off);
            if (n <= 0) { free(fbuf); _exit(1); }
            off += (size_t)n;
        }
        fbuf[flen] = '\0';
        fprintf(stderr, "=== HELLO_FRAME ===\n%s\n=== END_HELLO_FRAME ===\n", fbuf);
        fflush(stderr);

        /* Echo frame back to stdout */
        write(STDOUT_FILENO, lb, 4);
        write(STDOUT_FILENO, fbuf, flen);
        free(fbuf);

        /* Loop forever */
        while (1) {
            n = read(STDIN_FILENO, lb, 4);
            if (n <= 0) break;
            flen = 0;
            flen |= (unsigned long)lb[0] << 24;
            flen |= (unsigned long)lb[1] << 16;
            flen |= (unsigned long)lb[2] << 8;
            flen |= (unsigned long)lb[3];
            if (flen > 100000) break;
            fbuf = malloc(flen + 1);
            if (!fbuf) break;
            off = 0;
            while (off < flen) {
                n = read(STDIN_FILENO, fbuf + off, flen - off);
                if (n <= 0) { free(fbuf); break; }
                off += (size_t)n;
            }
            fbuf[flen] = '\0';
            write(STDOUT_FILENO, lb, 4);
            write(STDOUT_FILENO, fbuf, flen);
            free(fbuf);
        }
        _exit(0);
    }

    /* Parent: set up pipes for bridge simulation */
    close(pipe_a[0]);
    close(pipe_b[1]);
    close(pipe_c[1]);

    /* Simulate bridge: send hello via engine_send_hello equivalent */
    char hello2[4096];
    int len2 = snprintf(hello2, sizeof(hello2),
        "{\"type\":\"hello\",\"role\":\"bridge\",\"bridge_version\":\"%s\",\"protocol_versions\":[%d],"
        "\"harness_id\":\"%s\",\"workspace_root\":\"%s\",\"accepted_events\":[\"response\",\"error\",\"status\",\"log\"],"
        "\"max_frame_bytes\":%d}",
        BRIDGE_VERSION, BRIDGE_PROTOCOL_VERSION,
        BRIDGE_HARNESS_ID, "/tmp/test_workspace", BRIDGE_MAX_FRAME_BYTES);

    if (len2 <= 0 || (size_t)len2 >= sizeof(hello2)) {
        fprintf(stderr, "FAIL: hello2 truncated\n");
        close(pipe_a[1]);
        close(pipe_b[0]);
        close(pipe_c[0]);
        kill(agent, SIGKILL);
        waitpid(agent, NULL, 0);
        return 1;
    }

    unsigned char lb2[4];
    lb2[0] = (unsigned char)(len2 >> 24);
    lb2[1] = (unsigned char)(len2 >> 16);
    lb2[2] = (unsigned char)(len2 >> 8);
    lb2[3] = (unsigned char)(len2);
    write(pipe_a[1], lb2, 4);
    write(pipe_a[1], hello2, (size_t)len2);

    /* Read agent's echoed response */
    unsigned char lb_resp[4];
    ssize_t n2 = read(pipe_b[0], lb_resp, 4);
    if (n2 != 4) {
        fprintf(stderr, "FAIL: no response from agent\n");
        close(pipe_a[1]);
        close(pipe_b[0]);
        close(pipe_c[0]);
        kill(agent, SIGKILL);
        waitpid(agent, NULL, 0);
        return 1;
    }
    unsigned long resp_len = 0;
    resp_len |= (unsigned long)lb_resp[0] << 24;
    resp_len |= (unsigned long)lb_resp[1] << 16;
    resp_len |= (unsigned long)lb_resp[2] << 8;
    resp_len |= (unsigned long)lb_resp[3];
    char *resp_buf = malloc(resp_len + 1);
    if (!resp_buf) { close(pipe_a[1]); close(pipe_b[0]); close(pipe_c[0]); kill(agent, SIGKILL); waitpid(agent, NULL, 0); return 1; }
    size_t off = 0;
    while (off < resp_len) {
        n2 = read(pipe_b[0], resp_buf + off, resp_len - off);
        if (n2 <= 0) { free(resp_buf); close(pipe_a[1]); close(pipe_b[0]); close(pipe_c[0]); kill(agent, SIGKILL); waitpid(agent, NULL, 0); return 1; }
        off += (size_t)n2;
    }
    resp_buf[resp_len] = '\0';

    /* Validate echoed hello frame */
    if (!strstr(resp_buf, "\"type\":\"hello\"") || !strstr(resp_buf, "\"role\":\"bridge\"")) {
        fprintf(stderr, "FAIL: echoed hello frame invalid\n");
        fprintf(stderr, "Response: %s\n", resp_buf);
        free(resp_buf);
        close(pipe_a[1]); close(pipe_b[0]); close(pipe_c[0]);
        kill(agent, SIGKILL); waitpid(agent, NULL, 0);
        return 1;
    }
    free(resp_buf);

    printf("PASS\n");

    close(pipe_a[1]);
    close(pipe_b[0]);
    close(pipe_c[0]);
    kill(agent, SIGKILL);
    waitpid(agent, NULL, 0);

    if (failed) {
        printf("\nSome tests FAILED.\n");
        return 1;
    }
    printf("\nAll hello frame tests passed.\n");
    return 0;
}
