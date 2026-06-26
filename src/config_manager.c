#include "config_manager.h"
#include "json_utils.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/stat.h>
#include <glob.h>

BridgeConfig global_config;

/* --------------------------------------------------------------- */
/*  Config file buffer size — large enough for any reasonable file */
/* --------------------------------------------------------------- */
#define CONFIG_FILE_MAX_BYTES (512 * 1024) /* 512 KB */

/* --------------------------------------------------------------- */
/*  Internal helpers                                                */
/* --------------------------------------------------------------- */

static void parse_string_value(const void *obj, const char *key, char *dest, size_t max_len) {
    if (!obj) return;
    json_get_string(obj, key, dest, max_len);
}

static int parse_int_value(const void *obj, const char *key, int fallback) {
    if (!obj) return fallback;
    /* Use cJSON directly for field-existence check */
    cJSON *item = cJSON_GetObjectItem((const cJSON *)obj, key);
    if (!item || !cJSON_IsNumber(item)) return fallback;
    return (int)cJSON_GetNumberValue(item);
}

static bool parse_bool_value(const void *obj, const char *key, bool fallback) {
    if (!obj) return fallback;
    /* Use cJSON directly for field-existence check */
    cJSON *item = cJSON_GetObjectItem((const cJSON *)obj, key);
    if (!item || !cJSON_IsBool(item)) return fallback;
    return cJSON_IsTrue(item);
}

/* Helper: append an error message to a ConfigValidationResult */
static void config_add_error(ConfigValidationResult *vr, const char *fmt, ...) {
    if (!vr || vr->error_count >= CONFIG_MAX_ERRORS) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vr->errors[vr->error_count], CONFIG_ERROR_MAX_LEN, fmt, ap);
    va_end(ap);
    vr->error_count++;
}

/* --------------------------------------------------------------- */
/*  init_global_config                                              */
/* --------------------------------------------------------------- */

void init_global_config(int default_port) {
    memset(&global_config, 0, sizeof(global_config));
    global_config.server_port = default_port;
    global_config.max_buffer_chunk = 32768;
    snprintf(global_config.theme, sizeof(global_config.theme), "dracula");
    snprintf(global_config.codex_harness_id, sizeof(global_config.codex_harness_id), "codex.responses");
    snprintf(global_config.codex_model, sizeof(global_config.codex_model), "star-bridge-ds4");
    snprintf(global_config.codex_model_alias, sizeof(global_config.codex_model_alias), "star-bridge-ds4");
    snprintf(global_config.codex_model_display_name, sizeof(global_config.codex_model_display_name), "Star Bridge ds4");
    global_config.automatic_tree_expand = true;
    global_config.use_framed_protocol = false;
    global_config.auto_load_resume_session = true;
    global_config.auto_save_kv_cache = true;
    global_config.auto_load_project_session = true;
    snprintf(global_config.kv_cache_policy, sizeof(global_config.kv_cache_policy), "per_session");
    global_config.context_tokens = 150000;
    global_config.model_path[0] = '\0';
    global_config.kv_cache_dir[0] = '\0';
    global_config.browser_state_dir[0] = '\0';
    snprintf(global_config.preferred_browser, sizeof(global_config.preferred_browser), "brave");
    global_config.agent_env[0] = '\0';
    global_config.extra_native_args[0] = '\0';
    global_config.shell_command_enabled = false;
    global_config.google_search_timeout_ms = 5000;
    global_config.browse_url_timeout_ms = 5000;
    global_config.shell_command_timeout_ms = 5000;
    global_config.response_timeout_ms = 60000;
    global_config.max_history_entries = 20;
    global_config.max_request_size = 1048576;
    global_config.max_request_input_bytes = 8 * 1024 * 1024;
    global_config.auth_token[0] = '\0';
    snprintf(global_config.bind_host, sizeof(global_config.bind_host), "127.0.0.1");
    global_config.host_allowlist[0] = '\0';
    global_config.keep_alive_enabled = false;
    snprintf(global_config.native_transport, sizeof(global_config.native_transport), "auto");
    global_config.native_socket_path[0] = '\0';
    global_config.uds_connect_timeout_ms = 15000;
    global_config.hello_timeout_ms = 10000;
    global_config.model_load_timeout_ms = 120000;
    global_config.uds_reconnect_max_attempts = 3;
    global_config.uds_reconnect_backoff_ms = 2000;
    snprintf(global_config.uds_owner_mode, sizeof(global_config.uds_owner_mode), "connect_existing");
    global_config.trace = true;
    global_config.debug_log_enabled = true;
    snprintf(global_config.debug_log_path, sizeof(global_config.debug_log_path), ".codex-bridge-debug.log");
    global_config.bridge_workspace_monitor_enabled = false;
    global_config.hide_tool_transcripts = true;  /* default: hide 🛠️ RTK tool lines from Codex deltas/output for clean UI */
    snprintf(global_config.model_reasoning_effort, sizeof(global_config.model_reasoning_effort), "medium");
    global_config.max_output_buffer = 262144;  /* 256 KB default */
    global_config.max_output_chars = 0;  /* 0 = unlimited */
    global_config.max_turn_events = 65536;
    pthread_mutex_init(&global_config.lock, NULL);
    global_config.session_key[0] = '\0';
    global_config.session_id[0] = '\0';
}

/* --------------------------------------------------------------- */
/*  load_config_from_file — full JSON parse with large buffer      */
/* --------------------------------------------------------------- */

