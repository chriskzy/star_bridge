#ifndef CODEX_TOOL_NORMALIZER_H
#define CODEX_TOOL_NORMALIZER_H

#include "json_utils.h"
#include "cJSON.h"

#include <stdbool.h>
#include <stddef.h>

/* Maximum number of tool definitions */
#define MAX_TOOL_DEFS 64

/* Tool choice modes */
typedef enum {
    TOOL_CHOICE_NONE = 0,    /* tool_choice: "none" */
    TOOL_CHOICE_AUTO = 1,    /* tool_choice: "auto" / not specified */
    TOOL_CHOICE_REQUIRED = 2,/* tool_choice: "required" / "any" */
    TOOL_CHOICE_NAMED = 3,   /* tool_choice: {"type": "function", "function": {"name": "..."}} */
} ToolChoiceMode;

/* A single tool definition after normalization */
typedef struct {
    char original_name[128];       /* Original name from the request */
    char normalized_name[128];     /* Bridge-owned stable name */
    char type[32];                 /* "function", "computer", "web_search", etc. */
    char description[4096];        /* Description */
    char input_schema_json[16384]; /* JSON schema as string */
    bool is_bridge_owned;          /* True if this tool is mapped to a bridge-owned schema */
} NormalizedToolDef;

/* Tool choice specification */
typedef struct {
    ToolChoiceMode mode;
    char named_tool[128];          /* Tool name for TOOL_CHOICE_NAMED */
} ToolChoiceSpec;

/* Parse and normalize tools from a request body JSON object */
int codex_normalize_tools(cJSON *body, NormalizedToolDef *tools, int max_tools);

/* Resolve tool_choice from request body */
ToolChoiceSpec codex_resolve_tool_choice(cJSON *body);

/* Look up a normalized tool definition by name */
const NormalizedToolDef *codex_find_normalized_tool(const NormalizedToolDef *tools, int count, const char *name);

/* Map a Responses tool type/name to a bridge-owned stable function schema name.
 * Returns the normalized name, or the original if no mapping exists. */
const char *codex_normalize_tool_name(const char *type, const char *name);

/* Build the native agent tool prompt from normalized tools */
void codex_build_tool_prompt(const NormalizedToolDef *tools, int count,
                             char *dest, size_t max_len);

#endif /* CODEX_TOOL_NORMALIZER_H */
