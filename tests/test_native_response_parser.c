/**
 * test_native_response_parser.c - Regression test for schema-aware native response parser
 *
 * Task 169: Verify that native response frames parse into typed response/event structs,
 * JSON strings decode correctly for escaped quotes, newline, tab, unicode, and nested
 * text fields; first arbitrary text field is not treated as final answer.
 */

#include "native_response.h"
#include "json_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Test 1: Parse a simple response frame */
static int test_parse_response(void) {
    const char *frame = "{\"type\":\"response\",\"id\":\"req-1\",\"status\":\"completed\",\"text\":\"Hello, world!\"}";
    NativeEvent ev;
    memset(&ev, 0, sizeof(ev));
    if (!native_parse_frame(frame, strlen(frame), &ev)) {
        fprintf(stderr, "FAIL: parse_response: native_parse_frame returned false\n");
        return 1;
    }
    if (ev.type != NATIVE_EVENT_RESPONSE) {
        fprintf(stderr, "FAIL: parse_response: type=%d, expected %d\n", ev.type, NATIVE_EVENT_RESPONSE);
        return 1;
    }
    if (strcmp(ev.data.response.text, "Hello, world!") != 0) {
        fprintf(stderr, "FAIL: parse_response: text='%s', expected 'Hello, world!'\n", ev.data.response.text);
        return 1;
    }
    if (strcmp(ev.data.response.status, "completed") != 0) {
        fprintf(stderr, "FAIL: parse_response: status='%s', expected 'completed'\n", ev.data.response.status);
        return 1;
    }
    if (strcmp(ev.id, "req-1") != 0) {
        fprintf(stderr, "FAIL: parse_response: id='%s', expected 'req-1'\n", ev.id);
        return 1;
    }
    printf("PASS: parse_response\n");
    return 0;
}

/* Test 2: Parse response with escaped quotes in text */
static int test_parse_escaped_quotes(void) {
    const char *frame = "{\"type\":\"response\",\"id\":\"req-2\",\"status\":\"completed\",\"text\":\"She said \\\"Hello\\\"\"}";
    NativeEvent ev;
    memset(&ev, 0, sizeof(ev));
    if (!native_parse_frame(frame, strlen(frame), &ev)) {
        fprintf(stderr, "FAIL: parse_escaped_quotes: native_parse_frame returned false\n");
        return 1;
    }
    if (ev.type != NATIVE_EVENT_RESPONSE) {
        fprintf(stderr, "FAIL: parse_escaped_quotes: type=%d\n", ev.type);
        return 1;
    }
    /* cJSON decodes \" to " */
    if (strcmp(ev.data.response.text, "She said \"Hello\"") != 0) {
        fprintf(stderr, "FAIL: parse_escaped_quotes: text='%s', expected 'She said \"Hello\"'\n", ev.data.response.text);
        return 1;
    }
    printf("PASS: parse_escaped_quotes\n");
    return 0;
}

/* Test 3: Parse response with escaped newline in text */
static int test_parse_escaped_newline(void) {
    const char *frame = "{\"type\":\"response\",\"id\":\"req-3\",\"status\":\"completed\",\"text\":\"line1\\nline2\"}";
    NativeEvent ev;
    memset(&ev, 0, sizeof(ev));
    if (!native_parse_frame(frame, strlen(frame), &ev)) {
        fprintf(stderr, "FAIL: parse_escaped_newline: native_parse_frame returned false\n");
        return 1;
    }
    if (strcmp(ev.data.response.text, "line1\nline2") != 0) {
        fprintf(stderr, "FAIL: parse_escaped_newline: text='%s', expected 'line1\\nline2'\n", ev.data.response.text);
        return 1;
    }
    printf("PASS: parse_escaped_newline\n");
    return 0;
}

/* Test 4: Parse response with escaped tab in text */
static int test_parse_escaped_tab(void) {
    const char *frame = "{\"type\":\"response\",\"id\":\"req-4\",\"status\":\"completed\",\"text\":\"col1\\tcol2\"}";
    NativeEvent ev;
    memset(&ev, 0, sizeof(ev));
    if (!native_parse_frame(frame, strlen(frame), &ev)) {
        fprintf(stderr, "FAIL: parse_escaped_tab: native_parse_frame returned false\n");
        return 1;
    }
    if (strcmp(ev.data.response.text, "col1\tcol2") != 0) {
        fprintf(stderr, "FAIL: parse_escaped_tab: text='%s', expected 'col1\\tcol2'\n", ev.data.response.text);
        return 1;
    }
    printf("PASS: parse_escaped_tab\n");
    return 0;
}