bool load_config_from_file(const char *json_path) {
    FILE *f = fopen(json_path, "r");
    if (!f) return false;

    /* Determine file size and allocate buffer dynamically */
    struct stat st;
    size_t buf_size = CONFIG_FILE_MAX_BYTES;
    if (fstat(fileno(f), &st) == 0) {
        if ((size_t)st.st_size > buf_size) {
            fprintf(stderr, "Config file too large (%lld bytes, max %zu)\n",
                    (long long)st.st_size, buf_size);
            fclose(f);
            return false;
        }
        if (st.st_size > 0) buf_size = (size_t)st.st_size + 1;
    }

    char *buffer = (char *)malloc(buf_size);
    if (!buffer) {
        fclose(f);
        return false;
    }

    size_t len = fread(buffer, 1, buf_size - 1, f);
    fclose(f);
    buffer[len] = '\0';

    void *obj = json_parse(buffer);
    if (!obj) {
        fprintf(stderr, "Config file JSON parse error: %s\n",
                cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "unknown");
        free(buffer);
        return false;
    }
    free(buffer);

    pthread_mutex_lock(&global_config.lock);
    global_config.server_port = parse_int_value(obj, "port", global_config.server_port);
    global_config.max_buffer_chunk = parse_int_value(obj, "max_buffer_chunk", global_config.max_buffer_chunk);
    global_config.context_tokens = parse_int_value(obj, "context_tokens", global_config.context_tokens);
    global_config.response_timeout_ms = parse_int_value(obj, "response_timeout_ms", global_config.response_timeout_ms);
    global_config.automatic_tree_expand = parse_bool_value(obj, "automatic_tree_expand", global_config.automatic_tree_expand);
    global_config.use_framed_protocol = parse_bool_value(obj, "use_framed_protocol", global_config.use_framed_protocol);
    global_config.auto_load_resume_session = parse_bool_value(obj, "auto_load_resume_session", global_config.auto_load_resume_session);
    global_config.auto_save_kv_cache = parse_bool_value(obj, "auto_save_kv_cache", global_config.auto_save_kv_cache);
    global_config.auto_load_project_session = parse_bool_value(obj, "auto_load_project_session", global_config.auto_load_project_session);
    parse_string_value(obj, "kv_cache_policy", global_config.kv_cache_policy, sizeof(global_config.kv_cache_policy));
    parse_string_value(obj, "theme", global_config.theme, sizeof(global_config.theme));
    parse_string_value(obj, "codex_harness_id", global_config.codex_harness_id, sizeof(global_config.codex_harness_id));
    parse_string_value(obj, "codex_model", global_config.codex_model, sizeof(global_config.codex_model));
    parse_string_value(obj, "codex_model_alias", global_config.codex_model_alias, sizeof(global_config.codex_model_alias));
    parse_string_value(obj, "codex_model_display_name", global_config.codex_model_display_name, sizeof(global_config.codex_model_display_name));
    parse_string_value(obj, "model_path", global_config.model_path, sizeof(global_config.model_path));
    parse_string_value(obj, "kv_cache_dir", global_config.kv_cache_dir, sizeof(global_config.kv_cache_dir));
    parse_string_value(obj, "browser_state_dir", global_config.browser_state_dir, sizeof(global_config.browser_state_dir));
    parse_string_value(obj, "preferred_browser", global_config.preferred_browser, sizeof(global_config.preferred_browser));
    parse_string_value(obj, "agent_env", global_config.agent_env, sizeof(global_config.agent_env));
    parse_string_value(obj, "extra_native_args", global_config.extra_native_args, sizeof(global_config.extra_native_args));
    global_config.shell_command_enabled = parse_bool_value(obj, "shell_command_enabled", global_config.shell_command_enabled);
    global_config.google_search_timeout_ms = parse_int_value(obj, "google_search_timeout_ms", global_config.google_search_timeout_ms);
    global_config.browse_url_timeout_ms = parse_int_value(obj, "browse_url_timeout_ms", global_config.browse_url_timeout_ms);
    global_config.shell_command_timeout_ms = parse_int_value(obj, "shell_command_timeout_ms", global_config.shell_command_timeout_ms);
    global_config.max_history_entries = parse_int_value(obj, "max_history_entries", global_config.max_history_entries);
    global_config.max_request_size = parse_int_value(obj, "max_request_size", global_config.max_request_size);
    global_config.max_request_input_bytes = parse_int_value(obj, "max_request_input_bytes", global_config.max_request_input_bytes);
    parse_string_value(obj, "auth_token", global_config.auth_token, sizeof(global_config.auth_token));
    parse_string_value(obj, "bind_host", global_config.bind_host, sizeof(global_config.bind_host));
    parse_string_value(obj, "host_allowlist", global_config.host_allowlist, sizeof(global_config.host_allowlist));
    global_config.keep_alive_enabled = parse_bool_value(obj, "keep_alive_enabled", global_config.keep_alive_enabled);
    parse_string_value(obj, "native_transport", global_config.native_transport, sizeof(global_config.native_transport));
    parse_string_value(obj, "native_socket_path", global_config.native_socket_path, sizeof(global_config.native_socket_path));
    global_config.uds_connect_timeout_ms = parse_int_value(obj, "uds_connect_timeout_ms", global_config.uds_connect_timeout_ms);
    global_config.hello_timeout_ms = parse_int_value(obj, "hello_timeout_ms", global_config.hello_timeout_ms);
    global_config.model_load_timeout_ms = parse_int_value(obj, "model_load_timeout_ms", global_config.model_load_timeout_ms);
    global_config.uds_reconnect_max_attempts = parse_int_value(obj, "uds_reconnect_max_attempts", global_config.uds_reconnect_max_attempts);
    global_config.uds_reconnect_backoff_ms = parse_int_value(obj, "uds_reconnect_backoff_ms", global_config.uds_reconnect_backoff_ms);
    parse_string_value(obj, "uds_owner_mode", global_config.uds_owner_mode, sizeof(global_config.uds_owner_mode));
    parse_string_value(obj, "session_id", global_config.session_id, sizeof(global_config.session_id));
    parse_string_value(obj, "session_index_path", global_config.session_index_path, sizeof(global_config.session_index_path));
    global_config.trace = parse_bool_value(obj, "trace", global_config.trace);
    global_config.debug_log_enabled = parse_bool_value(obj, "debug_log", global_config.debug_log_enabled);
    parse_string_value(obj, "debug_log_path", global_config.debug_log_path, sizeof(global_config.debug_log_path));
    global_config.bridge_workspace_monitor_enabled = parse_bool_value(obj, "bridge_workspace_monitor_enabled", global_config.bridge_workspace_monitor_enabled);
    global_config.hide_tool_transcripts = parse_bool_value(obj, "hide_tool_transcripts", global_config.hide_tool_transcripts);
    parse_string_value(obj, "model_reasoning_effort", global_config.model_reasoning_effort, sizeof(global_config.model_reasoning_effort));
    global_config.max_output_buffer = parse_int_value(obj, "max_output_buffer", global_config.max_output_buffer);
    global_config.max_output_chars = parse_int_value(obj, "max_output_chars", global_config.max_output_chars);
    global_config.max_turn_events = parse_int_value(obj, "max_turn_events", global_config.max_turn_events);
    json_free(obj);
    pthread_mutex_unlock(&global_config.lock);
    return true;
}

/* --------------------------------------------------------------- */
/*  config_validate_all — range and enum validation                 */
/* --------------------------------------------------------------- */

