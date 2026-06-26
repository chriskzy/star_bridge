#include "server.h"
#include "json_utils.h"
#include "turn_context.h"
#include "bridge_core.h"
#include "codex_adapter.h"
#include "codex_tool_normalizer.h"
#include "capability_router.h"
#include "config_manager.h"
#include "debug_trace.h"
#include "native_frame.h"
#include "responses_stream_state.h"
#include "tool_runner.h"
#include "tool_policy.h"
#include "tool_history.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <limits.h>
#include <poll.h>
#include <unistd.h>
#include <zlib.h>

/* Portable constant-time byte comparison — timingsafe_bcmp on BSD/macOS,
 * manual fallback on Linux where it may not be available. */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#  include <string.h>   /* timingsafe_bcmp already declared via <string.h> */
#else
static int timingsafe_bcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    int result = 0;
    for (size_t i = 0; i < n; i++) result |= p[i] ^ q[i];
    return result != 0;
}
#endif

/* Safe subprocess runner: executes an argv-based command with fork/execvp
 * and captures the first line of stdout. Returns true if output was captured.
 * Never uses shell interpolation — safe for workspace paths with metacharacters.
 * Times out after timeout_s seconds to prevent hangs. */
static bool safe_subprocess_capture(const char *cwd, char *const argv[], char *out, size_t out_max, int timeout_s) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return false;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return false; }

    if (pid == 0) {
        /* Child: redirect stdout to pipe */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        /* Change to workspace directory if provided */
        if (cwd && cwd[0]) {
            chdir(cwd);
        }
        execvp(argv[0], argv);
        /* If execvp fails, exit with error */
        _exit(127);
    }

    /* Parent: read output with timeout */
    close(pipefd[1]);

    char buf[4096];
    size_t pos = 0;
    struct timeval tv = { .tv_sec = timeout_s, .tv_usec = 0 };

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(pipefd[0], &readfds);

    while (1) {
        int ready = select(pipefd[0] + 1, &readfds, NULL, NULL, &tv);
        if (ready <= 0) break; /* timeout or error */
        ssize_t n = read(pipefd[0], buf + pos, sizeof(buf) - 1 - pos);
        if (n <= 0) break; /* EOF or error */
        pos += (size_t)n;
        /* Stop at first newline */
        bool found_newline = false;
        for (size_t i = 0; i < pos; i++) {
            if (buf[i] == '\n') {
                pos = i;
                found_newline = true;
                break;
            }
        }
        if (found_newline) break;
        if (pos >= sizeof(buf) - 1) break;
    }

    buf[pos] = '\0';
    close(pipefd[0]);

    /* Wait for child with timeout */
    int status = 0;
    pid_t done;
    for (int i = 0; i < timeout_s * 10; i++) {
        done = waitpid(pid, &status, WNOHANG);
        if (done == pid) break;
        usleep(100000);
    }
    if (done != pid) {
        /* Timed out — kill child */
        kill(pid, SIGTERM);
        usleep(100000);
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }

    if (pos > 0 && out && out_max > 0) {
        snprintf(out, out_max, "%s", buf);
        return true;
    }
    return false;
}

/* Case-insensitive substring search for HTTP header lookup */
static const char *find_header_ci(const char *haystack, const char *needle) {
    for (const char *p = haystack; *p; p++) {
        const char *h = p, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
        if (*n == '\0') return p;
    }
    return NULL;
}

/* Global shutdown flag for graceful exit */
volatile sig_atomic_t shutdown_requested = 0;

/* Signal handler for SIGINT/SIGTERM */
static void handle_shutdown_signal(int sig) {
    (void)sig;
    shutdown_requested = 1;
}

static unsigned long g_request_counter = 0;
static int g_server_fd = -1;
static bool g_gzip_supported = false;

/* TraceSession typedef moved to server.h */
TraceSession g_trace_session;

/* Structured tool history ring buffer — now in tool_history.c */

/* -------------------------------------------------------------------
 *  HTTP helpers
 * ------------------------------------------------------------------- */

/* Forward declarations for chunked encoding helpers */
static int is_chunked_request(const char *raw_request);
static int chunked_body_complete(const char *raw_request, size_t raw_len);

static const char *find_header_value(const char *raw_request, const char *name) {
    if (!raw_request || !name) return NULL;
    const char *header_end = strstr(raw_request, "\r\n\r\n");
    if (!header_end) return NULL;
    size_t name_len = strlen(name);
    const char *line = raw_request;
    while (line < header_end) {
        const char *line_end = strstr(line, "\r\n");
        if (!line_end || line_end > header_end) line_end = header_end;
        if ((size_t)(line_end - line) > name_len &&
            strncasecmp(line, name, name_len) == 0 &&
            line[name_len] == ':') {
            const char *value = line + name_len + 1;
            while (value < line_end && (*value == ' ' || *value == '\t')) value++;
            return value;
        }
        if (line_end >= header_end) break;
        line = line_end + 2;
    }
    return NULL;
}

static void copy_header_value(const char *raw_request, const char *name, char *dest, size_t dest_len, const char *fallback) {
    if (!dest || dest_len == 0) return;
    snprintf(dest, dest_len, "%s", fallback ? fallback : "");
    const char *value = find_header_value(raw_request, name);
    if (!value) return;
    const char *end = value;
    const char *header_end = strstr(raw_request, "\r\n\r\n");
    while (*end && end < header_end && *end != '\r' && *end != '\n') end++;
    size_t len = (size_t)(end - value);
    if (len >= dest_len) len = dest_len - 1;
    memcpy(dest, value, len);
    dest[len] = '\0';
}

/* Wait for native agent text output by polling the ring buffer directly.
 * Reader thread fills the buffer; we just check it periodically.
 * Uses a short usleep between checks instead of pipe poll because the
 * reader thread may already have consumed data from the pipe.
 */
void wait_for_native_output(BridgeEngine *eng, int timeout_ms) {
    int remaining = timeout_ms;
    /* Check buffer first */
    if (engine_text_available(eng) > 0) return;
    /* Poll buffer in short bursts */
    do {
        int chunk = (remaining < 10) ? remaining : 10;
        usleep((chunk < 1000) ? 1000 : chunk);
        if (engine_text_available(eng) > 0) return;
        remaining -= chunk;
    } while (remaining > 0);
}

/* Forward declaration for Content-Length parser used by read_http_body */
static long parse_content_length_safe(const char *val);

/* Read entire HTTP request body using Content-Length or Transfer-Encoding: chunked.
 * Returns a malloc'd buffer containing the body (caller must free).
 * On failure returns NULL.
 */
static char *read_http_body(const char *raw_request, size_t raw_len, size_t *out_len) {
    const char *header_end = strstr(raw_request, "\r\n\r\n");
    if (!header_end) return NULL;
    size_t header_len = (header_end - raw_request) + 4;
    size_t body_available = raw_len - header_len;

    /* If chunked encoding, decode the chunks into a contiguous body */
    if (is_chunked_request(raw_request)) {
        const char *body_start = raw_request + header_len;
        const char *p = body_start;
        size_t decoded_cap = 65536;
        size_t decoded_len = 0;
        char *decoded = calloc(1, decoded_cap + 1);
        if (!decoded) return NULL;
        size_t max_body = (size_t)global_config.max_request_size;

        while (p < raw_request + raw_len) {
            /* Skip trailing \r\n from previous chunk */
            while (p < raw_request + raw_len && (p[0] == '\r' || p[0] == '\n')) p++;
            if (p >= raw_request + raw_len) break;

            /* Read chunk size (hex) until \r\n */
            long chunk_size = 0;
            const char *hex_start = p;
            while (p < raw_request + raw_len && p[0] != '\r' && p[0] != '\n') {
                p++;
            }
            if (p >= raw_request + raw_len || p[0] != '\r') {
                free(decoded);
                return NULL;
            }
            /* Extract hex string between hex_start and p */
            size_t hex_len = (size_t)(p - hex_start);
            char hex_buf[32];
            if (hex_len >= sizeof(hex_buf)) hex_len = sizeof(hex_buf) - 1;
            memcpy(hex_buf, hex_start, hex_len);
            hex_buf[hex_len] = '\0';

            /* Skip \r\n after chunk size */
            if (p + 1 < raw_request + raw_len && p[1] == '\n') p += 2;
            else if (p + 1 < raw_request + raw_len) p += 1;
            else { free(decoded); return NULL; }

            chunk_size = (long)strtol(hex_buf, NULL, 16);
            if (chunk_size < 0) { free(decoded); return NULL; }
            if (chunk_size == 0) break; /* terminating chunk */

            /* Copy chunk data */
            if (decoded_len + (size_t)chunk_size > decoded_cap) {
                size_t next_cap = decoded_cap * 2;
                while (next_cap < decoded_len + (size_t)chunk_size) next_cap *= 2;
                if (next_cap > 16777216) next_cap = 16777216;
                char *grown = realloc(decoded, next_cap + 1);
                if (!grown) { free(decoded); return NULL; }
                decoded = grown;
                decoded_cap = next_cap;
            }
            size_t copy_len = (size_t)chunk_size;
            if (p + copy_len > raw_request + raw_len) {
                copy_len = (size_t)(raw_request + raw_len - p);
            }
            memcpy(decoded + decoded_len, p, copy_len);
            decoded_len += copy_len;
            decoded[decoded_len] = '\0';

            /* Reject if decoded body exceeds max_request_size */
            if (decoded_len > max_body) {
                if (out_len) *out_len = (size_t)-1;
                free(decoded);
                return NULL;
            }

            /* Skip \r\n after chunk data */
            if (copy_len > 0) {
                p += copy_len;
                if (p < raw_request + raw_len && p[0] == '\r') p++;
                if (p < raw_request + raw_len && p[0] == '\n') p++;
            }
        }

        if (out_len) *out_len = decoded_len;
        return decoded;
    }

    const char *cl_val = find_header_value(raw_request, "Content-Length");
    if (!cl_val) {
        if (body_available == 0) return NULL;
        char *body = strndup(raw_request + header_len, body_available);
        if (out_len) *out_len = body_available;
        return body;
    }
    long cl = parse_content_length_safe(cl_val);
    if (cl < 0) return NULL;
    if (cl == 0) {
        if (out_len) *out_len = 0;
        return strdup("");
    }

    if ((long)body_available >= cl) {
        char *body = strndup(raw_request + header_len, (size_t)cl);
        if (out_len) *out_len = (size_t)cl;
        return body;
    }
    char *body = strndup(raw_request + header_len, body_available);
    if (out_len) *out_len = body_available;
    return body;
}

static int is_chunked_request(const char *raw_request) {
    const char *te = find_header_value(raw_request, "Transfer-Encoding");
    if (!te) return 0;
    const char *header_end = strstr(raw_request, "\r\n\r\n");
    const char *line_end = te;
    while (*line_end && line_end < header_end && *line_end != '\r' && *line_end != '\n') line_end++;
    for (const char *p = te; p + 7 <= line_end; p++) {
        if (strncasecmp(p, "chunked", 7) == 0) return 1;
    }
    return 0;
}

