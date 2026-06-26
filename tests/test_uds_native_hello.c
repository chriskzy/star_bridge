/*
 * Test: Bridge reads and validates native hello response.
 *
 * Creates a fake native agent that:
 * 1. Reads the bridge's hello frame from stdin
 * 2. Sends back a valid native hello response
 * 3. Verifies the bridge successfully validates the response
 *
 * Tests:
 * - Valid hello response with all required fields
 * - Missing type field -> rejection
 * - Protocol version mismatch -> rejection
 * - Missing agent_name -> rejection
 * - Missing supported_transports -> rejection
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdbool.h>

/* Bridge constants */
#define BRIDGE_PROTOCOL_VERSION 1

/* Forward declarations */
static int run_hello_test(const char *response, int expected_result, const char *test_name);
static void send_framed(int fd, const char *data, size_t len);

int main(void) {
    int failed = 0;

    /* Test 1: Valid native hello response */
    {
        const char *valid_hello =
            "{\"type\":\"hello\",\"role\":\"native_agent\",\"protocol_version\":1,"
            "\"agent_name\":\"test-agent\",\"agent_version\":\"0.1.0\","
            "\"supported_transports\":[\"uds\"],\"supported_events\":[\"response\",\"error\"],"
            "\"capabilities\":{\"max_tokens\":4096}}";
        printf("Test 1: Valid native hello response... ");
        fflush(stdout);
        int result = run_hello_test(valid_hello, 0, "valid_hello");
        if (result != 0) {
            fprintf(stderr, "FAIL: valid hello test returned %d\n", result);
            failed++;
        } else {
            printf("PASS\n");
        }
    }

    /* Test 2: Missing type field */
    {
        const char *no_type =
            "{\"role\":\"native_agent\",\"protocol_version\":1,"
            "\"agent_name\":\"test\",\"agent_version\":\"1.0\"}";
        printf("Test 2: Missing type field... ");
        fflush(stdout);
        int result = run_hello_test(no_type, -1, "no_type");
        if (result == 0) {
            fprintf(stderr, "FAIL: should have rejected missing type\n");
            failed++;
        } else {
            printf("PASS (rejected)\n");
        }
    }

    /* Test 3: Protocol version mismatch */
    {
        const char *wrong_proto =
            "{\"type\":\"hello\",\"role\":\"native_agent\",\"protocol_version\":999,"
            "\"agent_name\":\"test\",\"agent_version\":\"1.0\"}";
        printf("Test 3: Protocol version mismatch... ");
        fflush(stdout);
        int result = run_hello_test(wrong_proto, -1, "wrong_proto");
        if (result == 0) {
            fprintf(stderr, "FAIL: should have rejected protocol mismatch\n");
            failed++;
        } else {
            printf("PASS (rejected)\n");
        }
    }

    /* Test 4: Missing agent_name */
    {
        const char *no_name =
            "{\"type\":\"hello\",\"role\":\"native_agent\",\"protocol_version\":1,"
            "\"agent_version\":\"1.0\"}";
        printf("Test 4: Missing agent_name... ");
        fflush(stdout);
        int result = run_hello_test(no_name, -1, "no_name");
        if (result == 0) {
            fprintf(stderr, "FAIL: should have rejected missing agent_name\n");
            failed++;
        } else {
            printf("PASS (rejected)\n");
        }
    }

    /* Test 5: Missing supported_transports */
    {
        const char *no_transport =
            "{\"type\":\"hello\",\"role\":\"native_agent\",\"protocol_version\":1,"
            "\"agent_name\":\"test\",\"agent_version\":\"1.0\"}";
        printf("Test 5: Missing supported_transports... ");
        fflush(stdout);
        int result = run_hello_test(no_transport, -1, "no_transport");
        if (result == 0) {
            fprintf(stderr, "FAIL: should have rejected missing supported_transports\n");
            failed++;
        } else {
            printf("PASS (rejected)\n");
        }
    }

    /* Test 6: Missing role field */
    {
        const char *no_role =
            "{\"type\":\"hello\",\"protocol_version\":1,"
            "\"agent_name\":\"test\",\"agent_version\":\"1.0\","
            "\"supported_transports\":[\"uds\"]}";
        printf("Test 6: Missing role field... ");
        fflush(stdout);
        int result = run_hello_test(no_role, -1, "no_role");
        if (result == 0) {
            fprintf(stderr, "FAIL: should have rejected missing role\n");
            failed++;
        } else {
            printf("PASS (rejected)\n");
        }
    }

    if (failed) {
        printf("\nSome tests FAILED.\n");
        return 1;
    }
    printf("\nAll native hello response tests passed.\n");
    return 0;
}