ConfigValidationResult config_validate_all(void) {
    ConfigValidationResult vr;
    memset(&vr, 0, sizeof(vr));

    /* ---- Port ---- */
    if (global_config.server_port < 1 || global_config.server_port > 65535) {
        config_add_error(&vr, "port %d out of range (1-65535)", global_config.server_port);
    }

    /* ---- Timeouts (must be > 0 or >= 0 depending on field) ---- */
    if (global_config.uds_connect_timeout_ms < 0) {
        config_add_error(&vr, "uds_connect_timeout_ms negative (%d)", global_config.uds_connect_timeout_ms);
    }
    if (global_config.hello_timeout_ms < 0) {
        config_add_error(&vr, "hello_timeout_ms negative (%d)", global_config.hello_timeout_ms);
    }
    if (global_config.model_load_timeout_ms < 0) {
        config_add_error(&vr, "model_load_timeout_ms negative (%d)", global_config.model_load_timeout_ms);
    }
    if (global_config.response_timeout_ms < 0) {
        config_add_error(&vr, "response_timeout_ms negative (%d)", global_config.response_timeout_ms);
    }
    if (global_config.google_search_timeout_ms < 0) {
        config_add_error(&vr, "google_search_timeout_ms negative (%d)", global_config.google_search_timeout_ms);
    }
    if (global_config.browse_url_timeout_ms < 0) {
        config_add_error(&vr, "browse_url_timeout_ms negative (%d)", global_config.browse_url_timeout_ms);
    }
    if (global_config.shell_command_timeout_ms < 0) {
        config_add_error(&vr, "shell_command_timeout_ms negative (%d)", global_config.shell_command_timeout_ms);
    }
    if (global_config.uds_reconnect_backoff_ms < 0) {
        config_add_error(&vr, "uds_reconnect_backoff_ms negative (%d)", global_config.uds_reconnect_backoff_ms);
    }

    /* ---- Sizes (must be > 0) ---- */
    if (global_config.max_buffer_chunk < 64) {
        config_add_error(&vr, "max_buffer_chunk %d too small (minimum 64)", global_config.max_buffer_chunk);
    }
    if (global_config.max_buffer_chunk > 10485760) {
        config_add_error(&vr, "max_buffer_chunk %d too large (maximum 10485760)", global_config.max_buffer_chunk);
    }
    if (global_config.max_request_size < 1024) {
        config_add_error(&vr, "max_request_size %d too small (minimum 1024)", global_config.max_request_size);
    }
    if (global_config.max_request_size > 104857600) {
        config_add_error(&vr, "max_request_size %d too large (maximum 104857600)", global_config.max_request_size);
    }
    if (global_config.max_request_input_bytes < 1024) {
        config_add_error(&vr, "max_request_input_bytes %d too small (minimum 1024)", global_config.max_request_input_bytes);
    }
    if (global_config.max_request_input_bytes > 104857600) {
        config_add_error(&vr, "max_request_input_bytes %d too large (maximum 104857600)", global_config.max_request_input_bytes);
    }
    if (global_config.context_tokens < 1024) {
        config_add_error(&vr, "context_tokens %d too small (minimum 1024)", global_config.context_tokens);
    }
    if (global_config.context_tokens > 10000000) {
        config_add_error(&vr, "context_tokens %d too large (maximum 10000000)", global_config.context_tokens);
    }

    /* ---- History entries ---- */
    if (global_config.max_history_entries < 0) {
        config_add_error(&vr, "max_history_entries negative (%d)", global_config.max_history_entries);
    }
    if (global_config.max_history_entries > 1000) {
        config_add_error(&vr, "max_history_entries %d too large (maximum 1000)", global_config.max_history_entries);
    }

    /* ---- Reconnect max attempts ---- */
    if (global_config.uds_reconnect_max_attempts < 0) {
        config_add_error(&vr, "uds_reconnect_max_attempts negative (%d)", global_config.uds_reconnect_max_attempts);
    }
    if (global_config.uds_reconnect_max_attempts > 100) {
        config_add_error(&vr, "uds_reconnect_max_attempts %d too large (maximum 100)", global_config.uds_reconnect_max_attempts);
    }

    /* ---- Enum: native_transport ---- */
    const char *valid_transports[] = {"auto", "uds", "stdio_framed", "stdio", NULL};
    {
        int found = 0;
        for (int i = 0; valid_transports[i]; i++) {
            if (strcmp(global_config.native_transport, valid_transports[i]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            config_add_error(&vr, "native_transport '%s' invalid (must be auto|uds|stdio_framed|stdio)",
                             global_config.native_transport);
        }
    }

    /* ---- Enum: uds_owner_mode ---- */
    const char *valid_owner_modes[] = {"connect_existing", "launch_and_connect", "bridge_listen", NULL};
    {
        int found = 0;
        for (int i = 0; valid_owner_modes[i]; i++) {
            if (strcmp(global_config.uds_owner_mode, valid_owner_modes[i]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            config_add_error(&vr, "uds_owner_mode '%s' invalid (must be connect_existing|launch_and_connect|bridge_listen)",
                             global_config.uds_owner_mode);
        }
    }

    /* ---- Enum: kv_cache_policy ---- */
    const char *valid_kv_policies[] = {"per_session", "global", "disabled", NULL};
    {
        int found = 0;
        for (int i = 0; valid_kv_policies[i]; i++) {
            if (strcmp(global_config.kv_cache_policy, valid_kv_policies[i]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            config_add_error(&vr, "kv_cache_policy '%s' invalid (must be per_session|global|disabled)",
                             global_config.kv_cache_policy);
        }
    }

    /* ---- model_reasoning_effort ---- */
    const char *valid_effort[] = {"low", "medium", "high", "extra high", NULL};
    {
        int found = 0;
        for (int i = 0; valid_effort[i]; i++) {
            if (strcmp(global_config.model_reasoning_effort, valid_effort[i]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            config_add_error(&vr, "model_reasoning_effort '%s' invalid (must be low|medium|high|extra high)",
                             global_config.model_reasoning_effort);
        }
    }

    /* ---- preferred_browser (warn on unknown, but allow any non-empty) ---- */
    /* No strict validation — browser can be any executable */

    return vr;
}

/* --------------------------------------------------------------- */
/*  Socket path resolution / validation (unchanged)                 */
/* --------------------------------------------------------------- */

void resolve_native_socket_path(const char *workspace) {
    pthread_mutex_lock(&global_config.lock);
    if (global_config.native_socket_path[0] == '\0') {
        pthread_mutex_unlock(&global_config.lock);
        return;
    }
    char resolved[1024];
    resolved[0] = '\0';
    if (global_config.native_socket_path[0] == '/') {
        /* Already absolute — use as-is but log it */
        snprintf(resolved, sizeof(resolved), "%s", global_config.native_socket_path);
    } else {
        /* Relative path; resolve against workspace directory */
        const char *base = (workspace && workspace[0]) ? workspace : ".";
        char base_abs[1024];
        char *rp = realpath(base, base_abs);
        if (rp) {
            snprintf(resolved, sizeof(resolved), "%s/%s", base_abs, global_config.native_socket_path);
        } else {
            /* realpath failed; use cwd + relative path */
            char cwd[1024];
            if (getcwd(cwd, sizeof(cwd))) {
                snprintf(resolved, sizeof(resolved), "%s/%s", cwd, global_config.native_socket_path);
            } else {
                snprintf(resolved, sizeof(resolved), "%s", global_config.native_socket_path);
            }
        }
    }
    /* Store resolved path back into config */
    snprintf(global_config.native_socket_path, sizeof(global_config.native_socket_path), "%s", resolved);
    fprintf(stdout, "Resolved native_socket_path to: %s\n", resolved);
    pthread_mutex_unlock(&global_config.lock);
}

bool cleanup_stale_socket(void) {
    pthread_mutex_lock(&global_config.lock);
    bool did_cleanup = false;
    if (global_config.native_socket_path[0] != '\0' &&
        strcmp(global_config.uds_owner_mode, "launch_and_connect") == 0) {
        fprintf(stdout, "Cleaning up stale socket: %s\n", global_config.native_socket_path);
        unlink(global_config.native_socket_path);
        did_cleanup = true;
    }
    /* Also remove any leftover test socket files (.sock) in the working directory */
    {
        const char *patterns[] = {"./*.sock", "./.stale_cleanup_test_*.sock",
                                   "./.fake_uds_*.sock", "./.fake_sock", "./.sock"};
        int np = sizeof(patterns) / sizeof(patterns[0]);
        for (int i = 0; i < np; i++) {
            glob_t g;
            int ret = glob(patterns[i], 0, NULL, &g);
            if (ret == 0) {
                for (size_t j = 0; j < g.gl_pathc; j++) {
                    fprintf(stdout, "Cleaning up stale socket: %s\n", g.gl_pathv[j]);
                    unlink(g.gl_pathv[j]);
                    did_cleanup = true;
                }
                globfree(&g);
            }
        }
    }
    pthread_mutex_unlock(&global_config.lock);
    return did_cleanup;
}

const char *validate_socket_parent_dir(void) {
    pthread_mutex_lock(&global_config.lock);
    const char *path = global_config.native_socket_path;
    const char *result = NULL;

    if (path[0] == '\0') {
        pthread_mutex_unlock(&global_config.lock);
        return NULL;
    }

    char dir[1024];
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) {
        snprintf(dir, sizeof(dir), "/");
    } else {
        size_t dir_len = (size_t)(slash - path);
        memcpy(dir, path, dir_len);
        dir[dir_len] = '\0';
    }

    struct stat st;
    if (stat(dir, &st) != 0) {
        result = "native_socket_parent_dir_missing";
        goto done;
    }

    if (!(st.st_mode & S_IXUSR)) {
        result = "native_socket_parent_dir_not_searchable";
        goto done;
    }
    if (!(st.st_mode & S_IRUSR)) {
        result = "native_socket_parent_dir_not_readable";
        goto done;
    }

    if ((st.st_mode & S_IWOTH) && !(st.st_mode & S_ISVTX)) {
        result = "native_socket_parent_dir_world_writable_no_sticky";
        goto done;
    }

    if (st.st_uid == 0) {
        result = "native_socket_parent_dir_root_owned";
        goto done;
    }

done:
    pthread_mutex_unlock(&global_config.lock);
    return result;
}

const char *validate_native_socket_path(void) {
    pthread_mutex_lock(&global_config.lock);
    const char *path = global_config.native_socket_path;
    const char *result = NULL;

    if (path[0] == '\0') {
        result = "native_socket_path_empty";
        goto done;
    }

    if (path[0] == '\0') {
        result = "native_socket_path_abstract";
        goto done;
    }

    if (strlen(path) != strnlen(path, sizeof(global_config.native_socket_path))) {
        result = "native_socket_path_embedded_null";
        goto done;
    }

    size_t len = strlen(path);
    if (len > 103) {
        result = "native_socket_path_too_long";
        goto done;
    }

    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            result = "native_socket_path_is_directory";
            goto done;
        }
        if (st.st_uid == 0) {
            result = "native_socket_path_root_owned";
            goto done;
        }
        if (st.st_mode & S_IWOTH) {
            result = "native_socket_path_world_writable";
            goto done;
        }
    }

done:
    pthread_mutex_unlock(&global_config.lock);
    return result;
}

void inject_config_ui_html(char *dest, size_t max_len) {
    pthread_mutex_lock(&global_config.lock);
    snprintf(dest, max_len,
             "<div class='config-strip'><span>port %d</span><span>theme %s</span><span>chunk %d</span><span>context %d</span><span>resume %s</span><span>framed %s</span><span>browser %s</span></div>",
             global_config.server_port,
             global_config.theme,
             global_config.max_buffer_chunk,
             global_config.context_tokens,
             global_config.auto_load_resume_session ? "on" : "off",
             global_config.use_framed_protocol ? "on" : "off",
             global_config.preferred_browser);
    pthread_mutex_unlock(&global_config.lock);
}

void update_config_from_query(const char *query_string) {
    if (!query_string) return;
    pthread_mutex_lock(&global_config.lock);
    const char *theme = strstr(query_string, "theme=");
    if (theme) {
        theme += 6;
        size_t i = 0;
        while (theme[i] && theme[i] != '&' && i + 1 < sizeof(global_config.theme)) {
            global_config.theme[i] = theme[i];
            i++;
        }
        global_config.theme[i] = '\0';
    }
    pthread_mutex_unlock(&global_config.lock);
}

/* --------------------------------------------------------------- */
/*  Session key helpers                                             */
/* --------------------------------------------------------------- */

void compute_session_key(const char *workspace_root, const char *session_id) {
    if (!workspace_root || !session_id) {
        global_config.session_key[0] = '\0';
        return;
    }
    unsigned long h = 0;
    for (const char *p = workspace_root; *p; p++) {
        h = h * 131 + (unsigned char)*p;
    }
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%lx_%s", h, session_id);
    if (n > 0 && (size_t)n < sizeof(buf)) {
        memcpy(global_config.session_key, buf, (size_t)n + 1);
    } else {
        global_config.session_key[0] = '\0';
    }
}

void session_key_for(const char *workspace_root, const char *session_id, char *dest, size_t max_len) {
    if (!dest || max_len == 0) return;
    dest[0] = '\0';
    if (!workspace_root || !session_id) return;

    unsigned long h = 0;
    for (const char *p = workspace_root; *p; p++) {
        h = h * 131 + (unsigned char)*p;
    }
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%lx_%s", h, session_id);
    if (n > 0 && (size_t)n < sizeof(buf)) {
        size_t copy_len = (size_t)n;
        if (copy_len >= max_len) copy_len = max_len - 1;
        memcpy(dest, buf, copy_len);
        dest[copy_len] = '\0';
    }
}

void set_session_id(const char *session_id) {
    if (!session_id) {
        global_config.session_id[0] = '\0';
        return;
    }
    size_t len = strlen(session_id);
    if (len >= sizeof(global_config.session_id))
        len = sizeof(global_config.session_id) - 1;
    memcpy(global_config.session_id, session_id, len);
    global_config.session_id[len] = '\0';
}

const char *get_session_key(void) {
    return global_config.session_key;
}

/* --------------------------------------------------------------- */
/*  Session index                                                   */
/* --------------------------------------------------------------- */

#define SESSION_INDEX_MAX_ENTRIES 64

typedef struct {
    char key[256];
    char state_id[128];
    char workspace_root[1024];
    char model_id[128];
    int context_budget;
    char updated_at[32];
} SessionIndexEntry;

static SessionIndexEntry s_index[SESSION_INDEX_MAX_ENTRIES];
static int s_index_count = 0;
static int s_index_dirty = 0;

static void session_index_set_default_path(void) {
    if (global_config.session_index_path[0] == '\0') {
        const char *home = getenv("HOME");
        if (home) {
            snprintf(global_config.session_index_path,
                     sizeof(global_config.session_index_path),
                     "%s/.bridge_session_index.json", home);
        } else {
            snprintf(global_config.session_index_path,
                     sizeof(global_config.session_index_path),
                     ".bridge_session_index.json");
        }
    }
}

bool load_session_index(void) {
    session_index_set_default_path();
    s_index_count = 0;
    s_index_dirty = 0;

    FILE *f = fopen(global_config.session_index_path, "r");
    if (!f) return false;

    char buf[32768];
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = '\0';

    void *obj = json_parse(buf);
    if (!obj) return false;

    const void *entries = json_get_array(obj, "entries");
    if (!entries) {
        json_free(obj);
        return false;
    }

    int count = json_array_count(entries);
    if (count > SESSION_INDEX_MAX_ENTRIES) count = SESSION_INDEX_MAX_ENTRIES;

    for (int i = 0; i < count; i++) {
        const void *entry = json_array_item(entries, i);
        if (!entry) continue;

        json_get_string(entry, "key", s_index[s_index_count].key, sizeof(s_index[s_index_count].key));
        json_get_string(entry, "state_id", s_index[s_index_count].state_id, sizeof(s_index[s_index_count].state_id));
        json_get_string(entry, "workspace_root", s_index[s_index_count].workspace_root, sizeof(s_index[s_index_count].workspace_root));
        json_get_string(entry, "model_id", s_index[s_index_count].model_id, sizeof(s_index[s_index_count].model_id));
        s_index[s_index_count].context_budget = json_get_int(entry, "context_budget");
        json_get_string(entry, "updated_at", s_index[s_index_count].updated_at, sizeof(s_index[s_index_count].updated_at));
        s_index_count++;
    }

    json_free(obj);
    return true;
}

bool save_session_index(void) {
    if (!s_index_dirty) return true;
    session_index_set_default_path();

    FILE *f = fopen(global_config.session_index_path, "w");
    if (!f) return false;

    fprintf(f, "{\"entries\":[\n");
    for (int i = 0; i < s_index_count; i++) {
        char esc_key[512] = {0};
        char esc_state[512] = {0};
        char esc_ws[512] = {0};
        char esc_model[512] = {0};
        char esc_time[512] = {0};
        json_escape(s_index[i].key, esc_key, sizeof(esc_key));
        json_escape(s_index[i].state_id, esc_state, sizeof(esc_state));
        json_escape(s_index[i].workspace_root, esc_ws, sizeof(esc_ws));
        json_escape(s_index[i].model_id, esc_model, sizeof(esc_model));
        json_escape(s_index[i].updated_at, esc_time, sizeof(esc_time));
        fprintf(f, "  {\"key\":\"%s\",\"state_id\":\"%s\",\"workspace_root\":\"%s\",\"model_id\":\"%s\",\"context_budget\":%d,\"updated_at\":\"%s\"}",
                esc_key, esc_state, esc_ws, esc_model,
                s_index[i].context_budget, esc_time);
        if (i < s_index_count - 1) fprintf(f, ",");
        fprintf(f, "\n");
    }
    fprintf(f, "]}\n");
    fclose(f);
    s_index_dirty = 0;
    return true;
}

const char *session_index_lookup(const char *session_key) {
    if (!session_key) return NULL;
    for (int i = 0; i < s_index_count; i++) {
        if (strcmp(s_index[i].key, session_key) == 0) {
            return s_index[i].state_id;
        }
    }
    return NULL;
}

const char *session_index_get_workspace(const char *session_key) {
    if (!session_key) return NULL;
    for (int i = 0; i < s_index_count; i++) {
        if (strcmp(s_index[i].key, session_key) == 0) {
            if (s_index[i].workspace_root[0])
                return s_index[i].workspace_root;
            return NULL;
        }
    }
    return NULL;
}

const char *session_index_get_model_id(const char *session_key) {
    if (!session_key) return NULL;
    for (int i = 0; i < s_index_count; i++) {
        if (strcmp(s_index[i].key, session_key) == 0) {
            if (s_index[i].model_id[0])
                return s_index[i].model_id;
            return NULL;
        }
    }
    return NULL;
}

bool session_index_update(const char *session_key, const char *state_id, const char *workspace_root, const char *model_id, int context_budget) {
    if (!session_key || !state_id) return false;

    for (int i = 0; i < s_index_count; i++) {
        if (strcmp(s_index[i].key, session_key) == 0) {
            size_t sl = strlen(state_id);
            if (sl >= sizeof(s_index[i].state_id)) sl = sizeof(s_index[i].state_id) - 1;
            memcpy(s_index[i].state_id, state_id, sl);
            s_index[i].state_id[sl] = '\0';
            if (workspace_root && workspace_root[0]) {
                size_t wl = strlen(workspace_root);
                if (wl >= sizeof(s_index[i].workspace_root)) wl = sizeof(s_index[i].workspace_root) - 1;
                memcpy(s_index[i].workspace_root, workspace_root, wl);
                s_index[i].workspace_root[wl] = '\0';
            }
            if (model_id && model_id[0]) {
                size_t ml = strlen(model_id);
                if (ml >= sizeof(s_index[i].model_id)) ml = sizeof(s_index[i].model_id) - 1;
                memcpy(s_index[i].model_id, model_id, ml);
                s_index[i].model_id[ml] = '\0';
            }
            if (context_budget > 0) {
                s_index[i].context_budget = context_budget;
            }
            snprintf(s_index[i].updated_at, sizeof(s_index[i].updated_at), "%ld", time(NULL));
            s_index_dirty = 1;
            return true;
        }
    }

    if (s_index_count >= SESSION_INDEX_MAX_ENTRIES) return false;
    size_t kl = strlen(session_key);
    if (kl >= sizeof(s_index[s_index_count].key)) kl = sizeof(s_index[s_index_count].key) - 1;
    memcpy(s_index[s_index_count].key, session_key, kl);
    s_index[s_index_count].key[kl] = '\0';

    size_t sl = strlen(state_id);
    if (sl >= sizeof(s_index[s_index_count].state_id)) sl = sizeof(s_index[s_index_count].state_id) - 1;
    memcpy(s_index[s_index_count].state_id, state_id, sl);
    s_index[s_index_count].state_id[sl] = '\0';

    if (workspace_root && workspace_root[0]) {
        size_t wl = strlen(workspace_root);
        if (wl >= sizeof(s_index[s_index_count].workspace_root)) wl = sizeof(s_index[s_index_count].workspace_root) - 1;
        memcpy(s_index[s_index_count].workspace_root, workspace_root, wl);
        s_index[s_index_count].workspace_root[wl] = '\0';
    } else {
        s_index[s_index_count].workspace_root[0] = '\0';
    }

    if (model_id && model_id[0]) {
        size_t ml = strlen(model_id);
        if (ml >= sizeof(s_index[s_index_count].model_id)) ml = sizeof(s_index[s_index_count].model_id) - 1;
        memcpy(s_index[s_index_count].model_id, model_id, ml);
        s_index[s_index_count].model_id[ml] = '\0';
    } else {
        s_index[s_index_count].model_id[0] = '\0';
    }

    s_index[s_index_count].context_budget = context_budget;
    snprintf(s_index[s_index_count].updated_at, sizeof(s_index[s_index_count].updated_at), "%ld", time(NULL));
    s_index_count++;
    s_index_dirty = 1;
    return true;
}

/* --------------------------------------------------------------- */
/*  generate_codex_config — write Star Bridge Codex config artifacts */
/* --------------------------------------------------------------- */

bool generate_codex_config(const char *output_dir, int server_port) {
    char path[4096];
    const char *dir = output_dir ? output_dir : ".";
    if (server_port <= 0) server_port = 8080;

    /* ensure output dir exists (mkdir -p) so fresh paths work */
    if (dir && dir[0] && strcmp(dir, ".") != 0) {
        char tmp[4096];
        snprintf(tmp, sizeof(tmp), "%s", dir);
        for (char *p = tmp + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(tmp, 0755); /* ignore err, later fopen will surface real fail */
                *p = '/';
            }
        }
        mkdir(tmp, 0755);
    }

    /* ---- custom_model_catalog.json ---- */
    snprintf(path, sizeof(path), "%s/custom_model_catalog.json", dir);

    cJSON *catalog = cJSON_CreateObject();
    if (!catalog) return false;

    /* models array */
    cJSON *models = cJSON_CreateArray();
    if (!models) { cJSON_Delete(catalog); return false; }

    /* Single model entry */
    cJSON *model = cJSON_CreateObject();
    if (!model) { cJSON_Delete(catalog); cJSON_Delete(models); return false; }

    cJSON_AddStringToObject(model, "id", "star-bridge-ds4");
    cJSON_AddStringToObject(model, "name", "Star Bridge ds4");
    cJSON_AddStringToObject(model, "provider", "custom");

    cJSON *caps = cJSON_CreateObject();
    cJSON_AddBoolToObject(caps, "vision", 0);
    cJSON_AddBoolToObject(caps, "function_calling", 1);
    cJSON_AddBoolToObject(caps, "streaming", 1);
    cJSON_AddNumberToObject(caps, "context", 150000);
    cJSON_AddItemToObject(model, "capabilities", caps);

    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "wire_api", "responses");
    cJSON_AddStringToObject(cfg, "model_alias", "star-bridge-ds4");
    cJSON_AddStringToObject(cfg, "model_display_name", "Star Bridge ds4");
    cJSON_AddNumberToObject(cfg, "context_tokens", 150000);
    cJSON_AddNumberToObject(cfg, "response_timeout_ms", 60000);
    cJSON_AddNumberToObject(cfg, "max_request_size", 131072);
    cJSON_AddItemToObject(model, "config", cfg);

    cJSON_AddItemToArray(models, model);
    cJSON_AddItemToObject(catalog, "models", models);
    cJSON_AddNumberToObject(catalog, "version", 1);

    /* Serialize with pretty-print */
    char *json_str = cJSON_Print(catalog);
    cJSON_Delete(catalog);

    if (!json_str) return false;

    FILE *f = fopen(path, "w");
    if (!f) {
        free(json_str);
        return false;
    }
    size_t len = strlen(json_str);
    fwrite(json_str, 1, len, f);
    fclose(f);
    free(json_str);

    /* ---- star_bridge_provider.json ---- */
    snprintf(path, sizeof(path), "%s/star_bridge_provider.json", dir);

    cJSON *provider = cJSON_CreateObject();
    if (!provider) return false;

    cJSON_AddStringToObject(provider, "provider_id", "star-bridge-local");
    cJSON_AddStringToObject(provider, "provider_name", "Star Bridge Local");
    char api_base[256];
    snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d/v1", server_port);
    cJSON_AddStringToObject(provider, "api_base", api_base);
    cJSON_AddStringToObject(provider, "wire_api", "responses");
    cJSON_AddStringToObject(provider, "model_alias", "star-bridge-ds4");
    cJSON_AddBoolToObject(provider, "streaming", 1);
    cJSON_AddNumberToObject(provider, "response_timeout_ms", 60000);
    cJSON_AddNumberToObject(provider, "context_tokens", 150000);
    cJSON_AddNumberToObject(provider, "max_request_size", 131072);

    json_str = cJSON_Print(provider);
    cJSON_Delete(provider);

    if (!json_str) return false;

    f = fopen(path, "w");
    if (!f) {
        free(json_str);
        return false;
    }
    len = strlen(json_str);
    fwrite(json_str, 1, len, f);
    fclose(f);
    free(json_str);

    return true;
}

/* --------------------------------------------------------------- */
/*  Managed Codex config install/disable workflow                   */
/* --------------------------------------------------------------- */

/* Markers for managed config block */
#define MANAGED_BEGIN "# --- BEGIN star_bridge-managed ---\n"
#define MANAGED_END   "# --- END star_bridge-managed ---\n"
#define LEGACY_MANAGED_BEGIN "# --- BEGIN bridge-managed ---\n"
#define LEGACY_MANAGED_END   "# --- END bridge-managed ---\n"

/* Build the managed config block with the given bridge port.
 * Also injects model_catalog_json so that the local model appears in the
 * Codex Desktop App's model/intelligence picker (bypassing cloud allowlist).
 */
static char *build_managed_block(int port) {
    const char *home = getenv("HOME");
    char catalog_path[1024];
    if (home && home[0]) {
        snprintf(catalog_path, sizeof(catalog_path),
                 "%s/.codex/custom_catalog.json", home);
    } else {
        snprintf(catalog_path, sizeof(catalog_path),
                 "~/.codex/custom_catalog.json");
    }

    char buf[3072];
    snprintf(buf, sizeof(buf),
        "# --- BEGIN star_bridge-managed ---\n"
        "# This section is managed by star_bridge. Do not edit manually.\n"
        "#\n"
        "model = \"star-bridge-ds4\"\n"
        "model_provider = \"star-bridge-local\"\n"
        "\n"
        "# Force the Desktop UI picker to load our local model catalog\n"
        "model_catalog_json = \"%s\"\n"
        "\n"
        "[model_providers.star-bridge-local]\n"
        "name = \"Star Bridge Local\"\n"
        "base_url = \"http://127.0.0.1:%d/v1\"\n"
        "wire_api = \"responses\"\n"
        "streaming = true\n"
        "response_timeout_ms = 60000\n"
        "context_tokens = 150000\n"
        "max_request_size = 131072\n"
        "requires_openai_auth = false\n"
        "supports_websockets = false\n"
        "#\n"
        "# Reasoning effort control: Codex Desktop may update this via UI.\n"
        "# Valid values: low, medium, high, extra high\n"
        "model_reasoning_effort = \"%s\"\n"
        "# --- END star_bridge-managed ---\n",
        catalog_path,
        port > 0 ? port : 8080,
        global_config.model_reasoning_effort[0] ? global_config.model_reasoning_effort : "medium");
    return strdup(buf);
}

/* Forward declarations for static helper functions defined later */
static const char *get_codex_config_path(void);
static const char *get_backup_path(void);

/* --------------------------------------------------------------- */
/*  Read model_reasoning_effort from ~/.codex/config.toml          */
/*  Codex Desktop may write this setting when user changes it.     */
/*  If found and valid, update global_config.model_reasoning_effort.
 *  Returns true if a valid value was read.                        */
/* --------------------------------------------------------------- */
bool read_codex_toml_reasoning_effort(void) {
    const char *path = get_codex_config_path();
    if (!path) return false;

    FILE *f = fopen(path, "r");
    if (!f) return false;

    /* Simple line-by-line TOML parser: find model_reasoning_effort = "..." */
    char line[256];
    int found = 0;
    char value[32] = {0};
    while (fgets(line, sizeof(line), f)) {
        /* Skip comments and empty lines */
        char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        if (*trimmed == '#' || *trimmed == '\n' || *trimmed == '\r' || *trimmed == '\0') continue;

        /* Look for model_reasoning_effort = "..." */
        if (strstr(trimmed, "model_reasoning_effort") == trimmed) {
            char *eq = strchr(trimmed, '=');
            if (eq) {
                eq++;
                while (*eq == ' ' || *eq == '\t') eq++;
                if (*eq == '"') {
                    eq++;
                    char *end = strchr(eq, '"');
                    if (end) {
                        size_t len = (size_t)(end - eq);
                        if (len > sizeof(value) - 1) len = sizeof(value) - 1;
                        memcpy(value, eq, len);
                        value[len] = '\0';
                        found = 1;
                    }
                }
            }
            break;
        }
    }
    fclose(f);

    if (found && value[0]) {
        /* Validate: must be low/medium/high/extra high */
        const char *valid[] = {"low", "medium", "high", "extra high", NULL};
        int valid_found = 0;
        for (int i = 0; valid[i]; i++) {
            if (strcmp(value, valid[i]) == 0) {
                valid_found = 1;
                break;
            }
        }
        if (valid_found) {
            snprintf(global_config.model_reasoning_effort, sizeof(global_config.model_reasoning_effort), "%s", value);
            fprintf(stdout, "Read model_reasoning_effort from %s: %s\n", path, value);
            return true;
        }
    }
    return false;
}

/* Write the custom model catalog JSON in the format required by Codex Desktop
 * to populate the model/intelligence picker with our local ds4 option.
 * This is the key piece from the "custom local model catalog" methodology
 * to make the option appear in the UI dropdown instead of being hidden
 * behind the cloud allowlist.
 */
static bool write_star_bridge_catalog(const char *path) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return false;

    cJSON *models = cJSON_CreateArray();
    if (!models) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *model = cJSON_CreateObject();
    if (!model) {
        cJSON_Delete(root);
        cJSON_Delete(models);
        return false;
    }

    /* Schema follows codex-shim's custom model catalog so Codex Desktop's
     * picker renders the entry. slug + provider give identity; hidden:false
     * bypasses the Statsig allowlist; display_name/max_context_limit/
     * input_modalities/supports_parallel_tool_calls are the fields Codex reads. */
    cJSON_AddStringToObject(model, "slug", "star-bridge-ds4");
    cJSON_AddStringToObject(model, "display_name", "Star Bridge ds4");
    cJSON_AddStringToObject(model, "provider", "star-bridge-local");
    cJSON_AddNumberToObject(model, "max_context_limit", 150000);

    cJSON *modalities = cJSON_CreateArray();
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToObject(model, "input_modalities", modalities);

    cJSON_AddBoolToObject(model, "supports_parallel_tool_calls", true);
    cJSON_AddBoolToObject(model, "hidden", false);

    cJSON_AddItemToArray(models, model);
    cJSON_AddItemToObject(root, "models", models);

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str) return false;

    FILE *f = fopen(path, "w");
    if (!f) {
        free(json_str);
        return false;
    }
    size_t len = strlen(json_str);
    fwrite(json_str, 1, len, f);
    fclose(f);
    free(json_str);

    return true;
}

/* Returns the path to ~/.codex/config.toml (static buffer). */
static const char *get_codex_config_path(void) {
    static char path[1024];
    const char *home = getenv("HOME");
    if (!home) return "~/.codex/config.toml";
    snprintf(path, sizeof(path), "%s/.codex/config.toml", home);
    return path;
}

/* Returns the backup path for ~/.codex/config.toml (static buffer). */
static const char *get_backup_path(void) {
    static char backup[1024];
    const char *home = getenv("HOME");
    if (!home) return "~/.codex/config.toml.bridge-backup";
    snprintf(backup, sizeof(backup), "%s/.codex/config.toml.bridge-backup", home);
    return backup;
}

/* Read entire file into a malloc'd buffer. Caller must free. */
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size == 0) {
        fclose(f);
        return NULL;
    }

    size_t sz = (size_t)st.st_size + 1;
    char *buf = (char *)malloc(sz);
    if (!buf) { fclose(f); return NULL; }

    size_t len = fread(buf, 1, sz - 1, f);
    fclose(f);
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