/* Check if a chunked request body is complete by looking for the terminating 0\r\n\r\n */
static int chunked_body_complete(const char *raw_request, size_t raw_len) {
    const char *header_end = strstr(raw_request, "\r\n\r\n");
    if (!header_end) return 0;
    /* Search for \r\n0\r\n\r\n after header end */
    const char *body_start = header_end + 4;
    if (body_start >= raw_request + raw_len) return 0;
    /* Look for trailing 0\r\n\r\n — the terminating chunk marker */
    const char *term = raw_request + raw_len;
    /* Search backwards from end for 0\r\n\r\n */
    for (const char *p = term - 5; p >= body_start; p--) {
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '0' && p[3] == '\r' && p[4] == '\n') {
            /* Check that the 0\r\n is at a chunk boundary: before it should be \r\n from previous chunk,
             * or it could be the first chunk if the body starts with 0\r\n\r\n (empty body) */
            return 1;
        }
    }
    return 0;
}

/* Parse Content-Length value with overflow/negative validation.
 * The value may be terminated by \r, \n, \0, or end-of-headers (\r\n\r\n).
 */
static long parse_content_length_safe(const char *val) {
    if (!val || val[0] == '\0') return -1;
    /* Skip leading whitespace */
    while (*val == ' ' || *val == '\t') val++;
    /* Reject negative sign */
    if (val[0] == '-') return -1;
    /* Reject non-digit (including leading '+') */
    if (!isdigit((unsigned char)val[0])) return -1;
    long acc = 0;
    int digits = 0;
    while (*val >= '0' && *val <= '9') {
        /* Check overflow before adding */
        if (acc > LONG_MAX / 10) return -1;
        acc *= 10;
        if (acc < 0 || acc > LONG_MAX - (*val - '0')) return -1;
        acc += (*val - '0');
        val++;
        digits++;
    }
    /* Reject empty or trailing non-whitespace (accept \r, \n, \0 as terminators) */
    while (*val == ' ' || *val == '\t') val++;
    if (digits == 0 || (*val != '\0' && *val != '\r' && *val != '\n')) return -1;
    if (acc < 0) return -1;
    return acc;
}

static size_t http_expected_request_len(const char *raw_request) {
    const char *header_end = strstr(raw_request, "\r\n\r\n");
    if (!header_end) return 0;
    size_t header_len = (header_end - raw_request) + 4;

    /* If chunked, we don't know total length — keep reading until chunked body is complete */
    if (is_chunked_request(raw_request)) {
        return 0;
    }

    const char *cl_val = find_header_value(raw_request, "Content-Length");
    if (!cl_val) return header_len;
    long cl = parse_content_length_safe(cl_val);
    if (cl <= 0) return header_len;
    return header_len + (size_t)cl;
}

/* Write all bytes; retry on short write */
void write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t written = write(fd, buf, len);
        if (written <= 0) {
            if (written == -1 && (errno == EINTR || errno == EAGAIN)) continue;
            break;
        }
        buf += written;
        len -= (size_t)written;
    }
}

static char *read_http_request(int fd, size_t *out_len) {
    size_t max_req = (size_t)global_config.max_request_size;
    size_t cap = 1048576;
    size_t len = 0;
    char *raw = calloc(1, cap + 1);
    if (!raw) return NULL;

    while (len < cap) {
        ssize_t n = read(fd, raw + len, cap - len);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(raw);
            return NULL;
        }
        if (n == 0) break;
        len += (size_t)n;
        raw[len] = '\0';

        size_t expected = http_expected_request_len(raw);
        if (expected > max_req) {
            /* Request too large — send 413 and abort */
            const char *err = "HTTP/1.1 413 Request Entity Too Large\r\nContent-Type: application/json\r\nContent-Length: 36\r\nConnection: close\r\n\r\n{\"error\":\"request too large\"}";
            write_all(fd, err, strlen(err));
            free(raw);
            return NULL;
        }
        if (expected > cap && cap < 16777216) {
            size_t next_cap = cap * 2;
            while (next_cap < expected && next_cap < 16777216) next_cap *= 2;
            if (next_cap > 16777216) next_cap = 16777216;
            if (next_cap > max_req) next_cap = max_req;
            char *grown = realloc(raw, next_cap + 1);
            if (!grown) {
                free(raw);
                return NULL;
            }
            raw = grown;
            memset(raw + cap, 0, next_cap + 1 - cap);
            cap = next_cap;
        }
        if (expected > 0 && len >= expected) break;
        /* For chunked encoding, check if the terminating chunk marker is present */
        if (expected == 0 && len >= 4 && is_chunked_request(raw) && chunked_body_complete(raw, len)) break;
    }

    if (out_len) *out_len = len;
    return raw;
}

/* Send HTTP response with status line, headers, and body */
void send_http(int fd, const char *status, const char *content_type, const char *body) {
    char header[2048];
    size_t body_len = strlen(body);
    const char *connection = global_config.keep_alive_enabled ? "keep-alive" : "close";
    snprintf(header, sizeof(header),
             "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
             "Access-Control-Allow-Origin: *\r\n"
             "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
             "Access-Control-Allow-Headers: Content-Type, Authorization, X-Requested-With\r\n"
             "Connection: %s\r\n\r\n",
             status, content_type, body_len, connection);
    write_all(fd, header, strlen(header));
    write_all(fd, body, body_len);
}

/* ------------------------------------------------------------------ */
/*  Helper: gzip compress data                                        */
/* ------------------------------------------------------------------ */
static char *gzip_compress_srv(const char *data, size_t input_len, size_t *out_len) {
    if (!data || input_len == 0) return NULL;
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    int ret = deflateInit(&strm, Z_DEFAULT_COMPRESSION);
    if (ret != Z_OK) return NULL;
    size_t max_compressed = input_len + 128 + (input_len / 100) + 32;
    char *compressed = malloc(max_compressed);
    if (!compressed) { deflateEnd(&strm); return NULL; }
    strm.next_in = (Bytef *)data;
    strm.avail_in = (uInt)input_len;
    strm.next_out = (unsigned char *)compressed;
    strm.avail_out = (uInt)max_compressed;
    ret = deflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&strm);
        free(compressed);
        return NULL;
    }
    *out_len = strm.total_out;
    deflateEnd(&strm);
    return compressed;
}

/* Send SSE event — no truncation; writes directly */
void send_sse(int fd, const char *event_type, const char *data, int seq) {
    /* If gzip is globally enabled and data is large enough, compress */
    if (g_gzip_supported && data && strlen(data) > 256) {
        size_t input_len = strlen(data);
        size_t compressed_len = 0;
        char *compressed = gzip_compress_srv(data, input_len, &compressed_len);
        if (compressed && compressed_len > 0) {
            size_t total_bytes = 0;
            if (event_type && event_type[0]) {
                write_all(fd, "event: ", 7); total_bytes += 7;
                write_all(fd, event_type, strlen(event_type)); total_bytes += strlen(event_type);
                write_all(fd, "\n", 1); total_bytes += 1;
            }
            write_all(fd, "data: ", 6); total_bytes += 6;
            write_all(fd, compressed, compressed_len); total_bytes += compressed_len;
            write_all(fd, "\n\n", 2); total_bytes += 2;
            free(compressed);
            fprintf(stderr, "sse event=%s bytes=%zu data_len=%zu compressed_len=%zu seq=%d gzip=1\n",
                    event_type ? event_type : "", total_bytes, input_len, compressed_len, seq);
            return;
        }
    }
    size_t total_bytes = 0;
    if (event_type && event_type[0]) {
        /* Write event: <type>\n */
        write_all(fd, "event: ", 7); total_bytes += 7;
        write_all(fd, event_type, strlen(event_type)); total_bytes += strlen(event_type);
        write_all(fd, "\n", 1); total_bytes += 1;
    }
    /* Write data: <payload>\n\n */
    write_all(fd, "data: ", 6); total_bytes += 6;
    size_t data_len = strlen(data);
    write_all(fd, data, data_len); total_bytes += data_len;
    write_all(fd, "\n\n", 2); total_bytes += 2;
    fprintf(stderr, "sse event=%s bytes=%zu data_len=%zu seq=%d\n", event_type ? event_type : "", total_bytes, data_len, seq);
}

/* Send SSE heartbeat/keepalive comment */
void send_sse_heartbeat(int fd) {
    write_all(fd, ":heartbeat\n\n", 12);
}

/* Check if fd is writable with timeout (ms); returns true if writable */
bool fd_writable(int fd, int timeout_ms) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLOUT;
    int ret = poll(&pfd, 1, timeout_ms);
    return ret == 1 && (pfd.revents & POLLOUT);
}

static void copy_bounded(char *dest, size_t max_len, const char *src) {
    if (!dest || max_len == 0) return;
    snprintf(dest, max_len, "%s", src ? src : "");
}

void trace_store_text(char *dest, size_t max_len, const char *src) {
    if (!global_config.trace) return;
    copy_bounded(dest, max_len, src);
    debug_trace_redact_text(dest);
}

void trace_store_headers(const char *raw_request) {
    if (!global_config.trace || !raw_request) return;
    const char *header_end = strstr(raw_request, "\r\n\r\n");
    size_t len = header_end ? (size_t)(header_end - raw_request) : strlen(raw_request);
    if (len >= sizeof(g_trace_session.http_headers)) len = sizeof(g_trace_session.http_headers) - 1;
    memcpy(g_trace_session.http_headers, raw_request, len);
    g_trace_session.http_headers[len] = '\0';
    debug_trace_redact_text(g_trace_session.http_headers);
}

static void json_escape_text(const char *src, char *dest, size_t max_len) {
    json_escape(src ? src : "", dest, max_len);
}

