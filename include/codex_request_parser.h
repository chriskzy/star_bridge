#ifndef CODEX_REQUEST_PARSER_H
#define CODEX_REQUEST_PARSER_H

#include "harness_adapter.h"

#include <stddef.h>
#include <stdbool.h>

/* Parse an incoming Codex Responses-API request body into a HarnessRequest */
bool codex_parse_request(const char *body, HarnessRequest *out, HarnessError *err);

/* Schema validation for incoming Codex requests */
bool codex_validate_request_schema(const char *body, char *error, size_t error_max);



#endif /* CODEX_REQUEST_PARSER_H */