/* Write buf to path. Returns true on success. */
static bool write_file(const char *path, const char *buf) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    size_t len = strlen(buf);
    fwrite(buf, 1, len, f);
    fclose(f);
    return true;
}

/* Check if config.toml contains a managed block.
 * Returns pointer to begin marker in buf, or NULL. */
static char *find_managed_block(char *buf) {
    if (!buf) return NULL;
    char *begin = strstr(buf, MANAGED_BEGIN);
    if (!begin) begin = strstr(buf, LEGACY_MANAGED_BEGIN);
    if (!begin) return NULL;
    char *end = strstr(begin, MANAGED_END);
    if (!end) end = strstr(begin, LEGACY_MANAGED_END);
    if (!end || end < begin) return NULL;
    return begin;
}

/* Remove ALL managed sections (from any BEGIN to its END) from the buffer.
 * This ensures re-installs never leave duplicate blocks.
 * Returns a newly allocated string with managed sections stripped (caller must free).
 * User content (including any non-marked settings) is preserved.
 */
static char *remove_all_managed_sections(const char *buf) {
    if (!buf) return NULL;
    size_t buf_len = strlen(buf);
    char *result = (char *)malloc(buf_len + 1);
    if (!result) return NULL;
    result[0] = '\0';
    const char *pos = buf;
    while (pos && *pos) {
        const char *begin = strstr(pos, MANAGED_BEGIN);
        const char *legacy_begin = strstr(pos, LEGACY_MANAGED_BEGIN);
        if (!begin || (legacy_begin && legacy_begin < begin)) begin = legacy_begin;
        if (!begin) {
            strcat(result, pos);
            break;
        }
        // copy content before this begin
        size_t prefix_len = (size_t)(begin - pos);
        strncat(result, pos, prefix_len);
        // find end for this block
        const char *end = strstr(begin, MANAGED_END);
        const char *legacy_end = strstr(begin, LEGACY_MANAGED_END);
        if (!end || (legacy_end && legacy_end < end)) end = legacy_end;
        if (!end) {
            // malformed, include from begin onward?
            strcat(result, begin);
            break;
        }
        if (strncmp(end, MANAGED_END, strlen(MANAGED_END)) == 0) {
            end += strlen(MANAGED_END);
        } else {
            end += strlen(LEGACY_MANAGED_END);
        }
        // skip following whitespace/newlines to keep file tidy
        while (*end == '\n' || *end == '\r') ++end;
        pos = end;
    }
    return result;
}