static void build_debug_session_json(BridgeEngine *eng, char *body, size_t body_max) {
    char method[64], path[256], headers[8192], http_body[16384], normalized[16384];
    char native_request[16384], native_response[16384], error[1024];
    json_escape_text(g_trace_session.method, method, sizeof(method));
    json_escape_text(g_trace_session.path, path, sizeof(path));
    json_escape_text(g_trace_session.http_headers, headers, sizeof(headers));
    json_escape_text(g_trace_session.http_body, http_body, sizeof(http_body));
    json_escape_text(g_trace_session.normalized_input, normalized, sizeof(normalized));
    json_escape_text(g_trace_session.native_request, native_request, sizeof(native_request));
    json_escape_text(g_trace_session.native_response, native_response, sizeof(native_response));
    json_escape_text(g_trace_session.error, error, sizeof(error));

    /* Gather current session state info */
    char session_id[64] = "";
    char session_key[256] = "";
    char state_id[128] = "";
    if (eng && eng->last_session_id[0]) {
        char session_id_str[64] = "";
        if (global_config.session_id[0]) {
            size_t sl = strlen(global_config.session_id);
            if (sl >= sizeof(session_id_str)) sl = sizeof(session_id_str) - 1;
            memcpy(session_id_str, global_config.session_id, sl);
            session_id_str[sl] = '\0';
        }
        if (session_id_str[0]) {
            size_t sl = strlen(session_id_str);
            if (sl >= sizeof(session_id)) sl = sizeof(session_id) - 1;
            memcpy(session_id, session_id_str, sl);
            session_id[sl] = '\0';
        }
        const char *sk = get_session_key();
        if (sk && sk[0]) {
            size_t kl = strlen(sk);
            if (kl >= sizeof(session_key)) kl = sizeof(session_key) - 1;
            memcpy(session_key, sk, kl);
            session_key[kl] = '\0';
        }
        /* Look up state_id from session index */
        load_session_index();
        const char *sid_from_index = session_index_lookup(sk ? sk : "");
        if (sid_from_index) {
            size_t si = strlen(sid_from_index);
            if (si >= sizeof(state_id)) si = sizeof(state_id) - 1;
            memcpy(state_id, sid_from_index, si);
            state_id[si] = '\0';
        }
    }

    char esc_sid[256] = {0};
    char esc_sk[256] = {0};
    char esc_stid[256] = {0};
    json_escape(session_id, esc_sid, sizeof(esc_sid));
    json_escape(session_key, esc_sk, sizeof(esc_sk));
    json_escape(state_id, esc_stid, sizeof(esc_stid));

    snprintf(body, body_max,
             "{\"trace\":%s,\"session\":{\"session_id\":\"%s\",\"session_key\":\"%s\",\"state_id\":\"%s\"},"
             "\"last_request\":{\"request_number\":%lu,\"method\":\"%s\",\"path\":\"%s\","
             "\"http_headers\":\"%s\",\"http_body\":\"%s\",\"normalized_input\":\"%s\","
             "\"native_request\":\"%s\",\"native_response\":\"%s\",\"error\":\"%s\"}}",
             global_config.trace ? "true" : "false",
             esc_sid, esc_sk, esc_stid,
             g_trace_session.request_number,
             method,
             path,
             headers,
             http_body,
             normalized,
             native_request,
             native_response,
             error);
}

size_t append_text(char *dest, size_t max_len, size_t off, const char *text) {
    if (!dest || !text || off >= max_len) return off;
    size_t len = strlen(text);
    if (len > max_len - off - 1) len = max_len - off - 1;
    memcpy(dest + off, text, len);
    off += len;
    dest[off] = '\0';
    return off;
}

bool extract_json_string_field(const char *json, const char *key, char *dest, size_t max_len) {
    if (!json || !key || !dest || max_len == 0) return false;
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < max_len) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') dest[i++] = '\n';
            else if (*p == 't') dest[i++] = '\t';
            else if (*p != '\r') dest[i++] = *p;
            p++;
            continue;
        }
        dest[i++] = *p++;
    }
    dest[i] = '\0';
    return i > 0;
}

bool extract_balanced_json_local(const char *start, char *dest, size_t max_len) {
    if (!start || !dest || max_len == 0) return false;
    char opener = *start;
    char closer = opener == '{' ? '}' : opener == '[' ? ']' : '\0';
    if (!closer) return false;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    size_t out = 0;
    for (const char *p = start; *p && out + 1 < max_len; p++) {
        char c = *p;
        dest[out++] = c;
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\' && in_string) {
            escaped = true;
            continue;
        }
        if (c == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) continue;
        if (c == opener) depth++;
        if (c == closer) {
            depth--;
            if (depth == 0) {
                dest[out] = '\0';
                return true;
            }
        }
    }
    dest[out] = '\0';
    return out > 0;
}

const char *decode_json_string_local(const char *p, char *dest, size_t max_len) {
    /* Decode a JSON string at p (must point to opening '"'). */
    if (!p || *p != '"') return NULL;
    p++;
    size_t out = 0;
    while (*p && out + 1 < max_len) {
        if (*p == '\\') {
            p++;
            if (*p == 'u') {
                /* parse \uXXXX */
                p++;
                int val = 0;
                int digits = 0;
                for (; *p && digits < 4; p++, digits++) {
                    int d = -1;
                    if (*p >= '0' && *p <= '9') d = *p - '0';
                    else if (*p >= 'a' && *p <= 'f') d = 10 + (*p - 'a');
                    else if (*p >= 'A' && *p <= 'F') d = 10 + (*p - 'A');
                    else { break; }
                    val = (val << 4) | d;
                }
                if (digits == 4) {
                    if (val < 0x80) {
                        if (out + 1 < max_len) dest[out++] = (char)val;
                    } else if (val < 0x800) {
                        if (out + 2 < max_len) {
                            dest[out++] = (char)(0xC0 | ((val >> 6) & 0x1F));
                            dest[out++] = (char)(0x80 | (val & 0x3F));
                        }
                    } else {
                        if (out + 3 < max_len) {
                            dest[out++] = (char)(0xE0 | ((val >> 12) & 0x0F));
                            dest[out++] = (char)(0x80 | ((val >> 6) & 0x3F));
                            dest[out++] = (char)(0x80 | (val & 0x3F));
                        }
                    }
                    continue;
                }
                if (out + 1 < max_len) dest[out++] = '?';
                continue;
            }
            switch (*p) {
                case '"':  dest[out++] = '"';  p++; break;
                case '\\': dest[out++] = '\\'; p++; break;
                case '/':  dest[out++] = '/';  p++; break;
                case 'n':  dest[out++] = '\n'; p++; break;
                case 'r':  dest[out++] = '\r'; p++; break;
                case 't':  dest[out++] = '\t'; p++; break;
                case 'b':  dest[out++] = '\b'; p++; break;
                case 'f':  dest[out++] = '\f'; p++; break;
                default:   dest[out++] = *p;   p++; break;
            }
            continue;
        }
        if (*p == '"') {
            dest[out] = '\0';
            p++;
            while (*p == ' ' || *p == '\t' || *p == ',' || *p == ':') p++;
            return p;
        }
        dest[out++] = *p++;
    }
    dest[out] = '\0';
    return NULL;
}

bool extract_tool_intent(const char *frame, char *tool_name, size_t tool_name_max,
                                char *tool_args_json, size_t tool_args_max) {
    const char *p = strstr(frame, "\"tool_intent\"");
    if (!p) return false;
    const char *obj = strchr(p, '{');
    if (!obj) return false;

    if (!extract_json_string_field(obj, "name", tool_name, tool_name_max) || !tool_name[0]) {
        return false;
    }

    const char *args = strstr(obj, "\"arguments\":");
    if (args) {
        args = strchr(args, ':');
        if (args) {
            args++;
            while (*args == ' ' || *args == '\t') args++;
            if (*args == '"') {
                decode_json_string_local(args, tool_args_json, tool_args_max);
            } else if (*args == '{') {
                extract_balanced_json_local(args, tool_args_json, tool_args_max);
            }
        }
    }
    return true;
}

bool extract_compaction_event(const char *frame, char *message, size_t message_max) {
    if (!frame || !message || message_max == 0) return false;
    if (!strstr(frame, "\"type\":\"event\"")) return false;
    if (!strstr(frame, "compact") && !strstr(frame, "Compacting")) return false;

    message[0] = '\0';
    if (!extract_json_string_field(frame, "message", message, message_max) &&
        !extract_json_string_field(frame, "summary", message, message_max) &&
        !extract_json_string_field(frame, "text", message, message_max) &&
        !extract_json_string_field(frame, "status", message, message_max)) {
        snprintf(message, message_max, "Native agent is compacting context.");
    }
    return true;
}

size_t append_compaction_notice(char *dest, size_t max_len, size_t off, const char *message) {
    off = append_text(dest, max_len, off, "[COMPACT] ");
    off = append_text(dest, max_len, off, message && message[0] ? message : "Native agent is compacting context.");
    off = append_text(dest, max_len, off, "\n");
    return off;
}

/* tool_history_append and tool_history_build_json moved to tool_history.c */

void log_tool_diagnostics(const ToolRunResult *run) {
    if (!run || !run->stderr_text[0]) return;
    char safe[sizeof(run->stderr_text)];
    snprintf(safe, sizeof(safe), "%s", run->stderr_text);
    tool_runner_redact_secrets(safe);
    fprintf(stderr, "tool=%s exit=%d stderr=%s\n", run->tool_name, run->exit_code, safe);
}

int tool_timeout_ms(const char *tool_name) {
    if (!tool_name) return 5000;
    if (strcmp(tool_name, "google_search") == 0) return global_config.google_search_timeout_ms;
    if (strcmp(tool_name, "browse_url") == 0) return global_config.browse_url_timeout_ms;
    if (strcmp(tool_name, "shell_command") == 0) return global_config.shell_command_timeout_ms;
    return 5000;
}

/* -------------------------------------------------------------------
 *  Reasoning effort injection
 * ------------------------------------------------------------------- */
static void resolve_and_inject_reasoning(BridgeEngine *eng, const char *effort) {
    if (!effort || !effort[0]) return;
    if (strcmp(effort, "none") == 0 || strcmp(effort, "minimal") == 0 || strcmp(effort, "low") == 0) {
        const char *hint = "[SYSTEM MODE: reasoning_effort=low. Answer immediately in code blocks. Skip <think> processing.]\n";
        engine_write_stdin(eng, hint, strlen(hint));
        engine_push_html(eng, "<div class='event-node' data-action='MODIFIED'><span class='event-badge badge-modified'>MODE</span>reasoning_effort=low</div>\n");
    } else if (strcmp(effort, "medium") == 0) {
        const char *hint = "[SYSTEM MODE: reasoning_effort=medium. Balance speed and verification.]\n";
        engine_write_stdin(eng, hint, strlen(hint));
        engine_push_html(eng, "<div class='event-node' data-action='MODIFIED'><span class='event-badge badge-modified'>MODE</span>reasoning_effort=medium</div>\n");
    } else if (strcmp(effort, "high") == 0 || strcmp(effort, "xhigh") == 0 || strcmp(effort, "max") == 0) {
        const char *hint = "[SYSTEM MODE: reasoning_effort=max. Open verification chains. Run multi-step logic paths.]\n";
        engine_write_stdin(eng, hint, strlen(hint));
        engine_push_html(eng, "<div class='event-node' data-action='MODIFIED'><span class='event-badge badge-modified'>MODE</span>reasoning_effort=max</div>\n");
    } else {
        char hint[256];
        snprintf(hint, sizeof(hint), "[SYSTEM MODE: reasoning_effort=%s. Preserve requested thinking level.]\n", effort);
        engine_write_stdin(eng, hint, strlen(hint));
        engine_push_html(eng, "<div class='event-node' data-action='MODIFIED'><span class='event-badge badge-modified'>MODE</span>reasoning_effort=custom</div>\n");
    }
}

/* -------------------------------------------------------------------
 *  Build session metadata JSON with workspace root, git info, model id,
 *  timestamps, last response id, and cache path.
 * ------------------------------------------------------------------- */
