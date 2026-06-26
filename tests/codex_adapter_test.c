#include "codex_adapter.h"
#include "codex_tool_normalizer.h"

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

int main(void) {
    const char *body =
        "{"
        "\"model\":\"star-bridge-ds4\","
        "\"previous_response_id\":\"resp_prev_123\","
        "\"instructions\":\"system instruction smoke\","
        "\"reasoning\":{\"effort\":\"medium\"},"
        "\"metadata\":{\"repo\":\"star_bridge\",\"case\":\"phase3\"},"
        "\"stream\":true,"
        "\"reset_session\":true,"
        "\"input\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"array input smoke\"}]}],"
        "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"google_search\"}}]"
        "}";

    HarnessRequest req;
    HarnessError err;
    if (!codex_parse_request(body, &req, &err)) {
        fprintf(stderr, "FAIL: parse failed: %s\n", err.message);
        return 1;
    }

    int failed = 0;
    failed |= expect(strcmp(req.harness, "codex.responses") == 0, "harness id");
    failed |= expect(strcmp(req.model, "star-bridge-ds4") == 0, "model");
    failed |= expect(req.stream, "stream true");
    failed |= expect(req.reset_session, "reset_session true");
    failed |= expect(req.has_tools, "tool definitions detected");
    failed |= expect(!req.explicit_tool_call, "tool definitions are not executable tool call");
    failed |= expect(strcmp(req.previous_response_id, "resp_prev_123") == 0, "previous_response_id parsed");
    failed |= expect(strcmp(req.reasoning_effort, "medium") == 0, "nested reasoning_effort parsed");
    failed |= expect(strstr(req.metadata_json, "\"repo\":\"star_bridge\"") != NULL, "metadata json preserved");
    failed |= expect(strstr(req.normalized_input, "system instruction smoke") != NULL, "instructions normalized");
    failed |= expect(strstr(req.normalized_input, "previous_response_id: resp_prev_123") != NULL, "previous id normalized");
    failed |= expect(strstr(req.normalized_input, "metadata: {\"repo\":\"star_bridge\",\"case\":\"phase3\"}") != NULL, "metadata normalized");
    failed |= expect(strstr(req.normalized_input, "array input smoke") != NULL, "array input normalized");

    const char *tool_body = "{\"name\":\"browse_url\",\"arguments\":{\"url\":\"https://example.test\"}}";
    memset(&req, 0, sizeof(req));
    memset(&err, 0, sizeof(err));
    if (!codex_parse_request(tool_body, &req, &err)) {
        fprintf(stderr, "FAIL: tool parse failed: %s\n", err.message);
        return 1;
    }
    failed |= expect(req.explicit_tool_call, "explicit tool call detected");
    failed |= expect(strcmp(req.tool_name, "browse_url") == 0, "tool name parsed");
    failed |= expect(strcmp(req.tool_argument, "{\"url\":\"https://example.test\"}") == 0, "tool argument parsed");

    memset(&req, 0, sizeof(req));
    memset(&err, 0, sizeof(err));
    failed |= expect(!codex_parse_request("{\"input\":\"unterminated\"", &req, &err), "invalid json rejected");
    failed |= expect(strstr(err.message, "JSON") != NULL || strstr(err.message, "json") != NULL, "invalid json error message");

    char json[150000];
    size_t n = codex_serialize_text_response("resp-test", "star-bridge-ds4", "hello \"world\"", json, sizeof(json));
    failed |= expect(n > 0, "text response serialized");
    failed |= expect(strstr(json, "\"id\":\"resp-test\"") != NULL, "text response id");
    failed |= expect(strstr(json, "\"type\":\"text\"") != NULL, "text response item");
    failed |= expect(strstr(json, "hello \\\"world\\\"") != NULL, "text response escapes quotes");

    n = codex_serialize_error("bad \"input\"", json, sizeof(json));
    failed |= expect(n > 0, "error serialized");
    failed |= expect(strstr(json, "\"status\":\"failed\"") != NULL, "error status");
    failed |= expect(strstr(json, "bad \\\"input\\\"") != NULL, "error escapes quotes");

    HarnessStreamEvent event;
    failed |= expect(codex_stream_created("resp-stream", "star-bridge-ds4", &event), "stream created event");
    failed |= expect(strcmp(event.event, "response.created") == 0, "stream created event name");
    failed |= expect(strstr(event.data, "\"type\":\"response.created\"") != NULL, "stream created data");
    failed |= expect(strstr(event.data, "\"response\":{\"id\":\"resp-stream\"") != NULL, "stream created nested response");
    failed |= expect(strstr(event.data, "\"output\":[]") != NULL, "stream created empty output");

    failed |= expect(codex_stream_delta("resp-stream", "star-bridge-ds4", "delta\ntext", &event), "stream delta event");
    failed |= expect(strcmp(event.event, "response.output_text.delta") == 0, "stream delta event name");
    failed |= expect(strstr(event.data, "delta\\ntext") != NULL, "stream delta escapes newline");
    failed |= expect(strstr(event.data, "\"item_id\":\"msg_0\"") != NULL, "stream delta item id");

    failed |= expect(codex_stream_completed("resp-stream", "star-bridge-ds4", "final text", 7, 10, 5, "{\"cache_hit\":2,\"reasoning_tokens\":3}", NULL, &event), "stream completed event");
    failed |= expect(strcmp(event.event, "response.completed") == 0, "stream completed event name");
    failed |= expect(strstr(event.data, "\"response\":{\"id\":\"resp-stream\"") != NULL, "stream completed nested response");
    failed |= expect(strstr(event.data, "\"status\":\"completed\"") != NULL, "stream completed status");
    failed |= expect(strstr(event.data, "final text") != NULL, "stream completed final output");

    /* Test new lifecycle event functions */
    HarnessStreamEvent lc_events[6];
    memset(&lc_events, 0, sizeof(lc_events));

    failed |= expect(codex_stream_output_item_added("resp-stream", 0, 1, &lc_events[0]), "output_item_added");
    failed |= expect(strcmp(lc_events[0].event, "response.output_item.added") == 0, "output_item_added event name");
    failed |= expect(strstr(lc_events[0].data, "\"type\":\"response.output_item.added\"") != NULL, "output_item_added data");
    failed |= expect(strstr(lc_events[0].data, "\"output_index\":0") != NULL, "output_item_added output index");

    failed |= expect(codex_stream_content_part_added("resp-stream", 0, 0, 2, &lc_events[1]), "content_part_added");
    failed |= expect(strcmp(lc_events[1].event, "response.content_part.added") == 0, "content_part_added event name");
    failed |= expect(strstr(lc_events[1].data, "\"type\":\"response.content_part.added\"") != NULL, "content_part_added data");
    failed |= expect(strstr(lc_events[1].data, "\"annotations\":[]") != NULL, "content_part_added annotations");

    failed |= expect(codex_stream_output_text_delta("resp-stream", "star-bridge-ds4", "hello delta", 0, 0, 3, &lc_events[2]), "output_text_delta");
    failed |= expect(strcmp(lc_events[2].event, "response.output_text.delta") == 0, "output_text_delta event name");
    failed |= expect(strstr(lc_events[2].data, "hello delta") != NULL, "output_text_delta data");
    failed |= expect(strstr(lc_events[2].data, "\"sequence_number\":3") != NULL, "output_text_delta sequence");

    failed |= expect(codex_stream_output_text_done("resp-stream", "done text", 0, 0, 4, &lc_events[3]), "output_text_done");
    failed |= expect(strcmp(lc_events[3].event, "response.output_text.done") == 0, "output_text_done event name");
    failed |= expect(strstr(lc_events[3].data, "\"type\":\"response.output_text.done\"") != NULL, "output_text_done data");
    failed |= expect(strstr(lc_events[3].data, "\"text\":\"done text\"") != NULL, "output_text_done text");

    failed |= expect(codex_stream_content_part_done("resp-stream", "done text", 0, 0, 5, &lc_events[4]), "content_part_done");
    failed |= expect(strcmp(lc_events[4].event, "response.content_part.done") == 0, "content_part_done event name");
    failed |= expect(strstr(lc_events[4].data, "\"type\":\"response.content_part.done\"") != NULL, "content_part_done data");
    failed |= expect(strstr(lc_events[4].data, "\"part\":{\"type\":\"output_text\"") != NULL, "content_part_done part");

    failed |= expect(codex_stream_output_item_done("resp-stream", "done text", 0, 6, &lc_events[5]), "output_item_done");
    failed |= expect(strcmp(lc_events[5].event, "response.output_item.done") == 0, "output_item_done event name");
    failed |= expect(strstr(lc_events[5].data, "\"type\":\"response.output_item.done\"") != NULL, "output_item_done data");
    failed |= expect(strstr(lc_events[5].data, "\"status\":\"completed\"") != NULL, "output_item_done status");

    const HarnessAdapterVTable *adapter = codex_responses_adapter();
    failed |= expect(adapter != NULL, "adapter vtable exists");
    failed |= expect(strcmp(adapter->name, "codex.responses") == 0, "adapter name");
    failed |= expect(adapter->parse_request != NULL, "adapter parse hook");
    failed |= expect(adapter->serialize_text_response != NULL, "adapter response hook");
    failed |= expect(adapter->stream_delta != NULL, "adapter stream hook");

    /* Tool normalization tests */
    {
        /* Test 1: web_search → google_search */
        cJSON *body = cJSON_Parse("{\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"web_search\"}}]}");
        if (body) {
            NormalizedToolDef norm_tools[MAX_TOOL_DEFS];
            int n = codex_normalize_tools(body, norm_tools, MAX_TOOL_DEFS);
            failed |= expect(n == 1, "web_search normalized one tool");
            failed |= expect(strcmp(norm_tools[0].normalized_name, "google_search") == 0, "web_search → google_search");
            failed |= expect(strcmp(norm_tools[0].type, "function") == 0, "web_search → function type");
            cJSON_Delete(body);
        } else {
            failed |= expect(0, "web_search body parse");
        }
    }
    {
        /* Test 2: computer → computer_call */
        cJSON *body = cJSON_Parse("{\"tools\":[{\"type\":\"computer\",\"name\":\"computer\"}]}");
        if (body) {
            NormalizedToolDef norm_tools[MAX_TOOL_DEFS];
            int n = codex_normalize_tools(body, norm_tools, MAX_TOOL_DEFS);
            failed |= expect(n == 1, "computer normalized one tool");
            failed |= expect(strcmp(norm_tools[0].normalized_name, "computer_call") == 0, "computer → computer_call");
            failed |= expect(strcmp(norm_tools[0].type, "computer") == 0, "computer_call → computer type");
            cJSON_Delete(body);
        } else {
            failed |= expect(0, "computer body parse");
        }
    }
    {
        /* Test 3: shell → execute_shell */
        cJSON *body = cJSON_Parse("{\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"shell\"}}]}");
        if (body) {
            NormalizedToolDef norm_tools[MAX_TOOL_DEFS];
            int n = codex_normalize_tools(body, norm_tools, MAX_TOOL_DEFS);
            failed |= expect(n == 1, "shell normalized one tool");
            failed |= expect(strcmp(norm_tools[0].normalized_name, "execute_shell") == 0, "shell → execute_shell");
            cJSON_Delete(body);
        } else {
            failed |= expect(0, "shell body parse");
        }
    }
    {
        /* Test 4: browse_url preserved */
        cJSON *body = cJSON_Parse("{\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"browse_url\",\"description\":\"Open a URL\"}}]}");
        if (body) {
            NormalizedToolDef norm_tools[MAX_TOOL_DEFS];
            int n = codex_normalize_tools(body, norm_tools, MAX_TOOL_DEFS);
            failed |= expect(n == 1, "browse_url preserved");
            failed |= expect(strcmp(norm_tools[0].normalized_name, "browse_url") == 0, "browse_url name preserved");
            failed |= expect(strcmp(norm_tools[0].description, "Open a URL") == 0, "browse_url description");
            cJSON_Delete(body);
        } else {
            failed |= expect(0, "browse_url body parse");
        }
    }
    {
        /* Test 5: tool_choice none */
        cJSON *body = cJSON_Parse("{\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"browse_url\"}}],\"tool_choice\":\"none\"}");
        if (body) {
            ToolChoiceSpec tc = codex_resolve_tool_choice(body);
            failed |= expect(tc.mode == TOOL_CHOICE_NONE, "tool_choice none resolved");
            cJSON_Delete(body);
        } else {
            failed |= expect(0, "tool_choice none parse");
        }
    }
    {
        /* Test 6: tool_choice required (any) */
        cJSON *body = cJSON_Parse("{\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"browse_url\"}}],\"tool_choice\":\"any\"}");
        if (body) {
            ToolChoiceSpec tc = codex_resolve_tool_choice(body);
            failed |= expect(tc.mode == TOOL_CHOICE_REQUIRED, "tool_choice any resolved as required");
            cJSON_Delete(body);
        } else {
            failed |= expect(0, "tool_choice required parse");
        }
    }
    {
        /* Test 7: tool_choice auto */
        cJSON *body = cJSON_Parse("{\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"browse_url\"}}],\"tool_choice\":\"auto\"}");
        if (body) {
            ToolChoiceSpec tc = codex_resolve_tool_choice(body);
            failed |= expect(tc.mode == TOOL_CHOICE_AUTO, "tool_choice auto resolved");
            cJSON_Delete(body);
        } else {
            failed |= expect(0, "tool_choice auto parse");
        }
    }
    {
        /* Test 8: tool_choice named tool */
        cJSON *body = cJSON_Parse("{\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"browse_url\"}},{\"type\":\"function\",\"function\":{\"name\":\"google_search\"}}],\"tool_choice\":{\"type\":\"function\",\"function\":{\"name\":\"google_search\"}}}");
        if (body) {
            ToolChoiceSpec tc = codex_resolve_tool_choice(body);
            failed |= expect(tc.mode == TOOL_CHOICE_NAMED, "tool_choice named resolved");
            failed |= expect(strcmp(tc.named_tool, "google_search") == 0, "tool_choice named tool name");
            cJSON_Delete(body);
        } else {
            failed |= expect(0, "tool_choice named parse");
        }
    }
    {
        /* Test 9: tool prompt building */
        NormalizedToolDef tools[3];
        memset(tools, 0, sizeof(tools));
        strcpy(tools[0].original_name, "google_search");
        strcpy(tools[0].normalized_name, "google_search");
        strcpy(tools[0].description, "Search the web");
        strcpy(tools[0].type, "builtin");
        tools[0].is_bridge_owned = true;
        strcpy(tools[1].original_name, "browse_url");
        strcpy(tools[1].normalized_name, "browse_url");
        strcpy(tools[1].description, "Open a URL");
        strcpy(tools[1].type, "builtin");
        tools[1].is_bridge_owned = false;
        tools[1].input_schema_json[0] = '\0';
        char prompt[4096];
        codex_build_tool_prompt(tools, 2, prompt, sizeof(prompt));
        failed |= expect(strstr(prompt, "google_search") != NULL, "prompt contains google_search");
        failed |= expect(strstr(prompt, "browse_url") != NULL, "prompt contains browse_url");
        failed |= expect(strstr(prompt, "Search the web") != NULL, "prompt contains description");
    }
    {
        /* Test 10: empty tools */
        cJSON *body = cJSON_Parse("{\"tools\":[]}");
        if (body) {
            NormalizedToolDef norm_tools[MAX_TOOL_DEFS];
            int n = codex_normalize_tools(body, norm_tools, MAX_TOOL_DEFS);
            failed |= expect(n == 0, "empty tools yields zero");
            cJSON_Delete(body);
        } else {
            failed |= expect(0, "empty tools parse");
        }
    }
    {
        /* Test 11: no tools */
        cJSON *body = cJSON_Parse("{\"model\":\"test\"}");
        if (body) {
            NormalizedToolDef norm_tools[MAX_TOOL_DEFS];
            int n = codex_normalize_tools(body, norm_tools, MAX_TOOL_DEFS);
            failed |= expect(n == 0, "no tools yields zero");
            cJSON_Delete(body);
        } else {
            failed |= expect(0, "no tools parse");
        }
    }

    return failed ? 1 : 0;
}
