#include "unity.h"
#include "../mcp_common.h"

/* mcp_utils.c is linked without mcp_main.c in this unit test binary. */
char g_root_dir[MAX_PATH_LENGTH];
char g_native_root_dir[MAX_PATH_LENGTH];
int g_tool_call_active;
uint64_t g_max_read_file_size;
uint64_t g_max_download_file_size;
int g_download_timeout_seconds;
int g_max_download_timeout_seconds;
int g_search_max_results;
int g_search_max_results_limit;
size_t g_search_max_output_size;
int g_tree_default_depth;
int g_tree_max_depth;
int g_tree_max_entries;
size_t g_tree_max_output_size;
int g_run_cmd_timeout_seconds;
int g_max_run_cmd_timeout_seconds;
size_t g_max_run_cmd_output_size;
char g_ca_bundle_path[MAX_PATH_LENGTH];
char g_tag_file_path[MAX_PATH_LENGTH];
const char *supported_versions[] = {
    "2026-07-28",
    "2025-11-25",
    "2025-06-18",
    "2024-11-05",
    NULL
};

void setUp(void) {
    snprintf(g_native_root_dir, sizeof(g_native_root_dir), "%s", "/native/test-root");
    g_tool_call_active = 0;
    g_max_read_file_size = DEFAULT_MAX_READ_FILE_SIZE;
    g_max_download_file_size = DEFAULT_MAX_DOWNLOAD_FILE_SIZE;
    g_download_timeout_seconds = DEFAULT_DOWNLOAD_TIMEOUT_SECONDS;
    g_max_download_timeout_seconds = DEFAULT_MAX_DOWNLOAD_TIMEOUT_SECONDS;
    g_search_max_results = DEFAULT_SEARCH_MAX_RESULTS;
    g_search_max_results_limit = DEFAULT_SEARCH_MAX_RESULTS_LIMIT;
    g_search_max_output_size = DEFAULT_SEARCH_MAX_OUTPUT_SIZE;
    g_tree_default_depth = DEFAULT_TREE_DEPTH;
    g_tree_max_depth = DEFAULT_TREE_MAX_DEPTH;
    g_tree_max_entries = DEFAULT_TREE_MAX_ENTRIES;
    g_tree_max_output_size = DEFAULT_TREE_MAX_OUTPUT_SIZE;
    g_run_cmd_timeout_seconds = DEFAULT_RUN_CMD_TIMEOUT_SECONDS;
    g_max_run_cmd_timeout_seconds = DEFAULT_MAX_RUN_CMD_TIMEOUT_SECONDS;
    g_max_run_cmd_output_size = DEFAULT_MAX_RUN_CMD_OUTPUT_SIZE;
    g_ca_bundle_path[0] = '\0';
}
void tearDown(void) {}

static char *capture_stdout_for_handler(json_object *(*handler)(json_object *, json_object *),
                                        json_object *params, json_object *id,
                                        json_object **handler_result) {
    FILE *capture = tmpfile();
    TEST_ASSERT_NOT_NULL(capture);
    fflush(stdout);
    int saved_stdout = dup(STDOUT_FILENO);
    TEST_ASSERT_GREATER_OR_EQUAL(0, saved_stdout);
    TEST_ASSERT_GREATER_OR_EQUAL(0, dup2(fileno(capture), STDOUT_FILENO));

    *handler_result = handler(params, id);
    fflush(stdout);
    TEST_ASSERT_GREATER_OR_EQUAL(0, dup2(saved_stdout, STDOUT_FILENO));
    close(saved_stdout);

    TEST_ASSERT_EQUAL_INT(0, fseek(capture, 0, SEEK_END));
    long length = ftell(capture);
    TEST_ASSERT_GREATER_OR_EQUAL(0, length);
    TEST_ASSERT_EQUAL_INT(0, fseek(capture, 0, SEEK_SET));
    char *output = malloc((size_t)length + 1);
    TEST_ASSERT_NOT_NULL(output);
    TEST_ASSERT_EQUAL_UINT((size_t)length, fread(output, 1, (size_t)length, capture));
    output[length] = '\0';
    fclose(capture);
    return output;
}

static char *capture_stdout_for_request(json_object *request) {
    FILE *capture = tmpfile();
    TEST_ASSERT_NOT_NULL(capture);
    fflush(stdout);
    int saved_stdout = dup(STDOUT_FILENO);
    TEST_ASSERT_GREATER_OR_EQUAL(0, saved_stdout);
    TEST_ASSERT_GREATER_OR_EQUAL(0, dup2(fileno(capture), STDOUT_FILENO));

    handle_request(request);
    fflush(stdout);
    TEST_ASSERT_GREATER_OR_EQUAL(0, dup2(saved_stdout, STDOUT_FILENO));
    close(saved_stdout);

    TEST_ASSERT_EQUAL_INT(0, fseek(capture, 0, SEEK_END));
    long length = ftell(capture);
    TEST_ASSERT_GREATER_OR_EQUAL(0, length);
    TEST_ASSERT_EQUAL_INT(0, fseek(capture, 0, SEEK_SET));
    char *output = malloc((size_t)length + 1);
    TEST_ASSERT_NOT_NULL(output);
    TEST_ASSERT_EQUAL_UINT((size_t)length, fread(output, 1, (size_t)length, capture));
    output[length] = '\0';
    fclose(capture);
    return output;
}

/* ==================== Base64 テスト ==================== */

void test_base64_encode_simple(void) {
    char output[256];
    
    // "Hello" -> "SGVsbG8="
    base64_encode("Hello", 5, output);
    TEST_ASSERT_EQUAL_STRING("SGVsbG8=", output);
}

void test_base64_encode_empty(void) {
    char output[256];
    
    // "" -> ""
    base64_encode("", 0, output);
    TEST_ASSERT_EQUAL_STRING("", output);
}