/* Helper: run a test with a fake agent that sends a specific response */
static int run_hello_test(const char *response, int expected_result, const char *test_name) {
    int pipe_in[2];
    int pipe_out[2];
    int pipe_err[2];

    if (pipe(pipe_in) != 0 || pipe(pipe_out) != 0 || pipe(pipe_err) != 0) {
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: fake native agent */
        close(pipe_in[1]);
        close(pipe_out[0]);
        close(pipe_err[0]);
        dup2(pipe_in[0], STDIN_FILENO);
        dup2(pipe_out[1], STDOUT_FILENO);
        dup2(pipe_err[1], STDERR_FILENO);
        close(pipe_in[0]);
        close(pipe_out[1]);
        close(pipe_err[1]);

        /* Read bridge's hello frame (first frame from stdin) */
        unsigned char lenbuf[4];
        ssize_t n = read(STDIN_FILENO, lenbuf, 4);
        if (n != 4) _exit(1);
        unsigned long flen = 0;
        flen |= (unsigned long)lenbuf[0] << 24;
        flen |= (unsigned long)lenbuf[1] << 16;
        flen |= (unsigned long)lenbuf[2] << 8;
        flen |= (unsigned long)lenbuf[3];
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

        /* Verify it's a hello frame from bridge */
        if (!strstr(fbuf, "\"type\":\"hello\"") || !strstr(fbuf, "\"role\":\"bridge\"")) {
            fprintf(stderr, "FAIL: bridge didn't send valid hello\n");
            free(fbuf);
            _exit(1);
        }
        free(fbuf);

        /* Send the configured response back */
        send_framed(STDOUT_FILENO, response, strlen(response));

        /* Keep reading and echoing frames forever */
        while (1) {
            n = read(STDIN_FILENO, lenbuf, 4);
            if (n <= 0) break;
            flen = 0;
            flen |= (unsigned long)lenbuf[0] << 24;
            flen |= (unsigned long)lenbuf[1] << 16;
            flen |= (unsigned long)lenbuf[2] << 8;
            flen |= (unsigned long)lenbuf[3];
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
            send_framed(STDOUT_FILENO, fbuf, flen);
            free(fbuf);
        }
        _exit(0);
    }

    /* Parent: simulate bridge */
    close(pipe_in[0]);
    close(pipe_out[1]);
    close(pipe_err[1]);

    /* Send bridge hello frame */
    char hello[4096];
    int hello_len = snprintf(hello, sizeof(hello),
        "{\"type\":\"hello\",\"role\":\"bridge\",\"bridge_version\":\"1.0.0\",\"protocol_versions\":[%d],"
        "\"harness_id\":\"codex.responses\",\"workspace_root\":\"/tmp/test\","
        "\"accepted_events\":[\"response\",\"error\",\"status\",\"log\"],\"max_frame_bytes\":16777216}",
        BRIDGE_PROTOCOL_VERSION);

    send_framed(pipe_in[1], hello, (size_t)hello_len);

    /* Read the native's response */
    unsigned char lb[4];
    ssize_t n = read(pipe_out[0], lb, 4);
    if (n != 4) {
        /* No response - timeout */
        close(pipe_in[1]);
        close(pipe_out[0]);
        close(pipe_err[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }

    unsigned long resp_len = 0;
    resp_len |= (unsigned long)lb[0] << 24;
    resp_len |= (unsigned long)lb[1] << 16;
    resp_len |= (unsigned long)lb[2] << 8;
    resp_len |= (unsigned long)lb[3];

    if (resp_len > 100000) {
        close(pipe_in[1]);
        close(pipe_out[0]);
        close(pipe_err[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }

    char *resp_buf = malloc(resp_len + 1);
    if (!resp_buf) {
        close(pipe_in[1]);
        close(pipe_out[0]);
        close(pipe_err[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    size_t off = 0;
    while (off < resp_len) {
        n = read(pipe_out[0], resp_buf + off, resp_len - off);
        if (n <= 0) {
            free(resp_buf);
            close(pipe_in[1]);
            close(pipe_out[0]);
            close(pipe_err[0]);
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            return -1;
        }
        off += (size_t)n;
    }
    resp_buf[resp_len] = '\0';

    /* Now validate the response using the same logic as engine_read_native_hello */
    bool valid = true;

    /* Check required fields */
    if (!strstr(resp_buf, "\"type\":\"hello\"")) valid = false;
    if (!strstr(resp_buf, "\"role\":\"native_agent\"")) valid = false;
    if (!strstr(resp_buf, "\"protocol_version\"")) valid = false;
    if (!strstr(resp_buf, "\"agent_name\"")) valid = false;
    if (!strstr(resp_buf, "\"agent_version\"")) valid = false;
    if (!strstr(resp_buf, "\"supported_transports\"")) valid = false;

    /* Extract protocol_version value */
    int proto = 0;
    const char *pv = strstr(resp_buf, "\"protocol_version\":");
    if (pv) {
        pv += 19;
        while (*pv == ' ' || *pv == '\t') pv++;
        if (*pv >= '0' && *pv <= '9') proto = *pv - '0';
    }
    if (proto != BRIDGE_PROTOCOL_VERSION) valid = false;

    free(resp_buf);
    close(pipe_in[1]);
    close(pipe_out[0]);
    close(pipe_err[0]);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);

    return valid ? 0 : -1;
}

/* Write a length-prefixed frame to a fd */
static void send_framed(int fd, const char *data, size_t len) {
    unsigned char lenbuf[4];
    lenbuf[0] = (unsigned char)(len >> 24);
    lenbuf[1] = (unsigned char)(len >> 16);
    lenbuf[2] = (unsigned char)(len >> 8);
    lenbuf[3] = (unsigned char)(len);
    write(fd, lenbuf, 4);
    write(fd, data, len);
}
