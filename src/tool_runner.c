#include "tool_runner.h"
#include "tool_policy.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static ToolAppCallback g_app_callback = NULL;

/* ------------------------------------------------------------------ */
/*  Node path resolution                                               */
/* ------------------------------------------------------------------ */

static const char *s_resolved_node_path = NULL;

static bool file_exists(const char *path) {
    if (!path || !path[0]) return false;
    return access(path, X_OK) == 0;
}

const char *tool_runner_resolve_node_path(const char *configured_path) {
    /* If we already resolved, return cached path */
    if (s_resolved_node_path) return s_resolved_node_path;

    /* 1. Try configured path */
    if (configured_path && configured_path[0]) {
        if (file_exists(configured_path)) {
            s_resolved_node_path = configured_path;
            return s_resolved_node_path;
        }
    }

    /* 2. Try common paths */
    static const char *common_paths[] = {
        "/opt/homebrew/bin/node",
        "/usr/local/bin/node",
        "/usr/bin/node",
        "/opt/local/bin/node",
        NULL
    };
    for (int i = 0; common_paths[i]; i++) {
        if (file_exists(common_paths[i])) {
            s_resolved_node_path = common_paths[i];
            return s_resolved_node_path;
        }
    }

    /* 3. Fall back to "node" for posix_spawnp PATH lookup */
    s_resolved_node_path = "node";
    return s_resolved_node_path;
}

/* ------------------------------------------------------------------ */
/*  JSON helpers (local)                                               */
/* ------------------------------------------------------------------ */