/* Extract displaced top-level keys that are also in the managed block.
 * Returns a newly allocated string with those keys (TOML format), or NULL if none.
 * Caller must free.
 *
 * We look for [model "star-bridge-ds4"] or [provider "star-bridge-local"]
 * sections that exist BEFORE the managed_begin position, and copy them out.
 */
static char *extract_displaced_keys(const char *buf, const char *managed_begin) {
    if (!buf || !managed_begin) return NULL;

    /* Collect lines before managed_begin that define these sections */
    size_t prefix_len = (size_t)(managed_begin - buf);
    char *prefix = strndup(buf, prefix_len);
    if (!prefix) return NULL;

    char *result = (char *)malloc(1);
    if (!result) { free(prefix); return NULL; }
    result[0] = '\0';

    /* Look for [model "star-bridge-ds4"] (or legacy ds4-unix-bridge) */
    const char *model_header = "[model \"star-bridge-ds4\"]";
    char *model_sec = strstr(prefix, model_header);
    if (!model_sec) {
        model_header = "[model \"ds4-unix-bridge\"]";
        model_sec = strstr(prefix, model_header);
    }
    if (model_sec) {
        char *line_start = model_sec;
        while (line_start > prefix && line_start[-1] != '\n') line_start--;
        char *line_end = model_sec + strlen(model_header);
        char *next_sec = strstr(line_end, "\n[");
        if (!next_sec) next_sec = prefix + strlen(prefix);
        else next_sec++;

        size_t sec_len = (size_t)(next_sec - line_start);
        char *sec = strndup(line_start, sec_len);
        if (sec) {
            size_t rlen = strlen(result) + strlen(sec) + 1;
            char *tmp = (char *)realloc(result, rlen);
            if (tmp) {
                strcat(tmp, sec);
                result = tmp;
            }
            free(sec);
        }
    }

    /* Look for [provider "star-bridge-local"] (or legacy dwarfstar-bridge) */
    const char *provider_header = "[provider \"star-bridge-local\"]";
    char *prov_sec = strstr(prefix, provider_header);
    if (!prov_sec) {
        provider_header = "[provider \"dwarfstar-bridge\"]";
        prov_sec = strstr(prefix, provider_header);
    }
    if (prov_sec) {
        char *line_start = prov_sec;
        while (line_start > prefix && line_start[-1] != '\n') line_start--;
        char *line_end = prov_sec + strlen(provider_header);
        char *next_sec = strstr(line_end, "\n[");
        if (!next_sec) next_sec = prefix + strlen(prefix);
        else next_sec++;

        size_t sec_len = (size_t)(next_sec - line_start);
        char *sec = strndup(line_start, sec_len);
        if (sec) {
            size_t rlen = strlen(result) + strlen(sec) + 1;
            char *tmp = (char *)realloc(result, rlen);
            if (tmp) {
                strcat(tmp, sec);
                result = tmp;
            }
            free(sec);
        }
    }

    free(prefix);

    if (strlen(result) == 0) {
        free(result);
        return NULL;
    }
    return result;
}

