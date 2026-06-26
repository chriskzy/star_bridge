#include "codex_request_parser.h"
#include "codex_response_formatter.h"
#include "tool_runner.h"
#include "responses_api.h"
#include "json_utils.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/*  Original formatter helpers                                        */
/* ------------------------------------------------------------------ */
size_t codex_serialize_text_response(const char *id,
                                     const char *model,
                                     const char *text,
                                     char *buf,
                                     size_t max_len) {
    if (!buf || max_len == 0) return 0;
    ResponseObject resp;
    resp_init(&resp, model);
    resp_set_id(&resp, id && id[0] ? id : "resp-block");
    resp_add_text(&resp, text ? text : "");
    return resp_to_json(&resp, buf, max_len);
}

size_t codex_serialize_text_response_with_details(const char *id,
                                                  const char *model,
                                                  const char *text,
                                                  const char *incomplete_details,
                                                  char *buf,
                                                  size_t max_len) {
    if (!buf || max_len == 0) return 0;
    ResponseObject resp;
    resp_init(&resp, model);
    resp_set_id(&resp, id && id[0] ? id : "resp-block");
    resp_add_text(&resp, text ? text : "");
    if (incomplete_details && incomplete_details[0]) {
        snprintf(resp.incomplete_details, sizeof(resp.incomplete_details), "%s", incomplete_details);
    }
    return resp_to_json(&resp, buf, max_len);
}

size_t codex_serialize_tool_response(const char *id,
                                     const char *model,
                                     const char *tool_name,
                                     const char *tool_argument_json,
                                     const char *display_text,
                                     char *buf,
                                     size_t max_len) {
    if (!buf || max_len == 0) return 0;
    ResponseObject resp;
    resp_init(&resp, model);
    resp_set_id(&resp, id && id[0] ? id : "resp-tool");
    resp_add_text(&resp, display_text ? display_text : "");
    resp_add_tool_call(&resp, "call_tool", tool_name ? tool_name : "", tool_argument_json ? tool_argument_json : "{}");
    return resp_to_json(&resp, buf, max_len);
}

size_t codex_serialize_error(const char *message, char *buf, size_t max_len) {
    return resp_error_to_json(message ? message : "Unknown error", buf, max_len);
}

/* ------------------------------------------------------------------ */
/*  Synthesize structured tool-execution summary                      */
/* ------------------------------------------------------------------ */
size_t codex_synthesize_tool_summary(const char *tool_name,
                                     const char *tool_args,
                                     const ToolRunResult *run,
                                     char *buf,
                                     size_t max_len) {
    if (!buf || max_len == 0) return 0;
    if (!tool_name || !tool_name[0] || !run) {
        buf[0] = '\0';
        return 0;
    }

    cJSON *summary = cJSON_CreateObject();
    if (!summary) return 0;

    /* Intent: what tool was called with what arguments */
    cJSON_AddStringToObject(summary, "intent", tool_name);
    cJSON_AddStringToObject(summary, "tool_args", tool_args && tool_args[0] ? tool_args : "{}");

    /* State: success / error / timeout / denied */
    const char *state = "unknown";
    if (run->denied)           state = "denied";
    else if (run->timed_out)   state = "timeout";
    else if (run->exit_code)   state = "error";
    else if (run->ok)          state = "success";
    cJSON_AddStringToObject(summary, "state", state);
    cJSON_AddNumberToObject(summary, "exit_code", run->exit_code);

    /* Scored improvements: simple quality heuristic based on result */
    int score = 0;
    if (run->ok) {
        /* Successful tool execution */
        if (run->result_json[0]) {
            /* Check if result contains meaningful data */
            size_t rlen = strlen(run->result_json);
            if (rlen > 20) score = 90;
            else           score = 70;
        } else {
            score = 50;
        }
    } else if (run->timed_out) {
        score = 20;
    } else if (run->denied) {
        score = 10;
    } else {
        score = 5;
    }

    cJSON_AddNumberToObject(summary, "quality_score", score);

    /* Optional diagnostics */
    if (run->timed_out)
        cJSON_AddBoolToObject(summary, "timed_out", true);
    if (run->denied)
        cJSON_AddBoolToObject(summary, "denied", true);
    if (run->truncated)
        cJSON_AddBoolToObject(summary, "truncated", true);

    /* Suggestions for improvement based on score */
    const char *suggestion = "";
    if (score >= 90)
        suggestion = "Tool executed successfully with meaningful result.";
    else if (score >= 70)
        suggestion = "Tool succeeded but result may be minimal; consider refining arguments.";
    else if (score >= 50)
        suggestion = "Tool returned no output; verify tool configuration or input.";
    else if (score >= 20)
        suggestion = "Tool timed out; reduce execution scope or increase timeout.";
    else if (score >= 10)
        suggestion = "Tool call was denied by policy; review allowed tools list.";
    else
        suggestion = "Tool execution failed; check error details.";
    cJSON_AddStringToObject(summary, "suggestion", suggestion);

    /* Serialize to buf */
    bool ok = cJSON_PrintPreallocated(summary, buf, (int)max_len, false) != 0;
    cJSON_Delete(summary);

    if (!ok) { buf[0] = '\0'; return 0; }
    size_t written = strlen(buf);
    return written;
}

bool codex_validate_response_json(const char *json, char *error, size_t error_max) {
    if (!json || !*json) {
        snprintf(error, error_max, "empty response body");
        return false;
    }

    cJSON *obj = cJSON_Parse(json);
    if (!obj || !cJSON_IsObject(obj)) {
        snprintf(error, error_max, "response must be a JSON object");
        cJSON_Delete(obj);
        return false;
    }

    /* Required: id */
    if (!cJSON_GetObjectItem(obj, "id")) {
        snprintf(error, error_max, "missing required field 'id'");
        cJSON_Delete(obj);
        return false;
    }

    /* Required: object */
    if (!cJSON_GetObjectItem(obj, "object")) {
        snprintf(error, error_max, "missing required field 'object'");
        cJSON_Delete(obj);
        return false;
    }

    /* Required: created_at */
    if (!cJSON_GetObjectItem(obj, "created_at")) {
        snprintf(error, error_max, "missing required field 'created_at'");
        cJSON_Delete(obj);
        return false;
    }

    /* Required: status */
    if (!cJSON_GetObjectItem(obj, "status")) {
        snprintf(error, error_max, "missing required field 'status'");
        cJSON_Delete(obj);
        return false;
    }

    /* Required: model */
    if (!cJSON_GetObjectItem(obj, "model")) {
        snprintf(error, error_max, "missing required field 'model'");
        cJSON_Delete(obj);
        return false;
    }

    /* Required: output (array) */
    cJSON *output_item = cJSON_GetObjectItem(obj, "output");
    if (!output_item) {
        snprintf(error, error_max, "missing required field 'output'");
        cJSON_Delete(obj);
        return false;
    }

    /* output must be an array */
    if (!cJSON_IsArray(output_item)) {
        snprintf(error, error_max, "field 'output' must be an array");
        cJSON_Delete(obj);
        return false;
    }

    cJSON_Delete(obj);
    return true;
}
