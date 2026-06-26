#ifndef CODEX_RESPONSE_FORMATTER_H
#define CODEX_RESPONSE_FORMATTER_H

#include <stddef.h>
#include <stdbool.h>
#include "tool_runner.h"

/* Serialize a text-only response into JSON */
size_t codex_serialize_text_response(const char *id,
                                     const char *model,
                                     const char *text,
                                     char *buf,
                                     size_t max_len);

/* Serialize a text-only response into JSON, with incomplete_details */
size_t codex_serialize_text_response_with_details(const char *id,
                                                  const char *model,
                                                  const char *text,
                                                  const char *incomplete_details,
                                                  char *buf,
                                                  size_t max_len);

/* Serialize a tool-call response into JSON */
size_t codex_serialize_tool_response(const char *id,
                                     const char *model,
                                     const char *tool_name,
                                     const char *tool_argument_json,
                                     const char *display_text,
                                     char *buf,
                                     size_t max_len);

/* Serialize an error response into JSON */
size_t codex_serialize_error(const char *message, char *buf, size_t max_len);

/* Schema validation for outgoing Responses objects */
bool codex_validate_response_json(const char *json, char *error, size_t error_max);

/* Synthesize a structured tool-execution summary with intent, state, and scored improvements.
 * Returns number of bytes written to buf, or 0 on error.
 */
size_t codex_synthesize_tool_summary(const char *tool_name,
                                     const char *tool_args,
                                     const ToolRunResult *run,
                                     char *buf,
                                     size_t max_len);

#endif /* CODEX_RESPONSE_FORMATTER_H */
