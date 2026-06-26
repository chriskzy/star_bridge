#include "capability_router.h"
#include "codex_tool_normalizer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", name); \
        tests_failed++; \
    } else { \
        fprintf(stdout, "PASS: %s\n", name); \
        tests_passed++; \
    } \
} while(0)

/* Helper: analyze input for image intent */
static void test_image_positive(void) {
    /* Strong image keywords */
    TEST("image-strong: generate_image",
         capability_input_has_intent("please generate_image of a cat", 31, CAP_IMAGE));
    TEST("image-strong: create image",
         capability_input_has_intent("create image of a sunset", 23, CAP_IMAGE));
    TEST("image-strong: draw picture",
         capability_input_has_intent("draw picture of a house", 23, CAP_IMAGE));
    TEST("image-strong: dall-e",
         capability_input_has_intent("use dall-e to create an image", 32, CAP_IMAGE));
    TEST("image-strong: imagine",
         capability_input_has_intent("imagine a beautiful landscape", 31, CAP_IMAGE));
    TEST("image-strong: visualize",
         capability_input_has_intent("visualize the data as an image", 31, CAP_IMAGE));
    TEST("image-strong: generate an image",
         capability_input_has_intent("please generate an image of a dog", 34, CAP_IMAGE));

    /* Co-occurrence: action verb near image/picture */
    TEST("image-cooc: make image",
         capability_input_has_intent("can you make an image of a cat", 30, CAP_IMAGE));
    TEST("image-cooc: create picture",
         capability_input_has_intent("please create a picture for me", 30, CAP_IMAGE));
    TEST("image-cooc: render photo",
         capability_input_has_intent("render a photo of the scene", 28, CAP_IMAGE));
}

static void test_image_negative(void) {
    /* Code/UI mentions should NOT activate image capability */
    TEST("image-neg: img tag in html",
         !capability_input_has_intent("<img src='icon.png' alt='icon'>", 32, CAP_IMAGE));
    TEST("image-neg: image URL in code",
         !capability_input_has_intent("const url = 'https://example.com/image.jpg'", 44, CAP_IMAGE));
    TEST("image-neg: png extension",
         !capability_input_has_intent("file.png", 8, CAP_IMAGE));
    TEST("image-neg: icon mention",
         !capability_input_has_intent("update the icon color to red", 29, CAP_IMAGE));
    TEST("image-neg: css background",
         !capability_input_has_intent("background-image: url('bg.jpg')", 33, CAP_IMAGE));
    TEST("image-neg: base64 data",
         !capability_input_has_intent("data:image/png;base64,iVBOR", 28, CAP_IMAGE));
    TEST("image-neg: code function",
         !capability_input_has_intent("def process_image(image_path):", 32, CAP_IMAGE));
    TEST("image-neg: svg in code",
         !capability_input_has_intent("<svg width='100' height='100'>", 30, CAP_IMAGE));
    TEST("image-neg: alt attribute",
         !capability_input_has_intent("alt=\"Photo of a cat\"", 21, CAP_IMAGE));
    TEST("image-neg: import image",
         !capability_input_has_intent("import Image from 'next/image'", 30, CAP_IMAGE));
    TEST("image-neg: just mention image without intent",
         !capability_input_has_intent("The image shows the network topology", 39, CAP_IMAGE));
    TEST("image-neg: screenshot mention alone",
         !capability_input_has_intent("Here is a screenshot of the error", 34, CAP_IMAGE));
    TEST("image-neg: code block with image",
         !capability_input_has_intent("```\n<img src='test.png'>\n```", 29, CAP_IMAGE));

    /* Edge cases: image mentioned but not generation intent */
    TEST("image-neg: image in code review",
         !capability_input_has_intent("review the image processing code", 34, CAP_IMAGE));
    TEST("image-neg: image dimension",
         !capability_input_has_intent("set image dimensions to 800x600", 32, CAP_IMAGE));
}

static void test_computer_positive(void) {
    TEST("computer-strong: computer_call",
         capability_input_has_intent("use computer_call to click", 29, CAP_COMPUTER));
    TEST("computer-strong: use computer",
         capability_input_has_intent("use the computer to open browser", 33, CAP_COMPUTER));
    TEST("computer-strong: desktop control",
         capability_input_has_intent("control the desktop and click", 29, CAP_COMPUTER));
    TEST("computer-strong: take screenshot",
         capability_input_has_intent("take a screenshot of the desktop", 32, CAP_COMPUTER));
    TEST("computer-strong: scroll down",
         capability_input_has_intent("scroll down the page", 19, CAP_COMPUTER));
    TEST("computer-strong: open browser",
         capability_input_has_intent("open the browser and search", 27, CAP_COMPUTER));
    TEST("computer-strong: click the",
         capability_input_has_intent("click the button at coordinates", 30, CAP_COMPUTER));

    /* Co-occurrence */
    TEST("computer-cooc: use computer",
         capability_input_has_intent("please use the computer to help", 30, CAP_COMPUTER));
    TEST("computer-cooc: control desktop",
         capability_input_has_intent("control the desktop for me", 25, CAP_COMPUTER));
}

