#include "bridge_core.h"
#include "config_manager.h"
#include "debug_trace.h"
#include "file_monitor_expanded.h"
#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>

#define MAX_AGENT_ARGV 64

#define STAR_BRIDGE_CLI_VERSION "0.1.0-alpha"
#define STAR_BRIDGE_VALIDATED_DS4_COMMIT "d881f2a05e8f"

static const char *path_basename(const char *path);

static void print_version(void) {
    fprintf(stdout, "star_bridge version %s\n", STAR_BRIDGE_CLI_VERSION);
}

/* doctor: one-shot diagnostics. Prints PASS per check; on the first failing
 * check prints "DOCTOR FAIL: <check> — <detail>" and returns nonzero. */
static int doctor_fail(const char *check, const char *detail) {
    fprintf(stderr, "DOCTOR FAIL: %s — %s\n", check, detail);
    return EXIT_FAILURE;
}

static int capture_git_head_short(const char *repo_dir, char *dest, size_t dest_len) {
    if (!repo_dir || !repo_dir[0] || !dest || dest_len == 0) return 0;
    int pipefd[2];
    if (pipe(pipefd) != 0) return 0;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return 0;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execlp("git", "git", "-C", repo_dir, "rev-parse", "--short=12", "HEAD", (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    ssize_t n = read(pipefd[0], dest, dest_len - 1);
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (n <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        dest[0] = '\0';
        return 0;
    }
    dest[n] = '\0';
    dest[strcspn(dest, "\r\n")] = '\0';
    return dest[0] != '\0';
}

static void agent_dirname(const char *agent_path, char *dest, size_t dest_len) {
    if (!dest || dest_len == 0) return;
    dest[0] = '\0';
    if (!agent_path || !agent_path[0]) return;
    snprintf(dest, dest_len, "%s", agent_path);
    char *slash = strrchr(dest, '/');
    if (slash) {
        if (slash == dest) {
            slash[1] = '\0';
        } else {
            *slash = '\0';
        }
    } else {
        snprintf(dest, dest_len, ".");
    }
}

static int run_doctor(const char *agent_path, const char *config_path,
                      bool no_config, int port) {
    init_global_config(port);
    if (!no_config && config_path) {
        load_config_from_file(config_path); /* absent config is fine */
    }

    /* 1. config valid */
    ConfigValidationResult vr = config_validate_all();
    if (vr.error_count > 0) {
        return doctor_fail("config", vr.errors[0]);
    }
    fprintf(stdout, "DOCTOR OK: config\n");

    /* 2. agent binary found + executable (only if a path was given) */
    if (agent_path && agent_path[0]) {
        if (access(agent_path, X_OK) != 0) {
            return doctor_fail("agent", agent_path);
        }
        fprintf(stdout, "DOCTOR OK: agent (%s)\n", agent_path);
        if (strcmp(path_basename(agent_path), "ds4-agent") == 0) {
            char dir[1024];
            char commit[64];
            agent_dirname(agent_path, dir, sizeof(dir));
            if (capture_git_head_short(dir, commit, sizeof(commit))) {
                if (strcmp(commit, STAR_BRIDGE_VALIDATED_DS4_COMMIT) == 0) {
                    fprintf(stdout, "DOCTOR OK: ds4-version (validated commit %s)\n", commit);
                } else {
                    fprintf(stdout,
                            "DOCTOR WARN: ds4-version — detected commit %s; validated against %s\n",
                            commit, STAR_BRIDGE_VALIDATED_DS4_COMMIT);
                }
            } else {
                fprintf(stdout,
                        "DOCTOR WARN: ds4-version — unable to detect git commit; validated against %s\n",
                        STAR_BRIDGE_VALIDATED_DS4_COMMIT);
            }
        }
    }

    /* 3. wrapper venv OK */
    if (access(".venv/bin/python3", X_OK) != 0) {
        return doctor_fail("venv", ".venv/bin/python3 missing — run 'make venv'");
    }
    fprintf(stdout, "DOCTOR OK: venv\n");

    /* 4. socket parent writable + no stale socket (only when UDS configured) */
    if (global_config.native_socket_path[0]) {
        const char *sockerr = validate_socket_parent_dir();
        if (sockerr) {
            return doctor_fail("socket", sockerr);
        }
    }
    fprintf(stdout, "DOCTOR OK: socket\n");

    /* 5. port free */
    {
        int tfd = socket(AF_INET, SOCK_STREAM, 0);
        if (tfd < 0) {
            return doctor_fail("port", "cannot create probe socket");
        }
        int one = 1;
        setsockopt(tfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((uint16_t)port);
        int rc = bind(tfd, (struct sockaddr *)&addr, sizeof(addr));
        close(tfd);
        if (rc != 0) {
            char detail[64];
            snprintf(detail, sizeof(detail), "port %d in use", port);
            return doctor_fail("port", detail);
        }
    }
    fprintf(stdout, "DOCTOR OK: port (%d free)\n", port);

    /* 6. Codex managed block present */
    if (!managed_config_status()) {
        return doctor_fail("codex-config", "managed block not installed — run --install");
    }
    fprintf(stdout, "DOCTOR OK: codex-config\n");

    /* 7. catalog file present */
    {
        const char *home = getenv("HOME");
        char catalog[1024];
        if (home && home[0]) {
            snprintf(catalog, sizeof(catalog), "%s/.codex/custom_catalog.json", home);
        } else {
            snprintf(catalog, sizeof(catalog), ".codex/custom_catalog.json");
        }
        if (access(catalog, R_OK) != 0) {
            return doctor_fail("catalog", catalog);
        }
        fprintf(stdout, "DOCTOR OK: catalog (%s)\n", catalog);
    }

    fprintf(stdout, "DOCTOR: all checks passed\n");
    return EXIT_SUCCESS;
}

static void print_help(const char *prog) {
    fprintf(stdout,
        "Usage: %s <agent_binary> [workspace_path] [options]\n"
        "\n"
        "Arguments:\n"
        "  <agent_binary>       Path to the native agent binary/script\n"
        "  [workspace_path]     Workspace root directory (default: current dir)\n"
        "\n"
        "Options:\n"
        "  -p <port>            HTTP server port (default: 8080, env: PORT)\n"
        "  --framed             Enable framed protocol for stdio transport\n"
        "  --native-transport <mode>   Transport mode: auto|uds|stdio_framed|stdio\n"
        "  --native-socket-path <path> Unix domain socket path\n"
        "  --uds-connect-timeout-ms <ms>  UDS connect timeout (default: 15000)\n"
        "  --uds-owner-mode <mode>       UDS owner mode: connect_existing|launch_and_connect|bridge_listen (default: connect_existing)\n"
        "  --hello-timeout-ms <ms>       Hello frame timeout (default: 10000)\n"
        "  --model-load-timeout-ms <ms>  Model load timeout (default: 120000)\n"
        "  --turn-response-timeout-ms <ms> Turn response timeout (default: 5000)\n"
        "  --model-reasoning-effort <val>  Default reasoning effort: low|medium|high|extra high (default: medium)\n"
        "  --max-output-buffer <bytes>     Max output buffer size in bytes (default: 262144)\n"
        "  --max-output-chars <chars>      Max output character limit for non-streaming responses (default: 0 = unlimited)\n"
        "  --max-turn-events <n>           Max native frames per turn before partial completion (default: 65536)\n"
        "  --no-config          Skip loading config.json\n"
        "  --generate-config [output_dir] Generate DwarfStar Codex config artifacts and exit\n"
        "  --install            Install managed Codex config (backs up ~/.codex/config.toml)\n"
        "  --disable            Remove managed Codex config and restore displaced keys\n"
        "  --status             Show managed config install status\n"
        "  --doctor [agent]     Run one-shot diagnostics and exit (nonzero on first failure)\n"
        "  --dry-run            With --install or --disable, show what would change without modifying\n"
        "  --version            Print version and exit\n"
        "  --help               Print this help and exit\n"
        "\n"
        "Config file: config.json (loaded automatically unless --no-config)\n"
        "\n"
        "Star Bridge version %s\n"
        "Local ds4 bridge for Codex desktop app\n",
        prog, STAR_BRIDGE_CLI_VERSION);
}

static void append_arg(char **agent_argv, int *argc, char *arg) {
    if (*argc < MAX_AGENT_ARGV - 1) {
        agent_argv[(*argc)++] = arg;
        agent_argv[*argc] = NULL;
    }
}

static void append_split_args(char **agent_argv, int *argc, char *args) {
    if (!args || !args[0]) return;
    char *saveptr = NULL;
    for (char *tok = strtok_r(args, " \t\r\n", &saveptr); tok; tok = strtok_r(NULL, " \t\r\n", &saveptr)) {
        append_arg(agent_argv, argc, tok);
    }
}

static const char *path_basename(const char *path) {
    if (!path || !path[0]) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool validate_ds4_agent_preflight(const char *agent_path, const char *model_path) {
    if (!agent_path || !agent_path[0]) {
        fprintf(stderr, "Fatal: ds4-agent path is empty\n");
        return false;
    }
    if (access(agent_path, X_OK) != 0) {
        fprintf(stderr, "Fatal: ds4-agent not found or not executable: %s (%s)\n",
                agent_path, strerror(errno));
        return false;
    }

    char home[1024];
    snprintf(home, sizeof(home), "%s", agent_path);
    char *slash = strrchr(home, '/');
    if (slash) {
        *slash = '\0';
    } else {
        snprintf(home, sizeof(home), ".");
    }

    char metal_path[1200];
    snprintf(metal_path, sizeof(metal_path), "%s/metal/flash_attn.metal", home);
    if (access(metal_path, R_OK) != 0) {
        fprintf(stderr,
                "Fatal: ds4-agent Metal assets missing: %s\n"
                "ds4-agent must run with --chdir set to its install directory (found: %s).\n",
                metal_path, home);
        return false;
    }

    if (model_path && model_path[0] && access(model_path, R_OK) != 0) {
        fprintf(stderr, "Fatal: ds4 model file not readable: %s (%s)\n",
                model_path, strerror(errno));
        return false;
    }

    debug_trace_append("ds4_preflight_ok agent=%s home=%s", agent_path, home);
    return true;
}

static bool find_bundled_ds4_wrapper(const char *bridge_argv0, char *dest, size_t max_len) {
    if (!dest || max_len == 0) return false;

    /* Production wrapper lives in agent/ds4_wrapper.py. */
    static const char *candidates[] = {
        "agent/ds4_wrapper.py",
    };
    static const char *sibling_candidates[] = {
        "/../agent/ds4_wrapper.py",
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (snprintf(dest, max_len, "%s", candidates[i]) < (int)max_len &&
            access(dest, R_OK) == 0) {
            return true;
        }
    }

    const char *slash = bridge_argv0 ? strrchr(bridge_argv0, '/') : NULL;
    if (slash) {
        size_t dir_len = (size_t)(slash - bridge_argv0);
        for (size_t i = 0; i < sizeof(sibling_candidates) / sizeof(sibling_candidates[0]); i++) {
            if (dir_len + strlen(sibling_candidates[i]) + 1 < max_len) {
                memcpy(dest, bridge_argv0, dir_len);
                dest[dir_len] = '\0';
                strcat(dest, sibling_candidates[i]);
                if (access(dest, R_OK) == 0) return true;
            }
        }
    }

    return false;
}

static bool find_venv_python(const char *bridge_argv0, char *dest, size_t max_len) {
    if (!dest || max_len == 0) return false;

    if (snprintf(dest, max_len, ".venv/bin/python3") < (int)max_len && access(dest, X_OK) == 0) {
        return true;
    }

    const char *slash = bridge_argv0 ? strrchr(bridge_argv0, '/') : NULL;
    if (slash) {
        size_t dir_len = (size_t)(slash - bridge_argv0);
        if (dir_len + strlen("/../.venv/bin/python3") + 1 < max_len) {
            memcpy(dest, bridge_argv0, dir_len);
            dest[dir_len] = '\0';
            strcat(dest, "/../.venv/bin/python3");
            if (access(dest, X_OK) == 0) return true;
        }
    }

    return false;
}

int main(int argc, char *argv[]) {
    /* Handle --help, --version, --generate-config, and managed config flags before any other checks */
    bool dry_run = false;

    /* Early detection of config path and explicit port flag so that --install / --generate-config
     * can use the port from config.json (unless -p is passed on the command line).
     * This follows the same precedence as normal runs: explicit -p > PORT env > config.json > 8080
     */
    bool early_no_config = false;
    const char *early_config_path = "config.json";
    int cli_p_port = 0;
    const char *early_env_port = getenv("PORT");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-config") == 0) {
            early_no_config = true;
        }
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            early_config_path = argv[i + 1];
            i++;
        }
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            cli_p_port = atoi(argv[i + 1]);
        }
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        }
    }

    int early_port = 8080;
    if (cli_p_port > 0) {
        early_port = cli_p_port;
    } else if (early_env_port && early_env_port[0]) {
        int env_val = atoi(early_env_port);
        if (env_val > 0) early_port = env_val;
    } else if (!early_no_config) {
        /* Load config (just for port) unless --no-config. Use a fresh init so we don't pollute global yet. */
        init_global_config(8080);
        if (load_config_from_file(early_config_path)) {
            if (global_config.server_port > 0) {
                early_port = global_config.server_port;
            }
        }
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[i], "--version") == 0) {
            print_version();
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[i], "--generate-config") == 0) {
            const char *out_dir = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : ".";
            if (generate_codex_config(out_dir, early_port)) {
                fprintf(stdout, "Generated Codex config artifacts in '%s'\n", out_dir);
                return EXIT_SUCCESS;
            }
            fprintf(stderr, "Error: failed to generate Codex config artifacts\n");
            return EXIT_FAILURE;
        }
        if (strcmp(argv[i], "--install") == 0) {
            bool ok = install_managed_config(dry_run, early_port);
            if (!ok) {
                fprintf(stderr, "Error: failed to install managed config\n");
                return EXIT_FAILURE;
            }
            fprintf(stdout, dry_run ? "(dry-run) " : "");
            fprintf(stdout, "Managed config install %s\n", dry_run ? "preview completed" : "completed");
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[i], "--disable") == 0) {
            bool ok = disable_managed_config(dry_run);
            if (!ok) {
                fprintf(stderr, "Error: failed to disable managed config\n");
                return EXIT_FAILURE;
            }
            fprintf(stdout, dry_run ? "(dry-run) " : "");
            fprintf(stdout, "Managed config disable %s\n", dry_run ? "preview completed" : "completed");
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[i], "--status") == 0) {
            managed_config_status();
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[i], "--doctor") == 0 || strcmp(argv[i], "doctor") == 0) {
            /* Optional agent path follows --doctor */
            const char *agent_path = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[i + 1] : NULL;
            return run_doctor(agent_path, early_config_path, early_no_config, early_port);
        }
    }

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <agent_binary> [workspace_path] [options]\n", argv[0]);
        fprintf(stderr, "       %s --help for detailed help\n", argv[0]);
        return EXIT_FAILURE;
    }

    int target_port = 8080;
    const char *env_port = getenv("PORT");
    if (env_port && env_port[0]) target_port = atoi(env_port);

    init_global_config(target_port);

    /* Detect --no-config and --config before loading config */
    bool no_config = false;
    const char *config_path = "config.json";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-config") == 0) {
            no_config = true;
            break;
        }
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[i + 1];
            i++;
        }
    }

    /* Load config file; defaults to config.json, overridden by --config */
    if (!no_config) {
        if (!load_config_from_file(config_path)) {
            fprintf(stderr, "Warning: could not load config from '%s' (non-fatal, using defaults)\n", config_path);
        } else {
            /* Validate all config fields and report errors */
            ConfigValidationResult vr = config_validate_all();
            for (int i = 0; i < vr.error_count; i++) {
                fprintf(stderr, "Config error: %s\n", vr.errors[i]);
            }
            if (vr.error_count > 0) {
                fprintf(stderr, "Fatal: %d config validation error(s) — refusing to start\n", vr.error_count);
                return EXIT_FAILURE;
            }
        }
        /* After loading bridge config, read model_reasoning_effort from
         * ~/.codex/config.toml if present (Codex Desktop may have updated it). */
        read_codex_toml_reasoning_effort();
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            global_config.server_port = atoi(argv[i + 1]);
        }
        if (strcmp(argv[i], "--framed") == 0) {
            global_config.use_framed_protocol = true;
        }
        if (strcmp(argv[i], "--native-transport") == 0 && i + 1 < argc) {
            snprintf(global_config.native_transport, sizeof(global_config.native_transport), "%s", argv[i + 1]);
            i++;
        }
        if (strcmp(argv[i], "--native-socket-path") == 0 && i + 1 < argc) {
            snprintf(global_config.native_socket_path, sizeof(global_config.native_socket_path), "%s", argv[i + 1]);
            i++;
        }
        if (strcmp(argv[i], "--uds-connect-timeout-ms") == 0 && i + 1 < argc) {
            global_config.uds_connect_timeout_ms = atoi(argv[i + 1]);
            i++;
        }
        if (strcmp(argv[i], "--uds-owner-mode") == 0 && i + 1 < argc) {
            snprintf(global_config.uds_owner_mode, sizeof(global_config.uds_owner_mode), "%s", argv[i + 1]);
            i++;
        }
        if (strcmp(argv[i], "--hello-timeout-ms") == 0 && i + 1 < argc) {
            global_config.hello_timeout_ms = atoi(argv[i + 1]);
            i++;
        }
        if (strcmp(argv[i], "--model-load-timeout-ms") == 0 && i + 1 < argc) {
            global_config.model_load_timeout_ms = atoi(argv[i + 1]);
            i++;
        }
        if (strcmp(argv[i], "--turn-response-timeout-ms") == 0 && i + 1 < argc) {
            global_config.response_timeout_ms = atoi(argv[i + 1]);
            i++;
        }
        if (strcmp(argv[i], "--model-reasoning-effort") == 0 && i + 1 < argc) {
            snprintf(global_config.model_reasoning_effort, sizeof(global_config.model_reasoning_effort), "%s", argv[i + 1]);
            i++;
        }
        if (strcmp(argv[i], "--max-output-buffer") == 0 && i + 1 < argc) {
            global_config.max_output_buffer = atoi(argv[i + 1]);
            i++;
        }
        if (strcmp(argv[i], "--max-output-chars") == 0 && i + 1 < argc) {
            global_config.max_output_chars = atoi(argv[i + 1]);
            i++;
        }
        if (strcmp(argv[i], "--max-turn-events") == 0 && i + 1 < argc) {
            global_config.max_turn_events = atoi(argv[i + 1]);
            i++;
        }
    }

    /* Env PORT overrides config.json but not explicit -p flag */
    if (env_port && env_port[0]) {
        int env_port_val = atoi(env_port);
        if (env_port_val > 0) {
            /* Only override if -p was not used */
            /* We detect -p usage: if global_config.server_port != target_port,
               that means -p was used and set it to a different value. */
            /* Simple heuristic: check if server_port matches what -p would set */
            int p_flag_port = 0;
            for (int i = 1; i < argc; i++) {
                if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
                    p_flag_port = atoi(argv[i + 1]);
                    break;
                }
            }
            if (p_flag_port == 0) {
                global_config.server_port = env_port_val;
            }
        }
    }

    /* Resolve native_socket_path to absolute path */
    {
        const char *ws = ".";
        if (argc > 2 && argv[2][0] != '-') ws = argv[2];
        resolve_native_socket_path(ws);
    }

    /* Validate native_socket_path and its parent directory */
    if (global_config.native_socket_path[0] != '\0') {
        const char *err = validate_native_socket_path();
        if (err) {
            fprintf(stderr, "Fatal: socket path validation failed: %s\n", err);
            return EXIT_FAILURE;
        }
        err = validate_socket_parent_dir();
        if (err) {
            fprintf(stderr, "Fatal: socket parent directory validation failed: %s\n", err);
            return EXIT_FAILURE;
        }
    }

    /* Resolve native_transport: auto -> uds if native_socket_path set, else stdio_framed */
    {
        /* Save original transport BEFORE modifying native_transport (transport points into it) */
        char orig_transport[16];
        snprintf(orig_transport, sizeof(orig_transport), "%s", global_config.native_transport);
        const char *transport = global_config.native_transport;
        if (strcmp(transport, "auto") == 0) {
            if (global_config.native_socket_path[0] != '\0') {
                snprintf(global_config.native_transport, sizeof(global_config.native_transport), "uds");
            } else {
                snprintf(global_config.native_transport, sizeof(global_config.native_transport), "stdio_framed");
            }
        }
        if (strcmp(global_config.native_transport, "stdio") == 0) {
            /* Explicit legacy raw stdio transport. */
            global_config.use_framed_protocol = false;
        } else if (strcmp(global_config.native_transport, "uds") == 0) {
            /* UDS transport implies framed protocol */
            global_config.use_framed_protocol = true;
        } else if (strcmp(global_config.native_transport, "stdio_framed") == 0) {
            /* stdio_framed transport uses framed protocol; only set if explicitly
             * configured (not just auto-resolved) so non-framed fake-agent tests work */
            /* Check if transport was explicitly set via CLI or config */
            if (strcmp(orig_transport, "stdio_framed") == 0 || strcmp(orig_transport, "uds") == 0) {
                global_config.use_framed_protocol = true;
            }
            /* If auto-resolved to stdio_framed, also enable framed protocol
             * so transport and framing behavior remain consistent. */
            if (strcmp(orig_transport, "auto") == 0) {
                global_config.use_framed_protocol = true;
            }
            /* If auto-resolved to stdio_framed, keep current use_framed_protocol */
        } else {
            fprintf(stderr, "Warning: unknown native_transport '%s', falling back to stdio_framed\n", transport);
            snprintf(global_config.native_transport, sizeof(global_config.native_transport), "stdio_framed");
            /* Keep current use_framed_protocol */
        }
        /* Log selected transport, owner mode, socket path */
        char sp[128];
        debug_trace_compact_text(global_config.native_socket_path[0] ? global_config.native_socket_path : "(none)",
                                 sp, sizeof(sp));
        debug_trace_append("transport_selected transport=%s owner_mode=%s socket_path=%s connect_timeout_ms=%d",
                           global_config.native_transport,
                           global_config.uds_owner_mode,
                           sp,
                           global_config.uds_connect_timeout_ms);
    }

    const char *workspace = ".";
    if (argc > 2 && argv[2][0] != '-') workspace = argv[2];

    if (global_config.agent_env[0]) {
        putenv(global_config.agent_env);
    }

    /* Inject loopback proxy bypass for child agent processes.
     * Set both NO_PROXY and no_proxy to avoid system proxy interception
     * of localhost, IPv4 loopback, and IPv6 loopback connections.
     * Only set if not already defined in the environment. */
    {
        const char *proxy_bypass = "127.0.0.1,localhost,::1";
        /* Use setenv (copies the value) rather than putenv with a stack buffer:
         * putenv keeps the caller's pointer, which would dangle once these scopes
         * exit. setenv with overwrite=0 preserves any value already in the env. */
        if (!getenv("NO_PROXY")) {
            setenv("NO_PROXY", proxy_bypass, 0);
        }
        if (!getenv("no_proxy")) {
            setenv("no_proxy", proxy_bypass, 0);
        }
        debug_trace_append("loopback_proxy_bypass NO_PROXY=%s no_proxy=%s",
                           getenv("NO_PROXY") ? getenv("NO_PROXY") : "(unset)",
                           getenv("no_proxy") ? getenv("no_proxy") : "(unset)");
    }

    const char *agent_command = argv[1];
    char wrapper_path[1024];
    char venv_python[1024];
    char ds4_agent_env[1200];
    char ds4_model_env[1200];
    char ds4_startup_env[64];
    char ds4_turn_env[64];
    bool use_ds4_wrapper = false;
    if (strcmp(path_basename(argv[1]), "ds4-agent") == 0 &&
        find_bundled_ds4_wrapper(argv[0], wrapper_path, sizeof(wrapper_path))) {
        if (!find_venv_python(argv[0], venv_python, sizeof(venv_python))) {
            fprintf(stderr,
                    "Fatal: Python 3.14 venv missing at .venv/bin/python3\n"
                    "Run: make venv  (requires python3.14)\n");
            return EXIT_FAILURE;
        }
        if (!validate_ds4_agent_preflight(argv[1], global_config.model_path)) {
            return EXIT_FAILURE;
        }
        snprintf(ds4_agent_env, sizeof(ds4_agent_env), "DS4_AGENT_PATH=%s", argv[1]);
        putenv(ds4_agent_env);
        if (global_config.model_path[0]) {
            snprintf(ds4_model_env, sizeof(ds4_model_env), "DS4_MODEL_PATH=%s", global_config.model_path);
            putenv(ds4_model_env);
        }
        {
            int load_ms = global_config.model_load_timeout_ms > 0
                              ? global_config.model_load_timeout_ms
                              : BRIDGE_MODEL_LOAD_TIMEOUT_MS;
            int turn_ms = global_config.response_timeout_ms > 0
                              ? global_config.response_timeout_ms
                              : BRIDGE_TURN_RESPONSE_TIMEOUT_MS;
            int startup_sec = load_ms / 1000;
            int turn_sec = turn_ms / 1000;
            if (startup_sec < 30) startup_sec = 30;
            if (turn_sec < 30) turn_sec = 30;
            snprintf(ds4_startup_env, sizeof(ds4_startup_env), "DS4_STARTUP_TIMEOUT=%d", startup_sec);
            snprintf(ds4_turn_env, sizeof(ds4_turn_env), "DS4_TURN_TIMEOUT=%d", turn_sec);
            putenv(ds4_startup_env);
            putenv(ds4_turn_env);
        }
        {
            /* Drive the wrapper's tool transcript filter from bridge config (default hides
             * 🛠️ list/find etc for clean Codex UI per user request). SHOW=1 shows them. */
            const char *show = global_config.hide_tool_transcripts ? "0" : "1";
            static char ds4_show_env[64];
            snprintf(ds4_show_env, sizeof(ds4_show_env), "DS4_SHOW_TOOL_LINES=%s", show);
            putenv(ds4_show_env);
        }
        use_ds4_wrapper = true;
        agent_command = venv_python;
        /* ds4-agent special case: route via venv python + agent/ds4_wrapper.py.
         * The wrapper adapts framed protocol to real ds4 persistent --non-interactive (pty for output).
         * Now supports UDS for bridge<->wrapper framed comms (end-to-end UDS for "real ds4 agent to bridge").
         * Bridge will launch wrapper, wrapper binds UDS (launch_and_connect), bridge connects over UDS
         * and uses nc_set_uds + framed over the socket (instead of stdio pipes).
         * This finishes UDS for ds4 while keeping pty wrapper for the real binary (metal, line buffering, deltas).
         * Ensures venv always used. */
        // Generate a dedicated UDS path for this ds4 instance (absolute to avoid resolve issues)
        snprintf(global_config.native_socket_path, sizeof(global_config.native_socket_path),
                 "/tmp/ds4-bridge-uds-%d.sock", (int)getpid());
        snprintf(global_config.native_transport, sizeof(global_config.native_transport), "uds");
        global_config.use_framed_protocol = true;
        snprintf(global_config.uds_owner_mode, sizeof(global_config.uds_owner_mode), "launch_and_connect");
        // Tell wrapper via env to use UDS for framed protocol with bridge (instead of stdin/stdout)
        {
            static char ds4_uds_env[1024];
            snprintf(ds4_uds_env, sizeof(ds4_uds_env), "DS4_BRIDGE_UDS_SOCKET=%s", global_config.native_socket_path);
            putenv(ds4_uds_env);
        }
        fprintf(stderr, "Info: using ds4 framed wrapper for native agent '%s' via %s (UDS transport to bridge)\n",
                argv[1], venv_python);
        debug_trace_append("ds4_wrapper_selected agent=%s wrapper=%s python=%s transport=uds socket=%s",
                           argv[1], wrapper_path, venv_python, global_config.native_socket_path);
    }

    char extra_args[2048];
    snprintf(extra_args, sizeof(extra_args), "%s", global_config.extra_native_args);

    char *child_argv[MAX_AGENT_ARGV] = {0};
    int child_argc = 0;
    append_arg(child_argv, &child_argc, (char *)agent_command);
    if (use_ds4_wrapper) {
        append_arg(child_argv, &child_argc, "-u");
        append_arg(child_argv, &child_argc, wrapper_path);
    }
    if (global_config.model_path[0]) {
        append_arg(child_argv, &child_argc, "--model");
        append_arg(child_argv, &child_argc, global_config.model_path);
    }
    if (global_config.kv_cache_dir[0]) {
        append_arg(child_argv, &child_argc, "--kv-cache-dir");
        append_arg(child_argv, &child_argc, global_config.kv_cache_dir);
    }
    append_split_args(child_argv, &child_argc, extra_args);

    /* Clean up stale socket if in launch_and_connect mode */
    if (global_config.native_socket_path[0] != '\0' &&
        strcmp(global_config.uds_owner_mode, "launch_and_connect") == 0) {
        cleanup_stale_socket();
    }

    BridgeEngine engine;
    if (!engine_init(&engine, &global_config, agent_command, child_argv, workspace, global_config.use_framed_protocol,
                     global_config.native_transport, global_config.native_socket_path,
                     global_config.uds_owner_mode)) {
        fprintf(stderr, "Failed to start agent process: %s\n", agent_command);
        return EXIT_FAILURE;
    }

    /* Configure startup timeout buckets from global config (override engine defaults) */
    engine.socket_connect_timeout_ms = global_config.uds_connect_timeout_ms;
    engine.hello_timeout_ms = global_config.hello_timeout_ms;
    engine.model_load_timeout_ms = global_config.model_load_timeout_ms;
    engine.turn_response_timeout_ms = global_config.response_timeout_ms;

    /* Warn if running an interactive shell in non-framed mode for Codex app */
    if (!global_config.use_framed_protocol) {
        const char *agent_bin = argv[1];
        if (strstr(agent_bin, "zsh") || strstr(agent_bin, "bash") || strstr(agent_bin, "sh") || strstr(agent_bin, "cat")) {
            fprintf(stderr, "Warning: starting interactive shell '%s' in non-framed mode. "
                            "Codex app validation should use framed native-agent mode.\n", agent_bin);
        }
    }

    /* Truncate workspace path to avoid leaking directory structure */
    char ws_sample[128];
    debug_trace_compact_text(workspace, ws_sample, sizeof(ws_sample));
    debug_trace_append("agent_start command=%s workspace=%s transport=%s framed=%s pid=%ld",
                       agent_command,
                       ws_sample,
                       global_config.native_transport,
                       global_config.use_framed_protocol ? "true" : "false",
                       (long)engine.child_pid);

    /* Send hello frame to native agent if framed protocol */
    if (global_config.use_framed_protocol) {
        debug_trace_append("hello_send transport=%s workspace_root=%s",
                           global_config.native_transport, workspace);
        if (!engine_send_hello(&engine, workspace)) {
            fprintf(stderr, "Fatal: failed to send hello frame to native agent\n");
            engine_destroy(&engine);
            return EXIT_FAILURE;
        }
        /* Read and validate native hello response */
        debug_trace_append("native_hello_read transport=%s", global_config.native_transport);
        if (!engine_read_native_hello(&engine)) {
            fprintf(stderr, "Fatal: native hello response invalid or missing - "
                            "handshake failed (required fields: type, role, protocol_version, "
                            "agent_name, agent_version, supported_transports)\n");
            engine_destroy(&engine);
            return EXIT_FAILURE;
        }
        debug_trace_append("handshake_complete transport=%s protocol_version=%d",
                           global_config.native_transport, engine.nc.protocol_version);

        /* Wait for native ready frame (model load timeout) */
        debug_trace_append("ready_wait transport=%s timeout_ms=%d",
                           global_config.native_transport,
                           engine.model_load_timeout_ms > 0 ? engine.model_load_timeout_ms : BRIDGE_MODEL_LOAD_TIMEOUT_MS);
        if (!engine_wait_for_ready(&engine)) {
            fprintf(stderr, "Fatal: native agent did not become ready within timeout - "
                            "expected ready frame with model_loaded=true\n");
            engine_destroy(&engine);
            return EXIT_FAILURE;
        }
        debug_trace_append("ready_complete transport=%s session_state=%s",
                           global_config.native_transport, "idle");

        /* Start heartbeat/ping-pong only while idle; do not interleave
         * heartbeat frames into an active turn */
        debug_trace_append("heartbeat_start transport=%s interval_ms=%d timeout_ms=%d",
                           global_config.native_transport,
                           engine.heartbeat_interval_ms, engine.heartbeat_timeout_ms);
        if (!engine_start_heartbeat(&engine)) {
            fprintf(stderr, "Warning: heartbeat thread failed to start - "
                            "connection health monitoring disabled\n");
        }
    }

    MonitorRegistry monitor;
    memset(&monitor, 0, sizeof(monitor));
    if (global_config.bridge_workspace_monitor_enabled) {
        init_monitor_registry(&monitor, &engine);
        register_workspace_target(&monitor, workspace);
        pthread_t watch_worker;
        pthread_create(&watch_worker, NULL, expanded_kqueue_loop, &monitor);
        pthread_detach(watch_worker);
    }

    start_codex_api_server(&engine, global_config.server_port);
    if (global_config.bridge_workspace_monitor_enabled) {
        clear_monitor_registry(&monitor);
    }

    /* Stop heartbeat before destroying engine */
    engine_stop_heartbeat(&engine);
    engine_destroy(&engine);
    return EXIT_SUCCESS;
}
