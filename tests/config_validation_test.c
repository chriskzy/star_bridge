#include "config_manager.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int expect(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        return 1;
    }
    return 0;
}

/* Write a config JSON to a temp file and return the path.
 * Caller must free() the returned path. */
static char *write_temp_config(const char *json) {
    char tmpl[] = "/tmp/config_val_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); return NULL; }
    fprintf(f, "%s", json);
    fclose(f);
    return strdup(tmpl);
}

int main(void) {
    int failed = 0;
    char *path = NULL;

    /* ---- Test 1: valid config (no errors) ---- */
    {
        init_global_config(8080);
        path = write_temp_config(
            "{"
            "\"port\":8080,"
            "\"max_buffer_chunk\":65536,"
            "\"context_tokens\":150000,"
            "\"response_timeout_ms\":30000,"
            "\"uds_connect_timeout_ms\":15000,"
            "\"hello_timeout_ms\":10000,"
            "\"model_load_timeout_ms\":120000,"
            "\"uds_reconnect_max_attempts\":3,"
            "\"uds_reconnect_backoff_ms\":2000,"
            "\"google_search_timeout_ms\":5000,"
            "\"browse_url_timeout_ms\":5000,"
            "\"shell_command_timeout_ms\":5000,"
            "\"max_history_entries\":20,"
            "\"max_request_size\":1048576,"
            "\"native_transport\":\"auto\","
            "\"uds_owner_mode\":\"connect_existing\","
            "\"kv_cache_policy\":\"per_session\""
            "}");
        failed |= expect(path != NULL, "temp file created");
        if (path) {
            failed |= expect(load_config_from_file(path), "load valid config");
            ConfigValidationResult vr = config_validate_all();
            failed |= expect(vr.error_count == 0, "valid config: zero errors");
            if (vr.error_count > 0) {
                for (int i = 0; i < vr.error_count && i < 3; i++)
                    fprintf(stderr, "  Unexpected error: %s\n", vr.errors[i]);
            }
            unlink(path);
            free(path);
            path = NULL;
        }
    }

    /* ---- Test 2: port out of range (0) ---- */
    {
        init_global_config(8080);
        path = write_temp_config("{\"port\":0}");
        if (path) {
            load_config_from_file(path);
            ConfigValidationResult vr = config_validate_all();
            failed |= expect(vr.error_count > 0, "port 0 triggers error");
            int found = 0;
            for (int i = 0; i < vr.error_count; i++)
                if (strstr(vr.errors[i], "port") && strstr(vr.errors[i], "range")) found = 1;
            failed |= expect(found, "port 0 error mentions 'port' and 'range'");
            unlink(path);
            free(path);
            path = NULL;
        }
    }

    /* ---- Test 3: port out of range (65536) ---- */
    {
        init_global_config(8080);
        path = write_temp_config("{\"port\":65536}");
        if (path) {
            load_config_from_file(path);
            ConfigValidationResult vr = config_validate_all();
            failed |= expect(vr.error_count > 0, "port 65536 triggers error");
            int found = 0;
            for (int i = 0; i < vr.error_count; i++)
                if (strstr(vr.errors[i], "port") && strstr(vr.errors[i], "range")) found = 1;
            failed |= expect(found, "port 65536 error mentions 'port' and 'range'");
            unlink(path);
            free(path);
            path = NULL;
        }
    }

    /* ---- Test 4: negative timeout ---- */
    {
        init_global_config(8080);
        path = write_temp_config("{\"uds_connect_timeout_ms\":-1}");
        if (path) {
            load_config_from_file(path);
            ConfigValidationResult vr = config_validate_all();
            failed |= expect(vr.error_count > 0, "negative timeout triggers error");
            int found = 0;
            for (int i = 0; i < vr.error_count; i++)
                if (strstr(vr.errors[i], "negative")) found = 1;
            failed |= expect(found, "negative timeout error mentions 'negative'");
            unlink(path);
            free(path);
            path = NULL;
        }
    }

    /* ---- Test 5: max_buffer_chunk too small ---- */
    {
        init_global_config(8080);
        path = write_temp_config("{\"max_buffer_chunk\":1}");
        if (path) {
            load_config_from_file(path);
            ConfigValidationResult vr = config_validate_all();
            failed |= expect(vr.error_count > 0, "tiny max_buffer_chunk triggers error");
            int found = 0;
            for (int i = 0; i < vr.error_count; i++)
                if (strstr(vr.errors[i], "max_buffer_chunk") && strstr(vr.errors[i], "too small")) found = 1;
            failed |= expect(found, "max_buffer_chunk error mentions 'too small'");
            unlink(path);
            free(path);
            path = NULL;
        }
    }

    /* ---- Test 6: invalid native_transport enum ---- */
    {
        init_global_config(8080);
        path = write_temp_config("{\"native_transport\":\"invalid_mode\"}");
        if (path) {
            load_config_from_file(path);
            ConfigValidationResult vr = config_validate_all();
            failed |= expect(vr.error_count > 0, "invalid transport triggers error");
            int found = 0;
            for (int i = 0; i < vr.error_count; i++)
                if (strstr(vr.errors[i], "native_transport")) found = 1;
            failed |= expect(found, "invalid transport error mentions 'native_transport'");
            unlink(path);
            free(path);
            path = NULL;
        }
    }

    /* ---- Test 7: invalid uds_owner_mode enum ---- */
    {
        init_global_config(8080);
        path = write_temp_config("{\"uds_owner_mode\":\"bad_mode\"}");
        if (path) {
            load_config_from_file(path);
            ConfigValidationResult vr = config_validate_all();
            failed |= expect(vr.error_count > 0, "invalid owner mode triggers error");
            int found = 0;
            for (int i = 0; i < vr.error_count; i++)
                if (strstr(vr.errors[i], "uds_owner_mode")) found = 1;
            failed |= expect(found, "invalid owner mode error mentions 'uds_owner_mode'");
            unlink(path);
            free(path);
            path = NULL;
        }
    }

    /* ---- Test 8: invalid kv_cache_policy enum ---- */
    {
        init_global_config(8080);
        path = write_temp_config("{\"kv_cache_policy\":\"unknown_policy\"}");
        if (path) {
            load_config_from_file(path);
            ConfigValidationResult vr = config_validate_all();
            failed |= expect(vr.error_count > 0, "invalid kv_cache_policy triggers error");
            int found = 0;
            for (int i = 0; i < vr.error_count; i++)
                if (strstr(vr.errors[i], "kv_cache_policy")) found = 1;
            failed |= expect(found, "invalid kv_cache_policy error mentions 'kv_cache_policy'");
            unlink(path);
            free(path);
            path = NULL;
        }
    }

    /* ---- Test 9: large config file (> 4096 bytes) ---- */
    {
        init_global_config(8080);
        /* Build a config with many fields to exceed old 4096 limit */
        char big_json[8192];
        int pos = 0;
        pos += snprintf(big_json + pos, sizeof(big_json) - pos,
                        "{\"port\":8080,\"context_tokens\":150000");
        /* Add many dummy fields to push size past 4096 */
        for (int i = 0; i < 80 && pos < 7000; i++) {
            pos += snprintf(big_json + pos, sizeof(big_json) - pos,
                            ",\"dummy_%d\":\"%s\"", i, "abcdefghijklmnopqrstuvwxyz0123456789");
        }
        pos += snprintf(big_json + pos, sizeof(big_json) - pos, "}");
        path = write_temp_config(big_json);
        if (path) {
            failed |= expect(load_config_from_file(path), "load large config (>4096 bytes)");
            /* Should not crash or truncate */
            ConfigValidationResult vr = config_validate_all();
            failed |= expect(vr.error_count == 0, "large valid config: zero errors");
            if (vr.error_count > 0) {
                for (int i = 0; i < vr.error_count && i < 3; i++)
                    fprintf(stderr, "  Unexpected error: %s\n", vr.errors[i]);
            }
            unlink(path);
            free(path);
            path = NULL;
        }
    }

    /* ---- Test 10: malformed JSON (missing comma) ---- */
    {
        init_global_config(8080);
        path = write_temp_config("{\"port\":8080 \"missing_comma\":true}");
        if (path) {
            /* load_config_from_file should return false for malformed JSON */
            failed |= expect(!load_config_from_file(path), "malformed JSON: load fails");
            unlink(path);
            free(path);
            path = NULL;
        }
    }

    /* ---- Test 11: max_history_entries too large ---- */
    {
        init_global_config(8080);
        path = write_temp_config("{\"max_history_entries\":2000}");
        if (path) {
            load_config_from_file(path);
            ConfigValidationResult vr = config_validate_all();
            failed |= expect(vr.error_count > 0, "max_history_entries 2000 triggers error");
            int found = 0;
            for (int i = 0; i < vr.error_count; i++)
                if (strstr(vr.errors[i], "max_history_entries") && strstr(vr.errors[i], "too large")) found = 1;
            failed |= expect(found, "max_history_entries error mentions 'too large'");
            unlink(path);
            free(path);
            path = NULL;
        }
    }

    /* ---- Test 12: context_tokens too small ---- */
    {
        init_global_config(8080);
        path = write_temp_config("{\"context_tokens\":1}");
        if (path) {
            load_config_from_file(path);
            ConfigValidationResult vr = config_validate_all();
            failed |= expect(vr.error_count > 0, "context_tokens=1 triggers error");
            int found = 0;
            for (int i = 0; i < vr.error_count; i++)
                if (strstr(vr.errors[i], "context_tokens") && strstr(vr.errors[i], "too small")) found = 1;
            failed |= expect(found, "context_tokens error mentions 'too small'");
            unlink(path);
            free(path);
            path = NULL;
        }
    }

    /* ---- Test 13: max_request_size too small ---- */
    {
        init_global_config(8080);
        path = write_temp_config("{\"max_request_size\":1}");
        if (path) {
            load_config_from_file(path);
            ConfigValidationResult vr = config_validate_all();
            failed |= expect(vr.error_count > 0, "max_request_size=1 triggers error");
            int found = 0;
            for (int i = 0; i < vr.error_count; i++)
                if (strstr(vr.errors[i], "max_request_size") && strstr(vr.errors[i], "too small")) found = 1;
            failed |= expect(found, "max_request_size error mentions 'too small'");
            unlink(path);
            free(path);
            path = NULL;
        }
    }

    /* ---- Test 14: multiple errors at once ---- */
    {
        init_global_config(8080);
        path = write_temp_config(
            "{"
            "\"port\":-1,"
            "\"max_buffer_chunk\":1,"
            "\"context_tokens\":1,"
            "\"uds_connect_timeout_ms\":-5,"
            "\"native_transport\":\"invalid\","
            "\"uds_owner_mode\":\"invalid\","
            "\"kv_cache_policy\":\"invalid\""
            "}");
        if (path) {
            load_config_from_file(path);
            ConfigValidationResult vr = config_validate_all();
            failed |= expect(vr.error_count >= 7, "multiple errors: at least 7 errors");
            failed |= expect(vr.error_count <= CONFIG_MAX_ERRORS, "error count within limit");
            unlink(path);
            free(path);
            path = NULL;
        }
    }

    /* ---- Test 15: empty config file (no fields) ---- */
    {
        init_global_config(8080);
        path = write_temp_config("{}");
        if (path) {
            failed |= expect(load_config_from_file(path), "load empty config");
            ConfigValidationResult vr = config_validate_all();
            /* Empty config uses defaults — should have no errors since defaults are valid */
            failed |= expect(vr.error_count == 0, "empty config (defaults): zero errors");
            if (vr.error_count > 0) {
                for (int i = 0; i < vr.error_count && i < 3; i++)
                    fprintf(stderr, "  Unexpected error: %s\n", vr.errors[i]);
            }
            unlink(path);
            free(path);
            path = NULL;
        }
    }

    /* ---- Test 16: config file missing (non-existent path) ---- */
    {
        init_global_config(8080);
        failed |= expect(!load_config_from_file("/tmp/nonexistent_config.json"),
                         "missing config file: load fails");
    }

    printf("%s\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