/* Test 5: Parse response with unicode escape in text */
static int test_parse_unicode_escape(void) {
    const char *frame = "{\"type\":\"response\",\"id\":\"req-5\",\"status\":\"completed\",\"text\":\"hello\\u0042world\"}";
    NativeEvent ev;
    memset(&ev, 0, sizeof(ev));
    if (!native_parse_frame(frame, strlen(frame), &ev)) {
        fprintf(stderr, "FAIL: parse_unicode_escape: native_parse_frame returned false\n");
        return 1;
    }
    /* cJSON handles \u0042 as 'B', but our json_decode_string currently keeps \u escapes as-is.
     * However cJSON's Parse handles them natively, so via json_parse + json_get_string we get
     * the properly decoded unicode. Let's check for 'B' */
    if (strcmp(ev.data.response.text, "helloBworld") != 0) {
        fprintf(stderr, "FAIL: parse_unicode_escape: text='%s', expected 'helloBworld'\n", ev.data.response.text);
        return 1;
    }
    printf("PASS: parse_unicode_escape\n");
    return 0;
}

/* Test 6: Parse nested JSON with "text" fields inside tool arguments.
 * The parser must NOT extract text from nested objects, only from top-level "text". */
static int test_nested_text_not_extracted(void) {
    const char *frame = "{\"type\":\"response\",\"id\":\"req-6\",\"status\":\"completed\",\"text\":\"I am the real answer\",\"details\":{\"text\":\"echo hello\"}}";
    NativeEvent ev;
    memset(&ev, 0, sizeof(ev));
    if (!native_parse_frame(frame, strlen(frame), &ev)) {
        fprintf(stderr, "FAIL: nested_text_not_extracted: native_parse_frame returned false\n");
        return 1;
    }
    if (ev.type != NATIVE_EVENT_RESPONSE) {
        fprintf(stderr, "FAIL: nested_text_not_extracted: type=%d\n", ev.type);
        return 1;
    }
    /* The text field should be the top-level "text", not the nested one inside tool_intent.arguments */
    if (strcmp(ev.data.response.text, "I am the real answer") != 0) {
        fprintf(stderr, "FAIL: nested_text_not_extracted: text='%s', expected 'I am the real answer'\n", ev.data.response.text);
        return 1;
    }
    printf("PASS: nested_text_not_extracted\n");
    return 0;
}

/* Test 7: Parse tool intent frame */
static int test_parse_tool_intent(void) {
    const char *frame = "{\"type\":\"tool_intent\",\"id\":\"req-7\",\"tool\":{\"name\":\"bash\",\"args\":\"echo hello\"}}";
    NativeEvent ev;
    memset(&ev, 0, sizeof(ev));
    if (!native_parse_frame(frame, strlen(frame), &ev)) {
        fprintf(stderr, "FAIL: parse_tool_intent: native_parse_frame returned false\n");
        return 1;
    }
    if (ev.type != NATIVE_EVENT_TOOL_INTENT) {
        fprintf(stderr, "FAIL: parse_tool_intent: type=%d, expected %d\n", ev.type, NATIVE_EVENT_TOOL_INTENT);
        return 1;
    }
    if (strcmp(ev.data.tool_intent.tool_name, "bash") != 0) {
        fprintf(stderr, "FAIL: parse_tool_intent: tool_name='%s', expected 'bash'\n", ev.data.tool_intent.tool_name);
        return 1;
    }
    if (strcmp(ev.data.tool_intent.tool_args, "echo hello") != 0) {
        fprintf(stderr, "FAIL: parse_tool_intent: tool_args='%s', expected 'echo hello'\n", ev.data.tool_intent.tool_args);
        return 1;
    }
    printf("PASS: parse_tool_intent\n");
    return 0;
}

