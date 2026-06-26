#include "codex_tool_detector.h"
#include "codex_request_parser.h"
#include "codex_response_formatter.h"
#include "codex_stream_events.h"

/* ------------------------------------------------------------------ */
/*  Adapter vtable                                                     */
/* ------------------------------------------------------------------ */

const HarnessAdapterVTable *codex_responses_adapter(void) {
    static const HarnessAdapterVTable adapter = {
        .name = "codex.responses",
        .parse_request = codex_parse_request,
        .serialize_text_response = codex_serialize_text_response,
        .serialize_error = codex_serialize_error,
        .stream_created = codex_stream_created,
        .stream_delta = codex_stream_delta,
        .stream_completed = codex_stream_completed,
        .stream_output_item_added = codex_stream_output_item_added,
        .stream_content_part_added = codex_stream_content_part_added,
        .stream_output_text_delta = codex_stream_output_text_delta,
        .stream_output_text_done = codex_stream_output_text_done,
        .stream_content_part_done = codex_stream_content_part_done,
        .stream_output_item_done = codex_stream_output_item_done,
        .stream_error = codex_stream_error,
    };
    return &adapter;
}
