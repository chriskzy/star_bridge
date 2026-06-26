#include "native_response.h"
#include "json_utils.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------
 *  Type mapping
 * ------------------------------------------------------------------- */

NativeEventType native_event_type_from_string(const char *type_str) {
    if (!type_str || !type_str[0]) return NATIVE_EVENT_UNKNOWN;

    if (strcmp(type_str, "ack") == 0)            return NATIVE_EVENT_ACK;
    if (strcmp(type_str, "response") == 0)       return NATIVE_EVENT_RESPONSE;
    if (strcmp(type_str, "text_delta") == 0)     return NATIVE_EVENT_TEXT_DELTA;
    if (strcmp(type_str, "tool_intent") == 0)    return NATIVE_EVENT_TOOL_INTENT;
    if (strcmp(type_str, "error") == 0)          return NATIVE_EVENT_ERROR;
    if (strcmp(type_str, "health") == 0)         return NATIVE_EVENT_HEALTH;
    if (strcmp(type_str, "ping") == 0)           return NATIVE_EVENT_PING;
    if (strcmp(type_str, "pong") == 0)           return NATIVE_EVENT_PONG;
    if (strcmp(type_str, "ready") == 0)          return NATIVE_EVENT_READY;
    if (strcmp(type_str, "shutdown") == 0)       return NATIVE_EVENT_SHUTDOWN;
    if (strcmp(type_str, "cancelled") == 0)      return NATIVE_EVENT_CANCELLED;
    if (strcmp(type_str, "shutdown_ack") == 0)   return NATIVE_EVENT_SHUTDOWN_ACK;

    /* Compaction events */
    if (strcmp(type_str, "compaction.started") == 0)    return NATIVE_EVENT_COMPACTION;
    if (strcmp(type_str, "compaction.summary") == 0)    return NATIVE_EVENT_COMPACTION;
    if (strcmp(type_str, "compaction.completed") == 0)  return NATIVE_EVENT_COMPACTION;

    return NATIVE_EVENT_UNKNOWN;
}

/* -------------------------------------------------------------------
 *  Lifecycle check
 * ------------------------------------------------------------------- */

bool native_event_is_lifecycle(const NativeEvent *event) {
    if (!event) return false;
    switch (event->type) {
        case NATIVE_EVENT_HEALTH:
        case NATIVE_EVENT_PING:
        case NATIVE_EVENT_PONG:
        case NATIVE_EVENT_READY:
        case NATIVE_EVENT_SHUTDOWN:
        case NATIVE_EVENT_SHUTDOWN_ACK:
            return true;
        default:
            return false;
    }
}

/* -------------------------------------------------------------------
 *  Extract a string field from a parsed cJSON object.
 *  Returns the decoded value; empty string if not found.
 * ------------------------------------------------------------------- */
static void get_field_string(const void *obj, const char *field, char *dest, size_t max_len) {
    if (!obj || !field || !dest) return;
    json_get_string(obj, field, dest, max_len);
}

/* -------------------------------------------------------------------
 *  Extract nested tool intent from a parsed cJSON object.
 *  Looks for "tool":{"name":"...","args":...} or "tool":{"name":"...","input":...}
 * ------------------------------------------------------------------- */
static void extract_tool_from_obj(const void *obj, char *tool_name, size_t name_max,
                                   char *tool_args, size_t args_max) {
    if (!obj || !tool_name || !tool_args) return;
    tool_name[0] = '\0';
    tool_args[0] = '\0';

    const void *tool_obj = json_get_object(obj, "tool");
    if (!tool_obj) return;

    get_field_string(tool_obj, "name", tool_name, name_max);

    /* Try "args" first, then "input" */
    char buf[4096];
    get_field_string(tool_obj, "args", buf, sizeof(buf));
    if (buf[0]) {
        snprintf(tool_args, args_max, "%s", buf);
        return;
    }
    get_field_string(tool_obj, "input", buf, sizeof(buf));
    if (buf[0]) {
        snprintf(tool_args, args_max, "%s", buf);
        return;
    }

    /* If args/input is a JSON object (not a string), serialize it back */
    const void *args_obj = json_get_object(tool_obj, "args");
    if (args_obj) {
        /* We need to re-serialize the args object */
        /* For now, fall back to strstr extraction of the args block */
        /* Actually, we can use cJSON_Print on the args object */
        /* But that's a cJSON-specific API. Let's use a simple fallback. */
        /* Since we're in the migration, for object args we'll use json_extract_string
         * on the raw frame to get the args field as a raw JSON substring. */
        /* Best: leave as empty and caller will use raw frame for tool args. */
        tool_args[0] = '\0';
        return;
    }
}

/* -------------------------------------------------------------------
 *  Main parser
 * ------------------------------------------------------------------- */

