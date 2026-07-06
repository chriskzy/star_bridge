/*
 * UDS test for partial frame reads/writes.
 *
 * Tests that:
 * 1. Sending frame in fragments (partial writes) is assembled correctly
 * 2. Receiving frame in fragments (partial reads) works
 * 3. Multiple fragmented frames in sequence work
 * 4. Empty frames work
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
    if (len == 0) return strdup(""); /* empty frame */
    if (len > 16777216u) return NULL; /* sanity limit */
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

/* Write a length-prefixed JSON frame to fd (all at once) */
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

/* Write a frame in fragments: send length, then body in multiple chunks */
static int write_frame_fragmented(int fd, const char *json, size_t len) {
    uint32_t nlen = (uint32_t)len;
    /* Send length */
    if (write(fd, &nlen, 4) != 4) return -1;
    /* Send body in 3 fragments */
    size_t third = len / 3;
    if (third == 0) third = 1;
    size_t off = 0;
    while (off < len) {
        size_t chunk = (len - off > third) ? third : (len - off);
        ssize_t n = write(fd, json + off, chunk);
        if (n > 0) off += (size_t)n;
        else if (errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

/* Write a frame with the 4-byte length prefix sent separately from body */
static int write_frame_split(int fd, const char *json, size_t len) {
    uint32_t nlen = (uint32_t)len;
    /* Send length prefix */
    if (write(fd, &nlen, 4) != 4) return -1;
    /* Send body in one shot */
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, json + off, len - off);
        if (n > 0) off += (size_t)n;
        else if (errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

int main(void) {
    int failed = 0;

    /* Create a UDS pair (socketpair) for testing */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("socketpair");
        return 1;
    }

    /* Test 1: Fragmented write — send frame body in pieces */
    printf("Test 1: Fragmented write assembles correctly... ");
    fflush(stdout);

    const char *msg1 = "{\"type\":\"health\",\"data\":\"check\"}";
    size_t len1 = strlen(msg1);

    /* Write frame with body fragmented into 3 chunks */
    if (write_frame_fragmented(sv[0], msg1, len1) != 0) {
        printf("FAIL: write failed\n");
        return 1;
    }

    /* Read the frame */
    size_t rlen = 0;
    char *recv = read_frame(sv[1], &rlen);
    if (!recv) {
        printf("FAIL: read returned NULL\n");
        return 1;
    }
    if (rlen != len1 || strcmp(recv, msg1) != 0) {
        printf("FAIL: mismatch (got %zu bytes: %s)\n", rlen, recv);
        free(recv);
        return 1;
    }
    free(recv);
    printf("PASS\n");

    /* Test 2: Split write — send length prefix separate from body */
    printf("Test 2: Split write (length then body) works... ");
    fflush(stdout);

    const char *msg2 = "{\"type\":\"request\",\"input\":\"hello\"}";
    size_t len2 = strlen(msg2);

    if (write_frame_split(sv[0], msg2, len2) != 0) {
        printf("FAIL: write failed\n");
        return 1;
    }

    rlen = 0;
    recv = read_frame(sv[1], &rlen);
    if (!recv) {
        printf("FAIL: read returned NULL\n");
        return 1;
    }
    if (rlen != len2 || strcmp(recv, msg2) != 0) {
        printf("FAIL: mismatch (got %zu bytes: %s)\n", rlen, recv);
        free(recv);
        return 1;
    }
    free(recv);
    printf("PASS\n");

    /* Test 3: Multiple fragmented frames in sequence */
    printf("Test 3: Multiple fragmented frames in sequence... ");
    fflush(stdout);

    const char *frames[] = {
        "{\"a\":1}",
        "{\"b\":2}",
        "{\"c\":3}"
    };
    int nframes = 3;

    for (int i = 0; i < nframes; i++) {
        size_t flen = strlen(frames[i]);
        if (write_frame_fragmented(sv[0], frames[i], flen) != 0) {
            printf("FAIL: write %d failed\n", i);
            return 1;
        }
    }

    for (int i = 0; i < nframes; i++) {
        size_t rlen2 = 0;
        char *got = read_frame(sv[1], &rlen2);
        if (!got) {
            printf("FAIL: read %d returned NULL\n", i);
            return 1;
        }
        if (strcmp(got, frames[i]) != 0) {
            printf("FAIL: frame %d mismatch (got: %s)\n", i, got);
            free(got);
            return 1;
        }
        free(got);
    }
    printf("PASS\n");

    /* Test 4: Empty frame works */
    printf("Test 4: Empty frame works... ");
    fflush(stdout);

    if (write_frame(sv[0], "", 0) != 0) {
        printf("FAIL: write empty frame failed\n");
        return 1;
    }

    rlen = 0;
    recv = read_frame(sv[1], &rlen);
    if (!recv) {
        printf("FAIL: read empty frame returned NULL\n");
        return 1;
    }
    if (rlen != 0 || strcmp(recv, "") != 0) {
        printf("FAIL: empty frame mismatch (len=%zu, str=%s)\n", rlen, recv);
        free(recv);
        return 1;
    }
    free(recv);
    printf("PASS\n");

    /* Test 5: Byte-by-byte write (worst-case fragmentation) */
    printf("Test 5: Byte-by-byte write works... ");
    fflush(stdout);

    const char *msg5 = "{\"ok\":true}";
    size_t len5 = strlen(msg5);

    /* Send length prefix */
    uint32_t nlen5 = (uint32_t)len5;
    if (write(sv[0], &nlen5, 4) != 4) {
        printf("FAIL: write length failed\n");
        return 1;
    }
    /* Send body byte by byte */
    for (size_t i = 0; i < len5; i++) {
        if (write(sv[0], msg5 + i, 1) != 1) {
            printf("FAIL: byte write at %zu failed\n", i);
            return 1;
        }
    }

    rlen = 0;
    recv = read_frame(sv[1], &rlen);
    if (!recv) {
        printf("FAIL: read returned NULL\n");
        return 1;
    }
    if (rlen != len5 || strcmp(recv, msg5) != 0) {
        printf("FAIL: mismatch (got %zu bytes: %s)\n", rlen, recv);
        free(recv);
        return 1;
    }
    free(recv);
    printf("PASS\n");

    close(sv[0]);
    close(sv[1]);

    if (failed) {
        printf("\nSome tests FAILED.\n");
        return 1;
    }
    printf("\nAll partial frame read/write tests passed.\n");
    return 0;
}
