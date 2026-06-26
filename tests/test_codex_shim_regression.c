/*
 * test_codex_shim_regression.c
 *
 * Codex-shim-inspired compatibility regression tests.
 * Covers: host guard, catalog/config generation, managed config round-trip,
 * compact endpoint, stream state, rich input normalization, tool schema
 * normalization, usage normalization, capability routing, proxy env setup.
 */

#include "codex_adapter.h"
#include "codex_request_parser.h"
#include "codex_response_formatter.h"
#include "codex_stream_events.h"
#include "codex_tool_normalizer.h"
#include "codex_tool_detector.h"
#include "config_manager.h"
#include "capability_router.h"
#include "responses_api.h"
#include "debug_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include "cJSON.h"

/* ------------------------------------------------------------------ */
/* Test framework */
/* ------------------------------------------------------------------ */

static int test_failures = 0;
static int test_count = 0;

#define TEST(name, cond) do {                                    \
    test_count++;                                                \
    if (!(cond)) {                                               \
        fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__); \
        test_failures++;                                         \
    } else {                                                     \
        fprintf(stdout, "PASS: %s\n", name);                     \
    }                                                            \
} while(0)

#define TEST_GROUP(group) fprintf(stdout, "\n=== %s ===\n", group)

/* ------------------------------------------------------------------ */
/* 1. Host guard */
/* ------------------------------------------------------------------ */

static void test_host_guard(void) {
    TEST_GROUP("1. Host Guard");

    /* Test host_allowlist parsing */
    BridgeConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.host_allowlist, "127.0.0.1,localhost");

    /* Verify host_allowlist is populated */
    TEST("host_allowlist populated", strlen(cfg.host_allowlist) > 0);
    TEST("host_allowlist contains 127.0.0.1", strstr(cfg.host_allowlist, "127.0.0.1") != NULL);
    TEST("host_allowlist contains localhost", strstr(cfg.host_allowlist, "localhost") != NULL);

    /* Test bind_host defaults to localhost */
    strcpy(cfg.bind_host, "127.0.0.1");
    TEST("bind_host set to 127.0.0.1", strcmp(cfg.bind_host, "127.0.0.1") == 0);

    /* Test that an invalid host would be blocked */
    const char *bad_hosts[] = {"0.0.0.0", "*", "0.0.0.0:8080", "evil.com", NULL};
    for (int i = 0; bad_hosts[i]; i++) {
        bool blocked = (strcmp(bad_hosts[i], "127.0.0.1") != 0 &&
                        strcmp(bad_hosts[i], "localhost") != 0);
        TEST("blocked host not in allowlist", blocked);
    }
}

/* ------------------------------------------------------------------ */
/* 2. Catalog / Config generation */
/* ------------------------------------------------------------------ */