void test_base64_decode_simple(void) {
    size_t out_len;
    unsigned char *decoded = base64_decode("SGVsbG8=", &out_len);
    
    TEST_ASSERT_NOT_NULL(decoded);
    TEST_ASSERT_EQUAL_UINT(5, out_len);
    TEST_ASSERT_EQUAL_STRING("Hello", (char *)decoded);
    
    free(decoded);
}

void test_base64_roundtrip(void) {
    const char *original = "The quick brown fox jumps over the lazy dog";
    char encoded[256];
    size_t decoded_len;
    unsigned char *decoded;
    
    base64_encode(original, strlen(original), encoded);
    decoded = base64_decode(encoded, &decoded_len);
    
    TEST_ASSERT_NOT_NULL(decoded);
    TEST_ASSERT_EQUAL_STRING(original, (char *)decoded);
    TEST_ASSERT_EQUAL_UINT(strlen(original), decoded_len);
    
    free(decoded);
}

/* ==================== UTF-8 tests ==================== */

void test_utf8_accepts_ascii_and_multibyte(void) {
    const unsigned char valid[] = "ASCII and 日本語 \xf0\x9f\x98\x80";
    TEST_ASSERT_TRUE(is_valid_utf8(valid, sizeof(valid) - 1));
}

void test_utf8_rejects_legacy_and_malformed_sequences(void) {
    const unsigned char latin1[] = {0x63, 0x61, 0x66, 0xe9};
    const unsigned char overlong[] = {0xc0, 0xaf};
    const unsigned char surrogate[] = {0xed, 0xa0, 0x80};
    const unsigned char truncated[] = {0xe3, 0x81};
    TEST_ASSERT_FALSE(is_valid_utf8(latin1, sizeof(latin1)));
    TEST_ASSERT_FALSE(is_valid_utf8(overlong, sizeof(overlong)));
    TEST_ASSERT_FALSE(is_valid_utf8(surrogate, sizeof(surrogate)));
    TEST_ASSERT_FALSE(is_valid_utf8(truncated, sizeof(truncated)));
}

/* ==================== Search tests ==================== */

void test_append_search_result_handles_invalid_near_end_state(void) {
    unsigned char storage[110];
    memset(storage, 0xa5, sizeof(storage));
    char *buffer = (char *)storage;
    size_t text_len = 60;
    buffer[text_len] = '\0';

    TEST_ASSERT_FALSE(append_search_result(buffer, 100, &text_len,
                                           "record that must not be written"));
    TEST_ASSERT_EQUAL_UINT(60, text_len);
    for (size_t i = 61; i < sizeof(storage); i++) {
        TEST_ASSERT_EQUAL_HEX8(0xa5, storage[i]);
    }
}

void test_append_search_result_writes_marker_at_reserved_boundary(void) {
    char buffer[100];
    memset(buffer, 'x', sizeof(buffer));
    size_t text_len = 59;
    buffer[text_len] = '\0';

    TEST_ASSERT_FALSE(append_search_result(buffer, sizeof(buffer), &text_len, "x"));
    TEST_ASSERT_EQUAL_UINT(99, text_len);
    TEST_ASSERT_EQUAL_CHAR('\0', buffer[99]);
    TEST_ASSERT_NOT_NULL(strstr(buffer + 59, "Output truncated"));
}

void test_classify_search_entry_resolves_unknown_without_following_symlink(void) {
    char dir[] = "/tmp/mcp-search-dir-XXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));

    char file[MAX_PATH_LENGTH];
    snprintf(file, sizeof(file), "%s/file", dir);
    int fd = open(file, O_CREAT | O_WRONLY, 0600);
    TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
    close(fd);

    char link[MAX_PATH_LENGTH];
    snprintf(link, sizeof(link), "%s/link", dir);
    TEST_ASSERT_EQUAL_INT(0, symlink(file, link));

    TEST_ASSERT_EQUAL_INT(MCP_ENTRY_DIRECTORY, classify_search_entry(dir));
    TEST_ASSERT_EQUAL_INT(MCP_ENTRY_REGULAR, classify_search_entry(file));
    TEST_ASSERT_EQUAL_INT(MCP_ENTRY_UNKNOWN, classify_search_entry(link));

    TEST_ASSERT_EQUAL_INT(0, unlink(link));
    TEST_ASSERT_EQUAL_INT(0, unlink(file));
    TEST_ASSERT_EQUAL_INT(0, rmdir(dir));
}

void test_search_string_reports_incremental_line_numbers(void) {
    char path[] = "/tmp/mcp-search-text-XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
    const char content[] = "alpha\nneedle\nx needle\ny\nneedle\n";
    TEST_ASSERT_EQUAL_INT((int)(sizeof(content) - 1),
                          (int)write(fd, content, sizeof(content) - 1));
    close(fd);

    json_object *params = json_object_new_object();
    json_object_object_add(params, "path", json_object_new_string(path));
    json_object_object_add(params, "search_string", json_object_new_string("needle"));
    json_object *id = json_object_new_int(1);
    json_object *result = handle_search_string_in_file(params, id);
    TEST_ASSERT_NOT_NULL(result);

    json_object *items = json_object_object_get(result, "content");
    const char *text = json_object_get_string(
        json_object_object_get(json_object_array_get_idx(items, 0), "text"));
    TEST_ASSERT_NOT_NULL(strstr(text, "start_line=2, end_line=2"));
    TEST_ASSERT_NOT_NULL(strstr(text, "start_line=3, end_line=3"));
    TEST_ASSERT_NOT_NULL(strstr(text, "start_line=5, end_line=5"));

    json_object_put(result);
    json_object_put(id);
    json_object_put(params);
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
}

/* ==================== Handler/protocol tests ==================== */

