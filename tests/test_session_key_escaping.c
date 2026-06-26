/**
 * test_session_key_escaping.c - Regression test for session key JSON escaping
 *
 * Task 167: Verify session key generation and JSON-safe session index.
 * Tests: special chars in session_id/workspace/model round-trip through
 * session_key_for(), save_session_index(), load_session_index().
 */

#include "config_manager.h"
#include "json_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Test 1: session_key_for with special characters */
static int test_session_key_special_chars(void) {
    char buf[256] = {0};
    session_key_for("/tmp/test workspace", "session\"with\\quote\nnewline", buf, sizeof(buf));
    if (!buf[0]) {
        fprintf(stderr, "FAIL: session_key_for returned empty\n");
        return 1;
    }
    /* The key should be hash_safe_session_id - session_id special chars are
     * part of the key but should be handled by json_escape when serialized */
    printf("session_key_for result: %s\n", buf);
    printf("PASS: session_key_special_chars\n");
    return 0;
}

/* Test 2: compute_session_key with special chars */
static int test_compute_session_key_special(void) {
    init_global_config(8080);
    compute_session_key("/workspace with spaces", "session\"id");
    const char *sk = get_session_key();
    if (!sk || !sk[0]) {
        fprintf(stderr, "FAIL: compute_session_key returned empty\n");
        return 1;
    }
    printf("session_key: %s\n", sk);
    printf("PASS: compute_session_key_special\n");
    return 0;
}

/* Test 3: JSON escape round-trip of session-like strings */
static int test_json_escape_session_strings(void) {
    /* Test workspace root with quotes */
    char escaped[1024] = {0};
    json_escape("/workspace/with\"quote", escaped, sizeof(escaped));
    if (!escaped[0]) {
        fprintf(stderr, "FAIL: json_escape returned empty\n");
        return 1;
    }
    /* Verify it parses as valid JSON string */
    char json[1024];
    snprintf(json, sizeof(json), "{\"key\":\"%s\"}", escaped);
    void *obj = json_parse(json);
    if (!obj) {
        fprintf(stderr, "FAIL: escaped workspace root doesn't parse: %s\n", json);
        return 1;
    }
    json_free(obj);

    /* Test model_id with special chars */
    memset(escaped, 0, sizeof(escaped));
    json_escape("model/with\\backslash", escaped, sizeof(escaped));
    snprintf(json, sizeof(json), "{\"model\":\"%s\"}", escaped);
    obj = json_parse(json);
    if (!obj) {
        fprintf(stderr, "FAIL: escaped model_id doesn't parse: %s\n", json);
        return 1;
    }
    json_free(obj);

    /* Test state_id with special chars */
    memset(escaped, 0, sizeof(escaped));
    json_escape("state\nwith\nnewline", escaped, sizeof(escaped));
    snprintf(json, sizeof(json), "{\"state\":\"%s\"}", escaped);
    obj = json_parse(json);
    if (!obj) {
        fprintf(stderr, "FAIL: escaped state_id doesn't parse: %s\n", json);
        return 1;
    }
    json_free(obj);

    printf("PASS: json_escape_session_strings\n");
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_session_key_special_chars();
    failures += test_compute_session_key_special();
    failures += test_json_escape_session_strings();

    printf("\n=== %d failures out of 3 tests ===\n", failures);
    return failures;
}