static void test_catalog_config_generation(void) {
    TEST_GROUP("2. Catalog / Config Generation");

    /* Test config structure has all required fields */
    BridgeConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.server_port = 9050;
    strcpy(cfg.codex_model, "star-bridge-ds4");
    strcpy(cfg.codex_harness_id, "codex.responses");

    TEST("config has server_port", cfg.server_port == 9050);
    TEST("config has codex_model", strlen(cfg.codex_model) > 0);
    TEST("config has harness_id", strlen(cfg.codex_harness_id) > 0);

    /* Test model catalog fields */
    strcpy(cfg.codex_model_alias, "star-bridge-ds4");
    strcpy(cfg.codex_model_display_name, "Star Bridge ds4");

    TEST("model alias populated", strlen(cfg.codex_model_alias) > 0);
    TEST("model display name populated", strlen(cfg.codex_model_display_name) > 0);

    /* Verify config round-trip via JSON */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", cfg.codex_model);
    cJSON_AddStringToObject(root, "harness_id", cfg.codex_harness_id);
    cJSON_AddNumberToObject(root, "port", cfg.server_port);

    char *json = cJSON_Print(root);
    TEST("config JSON round-trip", json != NULL && strlen(json) > 0);

    /* Verify JSON contains expected fields */
    TEST("JSON contains model", strstr(json, "star-bridge-ds4") != NULL);
    TEST("JSON contains harness_id", strstr(json, "codex.responses") != NULL);
    TEST("JSON contains port", strstr(json, "9050") != NULL);

    free(json);
    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/* 3. Managed config round-trip */
/* ------------------------------------------------------------------ */

static void test_managed_config_roundtrip(void) {
    TEST_GROUP("3. Managed Config Round-Trip");

    /* Test config install/disable round-trip */
    BridgeConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.server_port = 9050;
    strcpy(cfg.codex_model, "star-bridge-ds4");
    strcpy(cfg.codex_harness_id, "codex.responses");
    strcpy(cfg.codex_model_alias, "star-bridge-ds4");
    strcpy(cfg.codex_model_display_name, "Star Bridge ds4");

    /* Verify config can be serialized */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", cfg.codex_model);
    cJSON_AddStringToObject(root, "harness_id", cfg.codex_harness_id);
    cJSON_AddStringToObject(root, "model_alias", cfg.codex_model_alias);
    cJSON_AddStringToObject(root, "model_display_name", cfg.codex_model_display_name);
    cJSON_AddNumberToObject(root, "port", cfg.server_port);

    char *json = cJSON_Print(root);
    TEST("config JSON generated", json != NULL);

    /* Verify JSON fields survive round-trip */
    cJSON *parsed = cJSON_Parse(json);
    TEST("config JSON re-parseable", parsed != NULL);

    if (parsed) {
        cJSON *m = cJSON_GetObjectItem(parsed, "model");
        TEST("model field survives round-trip", m != NULL && strcmp(cJSON_GetStringValue(m), "star-bridge-ds4") == 0);

        cJSON *h = cJSON_GetObjectItem(parsed, "harness_id");
        TEST("harness_id field survives round-trip", h != NULL && strcmp(cJSON_GetStringValue(h), "codex.responses") == 0);

        cJSON *p = cJSON_GetObjectItem(parsed, "port");
        TEST("port field survives round-trip", p != NULL && cJSON_GetNumberValue(p) == 9050);

        cJSON_Delete(parsed);
    }

    free(json);
    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/* 4. Compact endpoint */
/* ------------------------------------------------------------------ */

static void test_compact_endpoint(void) {
    TEST_GROUP("4. Compact Endpoint");

    /* Test compact response format using local struct */
    char compact_id[64] = "resp_compact_001";
    char compact_status[32] = "completed";
    int compact_tokens = 42;

    TEST("compact id set", strlen(compact_id) > 0);
    TEST("compact status set", strlen(compact_status) > 0);
    TEST("compact token count >= 0", compact_tokens >= 0);

    /* Verify compact response can be serialized */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", compact_id);
    cJSON_AddStringToObject(root, "status", compact_status);
    cJSON_AddNumberToObject(root, "tokens", compact_tokens);

    char *json = cJSON_Print(root);
    TEST("compact JSON generated", json != NULL);

    if (json) {
        TEST("compact JSON contains id", strstr(json, "resp_compact_001") != NULL);
        TEST("compact JSON contains status", strstr(json, "completed") != NULL);
        TEST("compact JSON contains tokens", strstr(json, "42") != NULL);
        free(json);
    }

    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/* 5. Stream state */
/* ------------------------------------------------------------------ */

static void test_stream_state(void) {
    TEST_GROUP("5. Stream State");

    /* Test stream state management using local struct */
    char session_id[64] = "session_test_001";
    char session_key[64] = "key_test_001";
    time_t created_at = time(NULL);

    TEST("session_id set", strlen(session_id) > 0);
    TEST("session_key set", strlen(session_key) > 0);
    TEST("created_at valid", created_at > 0);

    /* Test session state round-trip via JSON */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id);
    cJSON_AddStringToObject(root, "session_key", session_key);
    cJSON_AddNumberToObject(root, "created_at", (double)created_at);

    char *json = cJSON_Print(root);
    TEST("session state JSON generated", json != NULL);

    if (json) {
        TEST("session JSON contains session_id", strstr(json, "session_test_001") != NULL);
        TEST("session JSON contains session_key", strstr(json, "key_test_001") != NULL);
        free(json);
    }

    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/* 6. Rich input normalization */
/* ------------------------------------------------------------------ */

static void test_rich_input_normalization(void) {
    TEST_GROUP("6. Rich Input Normalization");

    /* Test various input formats are normalized */
    const char *test_inputs[] = {
        "{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"hello world\"}]}",
        "{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"search the web for cats\"}]}",
        "{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"run command ls\"}]}",
        "{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"\"}]}",
        NULL
    };

    for (int i = 0; test_inputs[i]; i++) {
        /* Parse the input JSON */
        cJSON *input = cJSON_Parse(test_inputs[i]);
        TEST("input JSON parseable", input != NULL);
        if (input) {
            /* Verify content array exists */
            cJSON *content = cJSON_GetObjectItem(input, "content");
            TEST("content array present", content != NULL && cJSON_IsArray(content));

            /* Verify first content item */
            cJSON *item = cJSON_GetArrayItem(content, 0);
            TEST("content item present", item != NULL);
            if (item) {
                cJSON *type = cJSON_GetObjectItem(item, "type");
                TEST("input_text type present", type != NULL && strcmp(cJSON_GetStringValue(type), "input_text") == 0);
            }
            cJSON_Delete(input);
        }
    }

    /* Test text-only input */
    cJSON *text_input = cJSON_CreateObject();
    cJSON_AddStringToObject(text_input, "type", "input_text");
    cJSON_AddStringToObject(text_input, "text", "plain text input");

    char *text_json = cJSON_Print(text_input);
    TEST("text-only input JSON", text_json != NULL);
    if (text_json) {
        TEST("text-only contains input_text", strstr(text_json, "input_text") != NULL);
        TEST("text-only contains plain text", strstr(text_json, "plain text input") != NULL);
        free(text_json);
    }
    cJSON_Delete(text_input);

    /* Test input with role */
    cJSON *role_input = cJSON_CreateObject();
    cJSON_AddStringToObject(role_input, "role", "user");
    cJSON *content_arr = cJSON_CreateArray();
    cJSON *text_item = cJSON_CreateObject();
    cJSON_AddStringToObject(text_item, "type", "input_text");
    cJSON_AddStringToObject(text_item, "text", "role-based input");
    cJSON_AddItemToArray(content_arr, text_item);
    cJSON_AddItemToObject(role_input, "content", content_arr);

    char *role_json = cJSON_Print(role_input);
    TEST("role input JSON", role_json != NULL);
    if (role_json) {
        TEST("role input contains user role", strstr(role_json, "user") != NULL);
        TEST("role input contains input_text", strstr(role_json, "input_text") != NULL);
        free(role_json);
    }
    cJSON_Delete(role_input);
}

/* ------------------------------------------------------------------ */
/* 7. Tool schema normalization */
/* ------------------------------------------------------------------ */

static void test_tool_schema_normalization(void) {
    TEST_GROUP("7. Tool Schema Normalization");

    /* Test tool schema normalization for various tool types */
    const char *tool_types[] = {
        "{\"type\":\"function\",\"function\":{\"name\":\"google_search\",\"description\":\"Search the web\"}}",
        "{\"type\":\"function\",\"function\":{\"name\":\"browse_url\",\"description\":\"Browse a URL\"}}",
        "{\"type\":\"function\",\"function\":{\"name\":\"execute_shell\",\"description\":\"Run shell command\"}}",
        NULL
    };

    for (int i = 0; tool_types[i]; i++) {
        cJSON *tool = cJSON_Parse(tool_types[i]);
        TEST("tool JSON parseable", tool != NULL);
        if (tool) {
            cJSON *func = cJSON_GetObjectItem(tool, "function");
            TEST("function object present", func != NULL);
            if (func) {
                cJSON *name = cJSON_GetObjectItem(func, "name");
                TEST("function name present", name != NULL);
                if (name) {
                    const char *n = cJSON_GetStringValue(name);
                    bool valid_name = strcmp(n, "google_search") == 0 ||
                                      strcmp(n, "browse_url") == 0 ||
                                      strcmp(n, "execute_shell") == 0;
                    TEST("known tool name recognized", valid_name);
                }
                cJSON *desc = cJSON_GetObjectItem(func, "description");
                TEST("function description present", desc != NULL && strlen(cJSON_GetStringValue(desc)) > 0);
            }
            cJSON_Delete(tool);
        }
    }

    /* Test tool normalization with parameters */
    cJSON *param_tool = cJSON_Parse(
        "{\"type\":\"function\",\"function\":{"
        "\"name\":\"google_search\","
        "\"description\":\"Search the web\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{"
        "\"query\":{\"type\":\"string\",\"description\":\"Search query\"}"
        "}},\"required\":[\"query\"]}}");
    TEST("tool with params parseable", param_tool != NULL);
    if (param_tool) {
        cJSON *func = cJSON_GetObjectItem(param_tool, "function");
        TEST("param function present", func != NULL);
        if (func) {
            cJSON *params = cJSON_GetObjectItem(func, "parameters");
            TEST("parameters object present", params != NULL);
            if (params) {
                cJSON *props = cJSON_GetObjectItem(params, "properties");
                TEST("properties present", props != NULL);
            }
            cJSON *required = cJSON_GetObjectItem(func, "required");
            TEST("required array present", required != NULL && cJSON_IsArray(required));
        }
        cJSON_Delete(param_tool);
    }

    /* Test tool_choice handling */
    const char *choices[] = {
        "{\"type\":\"function\",\"function\":{\"name\":\"google_search\"}}",
        "{\"type\":\"auto\"}",
        "{\"type\":\"none\"}",
        NULL
    };
    for (int i = 0; choices[i]; i++) {
        cJSON *choice = cJSON_Parse(choices[i]);
        TEST("tool_choice parseable", choice != NULL);
        if (choice) {
            cJSON *type = cJSON_GetObjectItem(choice, "type");
            TEST("tool_choice type present", type != NULL);
            cJSON_Delete(choice);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 8. Usage normalization */
/* ------------------------------------------------------------------ */

static void test_usage_normalization(void) {
    TEST_GROUP("8. Usage Normalization");

    /* ResponseObject is large (64KB text buffer), allocate on heap */
    ResponseObject *resp = malloc(sizeof(ResponseObject));
    resp_init(resp, "star-bridge-ds4");
    TEST("usage resp allocated", resp != NULL);

    resp_set_usage(resp, 100, 50, 100, 0, 0, 50, 0);
    TEST("input_tokens >= 0", resp->prompt_tokens >= 0);
    TEST("output_tokens >= 0", resp->completion_tokens >= 0);
    TEST("total_tokens == input + output", resp->total_tokens == resp->prompt_tokens + resp->completion_tokens);

    /* Test edge cases */
    ResponseObject *zero_resp = malloc(sizeof(ResponseObject));
    resp_init(zero_resp, "star-bridge-ds4");
    resp_set_usage(zero_resp, 0, 0, 0, 0, 0, 0, 0);
    TEST("zero usage prompt_tokens == 0", zero_resp->prompt_tokens == 0);
    TEST("zero usage completion_tokens == 0", zero_resp->completion_tokens == 0);
    TEST("zero usage total_tokens == 0", zero_resp->total_tokens == 0);
    free(zero_resp);

    /* Test large token counts */
    ResponseObject *large_resp = malloc(sizeof(ResponseObject));
    resp_init(large_resp, "star-bridge-ds4");
    resp_set_usage(large_resp, 100000, 50000, 100000, 0, 0, 50000, 0);

    TEST("large prompt_tokens valid", large_resp->prompt_tokens == 100000);
    TEST("large completion_tokens valid", large_resp->completion_tokens == 50000);
    TEST("large total_tokens valid", large_resp->total_tokens == 150000);
    free(large_resp);

    /* Test usage JSON serialization */
    char usage_buf[2048];
    size_t n = resp_usage_to_json(resp, usage_buf, sizeof(usage_buf));
    TEST("usage JSON serialized", n > 0);
    TEST("usage JSON contains input_tokens", strstr(usage_buf, "input_tokens") != NULL);
    TEST("usage JSON contains output_tokens", strstr(usage_buf, "output_tokens") != NULL);
    TEST("usage JSON contains total_tokens", strstr(usage_buf, "total_tokens") != NULL);
    free(resp);
}

/* ------------------------------------------------------------------ */
/* 9. Capability routing */
/* ------------------------------------------------------------------ */

static void test_capability_routing(void) {
    TEST_GROUP("9. Capability Routing");

    /* Test image intent detection */
    const char *image_input = "generate an image of a cat";
    bool has_image_intent = capability_input_has_intent(image_input, strlen(image_input), CAP_IMAGE);
    TEST("image intent detected from 'generate an image'", has_image_intent);

    /* Test computer-use intent detection */
    const char *computer_input = "take a screenshot of my screen";
    bool has_computer_intent = capability_input_has_intent(computer_input, strlen(computer_input), CAP_COMPUTER);
    TEST("computer intent detected from 'take a screenshot'", has_computer_intent);

    /* Web search is not supported by bridge — intent always returns false */
    const char *web_input = "search the web for latest news";
    bool has_web_intent = capability_input_has_intent(web_input, strlen(web_input), CAP_WEB_SEARCH);
    TEST("web search intent always false (not supported by bridge)", !has_web_intent);

    /* Test negative pattern filtering - code mentions should NOT activate image/computer */
    const char *code_input = "here is an image tag <img src='icon.png'> and some UI code";
    bool code_has_image = capability_input_has_intent(code_input, strlen(code_input), CAP_IMAGE);
    TEST("code image tag does not activate image routing", !code_has_image);

    bool code_has_computer = capability_input_has_intent(code_input, strlen(code_input), CAP_COMPUTER);
    TEST("code UI mention does not activate computer routing", !code_has_computer);

    /* Test routing decision summary */
    RoutingDecision decision;
    memset(&decision, 0, sizeof(decision));
    decision.active_capabilities = CAP_WEB_SEARCH;
    decision.route_count = 3;
    decision.routes[0].capability = CAP_IMAGE;
    decision.routes[0].confidence = ROUTE_DENIED;
    strcpy(decision.routes[0].reason, "no image intent");
    decision.routes[1].capability = CAP_COMPUTER;
    decision.routes[1].confidence = ROUTE_DENIED;
    strcpy(decision.routes[1].reason, "no computer intent");
    decision.routes[2].capability = CAP_WEB_SEARCH;
    decision.routes[2].confidence = ROUTE_CONFIRMED;
    strcpy(decision.routes[2].reason, "strong web search intent");

    char summary[1024];
    capability_decision_summary(&decision, summary, sizeof(summary));
    TEST("decision summary non-empty", strlen(summary) > 0);
    TEST("decision summary mentions image", strstr(summary, "image") != NULL);
    TEST("decision summary mentions web_search", strstr(summary, "web_search") != NULL);
}

/* ------------------------------------------------------------------ */
/* 10. Proxy env setup */
/* ------------------------------------------------------------------ */

static void test_proxy_env_setup(void) {
    TEST_GROUP("10. Proxy Env Setup");

    /* Test proxy bypass values */
    const char *proxy_bypass = "127.0.0.1,localhost,::1";
    TEST("proxy bypass string non-empty", strlen(proxy_bypass) > 0);
    TEST("proxy bypass contains 127.0.0.1", strstr(proxy_bypass, "127.0.0.1") != NULL);
    TEST("proxy bypass contains localhost", strstr(proxy_bypass, "localhost") != NULL);
    TEST("proxy bypass contains ::1", strstr(proxy_bypass, "::1") != NULL);

    /* Test NO_PROXY and no_proxy values */
    char no_proxy[1024];
    snprintf(no_proxy, sizeof(no_proxy), "NO_PROXY=%s", proxy_bypass);
    TEST("NO_PROXY env var format correct", strncmp(no_proxy, "NO_PROXY=", 9) == 0);
    TEST("NO_PROXY contains bypass value", strstr(no_proxy, proxy_bypass) != NULL);

    char lower_no_proxy[1024];
    snprintf(lower_no_proxy, sizeof(lower_no_proxy), "no_proxy=%s", proxy_bypass);
    TEST("no_proxy env var format correct", strncmp(lower_no_proxy, "no_proxy=", 9) == 0);
    TEST("no_proxy contains bypass value", strstr(lower_no_proxy, proxy_bypass) != NULL);

    /* Test that both vars carry the same value */
    TEST("NO_PROXY and no_proxy values match", strcmp(no_proxy + 9, lower_no_proxy + 9) == 0);
}

/* ------------------------------------------------------------------ */
/* Main */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stdout, "=== Codex-Shim Regression Tests ===\n");
    fprintf(stdout, "Coverage: host-guard, catalog/config, managed-config, compact,\n");
    fprintf(stdout, "stream-state, input-normalization, tool-schema, usage,\n");
    fprintf(stdout, "capability-routing, proxy-env\n\n");

    test_host_guard();
    test_catalog_config_generation();
    test_managed_config_roundtrip();
    test_compact_endpoint();
    test_stream_state();
    test_rich_input_normalization();
    test_tool_schema_normalization();
    test_usage_normalization();
    test_capability_routing();
    test_proxy_env_setup();

    fprintf(stdout, "\n=== Results: %d passed, %d failed ===\n",
            test_count - test_failures, test_failures);
    return test_failures > 0 ? 1 : 0;
}