void build_session_metadata_json(const BridgeEngine *eng, const char *last_response_id, char *buf, size_t buf_max) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    char created_str[64], updated_str[64];
    snprintf(created_str, sizeof(created_str), "%lld.%ld", (long long)now.tv_sec, (long)now.tv_nsec);
    snprintf(updated_str, sizeof(updated_str), "%lld.%ld", (long long)now.tv_sec, (long)now.tv_nsec);

    /* Git info: remote, branch, head (best-effort) */
    char git_remote[256] = "";
    char git_branch[256] = "";
    char git_head[64] = "";
    if (eng->workspace[0]) {
        /* Try git remote (safe subprocess, no shell) */
        char *argv_remote[] = {(char*)"git", (char*)"remote", (char*)"get-url", (char*)"origin", NULL};
        char line[256] = "";
        if (safe_subprocess_capture(eng->workspace, argv_remote, line, sizeof(line), 5)) {
            snprintf(git_remote, sizeof(git_remote), "%s", line);
        }
        /* Try git branch */
        char *argv_branch[] = {(char*)"git", (char*)"rev-parse", (char*)"--abbrev-ref", (char*)"HEAD", NULL};
        if (safe_subprocess_capture(eng->workspace, argv_branch, line, sizeof(line), 5)) {
            snprintf(git_branch, sizeof(git_branch), "%s", line);
        }
        /* Try git head commit */
        char *argv_head[] = {(char*)"git", (char*)"rev-parse", (char*)"HEAD", NULL};
        if (safe_subprocess_capture(eng->workspace, argv_head, line, sizeof(line), 5)) {
            snprintf(git_head, sizeof(git_head), "%s", line);
        }
    }

    /* JSON-escape strings */
    char escaped_remote[512], escaped_branch[256], escaped_head[128], escaped_cache_dir[1024];
    size_t ri = 0, bi = 0, hi = 0, ci = 0;
    for (const char *p = git_remote; *p && ri < sizeof(escaped_remote) - 4; p++) {
        if (*p == '"' || *p == '\\') escaped_remote[ri++] = '\\';
        escaped_remote[ri++] = *p;
    }
    escaped_remote[ri] = '\0';
    for (const char *p = git_branch; *p && bi < sizeof(escaped_branch) - 4; p++) {
        if (*p == '"' || *p == '\\') escaped_branch[bi++] = '\\';
        escaped_branch[bi++] = *p;
    }
    escaped_branch[bi] = '\0';
    for (const char *p = git_head; *p && hi < sizeof(escaped_head) - 4; p++) {
        if (*p == '"' || *p == '\\') escaped_head[hi++] = '\\';
        escaped_head[hi++] = *p;
    }
    escaped_head[hi] = '\0';
    const char *cache_dir = global_config.kv_cache_dir[0] ? global_config.kv_cache_dir : "";
    for (const char *p = cache_dir; *p && ci < sizeof(escaped_cache_dir) - 4; p++) {
        if (*p == '"' || *p == '\\') escaped_cache_dir[ci++] = '\\';
        escaped_cache_dir[ci++] = *p;
    }
    escaped_cache_dir[ci] = '\0';

    const char *safe_response_id = last_response_id ? last_response_id : "";
    snprintf(buf, buf_max,
             "{\"workspace_root\":\"%s\","
             "\"git_remote\":\"%s\","
             "\"git_branch\":\"%s\","
             "\"git_head\":\"%s\","
             "\"model_id\":\"%s\","
             "\"created_at\":\"%s\","
             "\"updated_at\":\"%s\","
             "\"last_response_id\":\"%s\","
             "\"cache_path\":\"%s\"}",
             eng->workspace,
             escaped_remote, escaped_branch, escaped_head,
             global_config.codex_model,
             created_str, updated_str,
             safe_response_id,
             escaped_cache_dir);
}

/* -------------------------------------------------------------------
 *  Route handlers
 * ------------------------------------------------------------------- */

/* Handle GET /v1/models — return model list */
static void handle_models(int fd) {
    char body[8192];
    const char *model_id = global_config.codex_model[0] ? global_config.codex_model : "star-bridge-ds4";
    const char *alias = global_config.codex_model_alias[0] ? global_config.codex_model_alias : model_id;
    const char *display = global_config.codex_model_display_name[0] ? global_config.codex_model_display_name : alias;
    /* Browser/search tools are cut from core; the agent browses natively.
       Advertise no bridge-owned tools to avoid offering capabilities we reject. */
    snprintf(body, sizeof(body),
        "{\"object\":\"list\",\"data\":[{\"id\":\"%s\",\"object\":\"model\",\"name\":\"%s\",\"display_name\":\"%s\",\"owned_by\":\"Star Bridge Local\",\"tools\":[]}]}",
        model_id, alias, display);
    send_http(fd, "200 OK", "application/json", body);
}

/* -------------------------------------------------------------------
 *  handle_response extracted phases
 * ------------------------------------------------------------------- */

/* Phase 1: Decode HTTP request — extract method, path, headers, body.
 * Returns decoded body (caller must free). On error sends HTTP response and returns NULL. */
char *handle_response_decode(int fd, const char *raw_request, size_t raw_len,
                             char *method_buf, size_t method_max,
                             char *path_buf, size_t path_max,
                             long *cl_val, char *te_buf, size_t te_max,
                             char *ce_buf, size_t ce_max,
                             size_t *body_len) {
    (void)fd;
    size_t blen = 0;
    char *body = read_http_body(raw_request, raw_len, &blen);
    if (body_len) *body_len = blen;

    /* Extract method/path from raw request */
    const char *mp = raw_request;
    if (mp) {
        const char *sp = strchr(mp, ' ');
        if (sp) {
            size_t mlen = (size_t)(sp - mp);
            if (mlen >= method_max) mlen = method_max - 1;
            memcpy(method_buf, mp, mlen);
            method_buf[mlen] = '\0';
            const char *sp2 = strchr(sp + 1, ' ');
            if (sp2) {
                size_t plen = (size_t)(sp2 - (sp + 1));
                if (plen >= path_max) plen = path_max - 1;
                memcpy(path_buf, sp + 1, plen);
                path_buf[plen] = '\0';
            }
        }
    }

    /* Extract content length and transfer/content encoding */
    *cl_val = 0;
    const char *cl_hdr = find_header_value(raw_request, "Content-Length");
    if (cl_hdr) *cl_val = atol(cl_hdr);
    copy_header_value(raw_request, "Transfer-Encoding", te_buf, te_max, "(none)");
    copy_header_value(raw_request, "Content-Encoding", ce_buf, ce_max, "(none)");

    return body;
}

/* Phase 2: Parse request body — schema validate + parse into HarnessRequest.
 * Returns true on success. On failure sends HTTP error response and returns false. */
bool handle_response_parse(int fd, unsigned long request_number,
                           const char *body, HarnessRequest *req,
                           HarnessError *err) {
    /* Schema validation */
    char schema_error[256];
    if (!codex_validate_request_schema(body, schema_error, sizeof(schema_error))) {
        fprintf(stderr, "request=%lu schema_fail=1 error=%s\n", request_number, schema_error);
        trace_store_text(g_trace_session.error, sizeof(g_trace_session.error), schema_error);
        debug_trace_append("request=%lu parse_fail=1 code=schema_validation error=%s", request_number, schema_error);
        char json[8192];
        codex_serialize_error(schema_error, json, sizeof(json));
        send_http(fd, "400 Bad Request", "application/json", json);
        return false;
    }

    /* Parse into HarnessRequest */
    if (!codex_parse_request(body, req, err)) {
        fprintf(stderr, "request=%lu parse_fail=1 error=%s\n", request_number, err->message);
        trace_store_text(g_trace_session.error, sizeof(g_trace_session.error), err->message);
        debug_trace_append("request=%lu parse_fail=1 code=%s error=%s", request_number, err->code, err->message);
        char json[8192];
        codex_serialize_error(err->message, json, sizeof(json));
        send_http(fd, "400 Bad Request", "application/json", json);
        return false;
    }

    fprintf(stderr, "request=%lu parse_ok=1 harness=%s model=%s public_alias=%s\n",
            request_number,
            global_config.codex_harness_id[0] ? global_config.codex_harness_id : req->harness,
            global_config.codex_model[0] ? global_config.codex_model : req->model,
            global_config.codex_model_alias[0] ? global_config.codex_model_alias : req->model);
    debug_trace_append("request=%lu parse_ok=1", request_number);
    return true;
}

/* Dynamic-buffer helper for handle_response_native.
 * Grows *buf as needed (doubling). Returns false on OOM. */
static bool dyn_append(char **buf, size_t *cap, size_t *len, const char *text) {
    if (!text || !*text) return true;
    size_t add = strlen(text);
    if (*len + add + 1 > *cap) {
        size_t next = *cap * 2;
        while (next < *len + add + 1) next *= 2;
        char *grown = realloc(*buf, next);
        if (!grown) return false;
        *buf = grown;
        *cap = next;
    }
    memcpy(*buf + *len, text, add);
    *len += add;
    (*buf)[*len] = '\0';
    return true;
}

/* Phase 3: Forward to native agent — build input, check connection, send turn.
 * Returns true on success. On failure sends error response and returns false.
 * out_buf is populated with agent output text for framed protocol. */
