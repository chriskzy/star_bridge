#include "capability_router.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Text scanning helpers                                              */
/* ------------------------------------------------------------------ */

/* Check if string starts at position p and is delimited by non-alnum */
static bool str_at_delimited(const char *text, size_t pos, const char *word) {
    size_t wlen = strlen(word);
    /* Check that word fits */
    if (text[pos + wlen - 1] == '\0') return false;
    if (strncmp(text + pos, word, wlen) != 0) return false;
    /* Check preceding character is boundary */
    if (pos > 0) {
        char c = text[pos - 1];
        if (isalnum((unsigned char)c) || c == '_') return false;
    }
    /* Check following character is boundary */
    char c = text[pos + wlen];
    if (c && (isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}

/* Count occurrences of keyword in text as delimited words */
static int count_keyword(const char *text, size_t len, const char *word) {
    int count = 0;
    size_t wlen = strlen(word);
    for (size_t i = 0; i + wlen <= len; i++) {
        if (str_at_delimited(text, i, word)) {
            count++;
            i += wlen - 1;
        }
    }
    return count;
}

/* Check if text contains all required keywords in close proximity (within window) */
static bool has_cooccurrence(const char *text, size_t len,
                             const char *word1, const char *word2,
                             int window) {
    size_t w1len = strlen(word1);
    size_t w2len = strlen(word2);
    for (size_t i = 0; i + w1len <= len; i++) {
        if (!str_at_delimited(text, i, word1)) continue;
        /* Found word1 — search for word2 within window forward */
        size_t start = i + w1len;
        size_t end = start + window;
        if (end > len) end = len;
        for (size_t j = start; j + w2len <= end; j++) {
            if (str_at_delimited(text, j, word2)) return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Negative patterns — accidental mentions that should NOT trigger    */
/* ------------------------------------------------------------------ */

/* Patterns that look like code, URLs, or UI references */
static const char *s_negative_image_patterns[] = {
    "<img", "src=\"", "src='", "png", "jpg", "jpeg", "gif", "svg",
    "icon", "favicon", "avatar", "thumbnail", "screenshot",
    "data:image", "base64", ".png", ".jpg", ".jpeg", ".gif", ".svg",
    "image/png", "image/jpeg", "image/gif", "image/svg",
    "alt=\"", "alt='",
    "background-image", "background: url", "background:url",
    "url(", "url('", "url(\"",
    "```", "def ", "function ", "const ", "let ", "var ",
    "import ", "require(", "<svg", "<canvas",
    NULL
};

static const char *s_negative_computer_patterns[] = {
    "```", "def ", "function ", "const ", "let ", "var ",
    "computer_science", "computer vision", "computer graphics",
    "computation", "computational", "compute",
    NULL
};

/* ------------------------------------------------------------------ */
/*  Image generation intent signals                                     */
/* ------------------------------------------------------------------ */

/* Strong image generation keywords — explicit generation intent */
static const char *s_image_strong_keywords[] = {
    "generate_image", "generate image", "create_image", "create image",
    "draw_image", "draw image", "make_image", "make image",
    "generate picture", "create picture", "draw picture",
    "generate photo", "create photo",
    "generate an image", "create an image", "draw an image",
    "generate a picture", "create a picture",
    "image generation", "image generator",
    "text_to_image", "text to image", "txt2img",
    "imagine", "imagine a", "imagine an",
    "visualize", "visualise",
    "dall-e", "dall e", "dalle", "midjourney", "stable diffusion",
    "stability ai", "openai image",
    NULL
};

/* Co-occurrence pairs for weaker image intent */
static const char *s_image_co_pair1[] = { "generate", "create", "draw", "make", "render", "produce", NULL };
static const char *s_image_co_pair2[] = { "image", "picture", "photo", "photograph", "illustration", "artwork", "visual", NULL };

/* ------------------------------------------------------------------ */
/*  Computer use intent signals                                         */
/* ------------------------------------------------------------------ */

static const char *s_computer_strong_keywords[] = {
    "computer_call", "computer use", "use computer",
    "control computer", "control desktop", "control browser",
    "desktop control", "browser control",
    "click", "type on", "move mouse", "press key",
    "take screenshot", "take a screenshot", "screenshot the", "screenshot of",
    "scroll", "scroll down", "scroll up",
    "open browser", "open application", "open app",
    "use the desktop", "use desktop",
    "operate computer", "operate desktop",
    NULL
};

/* Co-occurrence: action + computer/desktop/browser */
static const char *s_computer_action[] = { "use", "control", "operate", "open", "run", "launch", "click", "type", "scroll", NULL };
static const char *s_computer_target[] = { "computer", "desktop", "browser", "application", "app", "terminal", "window", NULL };

/* ------------------------------------------------------------------ */
/*  Web search intent signals — NOT USED: bridge does not support      */
/*  browser/search. Agent handles browsing natively.                   */

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* Check if input has negative patterns that should suppress a capability.
 * Uses delimited word matching to avoid false positives from substring matches. */
static bool has_negative_pattern(const char *input, size_t len,
                                 const char **patterns) {
    (void)len;
    for (int i = 0; patterns[i]; i++) {
        /* Use strstr for simple substring patterns like code markers */
        if (count_keyword(input, len, patterns[i]) > 0) return true;
    }
    return false;
}

/* Count strong keywords in input */
static int count_strong_keywords(const char *input, size_t len,
                                 const char **keywords) {
    int total = 0;
    for (int i = 0; keywords[i]; i++) {
        total += count_keyword(input, len, keywords[i]);
    }
    return total;
}

/* Analyze user input for image generation intent */
static RouteConfidence analyze_image_intent(const char *input, size_t len) {
    /* Check negative patterns first — if present, deny unless strong keywords */
    int neg_count = has_negative_pattern(input, len, s_negative_image_patterns) ? 1 : 0;

    /* Count strong image keywords */
    int strong = count_strong_keywords(input, len, s_image_strong_keywords);
    if (strong > 0) {
        /* Even with negative patterns, strong intent wins */
        return ROUTE_CONFIRMED;
    }

    /* Check co-occurrence: action verb near image/picture/photo */
    bool cooc = false;
    for (int i = 0; s_image_co_pair1[i]; i++) {
        for (int j = 0; s_image_co_pair2[j]; j++) {
            if (has_cooccurrence(input, len, s_image_co_pair1[i],
                                 s_image_co_pair2[j], 80)) {
                cooc = true;
                break;
            }
        }
        if (cooc) break;
    }

    if (cooc && !neg_count) {
        return ROUTE_CONFIRMED;
    }

    if (cooc && neg_count) {
        return ROUTE_WEAK;
    }

    /* Check for lone image/picture mentions (weak) */
    int img_count = count_keyword(input, len, "image") +
                    count_keyword(input, len, "picture") +
                    count_keyword(input, len, "photo");
    if (img_count > 0 && !neg_count) {
        return ROUTE_WEAK;
    }

    return ROUTE_DENIED;
}

/* Analyze user input for computer use intent */
static RouteConfidence analyze_computer_intent(const char *input, size_t len) {
    /* Check negative patterns */
    int neg = has_negative_pattern(input, len, s_negative_computer_patterns) ? 1 : 0;

    int strong = count_strong_keywords(input, len, s_computer_strong_keywords);
    if (strong > 0) {
        return ROUTE_CONFIRMED;
    }

    /* Check co-occurrence */
    bool cooc = false;
    for (int i = 0; s_computer_action[i]; i++) {
        for (int j = 0; s_computer_target[j]; j++) {
            if (has_cooccurrence(input, len, s_computer_action[i],
                                 s_computer_target[j], 60)) {
                cooc = true;
                break;
            }
        }
        if (cooc) break;
    }

    if (cooc && !neg) {
        return ROUTE_CONFIRMED;
    }

    if (cooc && neg) {
        return ROUTE_WEAK;
    }

    return ROUTE_DENIED;
}

/* Web search intent analysis is not used — bridge does not support browser/search */

CapabilityFlags capability_analyze_input(const char *input, size_t input_len) {
    if (!input || input_len == 0) return CAP_NONE;

    CapabilityFlags flags = CAP_NONE;

    if (analyze_image_intent(input, input_len) >= ROUTE_CONFIRMED)
        flags |= CAP_IMAGE;
    if (analyze_computer_intent(input, input_len) >= ROUTE_CONFIRMED)
        flags |= CAP_COMPUTER;
    /* Web search is not supported by bridge — agent handles browsing natively */
    return flags;
}

bool capability_input_has_intent(const char *input, size_t input_len,
                                 CapabilityFlags cap) {
    if (!input || input_len == 0) return false;

    switch (cap) {
        case CAP_IMAGE:
            return analyze_image_intent(input, input_len) >= ROUTE_CONFIRMED;
        case CAP_COMPUTER:
            return analyze_computer_intent(input, input_len) >= ROUTE_CONFIRMED;
        case CAP_WEB_SEARCH:
            /* Web search is not supported by bridge — agent handles browsing natively */
            return false;
        default:
            return false;
    }
}

/* ------------------------------------------------------------------ */
/*  Tool definition analysis                                            */
/* ------------------------------------------------------------------ */

/* Check if tool definitions include a specific capability type */
static bool tools_have_type(const NormalizedToolDef *tools, int count, const char *type) {
    for (int i = 0; i < count; i++) {
        if (strcmp(tools[i].type, type) == 0) return true;
    }
    return false;
}

/* Check if tool definitions include a specific normalized name */
static bool tools_have_name(const NormalizedToolDef *tools, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(tools[i].normalized_name, name) == 0) return true;
    }
    return false;
}

/* Build a single route entry */
static void build_route(RoutingDecision *dec, CapabilityFlags cap,
                        RouteConfidence conf, const char *reason) {
    if (dec->route_count >= 16) return;
    CapabilityRoute *r = &dec->routes[dec->route_count++];
    r->capability = cap;
    r->confidence = conf;
    snprintf(r->reason, sizeof(r->reason), "%s", reason);
    if (conf >= ROUTE_CONFIRMED) {
        dec->active_capabilities |= cap;
    }
}

RoutingDecision capability_route_tools(const NormalizedToolDef *tools, int tool_count,
                                       const ToolChoiceSpec *tc_spec,
                                       const char *user_input, size_t input_len) {
    RoutingDecision dec;
    memset(&dec, 0, sizeof(dec));

    /* --- Image capability --- */
    RouteConfidence img_conf = ROUTE_DENIED;
    const char *img_reason = "no intent detected";

    /* Check tool_choice first — strongest signal */
    if (tc_spec && tc_spec->mode == TOOL_CHOICE_NAMED) {
        const char *named = tc_spec->named_tool;
        /* Check if named tool is an image tool */
        if (strcmp(named, "image_generation") == 0 ||
            strcmp(named, "dall-e") == 0 ||
            strcmp(named, "dalle") == 0) {
            img_conf = ROUTE_CONFIRMED;
            img_reason = "tool_choice explicitly names image tool";
        } else if (tools_have_name(tools, tool_count, named) &&
                   strcmp(named, "image_generation") == 0) {
            img_conf = ROUTE_CONFIRMED;
            img_reason = "tool_choice names normalized image tool";
        }
    }

    /* Check tool definitions for image type */
    if (img_conf < ROUTE_CONFIRMED) {
        if (tools_have_type(tools, tool_count, "image") ||
            tools_have_type(tools, tool_count, "image_generation")) {
            /* Type present but need user intent confirmation */
            RouteConfidence input_conf = analyze_image_intent(user_input, input_len);
            if (input_conf >= ROUTE_CONFIRMED) {
                img_conf = ROUTE_CONFIRMED;
                img_reason = "image tool type defined with strong user intent";
            } else if (input_conf >= ROUTE_WEAK) {
                img_conf = ROUTE_WEAK;
                img_reason = "image tool type defined but user intent is weak";
            } else {
                img_conf = ROUTE_WEAK;
                img_reason = "image tool type defined but no user intent — conservative deny";
            }
        }
    }

    /* Check user input alone if no tool signals */
    if (img_conf < ROUTE_WEAK) {
        RouteConfidence input_conf = analyze_image_intent(user_input, input_len);
        if (input_conf >= ROUTE_CONFIRMED) {
            img_conf = ROUTE_CONFIRMED;
            img_reason = "strong user intent for image generation";
        } else if (input_conf >= ROUTE_WEAK) {
            img_conf = ROUTE_WEAK;
            img_reason = "weak user intent — not activating without explicit tool_choice";
        }
    }

    build_route(&dec, CAP_IMAGE, img_conf, img_reason);

    /* --- Computer capability --- */
    RouteConfidence comp_conf = ROUTE_DENIED;
    const char *comp_reason = "no intent detected";

    if (tc_spec && tc_spec->mode == TOOL_CHOICE_NAMED) {
        const char *named = tc_spec->named_tool;
        if (strcmp(named, "computer") == 0 ||
            strcmp(named, "computer_call") == 0) {
            comp_conf = ROUTE_CONFIRMED;
            comp_reason = "tool_choice explicitly names computer tool";
        }
    }

    if (comp_conf < ROUTE_CONFIRMED) {
        if (tools_have_type(tools, tool_count, "computer")) {
            RouteConfidence input_conf = analyze_computer_intent(user_input, input_len);
            if (input_conf >= ROUTE_CONFIRMED) {
                comp_conf = ROUTE_CONFIRMED;
                comp_reason = "computer tool type defined with strong user intent";
            } else if (input_conf >= ROUTE_WEAK) {
                comp_conf = ROUTE_WEAK;
                comp_reason = "computer tool type defined but user intent is weak";
            } else {
                comp_conf = ROUTE_WEAK;
                comp_reason = "computer tool type defined but no user intent — conservative deny";
            }
        }
    }

    if (comp_conf < ROUTE_WEAK) {
        RouteConfidence input_conf = analyze_computer_intent(user_input, input_len);
        if (input_conf >= ROUTE_CONFIRMED) {
            comp_conf = ROUTE_CONFIRMED;
            comp_reason = "strong user intent for computer use";
        }
    }

    build_route(&dec, CAP_COMPUTER, comp_conf, comp_reason);

    /* --- Web search capability --- */
    /* Web search is not supported by bridge — agent handles browsing natively */
    build_route(&dec, CAP_WEB_SEARCH, ROUTE_DENIED, "not supported by bridge; agent handles browsing natively");

    return dec;
}

void capability_decision_summary(const RoutingDecision *dec,
                                 char *dest, size_t max_len) {
    if (!dec || !dest || max_len == 0) return;
    dest[0] = '\0';
    size_t off = 0;

    for (int i = 0; i < dec->route_count; i++) {
        const CapabilityRoute *r = &dec->routes[i];
        const char *cap_str = "unknown";
        if (r->capability == CAP_IMAGE) cap_str = "image";
        else if (r->capability == CAP_COMPUTER) cap_str = "computer";
        else if (r->capability == CAP_WEB_SEARCH) cap_str = "web_search";

        const char *conf_str = "denied";
        if (r->confidence == ROUTE_WEAK) conf_str = "weak";
        else if (r->confidence == ROUTE_CONFIRMED) conf_str = "confirmed";

        size_t needed = off + strlen(cap_str) + strlen(": ") +
                        strlen(conf_str) + strlen(" (") +
                        strlen(r->reason) + strlen(")\n") + 1;
        if (needed >= max_len) break;

        off += snprintf(dest + off, max_len - off, "%s: %s (%s)\n",
                        cap_str, conf_str, r->reason);
    }

    if (off > 0 && dest[off - 1] == '\n') {
        dest[off - 1] = '\0'; /* trim trailing newline */
    }
}