static void test_computer_negative(void) {
    /* Code mentions should NOT activate */
    TEST("computer-neg: computer_science",
         !capability_input_has_intent("I am studying computer science", 30, CAP_COMPUTER));
    TEST("computer-neg: computer vision",
         !capability_input_has_intent("computer vision algorithms", 27, CAP_COMPUTER));
    TEST("computer-neg: computational",
         !capability_input_has_intent("computational complexity theory", 30, CAP_COMPUTER));
    TEST("computer-neg: compute resources",
         !capability_input_has_intent("compute resources needed", 23, CAP_COMPUTER));
    TEST("computer-neg: code with computer",
         !capability_input_has_intent("```\ndef compute(x):\n    return x\n```", 38, CAP_COMPUTER));
    TEST("computer-neg: computer in function",
         !capability_input_has_intent("function computer_vision_detect()", 35, CAP_COMPUTER));

    /* Just mentioning computer without intent */
    TEST("computer-neg: my computer",
         !capability_input_has_intent("My computer is running slow", 26, CAP_COMPUTER));
}

static void test_search_positive(void) {
    /* Web search is not supported by bridge — intent always returns false regardless of input */
    TEST("search-strong: google_search",
         !capability_input_has_intent("use google_search to find", 26, CAP_WEB_SEARCH));
    TEST("search-strong: web_search",
         !capability_input_has_intent("use web_search for news", 23, CAP_WEB_SEARCH));
    TEST("search-strong: search the web",
         !capability_input_has_intent("search the web for current events", 33, CAP_WEB_SEARCH));
    TEST("search-strong: look up",
         !capability_input_has_intent("look up the latest news", 23, CAP_WEB_SEARCH));
    TEST("search-strong: search for",
         !capability_input_has_intent("search for python tutorials", 27, CAP_WEB_SEARCH));
    TEST("search-strong: find online",
         !capability_input_has_intent("find online resources about", 25, CAP_WEB_SEARCH));
    TEST("search-strong: browse internet",
         !capability_input_has_intent("browse the internet for info", 27, CAP_WEB_SEARCH));
    TEST("search-strong: what is the",
         !capability_input_has_intent("what is the capital of France", 29, CAP_WEB_SEARCH));
    TEST("search-strong: latest news",
         !capability_input_has_intent("get the latest news about AI", 27, CAP_WEB_SEARCH));
}

static void test_search_negative(void) {
    /* All web search intents are false — bridge does not support web search */
    TEST("search-neg: find in code",
         !capability_input_has_intent("find the bug in this code", 25, CAP_WEB_SEARCH));
    TEST("search-neg: search directory",
         !capability_input_has_intent("search the directory for files", 29, CAP_WEB_SEARCH));
    TEST("search-neg: look up in docs",
         !capability_input_has_intent("look up the function in the docs", 31, CAP_WEB_SEARCH));
    TEST("search-neg: find function",
         !capability_input_has_intent("find the definition of the function", 34, CAP_WEB_SEARCH));
    TEST("search-neg: browse files",
         !capability_input_has_intent("browse the project files", 23, CAP_WEB_SEARCH));
}

