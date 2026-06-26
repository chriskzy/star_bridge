#ifndef CAPABILITY_ROUTER_H
#define CAPABILITY_ROUTER_H

#include "codex_tool_normalizer.h"

#include <stdbool.h>
#include <stddef.h>

/* Capability types that can be routed */
typedef enum {
    CAP_NONE           = 0,
    CAP_IMAGE          = 1 << 0,  /* image generation / vision */
    CAP_COMPUTER       = 1 << 1,  /* computer use / desktop control */
    CAP_WEB_SEARCH     = 1 << 2,  /* web search */
    CAP_ALL            = 0xFFFF,
} CapabilityFlags;

/* Routing confidence levels */
typedef enum {
    ROUTE_DENIED   = 0,   /* Capability should not be activated */
    ROUTE_WEAK     = 1,   /* Low confidence — do not activate without explicit tool_choice */
    ROUTE_CONFIRMED = 2,  /* Strong intent — safe to activate */
} RouteConfidence;

/* Result of routing analysis for a single capability */
typedef struct {
    CapabilityFlags capability;
    RouteConfidence confidence;
    char reason[256];      /* Human-readable reason for the decision */
} CapabilityRoute;

/* Full routing decision for all capabilities */
typedef struct {
    CapabilityFlags active_capabilities;   /* Bitmask of capabilities to activate */
    int route_count;
    CapabilityRoute routes[16];            /* Per-capability routing decisions */
} RoutingDecision;

/* Analyze user input for capability intent signals.
 * Scans input for explicit keywords while filtering out accidental
 * mentions (code, UI icons, image URLs, etc.).
 * Returns a bitmask of capabilities to activate based on input alone. */
CapabilityFlags capability_analyze_input(const char *input, size_t input_len);

/* Analyze tool definitions and tool_choice for capability signals.
 * Returns a RoutingDecision consolidating all evidence. */
RoutingDecision capability_route_tools(const NormalizedToolDef *tools, int tool_count,
                                       const ToolChoiceSpec *tc_spec,
                                       const char *user_input, size_t input_len);

/* Check if a specific capability is active in a decision */
static inline bool capability_is_active(const RoutingDecision *dec, CapabilityFlags cap) {
    return dec && (dec->active_capabilities & cap) != 0;
}

/* Check if a specific capability has confirmed confidence */
static inline bool capability_is_confirmed(const RoutingDecision *dec, CapabilityFlags cap) {
    if (!dec) return false;
    for (int i = 0; i < dec->route_count; i++) {
        if (dec->routes[i].capability == cap &&
            dec->routes[i].confidence >= ROUTE_CONFIRMED)
            return true;
    }
    return false;
}

/* Check if user input contains explicit intent for a capability,
 * filtering out accidental mentions. */
bool capability_input_has_intent(const char *input, size_t input_len,
                                 CapabilityFlags cap);

/* Return human-readable summary of routing decision */
void capability_decision_summary(const RoutingDecision *dec,
                                 char *dest, size_t max_len);

#endif /* CAPABILITY_ROUTER_H */