/* Install managed config block into ~/.codex/config.toml.
 * - Backs up existing file to config.toml.bridge-backup
 * - If file already has a managed block, replaces it in-place
 * - If file doesn't have a managed block, appends it
 * - Preserves displaced keys (user's own model/provider sections) in backup
 *
 * dry_run: if true, only print what would change, don't modify anything.
 * Returns true if successful or dry_run would succeed.
 */
bool install_managed_config(bool dry_run, int server_port) {
    if (server_port <= 0) server_port = 8080;
    const char *config_path = get_codex_config_path();
    const char *backup_path = get_backup_path();

    /* Read existing config.toml if it exists */
    char *existing = NULL;
    size_t existing_len = 0;
    {
        FILE *f = fopen(config_path, "r");
        if (f) {
            fclose(f);
            existing = read_file(config_path, &existing_len);
        }
    }

    /* Determine what we'll do */
    char *managed_begin = NULL;
    char *displaced = NULL;
    if (existing) {
        managed_begin = find_managed_block(existing);
        /* Extract displaced keys: keys that are also in the managed block.
         * If a managed block exists, extract keys from before it (user's original).
         * If no managed block exists, extract keys from the whole file (user's sections
         * that will be displaced by the managed block).
         */
        if (managed_begin) {
            displaced = extract_displaced_keys(existing, managed_begin);
        } else {
            /* No managed block yet. Extract any sections that conflict with managed block. */
            /* Use a sentinel past the end of the buffer to extract from entire file */
            displaced = extract_displaced_keys(existing, existing + strlen(existing) + 1);
        }
    }

    /* For non-dry runs, strip ALL previous managed sections so re-installs
     * produce a clean file with no duplicates. We do this after extracting
     * displaced info (which is based on the original for the backup).
     */
    if (!dry_run && existing) {
        char *cleaned = remove_all_managed_sections(existing);
        if (cleaned) {
            free(existing);
            existing = cleaned;
            existing_len = strlen(existing);
            managed_begin = NULL;  /* no longer valid after strip */
        }
    }

    if (dry_run) {
        fprintf(stdout, "Config: %s\n", config_path);
        fprintf(stdout, "Backup: %s\n", backup_path);
        if (existing) {
            if (managed_begin) {
                fprintf(stdout, "Action: update existing managed block in place\n");
                if (displaced) {
                    fprintf(stdout, "Displaced keys (preserved in backup):\n%s", displaced);
                }
            } else {
                fprintf(stdout, "Action: append managed block at end\n");
            }
        } else {
            fprintf(stdout, "Action: create new file with managed block\n");
        }
        if (existing) free(existing);
        if (displaced) free(displaced);
        return true;
    }

    /* Write the custom catalog JSON so the local model appears in the Codex picker.
     * We compute the same path that was embedded in the managed block.
     */
    const char *home = getenv("HOME");
    char catalog_path[1024];
    if (home && home[0]) {
        snprintf(catalog_path, sizeof(catalog_path),
                 "%s/.codex/custom_catalog.json", home);
    } else {
        snprintf(catalog_path, sizeof(catalog_path),
                 "~/.codex/custom_catalog.json");
    }
    if (!write_star_bridge_catalog(catalog_path)) {
        fprintf(stderr, "Warning: failed to write custom model catalog to %s (the picker may not show the local model until you create it manually)\n", catalog_path);
    } else {
        fprintf(stdout, "Wrote custom model catalog for picker: %s\n", catalog_path);
    }

    /* Backup existing file */
    if (existing) {
        if (!write_file(backup_path, existing)) {
            fprintf(stderr, "Error: failed to create backup at '%s'\n", backup_path);
            if (existing) free(existing);
            if (displaced) free(displaced);
            return false;
        }
        fprintf(stdout, "Backed up existing config to: %s\n", backup_path);
    }

    /* Build new content */
    char *new_content = NULL;

    char *managed_block = build_managed_block(server_port);
    if (!managed_block) {
        if (existing) free(existing);
        if (displaced) free(displaced);
        return false;
    }

    /* Always append the fresh block to the (cleaned) existing content.
     * Because we already stripped all previous managed sections above,
     * this guarantees exactly one managed block with no duplicates.
     */
    if (existing) {
        /* Append fresh managed block at end */
        size_t new_len = existing_len + strlen(managed_block) + 2;
        new_content = (char *)malloc(new_len);
        if (!new_content) { free(existing); if (displaced) free(displaced); free(managed_block); return false; }
        memcpy(new_content, existing, existing_len);
        /* Ensure a newline before appending */
        size_t off = existing_len;
        if (existing_len > 0 && existing[existing_len - 1] != '\n') {
            new_content[off++] = '\n';
        }
        memcpy(new_content + off, managed_block, strlen(managed_block));
        off += strlen(managed_block);
        new_content[off] = '\0';
        fprintf(stdout, "Replaced all previous managed sections with a single fresh block in: %s\n", config_path);
    } else {
        /* Create new file with managed block */
        new_content = strdup(managed_block);
        if (!new_content) { if (displaced) free(displaced); free(managed_block); return false; }
        fprintf(stdout, "Created new config at: %s\n", config_path);
    }

    free(managed_block);

    /* Write new content */
    bool ok = write_file(config_path, new_content);
    if (!ok) {
        fprintf(stderr, "Error: failed to write '%s'\n", config_path);
        if (new_content) free(new_content);
        if (existing) free(existing);
        if (displaced) free(displaced);
        return false;
    }

    /* Log displaced keys info */
    if (displaced) {
        fprintf(stdout, "Preserved displaced keys in backup:\n%s", displaced);
    }

    if (new_content) free(new_content);
    if (existing) free(existing);
    if (displaced) free(displaced);
    return true;
}