/* Test 8: Parse ack frame */
static int test_parse_ack(void) {
    const char *frame = "{\"type\":\"ack\",\"id\":\"req-1\",\"status\":\"accepted\"}";
    NativeEvent ev;
    memset(&ev, 0, sizeof(ev));
    if (!native_parse_frame(frame, strlen(frame), &ev)) {
        fprintf(stderr, "FAIL: parse_ack: native_parse_frame returned false\n");
        return 1;
    }
    if (ev.type != NATIVE_EVENT_ACK) {
        fprintf(stderr, "FAIL: parse_ack: type=%d, expected %d\n", ev.type, NATIVE_EVENT_ACK);
        return 1;
    }
    if (strcmp(ev.id, "req-1") != 0) {
        fprintf(stderr, "FAIL: parse_ack: id='%s', expected 'req-1'\n", ev.id);
        return 1;
    }
    printf("PASS: parse_ack\n");
    return 0;
}

/* Test 9: Parse error frame */
static int test_parse_error(void) {
    const char *frame = "{\"type\":\"error\",\"id\":\"req-1\",\"status\":\"failed\",\"message\":\"Something went wrong\"}";
    NativeEvent ev;
    memset(&ev, 0, sizeof(ev));
    if (!native_parse_frame(frame, strlen(frame), &ev)) {
        fprintf(stderr, "FAIL: parse_error: native_parse_frame returned false\n");
        return 1;
    }
    if (ev.type != NATIVE_EVENT_ERROR) {
        fprintf(stderr, "FAIL: parse_error: type=%d, expected %d\n", ev.type, NATIVE_EVENT_ERROR);
        return 1;
    }
    if (strcmp(ev.data.error.message, "Something went wrong") != 0) {
        fprintf(stderr, "FAIL: parse_error: message='%s'\n", ev.data.error.message);
        return 1;
    }
    printf("PASS: parse_error\n");
    return 0;
}

/* Test 10: Parse lifecycle frame (health) */
static int test_parse_health(void) {
    const char *frame = "{\"type\":\"health\",\"id\":\"health-1\"}";
    NativeEvent ev;
    memset(&ev, 0, sizeof(ev));
    if (!native_parse_frame(frame, strlen(frame), &ev)) {
        fprintf(stderr, "FAIL: parse_health: native_parse_frame returned false\n");
        return 1;
    }
    if (ev.type != NATIVE_EVENT_HEALTH) {
        fprintf(stderr, "FAIL: parse_health: type=%d, expected %d\n", ev.type, NATIVE_EVENT_HEALTH);
        return 1;
    }
    if (!native_event_is_lifecycle(&ev)) {
        fprintf(stderr, "FAIL: parse_health: is_lifecycle returned false\n");
        return 1;
    }
    printf("PASS: parse_health\n");
    return 0;
}

/* Test 11: Parse compaction event */
static int test_parse_compaction(void) {
    const char *frame = "{\"type\":\"compaction.started\",\"message\":\"Compacting context...\"}";
    NativeEvent ev;
    memset(&ev, 0, sizeof(ev));
    if (!native_parse_frame(frame, strlen(frame), &ev)) {
        fprintf(stderr, "FAIL: parse_compaction: native_parse_frame returned false\n");
        return 1;
    }
    if (ev.type != NATIVE_EVENT_COMPACTION) {
        fprintf(stderr, "FAIL: parse_compaction: type=%d, expected %d\n", ev.type, NATIVE_EVENT_COMPACTION);
        return 1;
    }
    if (strcmp(ev.data.compaction.message, "Compacting context...") != 0) {
        fprintf(stderr, "FAIL: parse_compaction: message='%s'\n", ev.data.compaction.message);
        return 1;
    }
    printf("PASS: parse_compaction\n");
    return 0;
}

/* Test 12: Parse cancelled event */
static int test_parse_cancelled(void) {
    const char *frame = "{\"type\":\"cancelled\",\"id\":\"req-1\"}";
    NativeEvent ev;
    memset(&ev, 0, sizeof(ev));
    if (!native_parse_frame(frame, strlen(frame), &ev)) {
        fprintf(stderr, "FAIL: parse_cancelled: native_parse_frame returned false\n");
        return 1;
    }
    if (ev.type != NATIVE_EVENT_CANCELLED) {
        fprintf(stderr, "FAIL: parse_cancelled: type=%d, expected %d\n", ev.type, NATIVE_EVENT_CANCELLED);
        return 1;
    }
    printf("PASS: parse_cancelled\n");
    return 0;
}