bool handle_response_native(int fd, BridgeEngine *eng, unsigned long request_number,
                            const HarnessRequest *req,
                            char *out_buf, size_t out_max) {
    /* Build normalized input with tool history, session hints, context tokens.
     * Use a heap-allocated dynamic buffer to avoid silent truncation of large inputs. */
    size_t input_len = req->normalized_input ? strlen(req->normalized_input) : 0;
    size_t input_cap = input_len + 4096;
    char *input = malloc(input_cap);
    if (!input) {
        send_http(fd, "500 Internal Server Error", "application/json", "{\"error\":\"oom\"}");
        return false;
    }
    if (req->normalized_input && input_len > 0) {
        memcpy(input, req->normalized_input, input_len);
    }
    input[input_len] = '\0';

    /* Inject workspace root directive so agent knows its working directory */
    if (eng->workspace[0]) {
        if (input_len > 0 && input[input_len - 1] != '\n') {
            if (!dyn_append(&input, &input_cap, &input_len, "\n")) goto oom;
        }
        if (!dyn_append(&input, &input_cap, &input_len, "workspace_root: ")) goto oom;
        if (!dyn_append(&input, &input_cap, &input_len, eng->workspace)) goto oom;
        if (!dyn_append(&input, &input_cap, &input_len, "\n")) goto oom;
    }
    /* Append tool history unless hide_tool_transcripts is enabled */
    if (!global_config.hide_tool_transcripts && tool_history_count() > 0) {
        char *hj = tool_history_build_json();
        if (hj) {
            bool ok = dyn_append(&input, &input_cap, &input_len, "tool_history:") &&
                      dyn_append(&input, &input_cap, &input_len, hj) &&
                      dyn_append(&input, &input_cap, &input_len, "\n");
            free(hj);
            if (!ok) goto oom;
        }
    }
    if (req->previous_response_id[0]) {
        if (input_len > 0 && input[input_len - 1] != '\n') {
            if (!dyn_append(&input, &input_cap, &input_len, "\n")) goto oom;
        }
        if (!dyn_append(&input, &input_cap, &input_len, "auto_load_resume_session: ")) goto oom;
        if (!dyn_append(&input, &input_cap, &input_len, global_config.auto_load_resume_session ? "true" : "false")) goto oom;
        if (!dyn_append(&input, &input_cap, &input_len, "\n")) goto oom;
    }

    /* Append tool definitions if present */
    if (req->tool_defs_json[0]) {
        /* Parse tools from stored JSON and build normalized prompt */
        cJSON *tools_arr = cJSON_Parse(req->tool_defs_json);
        if (tools_arr && cJSON_IsArray(tools_arr)) {
            /* Wrap in a fake body object for normalizer */
            cJSON *body = cJSON_CreateObject();
            if (body) {
                cJSON_AddItemToObject(body, "tools", tools_arr);
                /* Copy tool_choice if present */
                if (req->tool_choice_json[0]) {
                    cJSON *tc = cJSON_Parse(req->tool_choice_json);
                    if (tc) cJSON_AddItemToObject(body, "tool_choice", tc);
                }

                NormalizedToolDef norm_tools[MAX_TOOL_DEFS];
                int nt_count = codex_normalize_tools(body, norm_tools, MAX_TOOL_DEFS);
                ToolChoiceSpec tc_spec = codex_resolve_tool_choice(body);

                /* Build tool prompt */
                char tool_prompt[8192];
                codex_build_tool_prompt(norm_tools, nt_count, tool_prompt, sizeof(tool_prompt));
                if (tool_prompt[0]) {
                    if (input_len > 0 && input[input_len - 1] != '\n') {
                        if (!dyn_append(&input, &input_cap, &input_len, "\n")) { cJSON_Delete(body); goto oom; }
                    }
                    if (!dyn_append(&input, &input_cap, &input_len, "available_tools:")) { cJSON_Delete(body); goto oom; }
                    if (!dyn_append(&input, &input_cap, &input_len, tool_prompt)) { cJSON_Delete(body); goto oom; }
                }

                /* Append tool_choice directive */
                switch (tc_spec.mode) {
                    case TOOL_CHOICE_NONE:
                        if (!dyn_append(&input, &input_cap, &input_len, "tool_choice: none\n")) { cJSON_Delete(body); goto oom; }
                        break;
                    case TOOL_CHOICE_REQUIRED:
                        if (!dyn_append(&input, &input_cap, &input_len, "tool_choice: required\n")) { cJSON_Delete(body); goto oom; }
                        break;
                    case TOOL_CHOICE_NAMED:
                        if (!dyn_append(&input, &input_cap, &input_len, "tool_choice: ")) { cJSON_Delete(body); goto oom; }
                        if (!dyn_append(&input, &input_cap, &input_len, tc_spec.named_tool)) { cJSON_Delete(body); goto oom; }
                        if (!dyn_append(&input, &input_cap, &input_len, "\n")) { cJSON_Delete(body); goto oom; }
                        break;
                    default:
                        break; /* auto — no directive needed */
                }

                /* Capability routing: analyze tools and input for conservative activation */
                RoutingDecision cap_dec = capability_route_tools(
                    norm_tools, nt_count, &tc_spec,
                    req->normalized_input, strlen(req->normalized_input));

                /* Append capability routing hints to input */
                {
                    char cap_summary[1024];
                    capability_decision_summary(&cap_dec, cap_summary, sizeof(cap_summary));
                    if (cap_summary[0]) {
                        if (input_len > 0 && input[input_len - 1] != '\n') {
                            if (!dyn_append(&input, &input_cap, &input_len, "\n")) { cJSON_Delete(body); goto oom; }
                        }
                        if (!dyn_append(&input, &input_cap, &input_len, "capability_routing: ")) { cJSON_Delete(body); goto oom; }
                        if (!dyn_append(&input, &input_cap, &input_len, cap_summary)) { cJSON_Delete(body); goto oom; }
                        if (!dyn_append(&input, &input_cap, &input_len, "\n")) { cJSON_Delete(body); goto oom; }
                    }

                    /* Log routing decision */
                    debug_trace_append("request=%lu capability_routing active=0x%02x summary=\"%s\"",
                                       request_number,
                                       cap_dec.active_capabilities,
                                       cap_summary);
                }

                cJSON_Delete(body);
            }
        } else {
            cJSON_Delete(tools_arr);
        }
    }

    if (input_len > 0 && input[input_len - 1] != '\n') {
        if (!dyn_append(&input, &input_cap, &input_len, "\n")) goto oom;
    }
    if (!dyn_append(&input, &input_cap, &input_len, "context_tokens: ")) goto oom;
    char context_buf[32];
    snprintf(context_buf, sizeof(context_buf), "%d", global_config.context_tokens);
    if (!dyn_append(&input, &input_cap, &input_len, context_buf)) goto oom;
    if (!dyn_append(&input, &input_cap, &input_len, "\n")) goto oom;
    /* Belt-and-suspenders workspace directive for the native agent (including ds4 via wrapper).
     * Complements the hello "workspace_root" and the wrapper's prompt prefix. Ensures that
     * even if Codex request context mentions another dir ("New project 2"), the agent sees
     * the bridge-provided root for the current launch. The wrapper also
     * injects a human-readable version at the very front of what it sends to ds4 stdin. */
    if (eng->workspace[0]) {
        if (!dyn_append(&input, &input_cap, &input_len, "workspace_root: ")) goto oom;
        if (!dyn_append(&input, &input_cap, &input_len, eng->workspace)) goto oom;
        if (!dyn_append(&input, &input_cap, &input_len, "\n")) goto oom;
        if (!dyn_append(&input, &input_cap, &input_len,
            "DIRECTIVE: All file operations, list, find, read, edit must be performed under the above workspace_root. Ignore paths from history or other projects.\n")) goto oom;
        if (!dyn_append(&input, &input_cap, &input_len,
            "After exploration or tool results, always continue to a full structured final answer for the original user request (e.g. review summary with intent, state, scored improvements). Do not end after raw listings.\n")) goto oom;
    }
    char normalized_sample[512];
    debug_trace_compact_text(input, normalized_sample, sizeof(normalized_sample));
    debug_trace_append("bridge_normalized request=%lu input_bytes=%zu previous_response_id=%s reasoning_effort=%s sample=\"%s\"",
                       request_number,
                       strlen(input),
                       req->previous_response_id[0] ? req->previous_response_id : "(none)",
                       req->reasoning_effort[0] ? req->reasoning_effort : "(none)",
                       normalized_sample);

    /* Check connection availability before serving a turn */
    if (global_config.use_framed_protocol) {
        ConnectionState cs = eng->nc.state;
        if (cs != CONN_READY && cs != CONN_CONNECTED && cs != CONN_HANDSHAKING) {
            char err_json[8192];
            codex_serialize_error("native_agent_unavailable", err_json, sizeof(err_json));
            send_http(fd, "503 Service Unavailable", "application/json", err_json);
            free(input);
            return false;
        }
    }

    /* Forward input to native agent */
    if (input[0]) {
        if (global_config.use_framed_protocol) {
            /* Mark turn active so heartbeat skips pings during the turn */
            engine_set_turn_active(eng, true);
            /* Send input as framed request and receive framed response */
            TurnContext ctx;
            /* For streaming requests we pass the client fd so that text_delta frames
             * received from the native (wrapper) can be emitted live as SSE deltas.
             * This, combined with deltas sent from the ds4 wrapper, prevents long
             * "silent thinking" periods that trigger Codex "Reconnecting". */
            int live_fd = req->stream ? fd : -1;
            ResponsesStreamState *live_state = NULL;
            if (req->stream && global_config.use_framed_protocol) {
                live_state = malloc(sizeof(ResponsesStreamState));
                if (live_state) {
                    const char *stream_id = "resp-stream";
                    const char *model = global_config.codex_model_alias[0] ? global_config.codex_model_alias : global_config.codex_model;
                    responses_stream_init(live_state, stream_id, model, fd, g_gzip_supported);
                    responses_stream_emit_created(live_state);
                    responses_stream_start_item(live_state);
                    responses_stream_start_part(live_state, CONTENT_TYPE_OUTPUT_TEXT, NULL);
                }
            }
            turn_context_init(&ctx, eng, request_number, input, req->previous_response_id,
                              req->reasoning_effort, req->reset_session,
                              out_buf, out_max, live_fd, live_state,
                              g_server_fd);
            bool ok = turn_begin(&ctx);
            if (ok) ok = turn_await_ack(&ctx);
            if (ok) ok = turn_process_events(&ctx);
            /* Capture cancellation before turn_cleanup clears eng->cancelled. */
            bool turn_cancelled = eng->cancelled;
            turn_cleanup(&ctx);
            /* Turn complete — heartbeat may resume */
            engine_set_turn_active(eng, false);
            /* Record event-cap status so the non-stream writer can surface
             * incomplete_details=max_turn_events on the HTTP response. */
            eng->turn_event_limited = (ok && ctx.event_limit_exceeded);

            /* For live framed streaming: the early ResponsesStreamState (live_state) has already
             * received created + item + part + live text_deltas during turn_process.
             * Now that the turn ended (success or error after partial output), we MUST emit the
             * terminal events on *this* state so Codex always sees a proper response.completed
             * (or error) instead of "stream closed before response.completed" after one block.
             * We also free it here. The subsequent handle_response_stream will early-return for
             * framed to avoid duplicate lifecycle events on the wire. */
            if (live_state) {
                if (ok) {
                    /* Check for event limit exceeded — emit completed with incomplete_details */
                    if (ctx.event_limit_exceeded) {
                        snprintf(live_state->incomplete_details, sizeof(live_state->incomplete_details),
                                 "max_turn_events");
                    }
                    responses_stream_emit_text_done(live_state);
                    responses_stream_emit_content_part_done(live_state);
                    responses_stream_emit_output_item_done(live_state);
                    responses_stream_emit_completed(live_state);
                } else if (turn_cancelled) {
                    /* Mid-turn DELETE cancellation: terminate the live stream with a
                     * proper response.failed terminal so Codex stops cleanly. */
                    responses_stream_emit_failed(live_state, "cancelled");
                } else {
                    const char *emsg = (out_buf && out_buf[0]) ? out_buf : "native turn failed (see bridge logs)";
                    responses_stream_emit_error(live_state, emsg);
                }
                free(live_state);
                live_state = NULL;
            }

            if (ok) {
                /* Success: push agent output text into engine buffer */
                if (out_buf[0]) {
                    engine_push_text(eng, out_buf);
                } else {
                    engine_push_text(eng, "Native agent returned an empty framed response.\n");
                }
            } else {
                /* Protocol failure: return structured error */
                trace_store_text(g_trace_session.error, sizeof(g_trace_session.error), out_buf);
                debug_trace_append("request=%lu native_protocol_failure error=%s", request_number, out_buf);
                free(input);
                return false;
            }
        } else {
            resolve_and_inject_reasoning(eng, req->reasoning_effort);
            trace_store_text(g_trace_session.native_request, sizeof(g_trace_session.native_request), input);
            engine_write_stdin(eng, input, strlen(input));
            if (input[strlen(input) - 1] != '\n') engine_write_stdin(eng, "\n", 1);
        }
    } else {
        fprintf(stderr, "DEBUG: no input field found\n");
        trace_store_text(g_trace_session.error, sizeof(g_trace_session.error), "No normalized input field found");
    }

    /* Wait for agent output (only non-framed; framed already blocks in turn_process_events) */
    if (!global_config.use_framed_protocol) {
        if (engine_read_text(eng, NULL, 0) == 0) {
            wait_for_native_output(eng, global_config.response_timeout_ms);
        }
    }
    free(input);
    return true;

oom:
    send_http(fd, "500 Internal Server Error", "application/json", "{\"error\":\"oom\"}");
    free(input);
    return false;
}

