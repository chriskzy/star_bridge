#ifndef STRUCTURAL_OVERLAY_H
#define STRUCTURAL_OVERLAY_H

#include <stdbool.h>
#include <stdio.h>

const char *get_embedded_styles(void);
void render_directory_node(FILE *out, const char *name, bool is_dir, int depth);
void render_browser_tabs(const char *text, const char *screenshot, char *dest, size_t max_len);

#endif
