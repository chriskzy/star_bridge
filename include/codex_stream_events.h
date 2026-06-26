#ifndef CODEX_STREAM_EVENTS_H
#define CODEX_STREAM_EVENTS_H

#include "harness_adapter.h"

#include <stddef.h>
#include <stdbool.h>

bool codex_stream_created(const char *id, const char *model, HarnessStreamEvent *out);
bool codex_stream_delta(const char *id, const char *model, const char *delta_text, HarnessStreamEvent *out);
bool codex_stream_completed(const char *id, const char *model, const char *final_text, int sequence_number, int prompt_tokens, int completion_tokens, const char *usage_json, const char *incomplete_details, HarnessStreamEvent *out);
bool codex_stream_output_item_added(const char *id, int output_index, int sequence_number, HarnessStreamEvent *out);
bool codex_stream_content_part_added(const char *id, int output_index, int content_index, int sequence_number, HarnessStreamEvent *out);
bool codex_stream_output_text_delta(const char *id, const char *model, const char *delta_text, int item_index, int content_index, int sequence_number, HarnessStreamEvent *out);
bool codex_stream_output_text_done(const char *id, const char *text, int output_index, int content_index, int sequence_number, HarnessStreamEvent *out);
bool codex_stream_content_part_done(const char *id, const char *text, int output_index, int content_index, int sequence_number, HarnessStreamEvent *out);
bool codex_stream_output_item_done(const char *id, const char *text, int output_index, int sequence_number, HarnessStreamEvent *out);
bool codex_stream_error(const char *id, const char *error_message, int sequence_number, HarnessStreamEvent *out);

#endif /* CODEX_STREAM_EVENTS_H */