/* Phase 4: SSE streaming writer — emit Responses lifecycle events + text deltas.
 * Reads content from out_buf (framed) or engine buffer (non-framed). */
void handle_response_stream(int fd, BridgeEngine *eng, unsigned long request_number,
                            const HarnessRequest *req,
                            const char *out_buf) {
    (void)req;
    /* For framed + streaming (live deltas path), the Responses lifecycle (created, deltas via
     * live_state during turn, and terminal completed/error + [DONE]) was already fully emitted
     * right after turn_process_events in handle_response_native. Emitting again here would send
     * a second response.created + duplicate deltas + another completed after [DONE], which can
     * cause Codex to see the stream as restarted or closed prematurely.
     * Early return keeps exactly one clean stream per request. */
    if (global_config.use_framed_protocol) {
        return;
    }

    const char *stream_id = "resp-stream";
    const char *model = global_config.codex_model_alias[0] ? global_config.codex_model_alias : global_config.codex_model;

    /* Allocate state on heap (struct is ~64 MB due to ContentPart buffers) */
    ResponsesStreamState *state = malloc(sizeof(ResponsesStreamState));
    if (!state) {
        send_http(fd, "500 Internal Server Error", "application/json",
                  "{\"error\":\"out of memory\"}");
        return;
    }
    responses_stream_init(state, stream_id, model, fd, g_gzip_supported);

    /* Build content from engine buffer */
    size_t content_max = (size_t)global_config.max_output_buffer;
    char content[content_max];
    bool truncated_buffer = false;
    if (global_config.use_framed_protocol) {
        size_t out_len = out_buf ? strlen(out_buf) : 0;
        if (out_len >= sizeof(content)) {
            truncated_buffer = true;
            fprintf(stderr, "truncated assistant text at %zu bytes (max_output_buffer)\n", sizeof(content) - 1);
        }
        snprintf(content, sizeof(content), "%s", out_buf ? out_buf : "");
    } else {
        size_t text_filled = engine_read_text(eng, content, sizeof(content));
        if (text_filled >= sizeof(content) - 1) {
            truncated_buffer = true;
            fprintf(stderr, "truncated assistant text at %zu bytes (max_output_buffer)\n", sizeof(content) - 1);
        }
        if (text_filled == 0) {
            snprintf(content, sizeof(content),
                     "Native agent produced no assistant text. Check the agent command, framed protocol setting, or native-agent logs.");
        }
    }
    if (!global_config.use_framed_protocol) {
        trace_store_text(g_trace_session.native_response, sizeof(g_trace_session.native_response), content);
    }
    size_t content_len = strlen(content);
    size_t chunk_size = 4096;
    char content_sample[512];
    debug_trace_compact_text(content, content_sample, sizeof(content_sample));
    debug_trace_append("bridge_to_codex request=%lu stream=true content_bytes=%zu chunk_size=%zu sample=\"%s\"",
                       request_number, content_len, chunk_size, content_sample);
    fprintf(stderr, "request=%lu stream content_len=%zu chunk_size=%zu\n",
            request_number, content_len, chunk_size);

    /* Emit lifecycle via state */
    if (!responses_stream_emit_created(state)) {
        responses_stream_emit_error(state, "failed to emit created event");
        free(state); return;
    }
    if (!responses_stream_start_item(state)) {
        responses_stream_emit_error(state, "failed to start output item");
        free(state); return;
    }
    if (!responses_stream_start_part(state, CONTENT_TYPE_OUTPUT_TEXT, NULL)) {
        responses_stream_emit_error(state, "failed to start content part");
        free(state); return;
    }

    /* Emit text deltas in chunks with cancellation check */
    bool stream_cancelled = false;
    for (size_t offset = 0; offset < content_len; offset += chunk_size) {
        if (eng->cancelled) {
            fprintf(stderr, "request=%lu stream cancelled at offset=%zu\n", request_number, offset);
            stream_cancelled = true;
            break;
        }
        size_t chunk_len = content_len - offset;
        if (chunk_len > chunk_size) chunk_len = chunk_size;
        char chunk[4096 + 1];
        memcpy(chunk, content + offset, chunk_len);
        chunk[chunk_len] = '\0';
        if (!fd_writable(fd, 1000)) {
            fprintf(stderr, "request=%lu stream backpressure timeout at offset=%zu\n", request_number, offset);
            engine_cancel(eng);
            stream_cancelled = true;
            break;
        }
        if (!responses_stream_emit_text_delta(state, chunk)) {
            stream_cancelled = true;
            break;
        }
    }

    if (stream_cancelled) {
        responses_stream_emit_error(state, eng->cancelled ? "cancelled" : "client_disconnected");
    } else {
        /* Set incomplete_details if output was truncated */
        if (truncated_buffer) {
            snprintf(state->incomplete_details, sizeof(state->incomplete_details), "max_output_buffer");
        }
        responses_stream_emit_text_done(state);
        responses_stream_emit_content_part_done(state);
        responses_stream_emit_output_item_done(state);
        responses_stream_emit_completed(state);
    }
    free(state);
}

/* Phase 5: Blocking response writer — serialize + validate + send HTTP.
 * Reads content from out_buf (framed) or engine buffer (non-framed). */
void handle_response_block(int fd, unsigned long request_number,
                           BridgeEngine *eng,
                           const HarnessRequest *req,
                           const char *out_buf) {
    (void)req;
    bool truncated_buffer = false;
    bool truncated_chars = false;
    size_t content_max = (size_t)global_config.max_output_buffer;
    char content[content_max];
    if (global_config.use_framed_protocol) {
        size_t out_len = out_buf ? strlen(out_buf) : 0;
        /* out_buf was sized at max_output_buffer; if it's full, text was clipped */
        if (out_len >= content_max - 1) {
            truncated_buffer = true;
            fprintf(stderr, "truncated assistant text at %zu bytes (max_output_buffer)\n", out_len);
        }
        snprintf(content, sizeof(content), "%s", out_buf ? out_buf : "");
    } else {
        size_t text_filled = engine_read_text(eng, content, sizeof(content));
        if (text_filled >= sizeof(content) - 1) {
            truncated_buffer = true;
            fprintf(stderr, "truncated assistant text at %zu bytes (max_output_buffer)\n", sizeof(content) - 1);
        }
        if (text_filled == 0) {
            snprintf(content, sizeof(content),
                     "Native agent produced no assistant text. Check the agent command, framed protocol setting, or native-agent logs.");
        }
    }
    if (!global_config.use_framed_protocol) {
        trace_store_text(g_trace_session.native_response, sizeof(g_trace_session.native_response), content);
    }
    /* Truncate content to max_output_chars if configured */
    if (global_config.max_output_chars > 0) {
        size_t len = strlen(content);
        if (len > (size_t)global_config.max_output_chars) {
            content[global_config.max_output_chars] = '\0';
            truncated_chars = true;
            fprintf(stderr, "truncated assistant text to %d chars (was %zu)\n",
                    global_config.max_output_chars, len);
            debug_trace_append("truncated assistant text to %d chars (was %zu)",
                               global_config.max_output_chars, len);
        }
    }
    {
        char content_sample[512];
        debug_trace_compact_text(content, content_sample, sizeof(content_sample));
        debug_trace_append("bridge_to_codex request=%lu stream=false content_bytes=%zu sample=\"%s\"",
                           request_number, strlen(content), content_sample);
    }

    /* Read-and-clear the event-cap flag set by the framed turn. Output-buffer
     * truncation takes precedence, then char clip, then event cap. */
    bool event_limited = eng->turn_event_limited;
    eng->turn_event_limited = false;

    char json[524288];
    const char *model = global_config.codex_model_alias[0] ? global_config.codex_model_alias : global_config.codex_model;
    const char *details = truncated_buffer ? "max_output_buffer"
                          : (truncated_chars ? "max_output_chars"
                          : (event_limited ? "max_turn_events" : NULL));
    codex_serialize_text_response_with_details("resp-block", model, content, details, json, sizeof(json));
    char rsp_error[256];
    if (!codex_validate_response_json(json, rsp_error, sizeof(rsp_error))) {
        fprintf(stderr, "request=%lu response_schema_fail error=%s\n", request_number, rsp_error);
        debug_trace_append("request=%lu response_schema_fail error=%s", request_number, rsp_error);
        trace_store_text(g_trace_session.error, sizeof(g_trace_session.error), rsp_error);
        char err_json[8192];
        codex_serialize_error(rsp_error, err_json, sizeof(err_json));
        send_http(fd, "500 Internal Server Error", "application/json", err_json);
    } else {
        send_http(fd, "200 OK", "application/json", json);
    }
}

/* -------------------------------------------------------------------
 *  Host allowlist validation helpers
 * ------------------------------------------------------------------- */

/* Check if a hostname (without port) is in the configured allowlist.
 * Always allows: 127.0.0.1, localhost, ::1, and the configured bind_host.
 * Also checks comma-separated entries from host_allowlist config.
 * Returns true if the host is allowed. */
static bool host_is_allowed(const char *host) {
    if (!host || !host[0]) return false;

    /* Always allow loopback and configured bind_host */
    if (strcmp(host, "127.0.0.1") == 0) return true;
    if (strcmp(host, "localhost") == 0) return true;
    if (strcmp(host, "::1") == 0) return true;
    if (strcmp(host, "::ffff:127.0.0.1") == 0) return true;
    if (global_config.bind_host[0] && strcmp(host, global_config.bind_host) == 0) return true;

    /* Check host_allowlist (comma-separated entries) */
    const char *al = global_config.host_allowlist;
    if (!al || !al[0]) return false;

    const char *p = al;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',' && *p != ' ') p++;
        size_t len = (size_t)(p - start);
        if (len > 0 && strlen(host) == len && strncmp(host, start, len) == 0) return true;
    }
    return false;
}

/* Validate the Host header against the allowlist.
 * Returns true if Host is allowed, false otherwise.
 * OPTIONS requests bypass this check. */