/* Disable (remove) managed config block from ~/.codex/config.toml.
 * Removes the managed block and restores any displaced top-level keys
 * that were preserved during install.
 *
 * dry_run: if true, only print what would change, don't modify anything.
 * Returns true if successful, false if no managed block found or error.
 */
bool disable_managed_config(bool dry_run) {
    const char *config_path = get_codex_config_path();
    const char *backup_path = get_backup_path();

    char *existing = read_file(config_path, NULL);
    if (!existing) {
        fprintf(stdout, "No config file at '%s'\n", config_path);
        return false;
    }

    /* Check if there is any managed block */
    if (!strstr(existing, MANAGED_BEGIN)) {
        fprintf(stdout, "No managed config block found in '%s'\n", config_path);
        free(existing);
        return false;
    }

    if (dry_run) {
        fprintf(stdout, "Config: %s\n", config_path);
        fprintf(stdout, "Action: remove all managed blocks and restore displaced keys from backup\n");
        free(existing);
        return true;
    }

    /* Backup the current (pre-disable) file */
    if (!write_file(backup_path, existing)) {
        fprintf(stderr, "Error: failed to create backup at '%s'\n", backup_path);
        free(existing);
        return false;
    }
    fprintf(stdout, "Backed up existing config to: %s\n", backup_path);

    /* Strip all managed sections */
    char *cleaned = remove_all_managed_sections(existing);
    if (!cleaned) {
        fprintf(stderr, "Error: failed to strip managed sections\n");
        free(existing);
        return false;
    }

    /* Extract displaced keys from the backup (the user's original conflicting settings) */
    char *backup_buf = read_file(backup_path, NULL);
    char *displaced = NULL;
    if (backup_buf) {
        char *backup_managed = strstr(backup_buf, MANAGED_BEGIN);
        if (backup_managed) {
            displaced = extract_displaced_keys(backup_buf, backup_managed);
        } else {
            displaced = extract_displaced_keys(backup_buf, backup_buf + strlen(backup_buf));
        }
        free(backup_buf);
    }

    /* Build final content: cleaned (user stuff without our blocks) + restored displaced keys */
    size_t cleaned_len = strlen(cleaned);
    size_t displaced_len = displaced ? strlen(displaced) : 0;
    size_t new_len = cleaned_len + displaced_len + 2;
    char *new_content = (char *)malloc(new_len);
    if (!new_content) {
        free(existing);
        free(cleaned);
        if (displaced) free(displaced);
        return false;
    }

    memcpy(new_content, cleaned, cleaned_len);
    size_t offset = cleaned_len;
    if (displaced_len > 0) {
        if (cleaned_len > 0 && cleaned[cleaned_len-1] != '\n') {
            new_content[offset++] = '\n';
        }
        memcpy(new_content + offset, displaced, displaced_len);
        offset += displaced_len;
    }
    new_content[offset] = '\0';

    /* Write or delete if empty */
    if (strlen(new_content) == 0 || (cleaned_len == 0 && displaced_len == 0)) {
        unlink(config_path);
        fprintf(stdout, "Removed managed config block (config file was only managed block, deleted): %s\n", config_path);
    } else {
        bool ok = write_file(config_path, new_content);
        if (!ok) {
            fprintf(stderr, "Error: failed to write '%s'\n", config_path);
            free(new_content);
            free(existing);
            free(cleaned);
            if (displaced) free(displaced);
            return false;
        }
        fprintf(stdout, "Removed managed config block from: %s\n", config_path);
    }

    if (displaced) {
        fprintf(stdout, "Restored displaced keys:\n%s", displaced);
    }

    free(new_content);
    free(existing);
    free(cleaned);
    if (displaced) free(displaced);
    return true;
}