static const char *skip_ws(const char *p) {
    while (p && *p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static bool decode_json_string_value(const char **pp, char *dest, size_t max_len) {
    const char *p = pp ? *pp : NULL;
    if (!p || *p != '"' || !dest || max_len == 0) return false;
    p++;
    size_t i = 0;
    while (*p) {
        if (*p == '"') {
            dest[i] = '\0';
            *pp = p + 1;
            return true;
        }
        if (*p == '\\') {
            p++;
            if (!*p) return false;
            switch (*p) {
                case '"': case '\\': case '/': dest[i++] = *p; break;
                case 'b': dest[i++] = '\b'; break;
                case 'f': dest[i++] = '\f'; break;
                case 'n': dest[i++] = '\n'; break;
                case 'r': dest[i++] = '\r'; break;
                case 't': dest[i++] = '\t'; break;
                default: return false;
            }
            p++;
            if (i + 1 >= max_len) return false;
            continue;
        }
        if ((unsigned char)*p < 0x20 || i + 1 >= max_len) return false;
        dest[i++] = *p++;
    }
    return false;
}

static bool extract_object_string_field(const char *json, const char *field, char *dest, size_t max_len) {
    if (!json || !field || !field[0] || !dest || max_len == 0) return false;
    const char *p = skip_ws(json);
    if (!p || *p != '{') return false;
    p++;
    p = skip_ws(p);
    char key[64];
    if (!decode_json_string_value(&p, key, sizeof(key)) || strcmp(key, field) != 0) return false;
    p = skip_ws(p);
    if (*p != ':') return false;
    p++;
    p = skip_ws(p);
    if (!decode_json_string_value(&p, dest, max_len)) return false;
    p = skip_ws(p);
    if (*p != '}') return false;
    p++;
    return *skip_ws(p) == '\0' && dest[0] != '\0';
}

/* ------------------------------------------------------------------ */
/*  Callback registration                                              */
/* ------------------------------------------------------------------ */

void tool_runner_register_app_callback(ToolAppCallback callback) {
    g_app_callback = callback;
}

/* ------------------------------------------------------------------ */
/*  Secret redaction                                                   */
/* ------------------------------------------------------------------ */

static void redact_key_value(char *text, const char *key) {
    char *p = text;
    size_t key_len = strlen(key);
    while ((p = strstr(p, key)) != NULL) {
        char *value = p + key_len;
        if (*value != '=') {
            p = value;
            continue;
        }
        value++;
        char *end = value;
        while (*end && *end != ' ' && *end != '\n' && *end != '\r' && *end != '\t' &&
               *end != '"' && *end != '\'' && *end != ',' && *end != ';') {
            end++;
        }
        const char *redacted = "[redacted]";
        size_t redacted_len = strlen(redacted);
        size_t tail_len = strlen(end);
        memmove(value + redacted_len, end, tail_len + 1);
        memcpy(value, redacted, redacted_len);
        p = value + redacted_len;
    }
}

void tool_runner_redact_secrets(char *text) {
    if (!text || !text[0]) return;
    redact_key_value(text, "GOOGLE_SEARCH_API_KEY");
    redact_key_value(text, "GOOGLE_SEARCH_CX");
}

/* ------------------------------------------------------------------ */
/*  Build command args                                                 */
/* ------------------------------------------------------------------ */

static void build_command_args(const char *tool_name, const char *argument_json,
                               char *mode, size_t mode_max,
                               char *payload, size_t payload_max) {
    snprintf(mode, mode_max, "%s", tool_name ? tool_name : "");
    if (strcmp(tool_name, "shell_command") == 0) {
        if (!extract_object_string_field(argument_json, "command", payload, payload_max)) {
            payload[0] = '\0';
        }
        return;
    }
    snprintf(payload, payload_max, "%s", argument_json ? argument_json : "");
}

/* ------------------------------------------------------------------ */
/*  Main runner                                                        */
/* ------------------------------------------------------------------ */

bool tool_runner_run(const char *tool_name,
                     const char *argument_json,
                     const char *workspace,
                     const char *agent_env,
                     const char *extra_native_args,
                     const char *browser_state_dir,
                     const char *preferred_browser,
                     bool shell_command_enabled,
                     int timeout_ms,
                     ToolRunResult *out) {
    (void)extra_native_args;
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!tool_name || !tool_name[0]) return false;
    snprintf(out->tool_name, sizeof(out->tool_name), "%s", tool_name);
    if (tool_policy_is_denied(tool_name)) {
        out->denied = true;
        return false;
    }
    if (strcmp(tool_name, "shell_command") == 0 && !shell_command_enabled) {
        out->denied = true;
        snprintf(out->stderr_text, sizeof(out->stderr_text), "shell_command disabled by policy");
        return false;
    }
    if (!tool_policy_args_valid(tool_name, argument_json)) {
        out->ok = false;
        return false;
    }
    /* Browser/search tools are not supported — return structured error before allocating pipes */
    if (strcmp(tool_name, "google_search") == 0 || strcmp(tool_name, "browse_url") == 0) {
        out->ok = false;
        out->exit_code = 1;
        snprintf(out->stderr_text, sizeof(out->stderr_text),
                 "tool '%s' not supported by bridge; agent handles browsing natively", tool_name);
        return false;
    }
    if (strcmp(tool_name, "google_search") != 0 &&
        strcmp(tool_name, "browse_url") != 0 &&
        strcmp(tool_name, "shell_command") != 0 &&
        g_app_callback) {
        if (g_app_callback(tool_name, argument_json, out->result_json, sizeof(out->result_json), out->stderr_text, sizeof(out->stderr_text))) {
            out->ok = true;
            out->exit_code = 0;
            return true;
        }
    }

    char mode[64] = {0};
    char payload[4096] = {0};
    build_command_args(tool_name, argument_json, mode, sizeof(mode), payload, sizeof(payload));

    /* Create both pipes before checking success — avoid leaking fds */
    int out_pipe[2];
    int err_pipe[2];
    out_pipe[0] = -1;
    out_pipe[1] = -1;
    err_pipe[0] = -1;
    err_pipe[1] = -1;

    if (pipe(out_pipe) != 0) {
        snprintf(out->stderr_text, sizeof(out->stderr_text), "pipe failed: %s", strerror(errno));
        return false;
    }
    if (pipe(err_pipe) != 0) {
        /* err_pipe failed — close out_pipe fds before returning */
        close(out_pipe[0]);
        close(out_pipe[1]);
        snprintf(out->stderr_text, sizeof(out->stderr_text), "pipe failed: %s", strerror(errno));
        return false;
    }

    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

    pid_t pid = -1;
    char *argv[6];
    int argc = 0;
    if (strcmp(tool_name, "shell_command") == 0 && shell_command_enabled) {
        argv[argc++] = "sh";
        argv[argc++] = "-c";
        argv[argc++] = payload;
    } else {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        out->ok = false;
        out->exit_code = 1;
        snprintf(out->stderr_text, sizeof(out->stderr_text),
                 "tool '%s' not supported by bridge", tool_name);
        return false;
    }
    argv[argc] = NULL;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, err_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, out_pipe[0]);
    posix_spawn_file_actions_addclose(&fa, err_pipe[0]);
#ifdef __APPLE__
    if (workspace && workspace[0]) {
        posix_spawn_file_actions_addchdir_np(&fa, workspace);
    }
#endif

    /* Build PATH with common locations */
    char env_path[512] = "PATH=/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/bin:/bin:/sbin";
    char env_buf[4096];
    char *envp[6] = {env_path, NULL, NULL, NULL, NULL, NULL};
    int envc = 1;
    if (agent_env && agent_env[0]) {
        snprintf(env_buf, sizeof(env_buf), "%s", agent_env);
        envp[envc++] = env_buf;
    }
    char env_browser[1152];
    if (browser_state_dir && browser_state_dir[0]) {
        snprintf(env_browser, sizeof(env_browser), "PLAYWRIGHT_BROWSERS_PATH=%s", browser_state_dir);
        envp[envc++] = env_browser;
    }
    char env_preferred_browser[128];
    if (preferred_browser && preferred_browser[0]) {
        snprintf(env_preferred_browser, sizeof(env_preferred_browser), "PREFERRED_BROWSER=%s", preferred_browser);
        envp[envc++] = env_preferred_browser;
    }

    const char *spawn_prog = (strcmp(tool_name, "shell_command") == 0 && shell_command_enabled)
                                ? "sh"
                                : tool_runner_resolve_node_path(NULL);
    int spawn_rc = posix_spawnp(&pid, spawn_prog, &fa, NULL, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (spawn_rc != 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        snprintf(out->stderr_text, sizeof(out->stderr_text), "spawn failed: %s", strerror(spawn_rc));
        return false;
    }

    char stdout_buf[65536] = {0};
    char stderr_buf[8192] = {0};
    size_t stdout_len = 0;
    size_t stderr_len = 0;
    bool exited = false;
    int status = 0;

    for (;;) {
        struct pollfd fds[2];
        fds[0].fd = out_pipe[0];
        fds[0].events = POLLIN;
        fds[1].fd = err_pipe[0];
        fds[1].events = POLLIN;

        int rc = poll(fds, 2, timeout_ms > 0 ? 50 : 0);
        if (rc == 0 && timeout_ms > 0) {
            timeout_ms -= 50;
            if (timeout_ms <= 0) {
                out->timed_out = true;
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                break;
            }
        }

        if (fds[0].revents & POLLIN) {
            char buf[1024];
            ssize_t n = read(out_pipe[0], buf, sizeof(buf));
            if (n > 0) {
                size_t space = sizeof(stdout_buf) - stdout_len - 1;
                if ((size_t)n >= space) {
                    out->truncated = true;
                    n = (ssize_t)(space > 0 ? space - 1 : 0);
                }
                if (n > 0) {
                    memcpy(stdout_buf + stdout_len, buf, (size_t)n);
                    stdout_len += (size_t)n;
                    stdout_buf[stdout_len] = '\0';
                }
            }
        }
        if (fds[1].revents & POLLIN) {
            char buf[512];
            ssize_t n = read(err_pipe[0], buf, sizeof(buf));
            if (n > 0) {
                size_t space = sizeof(stderr_buf) - stderr_len - 1;
                if ((size_t)n >= space) {
                    out->truncated = true;
                    n = (ssize_t)(space > 0 ? space - 1 : 0);
                }
                if (n > 0) {
                    memcpy(stderr_buf + stderr_len, buf, (size_t)n);
                    stderr_len += (size_t)n;
                    stderr_buf[stderr_len] = '\0';
                }
            }
        }

        pid_t wp = waitpid(pid, &status, WNOHANG);
        if (wp == pid) {
            exited = true;
            break;
        }
    }

    close(out_pipe[0]);
    close(err_pipe[0]);

    snprintf(out->result_json, sizeof(out->result_json), "%s", stdout_buf[0] ? stdout_buf : "{}");
    snprintf(out->stderr_text, sizeof(out->stderr_text), "%s", stderr_buf);
    tool_runner_redact_secrets(out->stderr_text);
    if (strstr(out->result_json, "Search unavailable") || strstr(out->result_json, "Playwright dependency is not installed.") ||
        strstr(out->result_json, "No axios dependency available")) {
        snprintf(out->stderr_text, sizeof(out->stderr_text), "%s",
                 strstr(out->result_json, "Search unavailable")
                     ? "missing Google Search credentials or axios dependency"
                     : (strstr(out->result_json, "No axios dependency available")
                         ? "missing Playwright and axios dependencies"
                         : "missing Playwright dependency"));
    }
    out->exit_code = exited && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    out->ok = !out->timed_out && !out->denied;
    if (out->result_json[0] == '\0') {
        snprintf(out->result_json, sizeof(out->result_json), "{}");
    }
    return out->ok;
}