bool native_parse_frame(const char *frame, size_t len, NativeEvent *event) {
    if (!frame || !event) return false;

    /* Zero-initialize the event */
    memset(event, 0, sizeof(*event));

    /* Parse the frame with cJSON */
    void *obj = json_parse(frame);
    if (!obj) return false;

    /* Extract type string */
    char type_str[64] = {0};
    json_get_string(obj, "type", type_str, sizeof(type_str));
    if (!type_str[0]) {
        json_free(obj);
        return false;
    }
    snprintf(event->type_str, sizeof(event->type_str), "%s", type_str);
    event->type = native_event_type_from_string(type_str);

    /* Extract common id field */
    json_get_string(obj, "id", event->id, sizeof(event->id));

    /* Route by type */
    switch (event->type) {
        case NATIVE_EVENT_RESPONSE: {
            NativeResponseEvent *r = &event->data.response;
            json_get_string(obj, "id", r->id, sizeof(r->id));
            json_get_string(obj, "status", r->status, sizeof(r->status));
            /* Extract assistant text; framed native agents may send "text" or "output". */
            json_get_string(obj, "text", r->text, sizeof(r->text));
            if (!r->text[0]) {
                json_get_string(obj, "output", r->text, sizeof(r->text));
            }
            /* Extract tool_intent if present */
            cJSON *ti_obj = cJSON_GetObjectItem((cJSON *)obj, "tool_intent");
            if (ti_obj) {
                NativeToolIntentEvent *t = &event->data.tool_intent;
                event->type = NATIVE_EVENT_TOOL_INTENT;
                json_get_string(obj, "id", t->id, sizeof(t->id));
                json_get_string(ti_obj, "name", t->tool_name, sizeof(t->tool_name));
                cJSON *args_item = cJSON_GetObjectItem(ti_obj, "arguments");
                if (args_item) {
                    if (cJSON_IsString(args_item)) {
                        snprintf(t->tool_args, sizeof(t->tool_args), "%s", args_item->valuestring);
                    } else {
                        char *raw = cJSON_PrintUnformatted(args_item);
                        if (raw) {
                            snprintf(t->tool_args, sizeof(t->tool_args), "%s", raw);
                            cJSON_free(raw);
                        }
                    }
                }

                break;
            }
            /* Extract usage fields from native response */
            r->prompt_tokens = json_get_int(obj, "prompt_tokens");
            r->completion_tokens = json_get_int(obj, "completion_tokens");
            /* Extract "usage" sub-object if present (may contain cache details) */
            {
                const void *usage_obj = json_get_object(obj, "usage");
                if (usage_obj) {
                    char ubuf[4096];
                    json_get_string(obj, "usage", ubuf, sizeof(ubuf));
                    if (ubuf[0]) {
                        snprintf(r->usage_json, sizeof(r->usage_json), "%s", ubuf);
                    } else {
                        /* usage is an object — extract via strstr from raw frame */
                        const char *usage_start = strstr(frame, "\"usage\":");
                        if (usage_start) {
                            size_t off = usage_start - frame + 8;
                            size_t rem = len > off ? len - off : 0;
                            size_t cp = rem;
                            if (cp > sizeof(r->usage_json) - 1)
                                cp = sizeof(r->usage_json) - 1;
                            memcpy(r->usage_json, frame + off, cp);
                            r->usage_json[cp] = '\0';
                            /* Find end of usage object */
                            int depth = 1;
                            char *p = r->usage_json;
                            while (*p && depth > 0) {
                                if (*p == '{') depth++;
                                if (*p == '}') depth--;
                                if (depth == 0) { *(p + 1) = '\0'; break; }
                                p++;
                            }
                        }
                    }
                }
            }
            break;
        }

        case NATIVE_EVENT_TEXT_DELTA: {
            NativeResponseEvent *r = &event->data.response;
            json_get_string(obj, "id", r->id, sizeof(r->id));
            /* text or delta for the incremental chunk */
            json_get_string(obj, "text", r->text, sizeof(r->text));
            if (!r->text[0]) {
                json_get_string(obj, "delta", r->text, sizeof(r->text));
            }
            break;
        }

        case NATIVE_EVENT_TOOL_INTENT: {
            NativeToolIntentEvent *t = &event->data.tool_intent;
            json_get_string(obj, "id", t->id, sizeof(t->id));
            extract_tool_from_obj(obj, t->tool_name, sizeof(t->tool_name),
                                   t->tool_args, sizeof(t->tool_args));
            break;
        }

        case NATIVE_EVENT_ERROR: {
            NativeErrorEvent *e = &event->data.error;
            json_get_string(obj, "id", e->id, sizeof(e->id));
            json_get_string(obj, "status", e->status, sizeof(e->status));
            json_get_string(obj, "message", e->message, sizeof(e->message));
            break;
        }

        case NATIVE_EVENT_COMPACTION: {
            NativeCompactionEvent *c = &event->data.compaction;
            snprintf(c->type, sizeof(c->type), "%s", type_str);
            /* Compaction events have "message" or "text" or "summary" field */
            json_get_string(obj, "message", c->message, sizeof(c->message));
            if (!c->message[0]) {
                json_get_string(obj, "text", c->message, sizeof(c->message));
            }
            if (!c->message[0]) {
                json_get_string(obj, "summary", c->message, sizeof(c->message));
            }
            break;
        }

        /* Lifecycle events need no special data */
        case NATIVE_EVENT_ACK: {
            NativeResponseEvent *r = &event->data.response;
            json_get_string(obj, "status", r->status, sizeof(r->status));
            break;
        }
        case NATIVE_EVENT_HEALTH:
        case NATIVE_EVENT_PING:
        case NATIVE_EVENT_PONG:
        case NATIVE_EVENT_READY:
        case NATIVE_EVENT_SHUTDOWN:
        case NATIVE_EVENT_CANCELLED:
        case NATIVE_EVENT_SHUTDOWN_ACK:
            /* type_str and id already set; no extra data needed */
            break;

        case NATIVE_EVENT_UNKNOWN:
        default:
            /* Unknown type — still valid JSON; caller can inspect raw frame */
            break;
    }

    json_free(obj);
    return true;
}
