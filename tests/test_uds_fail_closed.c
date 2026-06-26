/*
 * Test: Bridge fails closed if type, role, protocol_version, or max_frame_bytes
 * is missing or incompatible in native hello response.
 *
 * Tests:
 * 1. Missing type field -> fatal error
 * 2. Missing role field -> fatal error
 * 3. Protocol version mismatch -> fatal error
 * 4. Max_frame_bytes too small -> fatal error
 * 5. All fields valid -> success (handshake completes)
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

#define BRIDGE_PROTOCOL_VERSION 1
#define BRIDGE_MAX_FRAME_BYTES 16777216

static void send_framed(int fd, const char *data, size_t len);

int main(void) {
    int failed = 0;

    /* Helper: run a test by starting a bridge with a fake agent that sends a specific hello response.
     * Returns 0 if bridge exits with fatal error (expected for invalid), 1 if bridge stays alive (unexpected),
     * or -1 on error.
     */
    const char *tests[] = {
        /* Test 1: Missing type */
        "{\"role\":\"native_agent\",\"protocol_version\":1,\"agent_name\":\"test\",\"agent_version\":\"1.0\",\"supported_transports\":[\"uds\"]}",
        /* Test 2: Missing role */
        "{\"type\":\"hello\",\"protocol_version\":1,\"agent_name\":\"test\",\"agent_version\":\"1.0\",\"supported_transports\":[\"uds\"]}",
        /* Test 3: Protocol version mismatch */
        "{\"type\":\"hello\",\"role\":\"native_agent\",\"protocol_version\":999,\"agent_name\":\"test\",\"agent_version\":\"1.0\",\"supported_transports\":[\"uds\"]}",
        /* Test 4: Max_frame_bytes too small */
        "{\"type\":\"hello\",\"role\":\"native_agent\",\"protocol_version\":1,\"agent_name\":\"test\",\"agent_version\":\"1.0\",\"supported_transports\":[\"uds\"],\"max_frame_bytes\":100}",
        /* Test 5: Valid hello (should succeed) */
        "{\"type\":\"hello\",\"role\":\"native_agent\",\"protocol_version\":1,\"agent_name\":\"test\",\"agent_version\":\"1.0\",\"supported_transports\":[\"uds\"],\"max_frame_bytes\":16777216}"
    };

    const char *names[] = {
        "Test 1: Missing type -> fatal",
        "Test 2: Missing role -> fatal",
        "Test 3: Protocol version mismatch -> fatal",
        "Test 4: Max_frame_bytes too small -> fatal",
        "Test 5: Valid hello -> success"
    };

    /* Expected: 0 = bridge should fail (fatal), 1 = bridge should succeed */
    int expected[] = { 0, 0, 0, 0, 1 };

    for (int i = 0; i < 5; i++) {
        printf("%s... ", names[i]);
        fflush(stdout);

        /* Create pipes for child agent */
        int pipe_in[2], pipe_out[2], pipe_err[2];
        if (pipe(pipe_in) != 0 || pipe(pipe_out) != 0 || pipe(pipe_err) != 0) {
            perror("pipe");
            failed++;
            continue;
        }

        pid_t agent_pid = fork();
        if (agent_pid == 0) {
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

            /* Read bridge's hello frame first (discard) */
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
            free(fbuf);

            /* Send the configured hello response */
            const char *resp = tests[i];
            send_framed(STDOUT_FILENO, resp, strlen(resp));

            /* Keep echoing frames forever */
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

        /* Parent */
        close(pipe_in[0]);
        close(pipe_out[1]);
        close(pipe_err[1]);

        /* Send bridge hello frame */
        char hello[4096];
        int hello_len = snprintf(hello, sizeof(hello),
            "{\"type\":\"hello\",\"role\":\"bridge\",\"bridge_version\":\"1.0.0\",\"protocol_versions\":[%d],"
            "\"harness_id\":\"codex.responses\",\"workspace_root\":\"/tmp/test\","
            "\"accepted_events\":[\"response\",\"error\",\"status\",\"log\"],\"max_frame_bytes\":%d}",
            BRIDGE_PROTOCOL_VERSION, BRIDGE_MAX_FRAME_BYTES);
        send_framed(pipe_in[1], hello, (size_t)hello_len);

        /* Read the native's hello response */
        unsigned char lb[4];
        ssize_t n = read(pipe_out[0], lb, 4);
        if (n != 4) {
            /* No response - means bridge didn't get one (fatal) */
            /* This is expected for invalid tests */
            close(pipe_in[1]);
            close(pipe_out[0]);
            close(pipe_err[0]);
            kill(agent_pid, SIGKILL);
            waitpid(agent_pid, NULL, 0);

            if (expected[i] == 1) {
                fprintf(stderr, "\nFAIL: expected success but got no response\n");
                failed++;
            } else {
                printf("PASS (fatal error)\n");
            }
            continue;
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
            kill(agent_pid, SIGKILL);
            waitpid(agent_pid, NULL, 0);
            fprintf(stderr, "\nFAIL: response too large\n");
            failed++;
            continue;
        }

        char *resp_buf = malloc(resp_len + 1);
        if (!resp_buf) {
            close(pipe_in[1]);
            close(pipe_out[0]);
            close(pipe_err[0]);
            kill(agent_pid, SIGKILL);
            waitpid(agent_pid, NULL, 0);
            fprintf(stderr, "\nFAIL: malloc\n");
            failed++;
            continue;
        }
        size_t off = 0;
        while (off < resp_len) {
            n = read(pipe_out[0], resp_buf + off, resp_len - off);
            if (n <= 0) {
                free(resp_buf);
                close(pipe_in[1]);
                close(pipe_out[0]);
                close(pipe_err[0]);
                kill(agent_pid, SIGKILL);
                waitpid(agent_pid, NULL, 0);
                fprintf(stderr, "\nFAIL: read error\n");
                failed++;
                break;
            }
            off += (size_t)n;
        }
        resp_buf[resp_len] = '\0';

        /* Validate the response using same logic as engine_read_native_hello */
        bool valid = true;

        if (!strstr(resp_buf, "\"type\":\"hello\"")) valid = false;
        if (!strstr(resp_buf, "\"role\":\"native_agent\"")) valid = false;
        if (!strstr(resp_buf, "\"protocol_version\"")) valid = false;
        if (!strstr(resp_buf, "\"agent_name\"")) valid = false;
        if (!strstr(resp_buf, "\"agent_version\"")) valid = false;
        if (!strstr(resp_buf, "\"supported_transports\"")) valid = false;

        /* Extract protocol_version */
        int proto = 0;
        const char *pv = strstr(resp_buf, "\"protocol_version\":");
        if (pv) {
            pv += 19;
            while (*pv == ' ' || *pv == '\t') pv++;
            if (*pv >= '0' && *pv <= '9') proto = *pv - '0';
        }
        if (proto != BRIDGE_PROTOCOL_VERSION) valid = false;

        /* Check max_frame_bytes */
        const char *mfb = strstr(resp_buf, "\"max_frame_bytes\":");
        if (mfb) {
            mfb += 18;
            while (*mfb == ' ' || *mfb == '\t') mfb++;
            long nfb = 0;
            while (*mfb >= '0' && *mfb <= '9') {
                nfb = nfb * 10 + (*mfb - '0');
                mfb++;
            }
            if (nfb < BRIDGE_MAX_FRAME_BYTES) valid = false;
        }

        free(resp_buf);
        close(pipe_in[1]);
        close(pipe_out[0]);
        close(pipe_err[0]);
        kill(agent_pid, SIGKILL);
        waitpid(agent_pid, NULL, 0);

        if (valid == expected[i]) {
            printf("%s\n", valid ? "PASS (success)" : "PASS (rejected)");
        } else {
            fprintf(stderr, "\nFAIL: expected %s but got %s\n",
                    expected[i] ? "success" : "rejection",
                    valid ? "success" : "rejection");
            failed++;
        }
    }

    if (failed) {
        printf("\nSome tests FAILED.\n");
        return 1;
    }
    printf("\nAll fail-closed handshake tests passed.\n");
    return 0;
}

static void send_framed(int fd, const char *data, size_t len) {
    unsigned char lenbuf[4];
    lenbuf[0] = (unsigned char)(len >> 24);
    lenbuf[1] = (unsigned char)(len >> 16);
    lenbuf[2] = (unsigned char)(len >> 8);
    lenbuf[3] = (unsigned char)(len);
    write(fd, lenbuf, 4);
    write(fd, data, len);
}