/* Check status of managed config.
 * Returns true if managed block is present, false otherwise.
 * Prints status to stdout.
 */
bool managed_config_status(void) {
    const char *config_path = get_codex_config_path();
    const char *backup_path = get_backup_path();

    char *existing = read_file(config_path, NULL);
    if (!existing) {
        /* Check if file exists but is empty */
        FILE *f = fopen(config_path, "r");
        if (f) {
            fclose(f);
            fprintf(stdout, "Status: config file is empty at '%s'\n", config_path);
        } else {
            fprintf(stdout, "Status: no config file at '%s'\n", config_path);
        }
        FILE *bf = fopen(backup_path, "r");
        if (bf) {
            fclose(bf);
            fprintf(stdout, "Backup exists at: %s\n", backup_path);
        } else {
            fprintf(stdout, "No backup at: %s\n", backup_path);
        }
        return false;
    }

    char *managed_begin = find_managed_block(existing);
    if (managed_begin) {
        fprintf(stdout, "Status: managed config is INSTALLED at '%s'\n", config_path);
    } else {
        fprintf(stdout, "Status: managed config is NOT installed at '%s'\n", config_path);
    }

    /* Check backup */
    FILE *bf = fopen(backup_path, "r");
    if (bf) {
        fclose(bf);
        fprintf(stdout, "Backup exists at: %s\n", backup_path);
    } else {
        fprintf(stdout, "No backup at: %s\n", backup_path);
    }

    free(existing);
    return (managed_begin != NULL);
}
