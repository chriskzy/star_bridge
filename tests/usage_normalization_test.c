#include "responses_api.h"
#include "codex_stream_events.h"

#include <stdio.h>
#include <string.h>
#include "cJSON.h"

static int expect(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        return 1;
    }
    return 0;
}

/* Verify JSON field exists with expected value using cJSON parse */
static int expect_json_field(const char *json, const char *field, int expected) {
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        fprintf(stderr, "FAIL: JSON parse error for field '%s'\n", field);
        return 1;
    }
    cJSON *item = cJSON_GetObjectItem(root, field);
    if (!item) {
        fprintf(stderr, "FAIL: JSON missing field '%s' in: %s\n", field, json);
        cJSON_Delete(root);
        return 1;
    }
    int val = (int)cJSON_GetNumberValue(item);
    if (val != expected) {
        fprintf(stderr, "FAIL: field '%s' expected %d but got %d\n", field, expected, val);
        cJSON_Delete(root);
        return 1;
    }
    cJSON_Delete(root);
    return 0;
}

int main(void) {
    int failed = 0;

    /* ---------------------------------------------------------------- */
    /* Test 1: resp_normalize_usage basic                                */
    /* ---------------------------------------------------------------- */
    {
        ResponseObject r;
        resp_init(&r, "star-bridge-ds4");
        resp_normalize_usage(&r, 100, 50, "");

        failed |= expect(r.prompt_tokens == 100, "normalize: prompt_tokens = 100");
        failed |= expect(r.completion_tokens == 50, "normalize: completion_tokens = 50");
        failed |= expect(r.total_tokens == 150, "normalize: total_tokens = 150");
        failed |= expect(r.input_text_tokens == 100, "normalize: input_text = prompt_tokens");
        failed |= expect(r.input_cached_tokens == 0, "normalize: input_cached default 0");
        failed |= expect(r.output_text_tokens == 50, "normalize: output_text = completion_tokens");
        failed |= expect(r.output_reasoning_tokens == 0, "normalize: output_reasoning default 0");
    }

    /* ---------------------------------------------------------------- */
    /* Test 2: resp_normalize_usage with cache_hit                       */
    /* ---------------------------------------------------------------- */
    {
        ResponseObject r;
        resp_init(&r, "star-bridge-ds4");
        resp_normalize_usage(&r, 200, 30, "{\"cache_hit\":15}");

        failed |= expect(r.prompt_tokens == 200, "cache: prompt_tokens = 200");
        failed |= expect(r.completion_tokens == 30, "cache: completion_tokens = 30");
        failed |= expect(r.total_tokens == 230, "cache: total_tokens = 230");
        failed |= expect(r.input_cached_tokens == 15, "cache: input_cached = 15");
        failed |= expect(r.input_text_tokens == 200, "cache: input_text still = 200");
        failed |= expect(r.output_text_tokens == 30, "cache: output_text = 30");
        failed |= expect(r.output_reasoning_tokens == 0, "cache: no reasoning");
    }

    /* ---------------------------------------------------------------- */
    /* Test 3: resp_normalize_usage with cache_create                    */
    /* ---------------------------------------------------------------- */
    {
        ResponseObject r;
        resp_init(&r, "star-bridge-ds4");
        resp_normalize_usage(&r, 300, 40, "{\"cache_create\":10}");

        failed |= expect(r.input_cached_tokens == 10, "cache_create: input_cached = 10");
        failed |= expect(r.input_text_tokens == 300, "cache_create: input_text = 300");
    }

    /* ---------------------------------------------------------------- */
    /* Test 4: resp_normalize_usage with both cache_hit + cache_create   */
    /* ---------------------------------------------------------------- */
    {
        ResponseObject r;
        resp_init(&r, "star-bridge-ds4");
        resp_normalize_usage(&r, 500, 60, "{\"cache_hit\":20,\"cache_create\":8}");

        failed |= expect(r.input_cached_tokens == 28, "dual-cache: input_cached = 20+8");
    }

    /* ---------------------------------------------------------------- */
    /* Test 5: resp_normalize_usage with reasoning_tokens                */
    /* ---------------------------------------------------------------- */
    {
        ResponseObject r;
        resp_init(&r, "star-bridge-ds4");
        resp_normalize_usage(&r, 400, 100, "{\"reasoning_tokens\":25}");

        failed |= expect(r.output_reasoning_tokens == 25, "reasoning: output_reasoning = 25");
        failed |= expect(r.output_text_tokens == 75, "reasoning: output_text adjusted = 100-25");
        failed |= expect(r.completion_tokens == 100, "reasoning: completion_tokens unchanged = 100");
    }

    /* ---------------------------------------------------------------- */
    /* Test 6: resp_normalize_usage with all fields                      */
    /* ---------------------------------------------------------------- */
    {
        ResponseObject r;
        resp_init(&r, "star-bridge-ds4");
        resp_normalize_usage(&r, 600, 200, "{\"cache_hit\":50,\"cache_create\":10,\"reasoning_tokens\":40}");

        failed |= expect(r.prompt_tokens == 600, "all: prompt_tokens = 600");
        failed |= expect(r.completion_tokens == 200, "all: completion_tokens = 200");
        failed |= expect(r.total_tokens == 800, "all: total_tokens = 800");
        failed |= expect(r.input_cached_tokens == 60, "all: input_cached = 50+10");
        failed |= expect(r.input_text_tokens == 600, "all: input_text = 600");
        failed |= expect(r.output_reasoning_tokens == 40, "all: output_reasoning = 40");
        failed |= expect(r.output_text_tokens == 160, "all: output_text = 200-40");
    }

    /* ---------------------------------------------------------------- */
    /* Test 7: resp_normalize_usage with zero tokens                     */
    /* ---------------------------------------------------------------- */
    {
        ResponseObject r;
        resp_init(&r, "star-bridge-ds4");
        resp_normalize_usage(&r, 0, 0, "");
        failed |= expect(r.total_tokens == 0, "zero: total_tokens = 0");
        failed |= expect(r.input_text_tokens == 0, "zero: input_text = 0");
        failed |= expect(r.output_text_tokens == 0, "zero: output_text = 0");
    }

    /* ---------------------------------------------------------------- */
    /* Test 8: resp_set_usage                                            */
    /* ---------------------------------------------------------------- */
    {
        ResponseObject r;
        resp_init(&r, "star-bridge-ds4");
        resp_set_usage(&r, 150, 75, 140, 10, 0, 70, 5);

        failed |= expect(r.prompt_tokens == 150, "set: prompt_tokens = 150");
        failed |= expect(r.completion_tokens == 75, "set: completion_tokens = 75");
        failed |= expect(r.total_tokens == 225, "set: total_tokens = 225");
        failed |= expect(r.input_text_tokens == 140, "set: input_text = 140");
        failed |= expect(r.input_cached_tokens == 10, "set: input_cached = 10");
        failed |= expect(r.input_audio_tokens == 0, "set: input_audio = 0");
        failed |= expect(r.output_text_tokens == 70, "set: output_text = 70");
        failed |= expect(r.output_reasoning_tokens == 5, "set: output_reasoning = 5");
    }

    /* ---------------------------------------------------------------- */
    /* Test 9: resp_usage_to_json basic                                  */
    /* ---------------------------------------------------------------- */
    {
        ResponseObject r;
        resp_init(&r, "star-bridge-ds4");
        resp_set_usage(&r, 100, 50, 95, 5, 0, 45, 5);

        char buf[2048];
        size_t n = resp_usage_to_json(&r, buf, sizeof(buf));
        failed |= expect(n > 0, "usage_to_json: non-zero length");
        failed |= expect(strstr(buf, "\"usage\":") != NULL, "usage_to_json: has usage wrapper");
        failed |= expect(strstr(buf, "\"input_tokens\":100") != NULL, "usage_to_json: input_tokens");
        failed |= expect(strstr(buf, "\"output_tokens\":50") != NULL, "usage_to_json: output_tokens");
        failed |= expect(strstr(buf, "\"total_tokens\":150") != NULL, "usage_to_json: total_tokens");

        /* Verify nested details */
        failed |= expect(strstr(buf, "\"input_tokens_details\"") != NULL, "usage_to_json: input details");
        failed |= expect(strstr(buf, "\"text_tokens\":95") != NULL, "usage_to_json: input text_tokens");
        failed |= expect(strstr(buf, "\"cached_tokens\":5") != NULL, "usage_to_json: cached_tokens");
        failed |= expect(strstr(buf, "\"output_tokens_details\"") != NULL, "usage_to_json: output details");
        failed |= expect(strstr(buf, "\"text_tokens\":45") != NULL, "usage_to_json: output text_tokens");
        failed |= expect(strstr(buf, "\"reasoning_tokens\":5") != NULL, "usage_to_json: reasoning_tokens");
    }

    /* ---------------------------------------------------------------- */
    /* Test 10: resp_usage_to_json omits zero details fields             */
    /* ---------------------------------------------------------------- */
    {
        ResponseObject r;
        resp_init(&r, "star-bridge-ds4");
        resp_set_usage(&r, 100, 50, 100, 0, 0, 50, 0);

        char buf[2048];
        size_t n = resp_usage_to_json(&r, buf, sizeof(buf));
        failed |= expect(n > 0, "usage_to_json_zero: non-zero length");

        /* cached_tokens and reasoning_tokens should be absent when zero */
        failed |= expect(strstr(buf, "\"cached_tokens\"") == NULL, "usage_to_json_zero: cached_tokens omitted");
        failed |= expect(strstr(buf, "\"reasoning_tokens\"") == NULL, "usage_to_json_zero: reasoning_tokens omitted");
        /* audio_tokens should also be absent when zero */
        failed |= expect(strstr(buf, "\"audio_tokens\"") == NULL, "usage_to_json_zero: audio_tokens omitted");
    }

    /* ---------------------------------------------------------------- */
    /* Test 11: resp_to_json includes usage when tokens present          */
    /* ---------------------------------------------------------------- */
    {
        ResponseObject r;
        resp_init(&r, "star-bridge-ds4");
        resp_set_id(&r, "resp_usage_test");
        resp_add_text(&r, "Hello world");
        resp_set_usage(&r, 10, 5, 10, 0, 0, 5, 0);

        char buf[4096];
        size_t n = resp_to_json(&r, buf, sizeof(buf));
        failed |= expect(n > 0, "resp_to_json_with_usage: non-zero length");

        /* Parse JSON and verify usage block */
        cJSON *root = cJSON_Parse(buf);
        failed |= expect(root != NULL, "resp_to_json_with_usage: valid JSON");
        if (root) {
            cJSON *usage = cJSON_GetObjectItem(root, "usage");
            failed |= expect(usage != NULL, "resp_to_json_with_usage: usage object present");
            if (usage) {
                failed |= expect_json_field(buf, "usage", 0); /* just verify parseable */
                cJSON *input_tokens = cJSON_GetObjectItem(usage, "input_tokens");
                failed |= expect(input_tokens != NULL, "resp_to_json_with_usage: usage.input_tokens present");
                if (input_tokens) {
                    failed |= expect((int)cJSON_GetNumberValue(input_tokens) == 10, "resp_to_json_with_usage: input_tokens = 10");
                }
                cJSON *output_tokens = cJSON_GetObjectItem(usage, "output_tokens");
                failed |= expect(output_tokens != NULL, "resp_to_json_with_usage: usage.output_tokens present");
                if (output_tokens) {
                    failed |= expect((int)cJSON_GetNumberValue(output_tokens) == 5, "resp_to_json_with_usage: output_tokens = 5");
                }
                cJSON *total_tokens = cJSON_GetObjectItem(usage, "total_tokens");
                failed |= expect(total_tokens != NULL, "resp_to_json_with_usage: usage.total_tokens present");
                if (total_tokens) {
                    failed |= expect((int)cJSON_GetNumberValue(total_tokens) == 15, "resp_to_json_with_usage: total_tokens = 15");
                }
            }
            cJSON_Delete(root);
        }
    }

    /* ---------------------------------------------------------------- */
    /* Test 12: resp_to_json omits usage when no tokens                  */
    /* ---------------------------------------------------------------- */
    {
        ResponseObject r;
        resp_init(&r, "star-bridge-ds4");
        resp_add_text(&r, "No usage");

        char buf[4096];
        size_t n = resp_to_json(&r, buf, sizeof(buf));
        failed |= expect(n > 0, "resp_to_json_no_usage: non-zero length");

        /* Parse JSON and verify usage is absent */
        cJSON *root = cJSON_Parse(buf);
        failed |= expect(root != NULL, "resp_to_json_no_usage: valid JSON");
        if (root) {
            cJSON *usage = cJSON_GetObjectItem(root, "usage");
            failed |= expect(usage == NULL, "resp_to_json_no_usage: usage absent");
            cJSON_Delete(root);
        }
    }

    /* ---------------------------------------------------------------- */
    /* Test 13: codex_stream_completed with usage (verify JSON valid)    */
    /* ---------------------------------------------------------------- */
    {
        HarnessStreamEvent event;
        memset(&event, 0, sizeof(event));
        codex_stream_completed("resp-test", "star-bridge-ds4", "final", 42,
                               100, 50, "{\"cache_hit\":10,\"reasoning_tokens\":5}", NULL, &event);

        failed |= expect(strcmp(event.event, "response.completed") == 0,
                         "stream_completed_usage: event name");
        failed |= expect(strstr(event.data, "\"type\":\"response.completed\"") != NULL,
                         "stream_completed_usage: type field");

        /* Parse the data JSON to verify it's valid */
        cJSON *root = cJSON_Parse(event.data);
        failed |= expect(root != NULL, "stream_completed_usage: valid JSON");
        if (root) {
            cJSON *response = cJSON_GetObjectItem(root, "response");
            failed |= expect(response != NULL, "stream_completed_usage: response object");
            if (response) {
                cJSON *usage = cJSON_GetObjectItem(response, "usage");
                failed |= expect(usage != NULL, "stream_completed_usage: usage object present");
                if (usage) {
                    cJSON *input_tokens = cJSON_GetObjectItem(usage, "input_tokens");
                    failed |= expect(input_tokens != NULL, "stream_completed_usage: input_tokens present");
                    if (input_tokens) {
                        failed |= expect((int)cJSON_GetNumberValue(input_tokens) == 100,
                                         "stream_completed_usage: input_tokens = 100");
                    }
                    cJSON *output_tokens = cJSON_GetObjectItem(usage, "output_tokens");
                    failed |= expect(output_tokens != NULL, "stream_completed_usage: output_tokens present");
                    if (output_tokens) {
                        failed |= expect((int)cJSON_GetNumberValue(output_tokens) == 50,
                                         "stream_completed_usage: output_tokens = 50");
                    }
                    cJSON *total_tokens = cJSON_GetObjectItem(usage, "total_tokens");
                    failed |= expect(total_tokens != NULL, "stream_completed_usage: total_tokens present");
                    if (total_tokens) {
                        failed |= expect((int)cJSON_GetNumberValue(total_tokens) == 150,
                                         "stream_completed_usage: total_tokens = 150");
                    }
                    /* Verify details sub-objects */
                    cJSON *input_details = cJSON_GetObjectItem(usage, "input_tokens_details");
                    failed |= expect(input_details != NULL, "stream_completed_usage: input details");
                    if (input_details) {
                        cJSON *cached = cJSON_GetObjectItem(input_details, "cached_tokens");
                        failed |= expect(cached != NULL, "stream_completed_usage: cached_tokens present");
                        if (cached) {
                            failed |= expect((int)cJSON_GetNumberValue(cached) == 10,
                                             "stream_completed_usage: cached_tokens = 10");
                        }
                    }
                    cJSON *output_details = cJSON_GetObjectItem(usage, "output_tokens_details");
                    failed |= expect(output_details != NULL, "stream_completed_usage: output details");
                    if (output_details) {
                        cJSON *reasoning = cJSON_GetObjectItem(output_details, "reasoning_tokens");
                        failed |= expect(reasoning != NULL, "stream_completed_usage: reasoning present");
                        if (reasoning) {
                            failed |= expect((int)cJSON_GetNumberValue(reasoning) == 5,
                                             "stream_completed_usage: reasoning = 5");
                        }
                    }
                }
            }
            cJSON_Delete(root);
        }
    }

    /* ---------------------------------------------------------------- */
    /* Test 14: codex_stream_completed without usage (verify JSON valid) */
    /* ---------------------------------------------------------------- */
    {
        HarnessStreamEvent event;
        memset(&event, 0, sizeof(event));
        codex_stream_completed("resp-test", "star-bridge-ds4", "no usage", 7,
                               0, 0, "", NULL, &event);

        failed |= expect(strcmp(event.event, "response.completed") == 0,
                         "stream_completed_no_usage: event name");

        /* Parse JSON to verify valid JSON structure */
        cJSON *root = cJSON_Parse(event.data);
        failed |= expect(root != NULL, "stream_completed_no_usage: valid JSON");
        if (root) {
            cJSON *response = cJSON_GetObjectItem(root, "response");
            failed |= expect(response != NULL, "stream_completed_no_usage: response object");
            if (response) {
                /* usage should be absent when tokens are 0 */
                cJSON *usage = cJSON_GetObjectItem(response, "usage");
                failed |= expect(usage == NULL, "stream_completed_no_usage: usage absent");
            }
            cJSON_Delete(root);
        }
    }

    /* ---------------------------------------------------------------- */
    /* Test 15: codex_stream_completed with usage but no usage_json      */
    /* ---------------------------------------------------------------- */
    {
        HarnessStreamEvent event;
        memset(&event, 0, sizeof(event));
        codex_stream_completed("resp-test", "star-bridge-ds4", "usage no extra", 99,
                               75, 25, "", NULL, &event);

        cJSON *root = cJSON_Parse(event.data);
        failed |= expect(root != NULL, "stream_completed_usage_no_extra: valid JSON");
        if (root) {
            cJSON *response = cJSON_GetObjectItem(root, "response");
            failed |= expect(response != NULL, "stream_completed_usage_no_extra: response object");
            if (response) {
                cJSON *usage = cJSON_GetObjectItem(response, "usage");
                failed |= expect(usage != NULL, "stream_completed_usage_no_extra: usage present");
                if (usage) {
                    failed |= expect((int)cJSON_GetNumberValue(cJSON_GetObjectItem(usage, "input_tokens")) == 75,
                                     "stream_completed_usage_no_extra: input_tokens = 75");
                    failed |= expect((int)cJSON_GetNumberValue(cJSON_GetObjectItem(usage, "output_tokens")) == 25,
                                     "stream_completed_usage_no_extra: output_tokens = 25");
                    failed |= expect((int)cJSON_GetNumberValue(cJSON_GetObjectItem(usage, "total_tokens")) == 100,
                                     "stream_completed_usage_no_extra: total_tokens = 100");
                    /* cached/reasoning should be 0 since no usage_json */
                    cJSON *input_details = cJSON_GetObjectItem(usage, "input_tokens_details");
                    if (input_details) {
                        cJSON *cached = cJSON_GetObjectItem(input_details, "cached_tokens");
                        if (cached) {
                            failed |= expect((int)cJSON_GetNumberValue(cached) == 0,
                                             "stream_completed_usage_no_extra: cached = 0");
                        }
                    }
                    cJSON *output_details = cJSON_GetObjectItem(usage, "output_tokens_details");
                    if (output_details) {
                        cJSON *reasoning = cJSON_GetObjectItem(output_details, "reasoning_tokens");
                        if (reasoning) {
                            failed |= expect((int)cJSON_GetNumberValue(reasoning) == 0,
                                             "stream_completed_usage_no_extra: reasoning = 0");
                        }
                    }
                }
            }
            cJSON_Delete(root);
        }
    }

    /* ---------------------------------------------------------------- */
    /* Test 16: Verify the existing codex_adapter_test still works       */
    /* ---------------------------------------------------------------- */
    {
        /* Re-run the same call from codex_adapter_test.c to verify compatibility */
        HarnessStreamEvent event;
        memset(&event, 0, sizeof(event));
        failed |= expect(codex_stream_completed("resp-stream", "star-bridge-ds4", "final text", 7, 10, 5, "{\"cache_hit\":2,\"reasoning_tokens\":3}", NULL, &event),
                         "stream_completed: returns true");
        failed |= expect(strcmp(event.event, "response.completed") == 0,
                         "stream_completed: event name");
        failed |= expect(strstr(event.data, "\"response\":{\"id\":\"resp-stream\"") != NULL,
                         "stream_completed: nested response id");
        failed |= expect(strstr(event.data, "\"status\":\"completed\"") != NULL,
                         "stream_completed: status");
        failed |= expect(strstr(event.data, "final text") != NULL,
                         "stream_completed: final text");

        /* Verify JSON is valid (no double commas) */
        cJSON *root = cJSON_Parse(event.data);
        failed |= expect(root != NULL, "stream_completed_adapter: valid JSON (no double comma)");
        cJSON_Delete(root);
    }

    /* ---------------------------------------------------------------- */
    /* Summary                                                          */
    /* ---------------------------------------------------------------- */
    if (failed == 0) {
        printf("OK: all 16 usage normalization tests passed\n");
        return 0;
    } else {
        printf("FAILED: %d usage normalization tests\n", failed);
        return 1;
    }
}