static void test_tool_choice_routing(void) {
    NormalizedToolDef tools[16];
    ToolChoiceSpec tc_spec;

    /* Test: named tool_choice for image */
    memset(&tc_spec, 0, sizeof(tc_spec));
    tc_spec.mode = TOOL_CHOICE_NAMED;
    snprintf(tc_spec.named_tool, sizeof(tc_spec.named_tool), "%s", "image_generation");

    RoutingDecision dec = capability_route_tools(NULL, 0, &tc_spec, "", 0);
    TEST("tc-named-image: active",
         capability_is_active(&dec, CAP_IMAGE));
    TEST("tc-named-image: confirmed",
         capability_is_confirmed(&dec, CAP_IMAGE));

    /* Test: named tool_choice for computer */
    memset(&tc_spec, 0, sizeof(tc_spec));
    tc_spec.mode = TOOL_CHOICE_NAMED;
    snprintf(tc_spec.named_tool, sizeof(tc_spec.named_tool), "%s", "computer_call");

    dec = capability_route_tools(NULL, 0, &tc_spec, "", 0);
    TEST("tc-named-computer: active",
         capability_is_active(&dec, CAP_COMPUTER));
    TEST("tc-named-computer: confirmed",
         capability_is_confirmed(&dec, CAP_COMPUTER));

    /* Test: named tool_choice for web search — bridge does not support it, route is DENIED */
    memset(&tc_spec, 0, sizeof(tc_spec));
    tc_spec.mode = TOOL_CHOICE_NAMED;
    snprintf(tc_spec.named_tool, sizeof(tc_spec.named_tool), "%s", "google_search");

    dec = capability_route_tools(NULL, 0, &tc_spec, "", 0);
    TEST("tc-named-search: active",
         !capability_is_active(&dec, CAP_WEB_SEARCH));
    TEST("tc-named-search: confirmed",
         !capability_is_confirmed(&dec, CAP_WEB_SEARCH));

    /* Test: tool_choice none should not activate anything */
    memset(&tc_spec, 0, sizeof(tc_spec));
    tc_spec.mode = TOOL_CHOICE_NONE;

    dec = capability_route_tools(NULL, 0, &tc_spec, "", 0);
    TEST("tc-none: no image",
         !capability_is_active(&dec, CAP_IMAGE));
    TEST("tc-none: no computer",
         !capability_is_active(&dec, CAP_COMPUTER));
    TEST("tc-none: no search",
         !capability_is_active(&dec, CAP_WEB_SEARCH));

    /* Test: tool_choice auto with no user intent should not activate */
    memset(&tc_spec, 0, sizeof(tc_spec));
    tc_spec.mode = TOOL_CHOICE_AUTO;

    dec = capability_route_tools(NULL, 0, &tc_spec, "", 0);
    TEST("tc-auto-empty: no image",
         !capability_is_active(&dec, CAP_IMAGE));
    TEST("tc-auto-empty: no computer",
         !capability_is_active(&dec, CAP_COMPUTER));
    TEST("tc-auto-empty: no search",
         !capability_is_active(&dec, CAP_WEB_SEARCH));

    /* Test: tool definitions with computer type + user intent */
    {
        NormalizedToolDef computer_tool;
        memset(&computer_tool, 0, sizeof(computer_tool));
        snprintf(computer_tool.type, sizeof(computer_tool.type), "%s", "computer");
        snprintf(computer_tool.normalized_name, sizeof(computer_tool.normalized_name), "%s", "computer_call");
        tools[0] = computer_tool;

        memset(&tc_spec, 0, sizeof(tc_spec));
        tc_spec.mode = TOOL_CHOICE_AUTO;

        const char *user_input = "use the computer to help me";
        dec = capability_route_tools(tools, 1, &tc_spec, user_input, strlen(user_input));
        TEST("tools-computer-with-intent: active",
             capability_is_active(&dec, CAP_COMPUTER));
        TEST("tools-computer-with-intent: confirmed",
             capability_is_confirmed(&dec, CAP_COMPUTER));
    }

    /* Test: tool definitions with computer type but NO user intent */
    {
        memset(&tc_spec, 0, sizeof(tc_spec));
        tc_spec.mode = TOOL_CHOICE_AUTO;

        const char *user_input = "what is computer science";
        dec = capability_route_tools(tools, 1, &tc_spec, user_input, strlen(user_input));
        TEST("tools-computer-no-intent: not active",
             !capability_is_active(&dec, CAP_COMPUTER));
    }
}

static void test_decision_summary(void) {
    char buf[1024];
    RoutingDecision dec;

    memset(&dec, 0, sizeof(dec));
    /* Manually build a simple decision */
    dec.active_capabilities = CAP_IMAGE | CAP_WEB_SEARCH;
    dec.route_count = 2;
    dec.routes[0].capability = CAP_IMAGE;
    dec.routes[0].confidence = ROUTE_CONFIRMED;
    snprintf(dec.routes[0].reason, sizeof(dec.routes[0].reason), "%s", "explicit intent");
    dec.routes[1].capability = CAP_WEB_SEARCH;
    dec.routes[1].confidence = ROUTE_DENIED;
    snprintf(dec.routes[1].reason, sizeof(dec.routes[1].reason), "%s", "no intent");

    capability_decision_summary(&dec, buf, sizeof(buf));
    TEST("summary: contains image",
         strstr(buf, "image: confirmed") != NULL);
    TEST("summary: contains web_search",
         strstr(buf, "web_search: denied") != NULL);
}

int main(void) {
    fprintf(stdout, "=== Capability Routing Tests ===\n\n");

    fprintf(stdout, "--- Image Generation Positive ---\n");
    test_image_positive();

    fprintf(stdout, "\n--- Image Generation Negative ---\n");
    test_image_negative();

    fprintf(stdout, "\n--- Computer Use Positive ---\n");
    test_computer_positive();

    fprintf(stdout, "\n--- Computer Use Negative ---\n");
    test_computer_negative();

    fprintf(stdout, "\n--- Web Search Positive ---\n");
    test_search_positive();

    fprintf(stdout, "\n--- Web Search Negative ---\n");
    test_search_negative();

    fprintf(stdout, "\n--- Tool Choice Routing ---\n");
    test_tool_choice_routing();

    fprintf(stdout, "\n--- Decision Summary ---\n");
    test_decision_summary();

    fprintf(stdout, "\n=== Results: %d passed, %d failed ===\n",
            tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