void test_create_directory_failure_emits_one_error_response(void) {
    char dir[] = "/tmp/mcp-existing-dir-XXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    json_object *params = json_object_new_object();
    json_object_object_add(params, "path", json_object_new_string(dir));
    json_object *id = json_object_new_int(77);
    json_object *handler_result = NULL;
    g_tool_call_active = 1;

    char *output = capture_stdout_for_handler(handle_create_directory, params, id,
                                               &handler_result);
    g_tool_call_active = 0;
    TEST_ASSERT_NULL(handler_result);
    TEST_ASSERT_EQUAL_INT(1, (int)(strchr(output, '\n') != NULL));
    TEST_ASSERT_NULL(strchr(strchr(output, '\n') + 1, '\n'));

    json_object *response = json_tokener_parse(output);
    TEST_ASSERT_NOT_NULL(response);
    json_object *result = json_object_object_get(response, "result");
    TEST_ASSERT_TRUE(json_object_get_boolean(json_object_object_get(result, "isError")));
    TEST_ASSERT_NOT_NULL(strstr(output, "path already exists"));
    TEST_ASSERT_NULL(strstr(output, "created successfully"));

    json_object_put(response);
    free(output);
    json_object_put(id);
    json_object_put(params);
    TEST_ASSERT_EQUAL_INT(0, rmdir(dir));
}

