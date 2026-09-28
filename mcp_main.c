#include "mcp_common.h"
#include "mcp_core.h"

/* ==================== バージョン管理 ==================== */
const char *supported_versions[] = {
    "2026-07-28",  // Latest (stable)
    "2025-11-25",  // Previous stable
    "2025-06-18",  // Older stable
    "2024-11-05",  // Initial release
    NULL
};

/* ==================== グローバル状態 ==================== */
char g_root_dir[MAX_PATH_LENGTH];
char g_native_root_dir[MAX_PATH_LENGTH];
int g_tool_call_active = 0;
uint64_t g_max_read_file_size = DEFAULT_MAX_READ_FILE_SIZE;
uint64_t g_max_download_file_size = DEFAULT_MAX_DOWNLOAD_FILE_SIZE;
int g_download_timeout_seconds = DEFAULT_DOWNLOAD_TIMEOUT_SECONDS;
int g_max_download_timeout_seconds = DEFAULT_MAX_DOWNLOAD_TIMEOUT_SECONDS;
int g_search_timeout_seconds = DEFAULT_SEARCH_TIMEOUT_SECONDS;
int g_search_max_results = DEFAULT_SEARCH_MAX_RESULTS;
int g_search_max_results_limit = DEFAULT_SEARCH_MAX_RESULTS_LIMIT;
size_t g_search_max_output_size = DEFAULT_SEARCH_MAX_OUTPUT_SIZE;
int g_tree_default_depth = DEFAULT_TREE_DEPTH;
int g_tree_max_depth = DEFAULT_TREE_MAX_DEPTH;
int g_tree_max_entries = DEFAULT_TREE_MAX_ENTRIES;
size_t g_tree_max_output_size = DEFAULT_TREE_MAX_OUTPUT_SIZE;
int g_run_cmd_timeout_seconds = DEFAULT_RUN_CMD_TIMEOUT_SECONDS;
int g_max_run_cmd_timeout_seconds = DEFAULT_MAX_RUN_CMD_TIMEOUT_SECONDS;
size_t g_max_run_cmd_output_size = DEFAULT_MAX_RUN_CMD_OUTPUT_SIZE;
char g_ca_bundle_path[MAX_PATH_LENGTH] = "";
char g_tag_file_path[MAX_PATH_LENGTH] = "/" TAGS_FILE;

/* ==================== メイン関数 ==================== */

static void print_usage(FILE *stream, const char *program) {
    fprintf(stream,
            "Usage: %s [--root_dir PATH] [--config FILE]\n"
            "  -r, --root_dir PATH  Filesystem root (default: MCP_ROOT_DIR or current directory)\n"
            "  -c, --config FILE    Configuration file (default: ROOT/mcp_config.json)\n"
            "  -h, --help           Show this help\n",
            program);
}

static void dispatch_request(json_object *request, void *userdata) {
    (void)userdata;
    handle_request(request);
}

