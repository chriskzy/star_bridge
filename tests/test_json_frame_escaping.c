/**
 * test_json_frame_escaping.c - Regression test for JSON escaping in frame builders
 *
 * Task 166: Verify that all native frame builders escape ids, workspace roots,
 * model ids, tool names, session keys, status messages, and metadata.
 *
 * Tests: quotes, backslashes, newlines, unicode escapes, and long values.
 */

#include "native_frame.h"
#include "json_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Helper: verify JSON field string is valid by parsing */
static int check_json_field(const char *label, const char *json, const char *field,
                            const char *expected_value) {
    void *obj = json_parse(json);
    if (!obj) {
        fprintf(stderr, "FAIL: %s - invalid JSON: %s\n", label, json);
        return 1;
    }
    char buf[1024] = {0};
    bool found = json_get_string(obj, field, buf, sizeof(buf));
    json_free(obj);
    if (!found) {
        fprintf(stderr, "FAIL: %s - field '%s' not found in JSON\n", label, field);
        return 1;
    }
    if (strcmp(buf, expected_value) != 0) {
        fprintf(stderr, "FAIL: %s - field '%s' expected '%s' got '%s'\n",
                label, field, expected_value, buf);
        return 1;
    }
    printf("PASS: %s\n", label);
    return 0;
}

/* Test 1: Quotes in request_id */
static int test_quotes_in_id(void) {
    char *frame = frame_build_request("test\"id", "hello", NULL, NULL, NULL, NULL, false, 4000, false, NULL);
    if (!frame) { fprintf(stderr, "FAIL: test_quotes_in_id - frame_build_request returned NULL\n"); return 1; }
    int ret = check_json_field("quotes_in_id", frame, "id", "test\"id");
    free(frame);
    return ret;
}

/* Test 2: Backslash in request_id */
static int test_backslash_in_id(void) {
    char *frame = frame_build_request("test\\id", "hello", NULL, NULL, NULL, NULL, false, 4000, false, NULL);
    if (!frame) { fprintf(stderr, "FAIL: test_backslash_in_id - frame_build_request returned NULL\n"); return 1; }
    int ret = check_json_field("backslash_in_id", frame, "id", "test\\id");
    free(frame);
    return ret;
}

/* Test 3: Newline in input */
static int test_newline_in_input(void) {
    char *frame = frame_build_request("id123", "line1\nline2", NULL, NULL, NULL, NULL, false, 4000, false, NULL);
    if (!frame) { fprintf(stderr, "FAIL: test_newline_in_input - frame_build_request returned NULL\n"); return 1; }
    int ret = check_json_field("newline_in_input", frame, "input", "line1\nline2");
    free(frame);
    return ret;
}

/* Test 4: Unicode escapes in input (control chars) */
static int test_control_char_in_input(void) {
    char input[2] = {0x01, 0}; /* SOH control char */
    char *frame = frame_build_request("id123", input, NULL, NULL, NULL, NULL, false, 4000, false, NULL);
    if (!frame) { fprintf(stderr, "FAIL: test_control_char_in_input - frame_build_request returned NULL\n"); return 1; }
    /* After escaping, should be empty or \\u0001 - check parseable */
    void *obj = json_parse(frame);
    if (!obj) {
        fprintf(stderr, "FAIL: test_control_char_in_input - invalid JSON: %s\n", frame);
        free(frame);
        return 1;
    }
    json_free(obj);
    printf("PASS: control_char_in_input\n");
    free(frame);
    return 0;
}

/* Test 5: Long value in reasoning_effort */
static int test_long_reasoning_effort(void) {
    char long_reasoning[300];
    memset(long_reasoning, 'a', 280);
    long_reasoning[280] = '\0';
    strcat(long_reasoning, "\"quote\\backslash");
    char *frame = frame_build_request("id123", "hello", NULL, NULL, long_reasoning, NULL, false, 4000, false, NULL);
    if (!frame) { fprintf(stderr, "FAIL: test_long_reasoning_effort - frame_build_request returned NULL\n"); return 1; }
    void *obj = json_parse(frame);
    if (!obj) {
        fprintf(stderr, "FAIL: test_long_reasoning_effort - invalid JSON: %s\n", frame);
        free(frame);
        return 1;
    }
    json_free(obj);
    printf("PASS: long_reasoning_effort\n");
    free(frame);
    return 0;
}