void test_read_file_schema_declares_utf8_only(void) {
    json_object *initialize = json_object_new_object();
    json_object_object_add(initialize, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(initialize, "id", json_object_new_int(1));
    json_object_object_add(initialize, "method", json_object_new_string("initialize"));
    json_object *init_params = json_object_new_object();
    json_object_object_add(init_params, "protocolVersion", json_object_new_string("2025-11-25"));
    json_object_object_add(initialize, "params", init_params);
    char *initialize_output = capture_stdout_for_request(initialize);
    json_object *initialize_response = json_tokener_parse(initialize_output);
    TEST_ASSERT_NOT_NULL(initialize_response);
    json_object *initialize_result = json_object_object_get(initialize_response, "result");
    const char *instructions = json_object_get_string(
        json_object_object_get(initialize_result, "instructions"));
    TEST_ASSERT_NOT_NULL(instructions);
    TEST_ASSERT_NOT_NULL(strstr(instructions, "/native/test-root"));
    TEST_ASSERT_NOT_NULL(strstr(instructions, "MCP virtual root \"/\""));
    TEST_ASSERT_NOT_NULL(strstr(instructions, "/project/file.txt"));
    json_object_put(initialize_response);
    free(initialize_output);
    json_object_put(initialize);

    json_object *initialized = json_object_new_object();
    json_object_object_add(initialized, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(initialized, "method",
                           json_object_new_string("notifications/initialized"));
    char *initialized_output = capture_stdout_for_request(initialized);
    TEST_ASSERT_EQUAL_STRING("", initialized_output);
    free(initialized_output);
    json_object_put(initialized);

    json_object *request = json_object_new_object();
    json_object_object_add(request, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(request, "id", json_object_new_int(2));
    json_object_object_add(request, "method", json_object_new_string("tools/list"));
    char *output = capture_stdout_for_request(request);
    json_object *response = json_tokener_parse(output);
    TEST_ASSERT_NOT_NULL(response);
    json_object *tools = json_object_object_get(
        json_object_object_get(response, "result"), "tools");
    json_object *read_tool = NULL;
    for (size_t i = 0; i < json_object_array_length(tools); i++) {
        json_object *candidate = json_object_array_get_idx(tools, i);
        if (!strcmp(json_object_get_string(json_object_object_get(candidate, "name")),
                    "read_file")) {
            read_tool = candidate;
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(read_tool);
    TEST_ASSERT_NOT_NULL(strstr(json_object_get_string(
        json_object_object_get(read_tool, "description")), "UTF-8"));
    json_object *properties = json_object_object_get(
        json_object_object_get(read_tool, "inputSchema"), "properties");
    TEST_ASSERT_NULL(json_object_object_get(properties, "encoding"));

    json_object *stat_tool = NULL;
    json_object *run_cmd_tool = NULL;
    for (size_t i = 0; i < json_object_array_length(tools); i++) {
        json_object *candidate = json_object_array_get_idx(tools, i);
        if (!strcmp(json_object_get_string(json_object_object_get(candidate, "name")),
                    "stat_file")) {
            stat_tool = candidate;
        }
        if (!strcmp(json_object_get_string(json_object_object_get(candidate, "name")),
                    "run_cmd")) {
            run_cmd_tool = candidate;
        }
        TEST_ASSERT_NOT_EQUAL_STRING("file_stat", json_object_get_string(
            json_object_object_get(candidate, "name")));
    }
    TEST_ASSERT_NOT_NULL(stat_tool);
    TEST_ASSERT_NOT_NULL(run_cmd_tool);
    TEST_ASSERT_NOT_NULL(strstr(json_object_get_string(
        json_object_object_get(run_cmd_tool, "description")), "UTF-8"));
    properties = json_object_object_get(
        json_object_object_get(run_cmd_tool, "inputSchema"), "properties");
    TEST_ASSERT_NOT_NULL(json_object_object_get(properties, "command"));
    TEST_ASSERT_NOT_NULL(json_object_object_get(properties, "path"));
    TEST_ASSERT_EQUAL_INT(g_max_run_cmd_timeout_seconds,
        json_object_get_int(json_object_object_get(
            json_object_object_get(properties, "timeout_seconds"), "maximum")));
    properties = json_object_object_get(
        json_object_object_get(stat_tool, "inputSchema"), "properties");
    TEST_ASSERT_NOT_NULL(json_object_object_get(properties, "path"));

    json_object_put(response);
    free(output);
    json_object_put(request);
}

void test_read_file_returns_non_utf8_text_as_base64(void) {
    char path[] = "/tmp/mcp-non-utf8-XXXXXX.txt";
    int fd = mkstemps(path, 4);
    TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
    const unsigned char latin1[] = {0x63, 0x61, 0x66, 0xe9};
    TEST_ASSERT_EQUAL_INT((int)sizeof(latin1),
                          (int)write(fd, latin1, sizeof(latin1)));
    close(fd);

    json_object *params = json_object_new_object();
    json_object_object_add(params, "path", json_object_new_string(path));
    json_object *id = json_object_new_int(88);
    json_object *result = handle_read_file(params, id);
    TEST_ASSERT_NOT_NULL(result);
    json_object *content = json_object_object_get(result, "content");
    json_object *item = json_object_array_get_idx(content, 0);
    TEST_ASSERT_EQUAL_STRING("text", json_object_get_string(
        json_object_object_get(item, "type")));
    TEST_ASSERT_EQUAL_STRING("Y2Fm6Q==", json_object_get_string(
        json_object_object_get(item, "text")));

    json_object_put(result);
    json_object_put(id);
    json_object_put(params);
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
}

static void write_stat_fixture(const char *root, const char *name,
                               const void *bytes, size_t length) {
    char path[MAX_PATH_LENGTH];
    snprintf(path, sizeof(path), "%s/%s", root, name);
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_UINT(length, fwrite(bytes, 1, length, fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
}

static const char *first_tool_text(json_object *result) {
    TEST_ASSERT_NOT_NULL(result);
    json_object *items = json_object_object_get(result, "content");
    TEST_ASSERT_NOT_NULL(items);
    json_object *item = json_object_array_get_idx(items, 0);
    TEST_ASSERT_NOT_NULL(item);
    const char *text = json_object_get_string(json_object_object_get(item, "text"));
    TEST_ASSERT_NOT_NULL(text);
    return text;
}

void test_hidden_directories_are_opt_in_for_tree_and_search(void) {
    char root[] = "/tmp/mcp-hidden-dirs-XXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(root));
    char previous_root[MAX_PATH_LENGTH];
    snprintf(previous_root, sizeof(previous_root), "%s", g_root_dir);
    snprintf(g_root_dir, sizeof(g_root_dir), "%s", root);

    char history_dir[MAX_PATH_LENGTH];
    snprintf(history_dir, sizeof(history_dir), "%s/.history", root);
    TEST_ASSERT_EQUAL_INT(0, mkdir(history_dir, 0700));
    write_stat_fixture(root, "visible.txt", "needle visible\n", 15);
    write_stat_fixture(root, ".dotfile", "needle dotfile\n", 15);
    write_stat_fixture(root, ".history/change.patch", "needle history\n", 15);

    json_object *id = json_object_new_int(91);
    json_object *params = json_object_new_object();
    json_object_object_add(params, "path", json_object_new_string("/"));
    json_object_object_add(params, "depth", json_object_new_int(2));
    json_object *result = handle_get_directory_tree(params, id);
    TEST_ASSERT_NOT_NULL(strstr(first_tool_text(result), "visible.txt"));
    TEST_ASSERT_NOT_NULL(strstr(first_tool_text(result), ".dotfile"));
    TEST_ASSERT_NULL(strstr(first_tool_text(result), ".history"));
    json_object_put(result);

    json_object_object_add(params, "include_hidden_directories", json_object_new_boolean(true));
    result = handle_get_directory_tree(params, id);
    TEST_ASSERT_NOT_NULL(strstr(first_tool_text(result), ".history"));
    TEST_ASSERT_NOT_NULL(strstr(first_tool_text(result), "change.patch"));
    json_object_put(result);
    json_object_object_add(params, "include_hidden_directories", json_object_new_boolean(false));
    json_object_object_add(params, "path", json_object_new_string("/.history"));
    result = handle_get_directory_tree(params, id);
    TEST_ASSERT_NOT_NULL(strstr(first_tool_text(result), "change.patch"));
    json_object_put(result);
    json_object_put(params);

    params = json_object_new_object();
    json_object_object_add(params, "path", json_object_new_string("/"));
    json_object_object_add(params, "pattern", json_object_new_string("needle"));
    result = handle_search_files(params, id);
    TEST_ASSERT_NOT_NULL(strstr(first_tool_text(result), "visible.txt"));
    TEST_ASSERT_NOT_NULL(strstr(first_tool_text(result), ".dotfile"));
    TEST_ASSERT_NULL(strstr(first_tool_text(result), "change.patch"));
    json_object_put(result);

    json_object_object_add(params, "include_hidden_directories", json_object_new_boolean(true));
    result = handle_search_files(params, id);
    TEST_ASSERT_NOT_NULL(strstr(first_tool_text(result), "change.patch"));
    json_object_put(result);
    json_object_object_add(params, "include_hidden_directories", json_object_new_boolean(false));
    json_object_object_add(params, "path", json_object_new_string("/.history"));
    result = handle_search_files(params, id);
    TEST_ASSERT_NOT_NULL(strstr(first_tool_text(result), "change.patch"));
    json_object_put(result);
    json_object_put(params);
    json_object_put(id);

    char path[MAX_PATH_LENGTH];
    snprintf(path, sizeof(path), "%s/.history/change.patch", root);
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
    TEST_ASSERT_EQUAL_INT(0, rmdir(history_dir));
    snprintf(path, sizeof(path), "%s/.dotfile", root);
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
    snprintf(path, sizeof(path), "%s/visible.txt", root);
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
    TEST_ASSERT_EQUAL_INT(0, rmdir(root));
    snprintf(g_root_dir, sizeof(g_root_dir), "%s", previous_root);
}

static json_object *stat_fixture(const char *virtual_path) {
    json_object *params = json_object_new_object();
    json_object_object_add(params, "path", json_object_new_string(virtual_path));
    json_object *id = json_object_new_int(89);
    json_object *result = handle_stat_file(params, id);
    json_object_put(id);
    json_object_put(params);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_NULL(json_object_object_get(result, "isError"));
    json_object *items = json_object_object_get(result, "content");
    json_object *item = json_object_array_get_idx(items, 0);
    const char *json_text = json_object_get_string(json_object_object_get(item, "text"));
    json_object *metadata = json_tokener_parse(json_text);
    TEST_ASSERT_NOT_NULL(metadata);
    json_object_put(result);
    return metadata;
}

void test_stat_file_reports_size_time_and_text_line_count(void) {
    char root[] = "/tmp/mcp-stat-XXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(root));
    char previous_root[MAX_PATH_LENGTH];
    snprintf(previous_root, sizeof(previous_root), "%s", g_root_dir);
    snprintf(g_root_dir, sizeof(g_root_dir), "%s", root);

    const char crlf[] = "one\r\ntwo\r\nthree";
    write_stat_fixture(root, "crlf.txt", crlf, sizeof(crlf) - 1);
    json_object *metadata = stat_fixture("/crlf.txt");
    TEST_ASSERT_EQUAL_STRING("/crlf.txt", json_object_get_string(
        json_object_object_get(metadata, "path")));
    TEST_ASSERT_EQUAL_STRING("ok", json_object_get_string(
        json_object_object_get(metadata, "status")));
    TEST_ASSERT_TRUE(json_object_get_boolean(json_object_object_get(metadata, "exists")));
    TEST_ASSERT_EQUAL_INT64(sizeof(crlf) - 1, json_object_get_int64(
        json_object_object_get(metadata, "size_bytes")));
    TEST_ASSERT_TRUE(json_object_get_boolean(json_object_object_get(metadata, "is_text")));
    TEST_ASSERT_EQUAL_INT64(3, json_object_get_int64(
        json_object_object_get(metadata, "line_count")));
    const char *modified_at = json_object_get_string(
        json_object_object_get(metadata, "modified_at"));
    TEST_ASSERT_NOT_NULL(modified_at);
    TEST_ASSERT_EQUAL_INT(20, (int)strlen(modified_at));
    TEST_ASSERT_EQUAL_CHAR('Z', modified_at[19]);
    json_object_put(metadata);

    const char newline[] = "one\n";
    write_stat_fixture(root, "newline.txt", newline, sizeof(newline) - 1);
    metadata = stat_fixture("/newline.txt");
    TEST_ASSERT_EQUAL_INT64(1, json_object_get_int64(
        json_object_object_get(metadata, "line_count")));
    json_object_put(metadata);

    write_stat_fixture(root, "empty.txt", "", 0);
    metadata = stat_fixture("/empty.txt");
    TEST_ASSERT_EQUAL_INT64(0, json_object_get_int64(
        json_object_object_get(metadata, "line_count")));
    json_object_put(metadata);

    const unsigned char binary[] = {'a', 0, 'b', '\n'};
    write_stat_fixture(root, "binary.dat", binary, sizeof(binary));
    metadata = stat_fixture("/binary.dat");
    TEST_ASSERT_FALSE(json_object_get_boolean(json_object_object_get(metadata, "is_text")));
    TEST_ASSERT_NULL(json_object_object_get(metadata, "line_count"));
    json_object_put(metadata);

    const unsigned char invalid_utf8[] = {'a', 0xc3, 0x28, '\n'};
    write_stat_fixture(root, "invalid.txt", invalid_utf8, sizeof(invalid_utf8));
    metadata = stat_fixture("/invalid.txt");
    TEST_ASSERT_FALSE(json_object_get_boolean(json_object_object_get(metadata, "is_text")));
    TEST_ASSERT_NULL(json_object_object_get(metadata, "line_count"));
    json_object_put(metadata);

    unsigned char boundary[65539];
    memset(boundary, 'a', sizeof(boundary));
    boundary[65535] = 0xe3;
    boundary[65536] = 0x81;
    boundary[65537] = 0x82;
    boundary[65538] = '\n';
    write_stat_fixture(root, "boundary.txt", boundary, sizeof(boundary));
    metadata = stat_fixture("/boundary.txt");
    TEST_ASSERT_TRUE(json_object_get_boolean(json_object_object_get(metadata, "is_text")));
    TEST_ASSERT_EQUAL_INT64(1, json_object_get_int64(
        json_object_object_get(metadata, "line_count")));
    json_object_put(metadata);

    const char *names[] = {"crlf.txt", "newline.txt", "empty.txt",
                           "binary.dat", "invalid.txt", "boundary.txt"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[MAX_PATH_LENGTH];
        snprintf(path, sizeof(path), "%s/%s", root, names[i]);
        TEST_ASSERT_EQUAL_INT(0, unlink(path));
    }
    TEST_ASSERT_EQUAL_INT(0, rmdir(root));
    snprintf(g_root_dir, sizeof(g_root_dir), "%s", previous_root);
}

void test_stat_file_reports_missing_and_permission_as_results(void) {
    char root[] = "/tmp/mcp-stat-unavailable-XXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(root));
    char previous_root[MAX_PATH_LENGTH];
    snprintf(previous_root, sizeof(previous_root), "%s", g_root_dir);
    snprintf(g_root_dir, sizeof(g_root_dir), "%s", root);

    json_object *metadata = stat_fixture("/missing.txt");
    TEST_ASSERT_EQUAL_STRING("not_found", json_object_get_string(
        json_object_object_get(metadata, "status")));
    TEST_ASSERT_FALSE(json_object_get_boolean(json_object_object_get(metadata, "exists")));
    TEST_ASSERT_EQUAL_STRING("No such file", json_object_get_string(
        json_object_object_get(metadata, "message")));
    json_object_put(metadata);

    metadata = stat_fixture("/missing-parent/child.txt");
    TEST_ASSERT_EQUAL_STRING("not_found", json_object_get_string(
        json_object_object_get(metadata, "status")));
    json_object_put(metadata);

    json_object *params = json_object_new_object();
    json_object_object_add(params, "path", json_object_new_string("/../invalid.txt"));
    json_object *id = json_object_new_int(90);
    json_object *invalid_result = NULL;
    char *response = capture_stdout_for_handler(handle_stat_file, params, id,
                                                 &invalid_result);
    TEST_ASSERT_NULL(invalid_result);
    TEST_ASSERT_NOT_NULL(strstr(response, "-32602"));
    free(response);
    json_object_put(id);
    json_object_put(params);

    write_stat_fixture(root, "private.txt", "private", 7);
    char private_path[MAX_PATH_LENGTH];
    snprintf(private_path, sizeof(private_path), "%s/private.txt", root);
    TEST_ASSERT_EQUAL_INT(0, chmod(private_path, 0000));
    FILE *probe = fopen(private_path, "rb");
    if (probe) {
        fclose(probe); /* Privileged test runners may bypass file mode bits. */
    } else {
        metadata = stat_fixture("/private.txt");
        TEST_ASSERT_EQUAL_STRING("permission_denied", json_object_get_string(
            json_object_object_get(metadata, "status")));
        TEST_ASSERT_NULL(json_object_object_get(metadata, "exists"));
        TEST_ASSERT_EQUAL_STRING("Permission denied", json_object_get_string(
            json_object_object_get(metadata, "message")));
        json_object_put(metadata);
    }
    TEST_ASSERT_EQUAL_INT(0, chmod(private_path, 0600));
    TEST_ASSERT_EQUAL_INT(0, unlink(private_path));
    TEST_ASSERT_EQUAL_INT(0, rmdir(root));
    snprintf(g_root_dir, sizeof(g_root_dir), "%s", previous_root);
}

static json_object *run_cmd_fixture(const char *command, const char *path,
                                    int timeout_seconds) {
    json_object *params = json_object_new_object();
    json_object_object_add(params, "command", json_object_new_string(command));
    if (path) json_object_object_add(params, "path", json_object_new_string(path));
    if (timeout_seconds > 0)
        json_object_object_add(params, "timeout_seconds", json_object_new_int(timeout_seconds));
    json_object *id = json_object_new_int(92);
    json_object *result = handle_run_cmd(params, id);
    json_object_put(id);
    json_object_put(params);
    TEST_ASSERT_NOT_NULL(result);
    json_object *items = json_object_object_get(result, "content");
    json_object *item = json_object_array_get_idx(items, 0);
    const char *text = json_object_get_string(json_object_object_get(item, "text"));
    json_object *summary = json_tokener_parse(text);
    TEST_ASSERT_NOT_NULL(summary);
    json_object_put(result);
    return summary;
}

void test_run_cmd_reports_output_exit_path_timeout_and_truncation(void) {
    char root[] = "/tmp/mcp-run-cmd-XXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(root));
    char previous_root[MAX_PATH_LENGTH];
    snprintf(previous_root, sizeof(previous_root), "%s", g_root_dir);
    snprintf(g_root_dir, sizeof(g_root_dir), "%s", root);
    char subdir[MAX_PATH_LENGTH];
    snprintf(subdir, sizeof(subdir), "%s/sub", root);
    TEST_ASSERT_EQUAL_INT(0, mkdir(subdir, 0700));

    json_object *summary = run_cmd_fixture(
        "printf 'out'; printf 'err' >&2; exit 7", "/sub", 0);
    TEST_ASSERT_EQUAL_INT(7, json_object_get_int(json_object_object_get(summary, "exit_code")));
    TEST_ASSERT_FALSE(json_object_get_boolean(json_object_object_get(summary, "timed_out")));
    TEST_ASSERT_EQUAL_STRING("/sub", json_object_get_string(json_object_object_get(summary, "path")));
    TEST_ASSERT_EQUAL_STRING("outerr", json_object_get_string(json_object_object_get(summary, "output")));
    json_object_put(summary);

    size_t previous_limit = g_max_run_cmd_output_size;
    g_max_run_cmd_output_size = 4;
    summary = run_cmd_fixture("printf '123456'", NULL, 0);
    TEST_ASSERT_TRUE(json_object_get_boolean(json_object_object_get(summary, "truncated")));
    TEST_ASSERT_EQUAL_STRING("1234", json_object_get_string(json_object_object_get(summary, "output")));
    json_object_put(summary);
    g_max_run_cmd_output_size = previous_limit;

    int previous_max_timeout = g_max_run_cmd_timeout_seconds;
    g_max_run_cmd_timeout_seconds = 1;
    summary = run_cmd_fixture("sleep 2", "/", 1);
    TEST_ASSERT_TRUE(json_object_get_boolean(json_object_object_get(summary, "timed_out")));
    TEST_ASSERT_EQUAL_INT(124, json_object_get_int(json_object_object_get(summary, "exit_code")));
    json_object_put(summary);
    g_max_run_cmd_timeout_seconds = previous_max_timeout;

    TEST_ASSERT_EQUAL_INT(0, rmdir(subdir));
    TEST_ASSERT_EQUAL_INT(0, rmdir(root));
    snprintf(g_root_dir, sizeof(g_root_dir), "%s", previous_root);
}

static json_object *make_edit_params(const char *old_content, const char *new_content) {
    json_object *params = json_object_new_object();
    json_object_object_add(params, "path", json_object_new_string("/sample.txt"));
    json_object_object_add(params, "start_line", json_object_new_int(2));
    json_object_object_add(params, "end_line", json_object_new_int(2));
    json_object_object_add(params, "old_content", json_object_new_string(old_content));
    json_object_object_add(params, "new_content", json_object_new_string(new_content));
    return params;
}

void test_edit_file_accepts_equivalent_line_endings_and_preserves_crlf(void) {
    char root[] = "/tmp/mcp-edit-crlf-XXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(root));
    char previous_root[MAX_PATH_LENGTH];
    snprintf(previous_root, sizeof(previous_root), "%s", g_root_dir);
    snprintf(g_root_dir, sizeof(g_root_dir), "%s", root);

    char path[MAX_PATH_LENGTH];
    snprintf(path, sizeof(path), "%s/sample.txt", root);
    const char initial[] = "first\r\nsecond\r\nthird\r\n";
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_UINT(sizeof(initial) - 1,
                           fwrite(initial, 1, sizeof(initial) - 1, fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));

    json_object *id = json_object_new_int(90);
    json_object *params = make_edit_params("second\r\n", "SECOND\r\n");
    json_object *result = handle_edit_file(params, id);
    TEST_ASSERT_NOT_NULL(result);
    json_object_put(result);
    json_object_put(params);

    params = make_edit_params("SECOND\n", "changed\n");
    result = handle_edit_file(params, id);
    TEST_ASSERT_NOT_NULL(result);
    json_object_put(result);
    json_object_put(params);
    json_object_put(id);

    const char expected[] = "first\r\nchanged\r\nthird\r\n";
    char actual[sizeof(expected)] = {0};
    fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_UINT(sizeof(expected) - 1,
                           fread(actual, 1, sizeof(expected) - 1, fp));
    TEST_ASSERT_EQUAL_INT(EOF, fgetc(fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
    TEST_ASSERT_EQUAL_MEMORY(expected, actual, sizeof(expected) - 1);

    char history_path[MAX_PATH_LENGTH];
    snprintf(history_path, sizeof(history_path), "%s/.history", root);
    DIR *history = opendir(history_path);
    TEST_ASSERT_NOT_NULL(history);
    struct dirent *entry;
    while ((entry = readdir(history)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        size_t artifact_length = strlen(history_path) + strlen(entry->d_name) + 2;
        char *artifact = malloc(artifact_length);
        TEST_ASSERT_NOT_NULL(artifact);
        snprintf(artifact, artifact_length, "%s/%s", history_path, entry->d_name);
        TEST_ASSERT_EQUAL_INT(0, unlink(artifact));
        free(artifact);
    }
    TEST_ASSERT_EQUAL_INT(0, closedir(history));
    TEST_ASSERT_EQUAL_INT(0, rmdir(history_path));
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
    TEST_ASSERT_EQUAL_INT(0, rmdir(root));
    snprintf(g_root_dir, sizeof(g_root_dir), "%s", previous_root);
}

/* ==================== SHA-256 テスト ==================== */

void test_sha256_empty(void) {
    uint8_t hash[32];
    _sha256_fips((const uint8_t *)"", 0, hash);
    
    // Empty string SHA-256: e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    TEST_ASSERT_EQUAL_HEX8(0xe3, hash[0]);
    TEST_ASSERT_EQUAL_HEX8(0xb0, hash[1]);
    TEST_ASSERT_EQUAL_HEX8(0xc4, hash[2]);
}

void test_sha256_hello(void) {
    uint8_t hash[32];
    _sha256_fips((const uint8_t *)"Hello", 5, hash);
    
    // "Hello" SHA-256: 185f8db32271fe25f561a6fc938b2e264306ec304eda518007d1764826381969
    TEST_ASSERT_EQUAL_HEX8(0x18, hash[0]);
    TEST_ASSERT_EQUAL_HEX8(0x5f, hash[1]);
}

/* ==================== Configuration tests ==================== */

void test_config_tag_file_path(void) {
    char config_path[] = "/tmp/mcp-file-config-XXXXXX";
    int fd = mkstemp(config_path);
    TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
    FILE *fp = fdopen(fd, "w");
    TEST_ASSERT_NOT_NULL(fp);
    fputs("{\"tag_file_path\":\"metadata/tags.json\"}", fp);
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));

    TEST_ASSERT_EQUAL_INT(0, load_server_config(config_path));
    TEST_ASSERT_EQUAL_STRING("/metadata/tags.json", g_tag_file_path);
    unlink(config_path);
}

void test_config_rejects_tag_file_path_outside_root(void) {
    char config_path[] = "/tmp/mcp-file-config-XXXXXX";
    int fd = mkstemp(config_path);
    TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
    FILE *fp = fdopen(fd, "w");
    TEST_ASSERT_NOT_NULL(fp);
    fputs("{\"tag_file_path\":\"../mcp_tags.json\"}", fp);
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));

    TEST_ASSERT_EQUAL_INT(0, load_server_config(config_path));
    TEST_ASSERT_EQUAL_STRING("/mcp_tags.json", g_tag_file_path);
    unlink(config_path);
}

void test_config_ca_bundle_path(void) {
    char config_path[] = "/tmp/mcp-file-config-XXXXXX";
    int fd = mkstemp(config_path);
    TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
    FILE *fp = fdopen(fd, "w");
    TEST_ASSERT_NOT_NULL(fp);
    fputs("{\"ca_bundle_path\":\"/etc/ssl/certs/ca-certificates.crt\"}", fp);
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));

    TEST_ASSERT_EQUAL_INT(0, load_server_config(config_path));
    TEST_ASSERT_EQUAL_STRING("/etc/ssl/certs/ca-certificates.crt", g_ca_bundle_path);
    unlink(config_path);
}

void test_config_rejects_nonvirtual_ca_bundle_paths(void) {
    const char *invalid_values[] = {
        "etc/ssl/certs/ca-certificates.crt",
        "/etc/../outside.crt",
        "C:\\\\certs\\\\ca-bundle.crt",
        "//server/share/ca-bundle.crt",
        NULL
    };

    for (size_t i = 0; invalid_values[i]; i++) {
        char config_path[] = "/tmp/mcp-file-config-XXXXXX";
        int fd = mkstemp(config_path);
        TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
        FILE *fp = fdopen(fd, "w");
        TEST_ASSERT_NOT_NULL(fp);
        fprintf(fp, "{\"ca_bundle_path\":\"%s\"}", invalid_values[i]);
        TEST_ASSERT_EQUAL_INT(0, fclose(fp));

        TEST_ASSERT_EQUAL_INT(0, load_server_config(config_path));
        TEST_ASSERT_EQUAL_STRING("", g_ca_bundle_path);
        unlink(config_path);
    }
}

void test_download_file_reports_configured_ca_bundle_path(void) {
    char root[] = "/tmp/mcp-file-root-XXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(root));
    snprintf(g_root_dir, sizeof(g_root_dir), "%s", root);
    snprintf(g_ca_bundle_path, sizeof(g_ca_bundle_path), "%s", "/missing-ca.pem");

    json_object *params = json_object_new_object();
    json_object_object_add(params, "url", json_object_new_string("https://example.invalid/file"));
    json_object_object_add(params, "filename", json_object_new_string("/download.bin"));
    json_object *id = json_object_new_int(1);
    json_object *handler_result = NULL;
    char *output = capture_stdout_for_handler(handle_download_file, params, id,
                                               &handler_result);

    TEST_ASSERT_NULL(handler_result);
    TEST_ASSERT_NOT_NULL(strstr(output, "Configured CA bundle"));
    TEST_ASSERT_NOT_NULL(strstr(output, "/missing-ca.pem"));

    free(output);
    json_object_put(id);
    json_object_put(params);
    TEST_ASSERT_EQUAL_INT(0, rmdir(root));
}

void test_resolve_mcp_path_uses_configured_root(void) {
    char root[] = "/tmp/mcp-file-root-XXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(root));
    char metadata[MAX_PATH_LENGTH];
    snprintf(metadata, sizeof(metadata), "%s/metadata", root);
    TEST_ASSERT_EQUAL_INT(0, mkdir(metadata, 0700));
    snprintf(g_root_dir, sizeof(g_root_dir), "%s", root);

    char resolved[MAX_PATH_LENGTH];
    TEST_ASSERT_EQUAL_INT(0, resolve_mcp_path(resolved, sizeof(resolved), "/"));
    TEST_ASSERT_EQUAL_STRING(root, resolved);
    TEST_ASSERT_EQUAL_INT(0, resolve_mcp_path(resolved, sizeof(resolved), "."));
    TEST_ASSERT_EQUAL_STRING(root, resolved);
    TEST_ASSERT_EQUAL_INT(-32602,
        resolve_mcp_path(resolved, sizeof(resolved), "./metadata"));
    TEST_ASSERT_EQUAL_INT(-32602,
        resolve_mcp_path(resolved, sizeof(resolved), ".."));

    TEST_ASSERT_EQUAL_INT(0, resolve_mcp_path(resolved, sizeof(resolved), "/metadata/tags.json"));
    char expected[MAX_PATH_LENGTH];
    snprintf(expected, sizeof(expected), "%s/metadata/tags.json", root);
    TEST_ASSERT_EQUAL_STRING(expected, resolved);

    const char contents[] = "{\"version\":1}\n";
    TEST_ASSERT_EQUAL_INT(0,
        atomic_write_file(resolved, contents, sizeof(contents) - 1));
    FILE *fp = fopen(expected, "r");
    TEST_ASSERT_NOT_NULL(fp);
    char actual[64] = {0};
    TEST_ASSERT_EQUAL_UINT(sizeof(contents) - 1,
        fread(actual, 1, sizeof(contents) - 1, fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
    TEST_ASSERT_EQUAL_STRING(contents, actual);

    TEST_ASSERT_EQUAL_INT(0, unlink(expected));
    TEST_ASSERT_EQUAL_INT(0, rmdir(metadata));
    TEST_ASSERT_EQUAL_INT(0, rmdir(root));
}

void test_resolve_mcp_path_rejects_symlink_escape(void) {
    char root[] = "/tmp/mcp-file-root-XXXXXX";
    char outside[] = "/tmp/mcp-file-outside-XXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(root));
    TEST_ASSERT_NOT_NULL(mkdtemp(outside));
    char link_path[MAX_PATH_LENGTH];
    snprintf(link_path, sizeof(link_path), "%s/metadata", root);
    TEST_ASSERT_EQUAL_INT(0, symlink(outside, link_path));
    snprintf(g_root_dir, sizeof(g_root_dir), "%s", root);

    char resolved[MAX_PATH_LENGTH];
    TEST_ASSERT_EQUAL_INT(-32602,
        resolve_mcp_path(resolved, sizeof(resolved), "/metadata/tags.json"));

    TEST_ASSERT_EQUAL_INT(0, unlink(link_path));
    TEST_ASSERT_EQUAL_INT(0, rmdir(outside));
    TEST_ASSERT_EQUAL_INT(0, rmdir(root));
}

/* ==================== テストスイート実行 ==================== */

int main(void) {
    UNITY_BEGIN();
    
    RUN_TEST(test_base64_encode_simple);
    RUN_TEST(test_base64_encode_empty);
    RUN_TEST(test_base64_decode_simple);
    RUN_TEST(test_base64_roundtrip);
    RUN_TEST(test_utf8_accepts_ascii_and_multibyte);
    RUN_TEST(test_utf8_rejects_legacy_and_malformed_sequences);
    RUN_TEST(test_append_search_result_handles_invalid_near_end_state);
    RUN_TEST(test_append_search_result_writes_marker_at_reserved_boundary);
    RUN_TEST(test_classify_search_entry_resolves_unknown_without_following_symlink);
    RUN_TEST(test_search_string_reports_incremental_line_numbers);
    RUN_TEST(test_create_directory_failure_emits_one_error_response);
    RUN_TEST(test_read_file_schema_declares_utf8_only);
    RUN_TEST(test_read_file_returns_non_utf8_text_as_base64);
    RUN_TEST(test_stat_file_reports_size_time_and_text_line_count);
    RUN_TEST(test_stat_file_reports_missing_and_permission_as_results);
    RUN_TEST(test_run_cmd_reports_output_exit_path_timeout_and_truncation);
    RUN_TEST(test_edit_file_accepts_equivalent_line_endings_and_preserves_crlf);
    RUN_TEST(test_hidden_directories_are_opt_in_for_tree_and_search);
    RUN_TEST(test_sha256_empty);
    RUN_TEST(test_sha256_hello);
    RUN_TEST(test_config_tag_file_path);
    RUN_TEST(test_config_rejects_tag_file_path_outside_root);
    RUN_TEST(test_config_ca_bundle_path);
    RUN_TEST(test_config_rejects_nonvirtual_ca_bundle_paths);
    RUN_TEST(test_download_file_reports_configured_ca_bundle_path);
    RUN_TEST(test_resolve_mcp_path_uses_configured_root);
    RUN_TEST(test_resolve_mcp_path_rejects_symlink_escape);
    
    return UNITY_END();
}