int main(int argc, char *argv[]) {
    const char *root_dir = NULL;
    const char *config_file = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(stdout, argv[0]);
            return 0;
        }
        if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--root_dir") ||
            !strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) {
            int is_root = argv[i][1] == 'r' || !strcmp(argv[i], "--root_dir");
            if (++i >= argc) {
                fprintf(stderr, "Option requires a value\n");
                print_usage(stderr, argv[0]);
                return 2;
            }
            if (is_root) root_dir = argv[i];
            else config_file = argv[i];
            continue;
        }
        fprintf(stderr, "Unknown option or positional argument: %s\n", argv[i]);
        print_usage(stderr, argv[0]);
        return 2;
    }
    if (!root_dir) root_dir = getenv("MCP_ROOT_DIR");
    if (!root_dir || strlen(root_dir) == 0) {
        root_dir = ".";
    }
    
    // ルートディレクトリを絶対パスに変換して保持
    char abs_root[MAX_PATH_LENGTH];
    if (mcp_path_canonicalize(root_dir, abs_root, sizeof(abs_root)) != 0) {
        fprintf(stderr, "Error resolving root directory: %s\n", root_dir);
        return 1;
    }
    struct stat root_stat;
    if (mcp_file_stat(abs_root, &root_stat) != 0 || !S_ISDIR(root_stat.st_mode)) {
        fprintf(stderr, "Root path is not a directory: %s\n", abs_root);
        return 1;
    }
    
    snprintf(g_root_dir, sizeof(g_root_dir), "%s", abs_root);
    snprintf(g_native_root_dir, sizeof(g_native_root_dir), "%s", abs_root);
    char default_config[MAX_PATH_LENGTH];
    char executable_config[MAX_PATH_LENGTH];
    char resolved_config[MAX_PATH_LENGTH];
    const char *config_path = default_config;
    if (config_file) {
        if (mcp_path_canonicalize(config_file, resolved_config, sizeof(resolved_config)) != 0) {
            fprintf(stderr, "Error resolving configuration file: %s\n", config_file);
            return 1;
        }
        struct stat config_stat;
        if (mcp_file_stat(resolved_config, &config_stat) != 0 || !S_ISREG(config_stat.st_mode)) {
            fprintf(stderr, "Configuration path is not a regular file: %s\n", resolved_config);
            return 1;
        }
        config_path = resolved_config;
    } else {
        int written = snprintf(default_config, sizeof(default_config), "%s/%s", abs_root, CONFIG_FILE);
        if (written < 0 || (size_t)written >= sizeof(default_config)) {
            fprintf(stderr, "Default configuration path is too long\n");
            return 1;
        }

        /* A packaged MCP server commonly keeps its configuration beside the
         * executable while exposing a different directory as the MCP root.
         * Prefer ROOT/mcp_config.json when it exists, then fall back to the
         * executable directory. */
        if (access(default_config, F_OK) != 0 && errno == ENOENT) {
            char executable_path[MAX_PATH_LENGTH];
            if (mcp_path_canonicalize(argv[0], executable_path, sizeof(executable_path)) == 0) {
                char *slash = strrchr(executable_path, '/');
                char *backslash = strrchr(executable_path, '\\');
                if (!slash || (backslash && backslash > slash)) slash = backslash;
                if (slash) {
                    *slash = '\0';
                    written = snprintf(executable_config, sizeof(executable_config),
                                       "%s/%s", executable_path, CONFIG_FILE);
                    if (written >= 0 && (size_t)written < sizeof(executable_config) &&
                        access(executable_config, R_OK) == 0) {
                        config_path = executable_config;
                    }
                }
            }
        }
    }
    load_server_config(config_path);

    if (config_file && access(config_path, F_OK) == 0) {
        char config_virtual[MAX_PATH_LENGTH];
        int config_is_in_root =
            mcp_path_to_virtual(abs_root, config_path, config_virtual,
                                sizeof(config_virtual)) == 0;
        if (!config_is_in_root &&
            mcp_sandbox_allow_startup_file(config_path) != 0) {
            fprintf(stderr, "Cannot grant sandbox access to configuration file\n");
            return 1;
        }
    }

    char visible_root[MAX_PATH_LENGTH];
    int sandbox_result = mcp_sandbox_enter(abs_root, visible_root, sizeof(visible_root));
    if (sandbox_result < 0) {
        perror("mcp_sandbox_enter");
        return 1;
    }
    if (sandbox_result > 0) return mcp_sandbox_child_exit_code();
    snprintf(g_root_dir, sizeof(g_root_dir), "%s", visible_root);

    // ========================================================================

    fprintf(stderr, "MCP Server started with root directory: %s\n", g_root_dir);
    
    if (mcp_transport_run(dispatch_request, NULL) != 0) return 1;
    
    fprintf(stderr, "MCP Server stopped\n");
    return 0;
}