static bool validate_host_header(const char *raw_request) {
    /* OPTIONS requests bypass Host validation */
    if (strncmp(raw_request, "OPTIONS ", 8) == 0) return true;

    const char *host_val = find_header_value(raw_request, "Host");
    if (!host_val) {
        /* No Host header — reject */
        return false;
    }

    /* Extract the host part (strip port) into a temporary buffer */
    char host_buf[256];
    const char *p = host_val;
    while (*p == ' ' || *p == '\t') p++;

    /* Handle IPv6 addresses like "[::1]" or "[::1]:8080" */
    if (*p == '[') {
        const char *close = strchr(p, ']');
        if (!close) return false;
        size_t len = (size_t)(close - p + 1);
        if (len >= sizeof(host_buf)) len = sizeof(host_buf) - 1;
        memcpy(host_buf, p, len);
        host_buf[len] = '\0';
        /* Also allow bare "[::1]" as equivalent to "::1" */
        if (strcmp(host_buf, "[::1]") == 0) return true;
        if (global_config.bind_host[0] && strcmp(host_buf, global_config.bind_host) == 0) return true;
        /* Check allowlist with bracketed form */
        return host_is_allowed(host_buf);
    }

    /* Strip port suffix for IPv4/hostname */
    const char *line_end = p;
    while (*line_end && *line_end != '\r' && *line_end != '\n') line_end++;
    const char *colon = memchr(p, ':', (size_t)(line_end - p));
    const char *end;
    if (colon && colon > p) {
        end = colon;
    } else {
        end = line_end;
    }
    size_t len = (size_t)(end - p);
    if (len >= sizeof(host_buf)) len = sizeof(host_buf) - 1;
    memcpy(host_buf, p, len);
    host_buf[len] = '\0';

    return host_is_allowed(host_buf);
}

/* -------------------------------------------------------------------
 *  Route handlers
 * ------------------------------------------------------------------- */

/* -------------------------------------------------------------------
 *  Handle POST /v1/responses/compact — Codex compaction request
 * ------------------------------------------------------------------- */

/* Parse a compact request body from JSON */
static void parse_compact_body(const char *body, size_t body_len,
                                char *prev_id, size_t prev_max,
                                char *reasoning, size_t reason_max) {
    (void)body_len; /* unused — json_parse uses null termination */
    if (!body) return;
    void *obj = json_parse(body);
    if (!obj) return;
    json_get_string(obj, "previous_response_id", prev_id, prev_max);
    json_get_string(obj, "reasoning_effort", reasoning, reason_max);
    json_free(obj);
}

/* Handle POST /v1/responses/compact */
static void handle_compact(int fd, BridgeEngine *eng, const char *raw_request, size_t raw_len) {
    unsigned long request_number = ++g_request_counter;
    if (global_config.trace) {
        memset(&g_trace_session, 0, sizeof(g_trace_session));
        g_trace_session.request_number = request_number;
        trace_store_headers(raw_request);
    }

    /* Decode HTTP */
    char method_buf[32] = "(unknown)", path_buf[128] = "(unknown)";
    long cl_val = 0;
    char te_buf[64] = "(none)", ce_buf[64] = "(none)";
    size_t body_len = 0;
    char *body = handle_response_decode(fd, raw_request, raw_len,
                                        method_buf, sizeof(method_buf),
                                        path_buf, sizeof(path_buf),
                                        &cl_val, te_buf, sizeof(te_buf),
                                        ce_buf, sizeof(ce_buf), &body_len);
    if (global_config.trace) {
        trace_store_text(g_trace_session.method, sizeof(g_trace_session.method), method_buf);
        trace_store_text(g_trace_session.path, sizeof(g_trace_session.path), path_buf);
        trace_store_text(g_trace_session.http_body, sizeof(g_trace_session.http_body), body ? body : "");
    }
    fprintf(stderr, "compact_request=%lu method=%s path=%s content_length=%ld raw_bytes=%zu decoded_bytes=%zu\n",
            request_number, method_buf, path_buf, cl_val, raw_len, body_len);
    debug_trace_append("compact_request=%lu method=%s path=%s content_length=%ld raw_bytes=%zu decoded_bytes=%zu",
                       request_number, method_buf, path_buf, cl_val, raw_len, body_len);

    if (!body) { free(body); return; }
    if (strcmp(ce_buf, "(none)") != 0 && strcasecmp(ce_buf, "identity") != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Unsupported Content-Encoding: %s", ce_buf);
        char json[8192];
        codex_serialize_error(msg, json, sizeof(json));
        send_http(fd, "415 Unsupported Media Type", "application/json", json);
        free(body); return;
    }

    /* Parse compact request body */
    char prev_id[64] = {0};
    char reasoning_effort[64] = {0};
    parse_compact_body(body, body_len, prev_id, sizeof(prev_id), reasoning_effort, sizeof(reasoning_effort));
    fprintf(stderr, "compact_request=%lu previous_response_id=%s reasoning_effort=%s\n",
            request_number, prev_id[0] ? prev_id : "(none)", reasoning_effort[0] ? reasoning_effort : "(none)");
    debug_trace_append("compact_request=%lu previous_response_id=%s reasoning_effort=%s",
                       request_number, prev_id[0] ? prev_id : "(none)", reasoning_effort[0] ? reasoning_effort : "(none)");

    /* Check if native agent supports compaction (connection available) */
    bool supported = false;
    if (global_config.use_framed_protocol) {
        ConnectionState cs = eng->nc.state;
        if (cs == CONN_READY || cs == CONN_CONNECTED) {
            supported = true;
        }
    }

    if (!supported) {
        /* Return structured unsupported error */
        char json[8192];
        snprintf(json, sizeof(json),
                 "{\"id\":\"compact-%lu\",\"object\":\"response\",\"status\":\"failed\","
                 "\"error\":{\"message\":\"Compaction not supported: native agent unavailable\"}}",
                 request_number);
        send_http(fd, "503 Service Unavailable", "application/json", json);
        free(body);
        return;
    }

    /* Send compact turn to native agent */
    size_t out_max = (size_t)global_config.max_output_buffer;
    char out_buf[out_max];
    out_buf[0] = '\0';
    char compact_input[4096];
    snprintf(compact_input, sizeof(compact_input),
             "Compact the conversation context. "
             "previous_response_id=%s reasoning_effort=%s",
             prev_id[0] ? prev_id : "none",
             reasoning_effort[0] ? reasoning_effort : "none");
    TurnContext ctx;
    turn_context_init(&ctx, eng, request_number, compact_input,
                      prev_id[0] ? prev_id : NULL,
                      reasoning_effort[0] ? reasoning_effort : NULL,
                      false, out_buf, sizeof(out_buf), -1, NULL,
                      g_server_fd);
    bool ok = turn_begin(&ctx);
    if (ok) ok = turn_await_ack(&ctx);
    if (ok) ok = turn_process_events(&ctx);
    turn_cleanup(&ctx);
    engine_set_turn_active(eng, false);

    /* Build Responses-shaped response */
    char resp_json[8192];
    if (ok) {
        /* Success: return compacted context */
        char escaped_text[4096];
        json_escape(out_buf[0] ? out_buf : "Context compacted successfully.", escaped_text, sizeof(escaped_text));
        snprintf(resp_json, sizeof(resp_json),
                 "{\"id\":\"compact-%lu\",\"object\":\"response\",\"status\":\"completed\","
                 "\"model\":\"%s\",\"created_at\":%lld,"
                 "\"output\":[{\"type\":\"text\",\"text\":\"%s\",\"annotations\":[]}],"
                 "\"error\":null,\"finish_reason\":\"stop\"}",
                 request_number, global_config.codex_model,
                 (long long)time(NULL), escaped_text);
        send_http(fd, "200 OK", "application/json", resp_json);
    } else {
        /* Failed compact */
        char escaped_msg[1024];
        json_escape(out_buf[0] ? out_buf : "Compaction failed: native agent error", escaped_msg, sizeof(escaped_msg));
        snprintf(resp_json, sizeof(resp_json),
                 "{\"id\":\"compact-%lu\",\"object\":\"response\",\"status\":\"failed\","
                 "\"error\":{\"message\":\"%s\"}}",
                 request_number, escaped_msg);
        send_http(fd, "503 Service Unavailable", "application/json", resp_json);
    }

    free(body);
}

