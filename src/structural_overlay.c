#include "structural_overlay.h"

#include "bridge_core.h"

#include <stdio.h>
#include <string.h>

const char *get_embedded_styles(void) {
    return "<style>"
           ":root{--bg-main:#1e1e2e;--bg-surface:#252538;--fg-main:#cdd6f4;--fg-muted:#7f849c;--accent-blue:#89b4fa;--accent-green:#a6e3a1;--accent-yellow:#f9e2af;--accent-red:#f38ba8;--border-color:#313244}"
           ".theme-monokai{--bg-main:#272822;--bg-surface:#3e3d32;--fg-main:#f8f8f2;--accent-blue:#66d9ef;--accent-green:#a6e22e;--accent-yellow:#e6db74;--accent-red:#f92672}"
           ".bridge-container{background:var(--bg-main);color:var(--fg-main);font-family:SFMono-Regular,Menlo,monospace;border:1px solid var(--border-color);border-radius:8px;overflow:hidden}"
           ".terminal-canvas{padding:12px;line-height:1.5;font-size:13px}"
           ".config-strip{display:flex;gap:12px;color:var(--fg-muted);border-bottom:1px solid var(--border-color);padding-bottom:8px;margin-bottom:8px}"
           ".tree-row,.log-row{padding:3px 8px;border-radius:4px}"
           ".event-node{margin:4px 0;border-left:3px solid var(--accent-blue);background:rgba(137,180,250,.06)}"
           ".event-node[data-action='CREATED']{border-color:var(--accent-green)}.event-node[data-action='MODIFIED']{border-color:var(--accent-yellow)}.event-node[data-action='REMOVED']{border-color:var(--accent-red)}"
           ".event-badge,.log-indicator{font-size:10px;font-weight:700;padding:2px 6px;border-radius:3px;margin-right:8px;color:#11111b;background:var(--accent-blue)}"
           ".badge-created{background:var(--accent-green)}.badge-modified{background:var(--accent-yellow)}.badge-removed,.badge-error{background:var(--accent-red)}.badge-warning{background:var(--accent-yellow)}"
           ".compiler-error{border-left:3px solid var(--accent-red);background:rgba(243,139,168,.06)}.compiler-warning{border-left:3px solid var(--accent-yellow);background:rgba(249,226,175,.06)}"
           ".log-link{text-decoration:none;cursor:pointer}.log-text strong{color:var(--accent-blue)}"
           ".browser-tabs{border:1px solid var(--border-color);border-radius:6px;overflow:hidden}.tab-nav{display:flex;background:var(--bg-surface)}.tab-label{padding:8px 12px;color:var(--fg-muted)}.tab-content{padding:12px;max-height:420px;overflow:auto}.snapshot-viewport{max-width:100%;background:#fff}"
           "</style>";
}

void render_directory_node(FILE *out, const char *name, bool is_dir, int depth) {
    fprintf(out, "<div class='tree-row' style='padding-left:%dpx'>%s%s</div>\n",
            depth * 16, is_dir ? "DIR " : "FILE ", name ? name : "");
}

void render_browser_tabs(const char *text, const char *screenshot, char *dest, size_t max_len) {
    char safe_text[12000];
    bridge_html_escape(text ? text : "", safe_text, sizeof(safe_text));
    snprintf(dest, max_len,
             "<div class='browser-tabs'><div class='tab-nav'><span class='tab-label'>Extracted text</span><span class='tab-label'>Snapshot</span></div><div class='tab-content'><pre>%s</pre></div><div class='tab-content'><img class='snapshot-viewport' src='%s' alt='browser snapshot'></div></div>",
             safe_text, screenshot ? screenshot : "");
}
