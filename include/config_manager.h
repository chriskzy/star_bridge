#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

/* Maximum number of validation error messages returned */
#define CONFIG_MAX_ERRORS 32
/* Maximum length of a single validation error message */
#define CONFIG_ERROR_MAX_LEN 128

/* Validation result: error messages + count */
typedef struct {
    char errors[CONFIG_MAX_ERRORS][CONFIG_ERROR_MAX_LEN];
    int error_count;
} ConfigValidationResult;

typedef struct {
    int server_port;
    int max_buffer_chunk;
    char theme[32];
    char codex_harness_id[32];
    char codex_model[128];
    char codex_model_alias[128];
    char codex_model_display_name[128];
    bool automatic_tree_expand;
    bool use_framed_protocol;
    bool auto_load_resume_session;
    bool auto_save_kv_cache;
    bool auto_load_project_session;
    char kv_cache_policy[32];
    int context_tokens;
    char model_path[1024];
    char kv_cache_dir[1024];
    char browser_state_dir[1024];
    char preferred_browser[64];
    char agent_env[512];
    char extra_native_args[2048];
    bool shell_command_enabled;
    int google_search_timeout_ms;
    int browse_url_timeout_ms;
    int shell_command_timeout_ms;
    int response_timeout_ms;
    int max_history_entries;
    int max_request_size;
    int max_request_input_bytes;
    char auth_token[256];
    char bind_host[64];
    char host_allowlist[1024];
    bool keep_alive_enabled;
    char native_transport[16];
    char native_socket_path[1024];
    int uds_connect_timeout_ms;
    int hello_timeout_ms;
    int model_load_timeout_ms;
    int uds_reconnect_max_attempts;
    int uds_reconnect_backoff_ms;
    char uds_owner_mode[32];
    char session_key[256];
    char session_id[256];
    char session_index_path[1024];
    bool trace;
    bool debug_log_enabled;
    char debug_log_path[1024];
    bool bridge_workspace_monitor_enabled;
    /* When true (default), the ds4 framed wrapper filters internal tool transcript
     * lines (🛠️ list/find, $ cmd echoes from rtk/caveman skills etc.) out of both
     * live text_deltas and the final collected output. This keeps Codex UI clean.
     * Set false (or env DS4_SHOW_TOOL_LINES=1 at launch) to see the agent's tool
     * execution chatter for debugging. Controlled from bridge config + passed as
     * env to the venv wrapper when using real ds4-agent. */
    bool hide_tool_transcripts;
    char model_reasoning_effort[32];
    int max_output_buffer;
    int max_output_chars;
    int max_turn_events;
    pthread_mutex_t lock;
} BridgeConfig;

extern BridgeConfig global_config;

void init_global_config(int default_port);
bool load_config_from_file(const char *json_path);

/* Validate all config fields after loading.
 * Returns a ConfigValidationResult with error messages for each invalid field.
 * Returns empty result (error_count=0) if all fields are valid.
 */
ConfigValidationResult config_validate_all(void);
void resolve_native_socket_path(const char *workspace);
const char *validate_native_socket_path(void);
const char *validate_socket_parent_dir(void);
bool cleanup_stale_socket(void);
void inject_config_ui_html(char *dest, size_t max_len);
void update_config_from_query(const char *query_string);
void compute_session_key(const char *workspace_root, const char *session_id);
void session_key_for(const char *workspace_root, const char *session_id, char *dest, size_t max_len);
void set_session_id(const char *session_id);
const char *get_session_key(void);
bool load_session_index(void);
bool save_session_index(void);
const char *session_index_lookup(const char *session_key);
const char *session_index_get_workspace(const char *session_key);
const char *session_index_get_model_id(const char *session_key);
bool session_index_update(const char *session_key, const char *state_id, const char *workspace_root, const char *model_id, int context_budget);

/* Generate Star Bridge Codex model catalog and provider config artifacts.
 * Writes custom_model_catalog.json and star_bridge_provider.json
 * to the given output directory (or current dir if NULL).
 * Returns true on success, false on failure.
 * This is an idempotent generation command — it does not mutate user config.
 */
bool generate_codex_config(const char *output_dir, int server_port);

/* Managed Codex config install/disable workflow.
 * install_managed_config backs up ~/.codex/config.toml, inserts a marked
 * bridge-managed block, and preserves displaced top-level keys in backup.
 * disable_managed_config removes the managed block and restores displaced keys.
 * managed_config_status prints status and returns true if installed.
 * All support a dry_run mode that prints what would change without modifying.
 */
bool install_managed_config(bool dry_run, int server_port);
bool disable_managed_config(bool dry_run);
bool managed_config_status(void);
bool read_codex_toml_reasoning_effort(void);

#endif