/* Handle POST /v1/responses — coordinator calling extracted phases */
static void handle_response(int fd, BridgeEngine *eng, const char *raw_request, size_t raw_len) {
    unsigned long request_number = ++g_request_counter;
    if (global_config.trace) {
        memset(&g_trace_session, 0, sizeof(g_trace_session));
        g_trace_session.request_number = request_number;
        trace_store_headers(raw_request);
    }

    /* Phase 1: Decode HTTP */
    char method_buf[32] = "(unknown)", path_buf[128] = "(unknown)";
    long cl_val = 0;
    char te_buf[64] = "(none)", ce_buf[64] = "(none)";
    char ae_buf[128] = "(none)";
    size_t body_len = 0;
    char *body = handle_response_decode(fd, raw_request, raw_len,
                                        method_buf, sizeof(method_buf),
                                        path_buf, sizeof(path_buf),
                                        &cl_val, te_buf, sizeof(te_buf),
                                        ce_buf, sizeof(ce_buf), &body_len);
    if (global_config.trace) {
        trace_store_text(g_trace_session.method, sizeof(g_trace_session.method), method_buf);
        trace_store_text(g_trace_session.path, sizeof(g_trace_session.path), path_buf);
        trace_store_text(g_trace_session.http_body, sizeof(g_trace_session.http_body), body ? body : "");
    }
    if (body_len == (size_t)-1) {
        char json[8192];
        codex_serialize_error("request body too large", json, sizeof(json));
        send_http(fd, "413 Request Entity Too Large", "application/json", json);
        free(body);
        return;
    }
    fprintf(stderr, "request=%lu method=%s path=%s content_length=%ld transfer_encoding=%s content_encoding=%s raw_bytes=%zu decoded_bytes=%zu\n",
            request_number, method_buf, path_buf, cl_val, te_buf, ce_buf, raw_len, body_len);
    debug_trace_append("request=%lu method=%s path=%s content_length=%ld transfer_encoding=%s content_encoding=%s raw_bytes=%zu decoded_bytes=%zu",
                       request_number, method_buf, path_buf, cl_val, te_buf, ce_buf, raw_len, body_len);
    debug_trace_append("codex_to_bridge request=%lu method=%s path=%s body_bytes=%zu raw_bytes=%zu",
                       request_number, method_buf, path_buf, body_len, raw_len);
    debug_trace_body_probe(request_number, body, body_len);
    debug_trace_fixture_capture(request_number, raw_request, raw_len, body, body_len);

    if (!body) { free(body); return; }
    /* Parse Accept-Encoding for gzip support */
    copy_header_value(raw_request, "Accept-Encoding", ae_buf, sizeof(ae_buf), "(none)");
    bool gzip_supported = (strstr(ae_buf, "gzip") != NULL);
    g_gzip_supported = gzip_supported;
        if (strcmp(ce_buf, "(none)") != 0 && strcasecmp(ce_buf, "identity") != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Unsupported Content-Encoding: %s", ce_buf);
        fprintf(stderr, "request=%lu parse_fail=1 error=%s\n", request_number, msg);
        trace_store_text(g_trace_session.error, sizeof(g_trace_session.error), msg);
        debug_trace_append("request=%lu parse_fail=1 code=unsupported_content_encoding error=%s", request_number, msg);
        char json[8192];
        codex_serialize_error(msg, json, sizeof(json));
        send_http(fd, "415 Unsupported Media Type", "application/json", json);
        free(body); return;
    }

    /* Phase 2: Parse request */
    HarnessRequest *req_ptr = calloc(1, sizeof(HarnessRequest));
    if (!req_ptr) {
        send_http(fd, "500 Internal Server Error", "application/json", "{\"error\":\"oom\"}");
        free(body); return;
    }
    HarnessError adapter_error;
    if (!handle_response_parse(fd, request_number, body, req_ptr, &adapter_error)) {
        harness_request_free(req_ptr);
        free(req_ptr);
        free(body); return;
    }

    /* Phase 3: Forward to native agent */
    size_t out_max = (size_t)global_config.max_output_buffer;
    char out_buf[out_max];
    out_buf[0] = '\0';

    if (!handle_response_native(fd, eng, request_number, req_ptr, out_buf, sizeof(out_buf))) {
        if (req_ptr->stream) {
            if (!global_config.use_framed_protocol) {
                /* Non-framed streaming: we own the SSE lifecycle here, so send the
                 * header and a terminal error event. (Framed streaming already emitted
                 * its terminal response.failed/response.error on the live stream inside
                 * handle_response_native — do not emit a second terminal here.) */
                char sse_hdr[512];
                int n = snprintf(sse_hdr, sizeof(sse_hdr),
                         "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                         "Cache-Control: no-cache\r\nConnection: keep-alive\r\n"
                         "%s"
                         "Access-Control-Allow-Origin: *\r\n\r\n",
                         g_gzip_supported ? "Content-Encoding: gzip\r\n" : "");
                if (n > 0 && (size_t)n < sizeof(sse_hdr))
                    write_all(fd, sse_hdr, (size_t)n);
                HarnessStreamEvent event;
                codex_stream_error("resp-stream", out_buf[0] ? out_buf : "Native agent protocol failure", 0, &event);
                send_sse(fd, event.event, event.data, 0);
            }
        } else {
            char err_json[8192];
            codex_serialize_error(out_buf[0] ? out_buf : "native_agent_protocol_failure", err_json, sizeof(err_json));
            send_http(fd, "503 Service Unavailable", "application/json", err_json);
        }
        harness_request_free(req_ptr);
        free(req_ptr);
        free(body); return;
    }

    /* Phase 4 or 5: stream or block response */
    if (req_ptr->stream) {
        handle_response_stream(fd, eng, request_number, req_ptr, out_buf);
    } else {
        handle_response_block(fd, request_number, eng, req_ptr, out_buf);
    }

    harness_request_free(req_ptr);
    free(req_ptr);
    free(body);
}

/* -------------------------------------------------------------------
 *  Server entry point
 * ------------------------------------------------------------------- */
void start_codex_api_server(BridgeEngine *eng, int port) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_shutdown_signal);
    signal(SIGTERM, handle_shutdown_signal);
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    const char *bind_host = global_config.bind_host[0] ? global_config.bind_host : "127.0.0.1";
    inet_pton(AF_INET, bind_host, &addr.sin_addr);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (errno == EADDRINUSE) {
            fprintf(stderr,
                    "Error: port %d already in use on %s. "
                    "Stop the existing star_bridge process (e.g. lsof -ti :%d | xargs kill) "
                    "or choose another port with -p <port>.\n",
                    port, bind_host, port);
        } else {
            fprintf(stderr, "Error: failed to bind http://%s:%d/v1: %s\n",
                    bind_host, port, strerror(errno));
        }
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 32) != 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    printf("codex bridge listening on http://%s:%d/v1\n", bind_host, port);
    fflush(stdout);

    /* Publish the listening fd so the mid-turn control plane (turn_context.c) can
     * accept /v1/models, DELETE cancel, and concurrent POST 409 while a turn is in
     * progress. Without this, g_server_fd stays -1 and the control plane is dead. */
    g_server_fd = server_fd;

    /* Set accept timeout so we can check shutdown flag periodically */
    while (!shutdown_requested) {
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        int ret = select(server_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) continue; /* timeout, re-check shutdown flag */
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        size_t raw_len = 0;
        char *raw = read_http_request(client_fd, &raw_len);
        if (!raw || raw_len == 0) {
            close(client_fd);
            free(raw);
            continue;
        }

        /* Auth token check if configured */
        if (global_config.auth_token[0] != '\0' && strncmp(raw, "OPTIONS ", 8) != 0) {
            const char *auth_start = find_header_ci(raw, "authorization: ");
            if (!auth_start) {
                send_http(client_fd, "401 Unauthorized", "application/json", "{\"error\":\"missing auth\"}");
                close(client_fd);
                free(raw);
                continue;
            }
            auth_start += 15; /* skip "authorization: " (case-insensitive) */
            while (*auth_start == ' ') auth_start++;
            /* Expect "Bearer " or "Token " prefix; reject if missing */
            if (strncmp(auth_start, "Bearer ", 7) == 0) auth_start += 7;
            else if (strncmp(auth_start, "Token ", 6) == 0) auth_start += 6;
            else {
                send_http(client_fd, "401 Unauthorized", "application/json", "{\"error\":\"missing Bearer/Token prefix\"}");
                close(client_fd);
                free(raw);
                continue;
            }
            const char *auth_end = strstr(auth_start, "\r\n");
            size_t token_len = auth_end ? (size_t)(auth_end - auth_start) : strlen(auth_start);
            size_t expected_len = strlen(global_config.auth_token);
            if (token_len == 0 || token_len != expected_len || timingsafe_bcmp(auth_start, global_config.auth_token, expected_len) != 0) {
                send_http(client_fd, "401 Unauthorized", "application/json", "{\"error\":\"invalid auth\"}");
                close(client_fd);
                free(raw);
                continue;
            }
        }

        /* Host header allowlist check — reject non-allowlisted hosts with 403 */
        if (!validate_host_header(raw)) {
            send_http(client_fd, "403 Forbidden", "application/json",
                      "{\"error\":\"Host not in allowlist\"}");
            close(client_fd);
            free(raw);
            continue;
        }

        /* Parse request line once: method, path, query */
        char rmethod[16] = "";
        char rpath[256] = "";
        char rquery[256] = "";
        {
            const char *sp1 = strchr(raw, ' ');
            if (sp1) {
                size_t mlen = (size_t)(sp1 - raw);
                if (mlen >= sizeof(rmethod)) mlen = sizeof(rmethod) - 1;
                memcpy(rmethod, raw, mlen);
                rmethod[mlen] = '\0';
                sp1++;
                const char *sp2 = strchr(sp1, ' ');
                if (sp2) {
                    /* Extract path up to ? or sp2 */
                    const char *qs = sp1;
                    while (qs < sp2 && *qs != '?') qs++;
                    size_t plen = (size_t)(qs - sp1);
                    if (plen >= sizeof(rpath)) plen = sizeof(rpath) - 1;
                    memcpy(rpath, sp1, plen);
                    rpath[plen] = '\0';
                    /* Extract query string after ? */
                    if (*qs == '?' && qs + 1 < sp2) {
                        size_t qlen = (size_t)(sp2 - (qs + 1));
                        if (qlen >= sizeof(rquery)) qlen = sizeof(rquery) - 1;
                        memcpy(rquery, qs + 1, qlen);
                        rquery[qlen] = '\0';
                    }
                }
            }
        }

        /* Route by exact method/path pairs */
        int matched = 0;
        if (strcmp(rmethod, "OPTIONS") == 0 && strcmp(rpath, "/") == 0) {
            send_http(client_fd, "200 OK", "text/plain", "");
            matched = 1;
        } else if (strcmp(rmethod, "GET") == 0 && strcmp(rpath, "/health") == 0) {
            bool ok = engine_health_check(eng, global_config.response_timeout_ms);
            send_http(client_fd, ok ? "200 OK" : "503 Service Unavailable", "application/json",
                      ok ? "{\"status\":\"ok\",\"native_status\":\"ok\"}" : "{\"status\":\"failed\",\"native_status\":\"unavailable\"}");
            matched = 1;
        } else if (strcmp(rmethod, "GET") == 0 && (strcmp(rpath, "/v1/models") == 0 || strcmp(rpath, "/models") == 0)) {
            handle_models(client_fd);
            matched = 1;
        } else if (strcmp(rmethod, "GET") == 0 && strcmp(rpath, "/debug/session") == 0) {
            if (!global_config.trace) {
                send_http(client_fd, "404 Not Found", "application/json", "{\"error\":\"trace disabled\"}");
            } else {
                char body[70000];
                build_debug_session_json(eng, body, sizeof(body));
                send_http(client_fd, "200 OK", "application/json", body);
            }
            matched = 1;
        } else if (strcmp(rmethod, "POST") == 0 && (strcmp(rpath, "/v1/responses/compact") == 0 || strcmp(rpath, "/responses/compact") == 0)) {
            handle_compact(client_fd, eng, raw, raw_len);
            matched = 1;
        } else if (strcmp(rmethod, "POST") == 0 && (strcmp(rpath, "/v1/responses") == 0 || strcmp(rpath, "/responses") == 0)) {
            handle_response(client_fd, eng, raw, raw_len);
            matched = 1;
        } else if (strcmp(rmethod, "DELETE") == 0 && (strncmp(rpath, "/v1/responses/", 14) == 0 || strncmp(rpath, "/responses/", 10) == 0)) {
            engine_cancel(eng);
            send_http(client_fd, "200 OK", "application/json", "{\"status\":\"cancelled\"}");
            matched = 1;
        }
        if (!matched) {
            send_http(client_fd, "404 Not Found", "application/json", "{\"error\":\"not found\"}");
        }
        /* Structured access log */
        {
            char method[16] = "?";
            char path[256] = "?";
            const char *sp1 = strchr(raw, ' ');
            if (sp1) {
                size_t mlen = (size_t)(sp1 - raw);
                if (mlen < sizeof(method)) {
                    memcpy(method, raw, mlen);
                    method[mlen] = '\0';
                }
                sp1++;
                const char *sp2 = strchr(sp1, ' ');
                if (sp2) {
                    size_t plen = (size_t)(sp2 - sp1);
                    if (plen < sizeof(path)) {
                        memcpy(path, sp1, plen);
                        path[plen] = '\0';
                    }
                }
            }
            debug_trace_append("access method=%s path=%s bytes=%zu", method, path, raw_len);
        }
        close(client_fd);
        free(raw);
    }

    /* Clean up on shutdown */
    close(server_fd);
    printf("codex bridge server stopped\n");
}