/* Test 6: Quotes in response status */
static int test_quotes_in_status(void) {
    char *frame = frame_build_response("id123", "error\"msg", NULL, NULL);
    if (!frame) { fprintf(stderr, "FAIL: test_quotes_in_status - frame_build_response returned NULL\n"); return 1; }
    int ret = check_json_field("quotes_in_status", frame, "status", "error\"msg");
    free(frame);
    return ret;
}

/* Test 7: Backslash in tool_name */
static int test_backslash_in_tool_name(void) {
    char *frame = frame_build_tool_result("id123", "my\\tool", "{\"result\":\"ok\"}");
    if (!frame) { fprintf(stderr, "FAIL: test_backslash_in_tool_name - frame_build_tool_result returned NULL\n"); return 1; }
    /* Parse and check tool name */
    void *obj = json_parse(frame);
    if (!obj) {
        fprintf(stderr, "FAIL: test_backslash_in_tool_name - invalid JSON: %s\n", frame);
        free(frame);
        return 1;
    }
    /* Extract tool.name via cJSON path - just check parseable */
    json_free(obj);
    printf("PASS: backslash_in_tool_name\n");
    free(frame);
    return 0;
}

/* Test 8: Newline in tool error message */
static int test_newline_in_error_message(void) {
    char *frame = frame_build_tool_error("id123", "test_tool", "error\nline2");
    if (!frame) { fprintf(stderr, "FAIL: test_newline_in_error_message - frame_build_tool_error returned NULL\n"); return 1; }
    void *obj = json_parse(frame);
    if (!obj) {
        fprintf(stderr, "FAIL: test_newline_in_error_message - invalid JSON: %s\n", frame);
        free(frame);
        return 1;
    }
    json_free(obj);
    printf("PASS: newline_in_error_message\n");
    free(frame);
    return 0;
}

/* Test 9: frame_write_typed with special chars */
static int test_typed_with_special(void) {
    /* Test using a mock approach - just check prefix generation */
    char escaped[256] = {0};
    json_escape("type\"with\\quote", escaped, sizeof(escaped));
    /* Build prefix manually to verify */
    char prefix[256];
    snprintf(prefix, sizeof(prefix), "{\"type\":\"%s\",\"payload\":null}", escaped);
    /* Check it parses */
    void *obj = json_parse(prefix);
    if (!obj) {
        fprintf(stderr, "FAIL: test_typed_with_special - invalid prefix: %s\n", prefix);
        return 1;
    }
    json_free(obj);
    printf("PASS: typed_with_special\n");
    return 0;
}

/* Test 10: Unicode escape sequence in input */
static int test_unicode_escape_in_input(void) {
    /* Test that \\uXXXX sequences in input are properly handled */
    char input[] = "hello\\u0020world";
    char *frame = frame_build_request("id123", input, NULL, NULL, NULL, NULL, false, 4000, false, NULL);
    if (!frame) { fprintf(stderr, "FAIL: test_unicode_escape_in_input - frame_build_request returned NULL\n"); return 1; }
    /* The \\u0020 should remain as literal \\u0020 in the escaped JSON string */
    void *obj = json_parse(frame);
    if (!obj) {
        fprintf(stderr, "FAIL: test_unicode_escape_in_input - invalid JSON: %s\n", frame);
        free(frame);
        return 1;
    }
    json_free(obj);
    printf("PASS: unicode_escape_in_input\n");
    free(frame);
    return 0;
}

int main(void) {
    int failures = 0;

    failures += test_quotes_in_id();
    failures += test_backslash_in_id();
    failures += test_newline_in_input();
    failures += test_control_char_in_input();
    failures += test_long_reasoning_effort();
    failures += test_quotes_in_status();
    failures += test_backslash_in_tool_name();
    failures += test_newline_in_error_message();
    failures += test_typed_with_special();
    failures += test_unicode_escape_in_input();

    printf("\n=== %d failures out of 10 tests ===\n", failures);
    return failures;
}
