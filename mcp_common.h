/*
 * MCP Server Common Definitions
 * Includes, Macros, and Global Variable Declarations
 */

#ifndef MCP_COMMON_H
#define MCP_COMMON_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#include <process.h>
#else
#include <unistd.h>
#endif
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include "mcp_core.h"

#ifdef _WIN32
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#define access _access
#define close _close
#define dup _dup
#define dup2 _dup2
#define fileno _fileno
#define fsync _commit
#define getpid _getpid
#define open _open
#define read _read
#define unlink _unlink
#define write _write
#define mkdir(path, mode) _mkdir(path)
#endif

/* ==================== 定数・マクロ ==================== */
#ifdef _WIN32
#define MAX_PATH_LENGTH 32768
#else
#define MAX_PATH_LENGTH 4096
#endif
#define MAX_BUFFER_SIZE 1024 * 1024  // 1MB
#define TAGS_FILE "mcp_tags.json"
#define CONFIG_FILE "mcp_config.json"
#define DEFAULT_MAX_READ_FILE_SIZE (100ULL * 1024ULL * 1024ULL)
#define DEFAULT_MAX_DOWNLOAD_FILE_SIZE (200ULL * 1024ULL * 1024ULL)
#define DEFAULT_DOWNLOAD_TIMEOUT_SECONDS 30
#define DEFAULT_MAX_DOWNLOAD_TIMEOUT_SECONDS 300
#define DEFAULT_SEARCH_TIMEOUT_SECONDS 25
#define DEFAULT_SEARCH_MAX_RESULTS 100
#define DEFAULT_SEARCH_MAX_RESULTS_LIMIT 1000
#define DEFAULT_SEARCH_MAX_OUTPUT_SIZE (1ULL * 1024ULL * 1024ULL)
#define DEFAULT_TREE_DEPTH 3
#define DEFAULT_TREE_MAX_DEPTH 32
#define DEFAULT_TREE_MAX_ENTRIES 5000
#define DEFAULT_TREE_MAX_OUTPUT_SIZE (MAX_BUFFER_SIZE / 8)
#define DEFAULT_RUN_CMD_TIMEOUT_SECONDS 30
#define DEFAULT_MAX_RUN_CMD_TIMEOUT_SECONDS 300
#define DEFAULT_MAX_RUN_CMD_OUTPUT_SIZE (1ULL * 1024ULL * 1024ULL)

/* ==================== バージョン管理 ==================== */
#define MAX_SUPPORTED_VERSIONS 5
extern const char *supported_versions[];

/* ==================== misc ==================== */
#define CHECK_BUFFER_SIZE 1024

typedef enum {
    FILE_TYPE_TEXT,
    FILE_TYPE_BINARY
} FileType;

typedef enum {
    MCP_ENTRY_UNKNOWN,
    MCP_ENTRY_DIRECTORY,
    MCP_ENTRY_REGULAR
} McpEntryType;


#define EOB_LINE_BREAK_INCLUDE 1
#define EOB_LINE_BREAK_EXCLUDE 0

/* ==================== グローバル状態 ==================== */
extern char g_root_dir[MAX_PATH_LENGTH];
/* Canonical root before Linux pivot_root or Windows AppContainer entry. */
extern char g_native_root_dir[MAX_PATH_LENGTH];
extern int g_tool_call_active;
extern uint64_t g_max_read_file_size;
extern uint64_t g_max_download_file_size;
extern int g_download_timeout_seconds;
extern int g_max_download_timeout_seconds;
extern int g_search_timeout_seconds;
extern int g_search_max_results;
extern int g_search_max_results_limit;
extern size_t g_search_max_output_size;
extern int g_tree_default_depth;
extern int g_tree_max_depth;
extern int g_tree_max_entries;
extern size_t g_tree_max_output_size;
extern int g_run_cmd_timeout_seconds;
extern int g_max_run_cmd_timeout_seconds;
extern size_t g_max_run_cmd_output_size;
/* Optional MCP-virtual absolute path to the TLS CA bundle. */
extern char g_ca_bundle_path[MAX_PATH_LENGTH];
/* Absolute path inside the pivoted MCP root. */
extern char g_tag_file_path[MAX_PATH_LENGTH];

FileType determine_file_type(const char *filename, const char *filepath);
int is_valid_utf8(const unsigned char *data, size_t len);
// Base64 エンコード関数
void base64_encode(const char *input, size_t input_len, char *output);
unsigned char* base64_decode(const char *src, size_t *out_len);
void _sha256_fips(const uint8_t *data, size_t length, uint8_t hash[32]);

int normalize_path(char *out_abs_path, int out_len,char *in_path);
int normalize_new_path(char *out_abs_path, int out_len, const char *in_path);
int resolve_mcp_path(char *out_abs_path, int out_len, const char *virtual_path);
int virtualize_mcp_path(char *out_path, int out_len, const char *native_path);
int load_server_config(const char *config_path);
int atomic_write_file(const char *path, const void *data, size_t len);
int atomic_copy_file(const char *source, const char *destination);
void add_text_first_line_fprintf(FILE *fp, const char *data, const char *add_text);
char *extract_text_by_line_range(const char *in_text, size_t *out_len, int start_line, int end_line, int flag_last_lf);
void condense_crlf2lf(char* buf, size_t len);

void send_json_rpc_response(json_object *id, json_object *result);
void send_json_rpc_error(json_object *id, int code, const char *message);
void handle_error(json_object *id, int err);

int append_search_result(char *result_text, size_t capacity,
                         size_t *text_len, const char *record);
McpEntryType classify_search_entry(const char *path);

json_object* handle_create_file(json_object *params, json_object *id);

json_object* handle_read_file(json_object *params, json_object *id);
json_object* handle_stat_file(json_object *params, json_object *id);
json_object* handle_write_file(json_object *params, json_object *id);
json_object* handle_edit_file(json_object *params, json_object *id);
json_object* handle_delete_file(json_object *params, json_object *id);
json_object* handle_run_cmd(json_object *params, json_object *id);

json_object* handle_get_directory_tree(json_object *params, json_object *id);
json_object* handle_create_directory(json_object *params, json_object *id);
json_object* handle_move_file(json_object *params, json_object *id);
json_object* handle_copy_file(json_object *params, json_object *id);

json_object* handle_search_files(json_object *params, json_object *id);
json_object* handle_tag_file(json_object *params, json_object *id);
json_object* handle_batch_tag_file(json_object *params, json_object *id);
json_object* handle_get_tags(json_object *params, json_object *id);
json_object* handle_delete_tag(json_object *params, json_object *id);
json_object* handle_download_file(json_object *params, json_object *id);

json_object* handle_search_string_in_file(json_object *params, json_object *id);

void handle_request(json_object *request);

#endif /* MCP_COMMON_H */
