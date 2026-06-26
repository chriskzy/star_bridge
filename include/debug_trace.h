#ifndef DEBUG_TRACE_H
#define DEBUG_TRACE_H

#include <stddef.h>

void debug_trace_append(const char *fmt, ...);
void debug_trace_body_probe(unsigned long request_number, const char *body, size_t body_len);
void debug_trace_fixture_capture(unsigned long request_number, const char *raw_request, size_t raw_len, const char *body, size_t body_len);
void debug_trace_redact_text(char *text);
void debug_trace_compact_text(const char *src, char *dest, size_t max_len);
#endif
