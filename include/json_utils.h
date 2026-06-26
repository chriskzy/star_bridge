#ifndef JSON_UTILS_H
#define JSON_UTILS_H

#include <stdbool.h>
#include <stddef.h>

/* Shared JSON utilities based on cJSON.
 *
 * This is the single authoritative JSON parser module.
 * All ad hoc strstr-based parsing should be migrated to use these functions.
 *
 * For JSON construction, use json_builder.h (JsonBuilder).
 */

/* Parse a JSON string into a cJSON object; returns NULL on error */
void *json_parse(const char *json);

/* Free a parsed JSON object */
void json_free(void *obj);

/* Get a string field from a JSON object into dest; returns true if found */
bool json_get_string(const void *obj, const char *field, char *dest, size_t max_len);

/* Get a pointer to a string field value (no copy). Returns NULL if missing/not string.
 * The pointer is valid only as long as the cJSON object is not freed. */
const char *json_get_string_ptr(const void *obj, const char *field);

/* Get an integer field from a JSON object; returns 0 if missing */
int json_get_int(const void *obj, const char *field);

/* Get a boolean field from a JSON object; returns false if missing */
bool json_get_bool(const void *obj, const char *field);

/* Get a pointer to a nested object field */
const void *json_get_object(const void *obj, const char *field);

/* Get a pointer to an array field */
const void *json_get_array(const void *obj, const char *field);

/* Get the number of items in a JSON array */
int json_array_count(const void *arr);

/* Get an item from a JSON array by index */
const void *json_array_item(const void *arr, int index);

/* Get a string from a JSON array item; returns true if found */
bool json_array_get_string(const void *arr, int index, char *dest, size_t max_len);

/* Validate that a JSON string is a well-formed object */
bool json_is_valid_object(const char *json);

/* Validate that a JSON string has a required field */
bool json_has_field(const char *json, const char *field);

/* Validate that a field has a specific type */
bool json_field_is_string(const char *json, const char *field);

/* Escape a string for inclusion in a JSON string value.
 * Writes escaped version of src into dest, up to max_len bytes.
 * Always NUL-terminates. Handles control chars, quotes, backslash, unicode. */
void json_escape(const char *src, char *dest, size_t max_len);

/* Decode a JSON string value (pointed to by pp, starting at opening quote).
 * Advances pp past the closing quote and writes decoded string into dest.
 * Returns true on success. Handles \n, \t, \\, \", \/ and \\uXXXX escapes. */
bool json_decode_string(const char **pp, char *dest, size_t max_len);

/* Probe first non-whitespace byte of body for diagnostics */
char json_probe_first_byte(const char *body, size_t len);

/* Extract a string field value from a JSON object at the top level.
 * Scans for \"field\":\"value\" pattern and writes value into dest.
 * Returns true if found and value is a string. */
bool json_extract_string(const char *json, const char *field, char *dest, size_t max_len);

/* Extract an integer field value from a JSON object at the top level.
 * Scans for \"field\":<number> pattern and returns the value.
 * Returns fallback if not found. */
int json_extract_int(const char *json, const char *field, int fallback);

/* Extract a boolean field value from a JSON object at the top level.
 * Returns the value if found, otherwise returns fallback. */
bool json_extract_bool(const char *json, const char *field, bool fallback);

/* Check if a string matches any of the provided candidate strings.
 * The argument list must be NULL-terminated.
 * Example: json_string_matches(type, "text", "input_text", NULL) */
bool json_string_matches(const char *str, ...);

#endif /* JSON_UTILS_H */