/* Test 13: Parse response with text that contains nested JSON-like structure */
static int test_text_contains_braces(void) {
    const char *frame = "{\"type\":\"response\",\"id\":\"req-13\",\"status\":\"completed\",\"text\":\"Here is some text with {braces} and \\\"quotes\\\"\"}";
    NativeEvent ev;
    memset(&ev, 0, sizeof(ev));
    if (!native_parse_frame(frame, strlen(frame), &ev)) {
        fprintf(stderr, "FAIL: text_contains_braces: native_parse_frame returned false\n");
        return 1;
    }
    if (ev.type != NATIVE_EVENT_RESPONSE) {
        fprintf(stderr, "FAIL: text_contains_braces: type=%d\n", ev.type);
        return 1;
    }
    if (strcmp(ev.data.response.text, "Here is some text with {braces} and \"quotes\"") != 0) {
        fprintf(stderr, "FAIL: text_contains_braces: text='%s'\n", ev.data.response.text);
        return 1;
    }
    printf("PASS: text_contains_braces\n");
    return 0;
}

/* Test 14: Parse frame with unknown type - should still succeed and return UNKNOWN */
static int test_unknown_type(void) {
    const char *frame = "{\"type\":\"custom_event\",\"id\":\"custom-1\",\"value\":42}";
    NativeEvent ev;
    memset(&ev, 0, sizeof(ev));
    if (!native_parse_frame(frame, strlen(frame), &ev)) {
        fprintf(stderr, "FAIL: unknown_type: native_parse_frame returned false\n");
        return 1;
    }
    if (ev.type != NATIVE_EVENT_UNKNOWN) {
        fprintf(stderr, "FAIL: unknown_type: type=%d, expected %d\n", ev.type, NATIVE_EVENT_UNKNOWN);
        return 1;
    }
    if (strcmp(ev.type_str, "custom_event") != 0) {
        fprintf(stderr, "FAIL: unknown_type: type_str='%s'\n", ev.type_str);
        return 1;
    }
    printf("PASS: unknown_type\n");
    return 0;
}

/* Test 15: Parse malformed JSON - should return false */
static int test_malformed_json(void) {
    const char *frame = "{\"type\":\"response\",\"text\":\"unclosed";
    NativeEvent ev;
    memset(&ev, 0, sizeof(ev));
    /* Must return false for malformed JSON */
    if (native_parse_frame(frame, strlen(frame), &ev)) {
        fprintf(stderr, "FAIL: malformed_json: native_parse_frame returned true\n");
        return 1;
    }
    printf("PASS: malformed_json\n");
    return 0;
}

/* Test 16: Parse response frame that uses native protocol "output" field */
static int test_response_output_field(void) {
    const char *frame = "{\"type\":\"response\",\"id\":\"req-16\",\"status\":\"completed\",\"output\":\"Assistant text from output\"}";
    NativeEvent ev;
    memset(&ev, 0, sizeof(ev));
    if (!native_parse_frame(frame, strlen(frame), &ev)) {
        fprintf(stderr, "FAIL: response_output_field: native_parse_frame returned false\n");
        return 1;
    }
    if (ev.type != NATIVE_EVENT_RESPONSE) {
        fprintf(stderr, "FAIL: response_output_field: type=%d, expected %d\n", ev.type, NATIVE_EVENT_RESPONSE);
        return 1;
    }
    if (strcmp(ev.data.response.text, "Assistant text from output") != 0) {
        fprintf(stderr, "FAIL: response_output_field: text='%s', expected 'Assistant text from output'\n", ev.data.response.text);
        return 1;
    }
    printf("PASS: response_output_field\n");
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_parse_response();
    failures += test_parse_escaped_quotes();
    failures += test_parse_escaped_newline();
    failures += test_parse_escaped_tab();
    failures += test_parse_unicode_escape();
    failures += test_nested_text_not_extracted();
    failures += test_parse_tool_intent();
    failures += test_parse_ack();
    failures += test_parse_error();
    failures += test_parse_health();
    failures += test_parse_compaction();
    failures += test_parse_cancelled();
    failures += test_text_contains_braces();
    failures += test_unknown_type();
    failures += test_malformed_json();
    failures += test_response_output_field();

    printf("\n=== %d failures out of 16 tests ===\n", failures);
    return failures;
}
