#ifndef JSON_BUILDER_H
#define JSON_BUILDER_H

#include <stdbool.h>
#include <stddef.h>

/* Simple JSON builder that constructs JSON strings into a fixed buffer.
 * Usage pattern:
 *   JsonBuilder b;
 *   jsonb_init(&b, buf, max_len);
 *   jsonb_object_begin(&b);
 *   jsonb_string(&b, "key", "value");
 *   jsonb_number(&b, "count", 42);
 *   jsonb_object_end(&b);
 *   // buf now contains {"key":"value","count":42}
 */

typedef struct {
    char *buf;
    size_t max;
    size_t off;
    int depth;
    bool first[16];
    bool failed;
} JsonBuilder;

void jsonb_init(JsonBuilder *b, char *buf, size_t max);
void jsonb_object_begin(JsonBuilder *b);
void jsonb_object_end(JsonBuilder *b);
void jsonb_array_begin(JsonBuilder *b);
void jsonb_array_end(JsonBuilder *b);
void jsonb_string(JsonBuilder *b, const char *key, const char *val);
void jsonb_string_escape(JsonBuilder *b, const char *key, const char *val);
void jsonb_number(JsonBuilder *b, const char *key, long val);
void jsonb_bool(JsonBuilder *b, const char *key, bool val);
void jsonb_raw(JsonBuilder *b, const char *key, const char *val);
void jsonb_raw_value(JsonBuilder *b, const char *val);
/* For array items or nested objects without a key */
void jsonb_element_string(JsonBuilder *b, const char *val);
void jsonb_element_number(JsonBuilder *b, long val);
void jsonb_element_bool(JsonBuilder *b, bool val);
void jsonb_element_raw(JsonBuilder *b, const char *val);

#endif /* JSON_BUILDER_H */
