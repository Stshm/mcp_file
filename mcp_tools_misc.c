#include "mcp_common.h"

#ifdef _WIN32
#include <windows.h>
#endif

static double search_now(void) {
#ifdef _WIN32
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
#endif
}

static int search_expired(double deadline, int *timed_out) {
    if (search_now() >= deadline) *timed_out = 1;
    return *timed_out;
}

#define SEARCH_TRUNCATION_MARKER "\n[Output truncated by configured limit]\n"

/* Appends only complete records and reserves space for a truncation marker. */
int append_search_result(char *result_text, size_t capacity, size_t *text_len, const char *record) {
    if (!result_text || !text_len || !record || *text_len >= capacity) return 0;

    size_t record_len = strlen(record);
    size_t marker_len = sizeof(SEARCH_TRUNCATION_MARKER) - 1;
    size_t remaining = capacity - *text_len;

    if (marker_len + 1 > remaining) return 0;
    if (record_len > remaining - marker_len - 1) {
        memcpy(result_text + *text_len, SEARCH_TRUNCATION_MARKER, marker_len + 1);
        *text_len += marker_len;
        return 0;
    }

    memcpy(result_text + *text_len, record, record_len + 1);
    *text_len += record_len;
    return 1;
}

McpEntryType classify_search_entry(const char *path) {
    if (mcp_path_is_link(path) != 0) return MCP_ENTRY_UNKNOWN;

    struct stat st;
    if (mcp_file_stat(path, &st) != 0) return MCP_ENTRY_UNKNOWN;
    if (S_ISDIR(st.st_mode)) return MCP_ENTRY_DIRECTORY;
    if (S_ISREG(st.st_mode)) return MCP_ENTRY_REGULAR;
    return MCP_ENTRY_UNKNOWN;
}

// search_files: ファイル名・内容検索（grep相当）の再帰ヘルパー関数
static void search_files_recursive(char *dir_path, char *result_text, size_t *text_len, int *match_count, 
                                   int *truncated, const char *pattern, int case_sensitive, int recursive,
                                   int include_hidden_directories, int max_results, size_t output_capacity,
                                   int depth, double deadline, int *timed_out) {
    if (search_expired(deadline, timed_out)) return;
    DIR *dir = opendir(dir_path);
    if (!dir) return;
    
    struct dirent *entry;
    
    while (!*truncated && *match_count < max_results &&
           !search_expired(deadline, timed_out) && (entry = readdir(dir)) != NULL) {
        // "." と ".." はスキップ
        if (*truncated || strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        char full_path[MAX_PATH_LENGTH];
        int path_len = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (path_len < 0 || (size_t)path_len >= sizeof(full_path)) continue;
        
        McpEntryType entry_type = classify_search_entry(full_path);
        if (entry_type == MCP_ENTRY_DIRECTORY && entry->d_name[0] == '.' && !include_hidden_directories) {
            continue;
        }

        // ディレクトリの場合
        if (entry_type == MCP_ENTRY_DIRECTORY) {
            if (recursive && depth > 0 && *match_count < max_results) {
                search_files_recursive(full_path, result_text, text_len, match_count, truncated,
                                      pattern, case_sensitive, recursive, include_hidden_directories,
                                      max_results, output_capacity, depth - 1, deadline, timed_out);
            }
        } else if (entry_type == MCP_ENTRY_REGULAR) {
            FILE *fp = fopen(full_path, "r");
            if (fp) {
                char line[1024];
                int line_num = 0;
                while (!search_expired(deadline, timed_out) && fgets(line, sizeof(line), fp)) {
                    // max_results に達したらループを抜ける
                    if (*truncated || *match_count >= max_results) {
                        break;
                    }
                    
                    line_num++;
                    int matched = 0;
                    if (case_sensitive) {
                        if (strstr(line, pattern)) {
                            matched = 1;
                        }
                    } else {
                        // 大文字小文字を区別しない検索
                        char *lower_line = strdup(line);
                        char *lower_pattern = strdup(pattern);
                        if (lower_line && lower_pattern) {
                            for (char *p = lower_line; *p; p++) *p = (char)tolower((unsigned char)*p);
                            for (char *p = lower_pattern; *p; p++) *p = (char)tolower((unsigned char)*p);
                            if (strstr(lower_line, lower_pattern)) {
                                matched = 1;
                            }
                        }
                        free(lower_line);
                        free(lower_pattern);
                    }
                    
                    if (matched) {
                        // grep風フォーマット: filename.c:line_num:matched line content
                        char record[MAX_PATH_LENGTH + 1024 + 32];
                        char visible_path[MAX_PATH_LENGTH];
                        const char *result_path =
                            virtualize_mcp_path(visible_path, sizeof(visible_path), full_path) == 0
                                ? visible_path : full_path;
                        int written = snprintf(record, sizeof(record), "%s:%d:%s", result_path, line_num, line);
                        if (written < 0 || (size_t)written >= sizeof(record) ||
                            !append_search_result(result_text, output_capacity, text_len, record)) {
                            *truncated = 1;
                            break;
                        }
                        (*match_count)++;
                    }
                }
                fclose(fp);
            }
        }
    }
    
    closedir(dir);
}

// search_files: ファイル名・内容検索（grep相当）
json_object* handle_search_files(json_object *params, json_object *id) {
    double deadline = search_now() + g_search_timeout_seconds;
    const char *pattern = json_object_get_string(json_object_object_get(params, "pattern"));
    const char *path = json_object_get_string(json_object_object_get(params, "path"));
    
    if (!pattern || !path) {
        send_json_rpc_error(id, -32602, "Invalid parameters: pattern and path are required");
        return NULL;
    }
    char resolved_path[MAX_PATH_LENGTH];
    if (normalize_path(resolved_path, sizeof(resolved_path), (char *)path) != 0) {
        send_json_rpc_error(id, -32603, "Invalid search path");
        return NULL;
    }
    
    // オプションパラメータの取得（デフォルト値設定）
    json_object *recursive_obj = json_object_object_get(params, "recursive");
    int recursive = (!recursive_obj || json_object_get_boolean(recursive_obj)) ? 1 : 0;  // デフォルト: true
    
    json_object *depth_obj = json_object_object_get(params, "depth");
    int depth = DEFAULT_TREE_DEPTH;
    if (depth_obj) {
        if (!json_object_is_type(depth_obj, json_type_int) ||
            json_object_get_int64(depth_obj) < 0 ||
            json_object_get_int64(depth_obj) > g_tree_max_depth) {
            send_json_rpc_error(id, -32602, "depth must be an integer between 0 and the configured tree_max_depth");
            return NULL;
        }
        depth = json_object_get_int(depth_obj);
    }
    if (depth > g_tree_max_depth) depth = g_tree_max_depth;
    if (!recursive) depth = 0;

    json_object *case_sensitive_obj = json_object_object_get(params, "case_sensitive");
    int case_sensitive = (!case_sensitive_obj || json_object_get_boolean(case_sensitive_obj)) ? 1 : 0;  // デフォルト: true

    json_object *hidden_obj = json_object_object_get(params, "include_hidden_directories");
    if (hidden_obj && !json_object_is_type(hidden_obj, json_type_boolean)) {
        send_json_rpc_error(id, -32602, "include_hidden_directories must be a boolean");
        return NULL;
    }
    int include_hidden_directories = hidden_obj ? json_object_get_boolean(hidden_obj) : 0;
    
    json_object *max_results_obj = json_object_object_get(params, "max_results");
    int max_results = g_search_max_results;
    if (max_results_obj && json_object_is_type(max_results_obj, json_type_int)) {
        max_results = json_object_get_int(max_results_obj);
        if (max_results <= 0 || max_results > g_search_max_results_limit) {
            send_json_rpc_error(id, -32602, "max_results is outside the configured range");
            return NULL;
        }
    }
    
    // ディレクトリを開いて再帰検索
    DIR *dir = opendir(resolved_path);
    if (!dir) {
        send_json_rpc_error(id, -32603, "Cannot open directory");
        return NULL;
    }
    closedir(dir);  // 存在確認のみで閉じる
    
    // 結果をテキスト形式で格納するためのバッファ
    char *result_text = malloc(g_search_max_output_size);
    if (!result_text) {
        send_json_rpc_error(id, -32603, "Memory allocation failed");
        return NULL;
    }
    result_text[0] = '\0';
    size_t text_len = 0;
    int match_count = 0;
    int truncated = 0;
    int timed_out = 0;
    
    // 再帰ヘルパー関数を呼び出し
    search_files_recursive(resolved_path, result_text, &text_len, &match_count, &truncated,
                          pattern, case_sensitive, recursive, include_hidden_directories,
                          max_results, g_search_max_output_size, depth, deadline, &timed_out);
    search_expired(deadline, &timed_out);
    
    // 結果をMCP準拠のオブジェクトにラップ
    json_object *result = json_object_new_object();
    json_object *content_array = json_object_new_array();
    
    json_object *text_content = json_object_new_object();
    json_object_object_add(text_content, "type", json_object_new_string("text"));
    json_object_object_add(text_content, "text", json_object_new_string(result_text));
    
    json_object_array_add(content_array, text_content);
    json_object_object_add(result, "content", content_array);
    
    int limited = timed_out || truncated || match_count >= max_results;
    json_object *metadata = json_object_new_object();
    json_object_object_add(metadata, "status", json_object_new_string(limited ? "limit_exceeded" : "complete"));
    json_object_object_add(metadata, "complete", json_object_new_boolean(!limited));
    json_object_object_add(metadata, "depth", json_object_new_int(depth));
    json_object_object_add(metadata, "timeout_seconds", json_object_new_int(g_search_timeout_seconds));
    if (limited) {
        json_object_object_add(metadata, "reason", json_object_new_string(
            timed_out ? "timeout" : truncated ? "output_limit" : "max_results"));
        json_object_object_add(metadata, "message", json_object_new_string(
            "Search stopped; results are incomplete. Narrow the search directory or reduce depth and retry."));
    }
    json_object *summary = json_object_new_object();
    json_object_object_add(summary, "type", json_object_new_string("text"));
    json_object_object_add(summary, "text", json_object_new_string(json_object_to_json_string_ext(metadata, JSON_C_TO_STRING_PLAIN)));
    json_object_array_add(content_array, summary);
    json_object_object_add(result, "structuredContent", metadata);

    free(result_text);
    
    return result;
}

// tag_file: ファイルにタグ付与（仕様書準拠版）

/* タグ名の正規化（小文字変換） */
static void normalize_tag_name(char *tag, size_t len) {
    for (size_t i = 0; i < len && tag[i]; i++) {
        tag[i] = tolower((unsigned char)tag[i]);
    }
}

/* Existing paths use normalize_path(); for a not-yet-created file, resolve its parent. */
static int normalize_tag_path(char *out, size_t out_size, const char *path) {
    char native[MAX_PATH_LENGTH];
    int result = normalize_new_path(native, sizeof(native), path);
    if (result != 0) return result;
    return virtualize_mcp_path(out, (int)out_size, native) == 0 ? 0 : -32602;
}

static int valid_tag_name(const char *tag) {
    if (!tag || !isalnum((unsigned char)tag[0]) || strlen(tag) >= 256) return 0;
    for (const unsigned char *p = (const unsigned char *)tag; *p; p++) {
        if (!isalnum(*p) && *p != '-' && *p != '_' && *p != ':') return 0;
    }
    return 1;
}

static int validate_tags_array(json_object *tags) {
    if (!json_object_is_type(tags, json_type_array)) return 0;
    int count = json_object_array_length(tags);
    for (int i = 0; i < count; i++) {
        json_object *tag = json_object_array_get_idx(tags, i);
        if (!json_object_is_type(tag, json_type_string) ||
            !valid_tag_name(json_object_get_string(tag))) return 0;
    }
    return 1;
}

static int valid_tag_mode(const char *mode) {
    return mode && (!strcmp(mode, "add") || !strcmp(mode, "set") || !strcmp(mode, "replace"));
}

static void update_tag_metadata(json_object *file_obj) {
    json_object *metadata = json_object_object_get(file_obj, "metadata");
    if (!metadata || !json_object_is_type(metadata, json_type_object)) {
        metadata = json_object_new_object();
        json_object_object_add(file_obj, "metadata", metadata);
    }
    time_t now = time(NULL);
    struct tm tm_info;
    char time_str[64];
    if (mcp_time_utc(&now, &tm_info) != 0) memset(&tm_info, 0, sizeof(tm_info));
    strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", &tm_info);
    json_object_object_add(metadata, "last_updated", json_object_new_string(time_str));
    if (!json_object_object_get(metadata, "updated_by")) {
        json_object_object_add(metadata, "updated_by", json_object_new_string("ai"));
    }
}

/* Configured tag database file, relative to the MCP root. */
static json_object* load_tag_file(int *error_code) {
    char tag_file_path[MAX_PATH_LENGTH];
    if (resolve_mcp_path(tag_file_path, sizeof(tag_file_path), g_tag_file_path) != 0) {
        if (error_code) *error_code = -32603;
        return NULL;
    }

    FILE *fp = fopen(tag_file_path, "r");
    if (!fp) {
        // ファイルが存在しない場合は空のオブジェクトを返す（エラーではない）
        if (errno != ENOENT) {
            if (error_code) *error_code = -32603;
            return NULL;
        }
        if (error_code) *error_code = 1; /* Newly created in memory; caller may need to persist it. */
        json_object *obj = json_object_new_object();
        json_object_object_add(obj, "version", json_object_new_int(1));
        json_object_object_add(obj, "files", json_object_new_object());
        json_object_object_add(obj, "_tags_index", json_object_new_object());
        return obj;
    }
    
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        if (error_code) *error_code = -32603;
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        if (error_code) *error_code = -32603;
        return NULL;
    }
    
    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(fp);
        if (error_code) *error_code = -32603;
        return NULL;
    }
    
    size_t bytes_read = fread(buf, 1, (size_t)size, fp);
    if (bytes_read != (size_t)size || ferror(fp)) {
        free(buf);
        fclose(fp);
        if (error_code) *error_code = -32603;
        return NULL;
    }
    buf[bytes_read] = '\0';
    fclose(fp);
    
    json_object *parsed = json_tokener_parse(buf);
    free(buf);
    
    if (!parsed || !json_object_is_type(parsed, json_type_object)) {
        if (parsed) json_object_put(parsed);
        if (error_code) *error_code = -32603;
        return NULL;
    }
    
    // バージョンチェック（必要に応じて）
    json_object *ver_obj = json_object_object_get(parsed, "version");
    if (!ver_obj || !json_object_is_type(ver_obj, json_type_int)) {
        // バージョンがない場合はデフォルト値を設定
        json_object_object_add(parsed, "version", json_object_new_int(1));
    }
    
    // files と _tags_index が存在しない、または型が不正な場合は作成
    json_object *files = json_object_object_get(parsed, "files");
    if (!files || !json_object_is_type(files, json_type_object)) {
        json_object_object_add(parsed, "files", json_object_new_object());
    }
    json_object *index = json_object_object_get(parsed, "_tags_index");
    if (!index || !json_object_is_type(index, json_type_object)) {
        json_object_object_add(parsed, "_tags_index", json_object_new_object());
    }
    
    return parsed;
}

/* Configured tag database file, relative to the MCP root. */
static int save_tag_file(json_object *tag_data) {
    char tag_file_path[MAX_PATH_LENGTH];
    if (resolve_mcp_path(tag_file_path, sizeof(tag_file_path), g_tag_file_path) != 0) {
        return -32603;
    }
    const char *str = json_object_to_json_string_ext(tag_data, JSON_C_TO_STRING_PRETTY);
    return atomic_write_file(tag_file_path, str, strlen(str)) == 0 ? 0 : -32603;
}

/* _tags_index の再構築 */
static void rebuild_tags_index(json_object *tag_data) {
    json_object *files = json_object_object_get(tag_data, "files");
    
    // json_object_object_add() が既存値を解放するため、手動で put しない。
    json_object *index = json_object_new_object();
    json_object_object_add(tag_data, "_tags_index", index);
    
    // files を走査してインデックスを再構築
    struct json_object_iter iter;
    json_object_object_foreachC(files, iter) {
        json_object *file_obj = iter.val;
        json_object *tags_arr = json_object_object_get(file_obj, "tags");
        
        if (json_object_is_type(tags_arr, json_type_array)) {
            int tag_count = json_object_array_length(tags_arr);
            for (int i = 0; i < tag_count; i++) {
                json_object *tag_obj = json_object_array_get_idx(tags_arr, i);
                const char *tag_name = json_object_get_string(tag_obj);
                
                if (!tag_name) continue;
                
                // タグ名がインデックスに存在するか確認
                json_object *existing_files = json_object_object_get(index, tag_name);
                if (!existing_files || !json_object_is_type(existing_files, json_type_array)) {
                    existing_files = json_object_new_array();
                    json_object_object_add(index, tag_name, existing_files);
                }
                
                // ファイルパスが既に存在するか確認（重複防止）
                int found = 0;
                int existing_count = json_object_array_length(existing_files);
                for (int j = 0; j < existing_count; j++) {
                    if (strcmp(json_object_get_string(json_object_array_get_idx(existing_files, j)), iter.key) == 0) {
                        found = 1;
                        break;
                    }
                }
                
                if (!found) {
                    json_object_array_add(existing_files, json_object_new_string(iter.key));
                }
            }
        }
    }
}

/* タグの追加モード処理 */
static void apply_tag_mode(json_object *tags_arr, const char **new_tags, int new_count, const char *mode) {
    if (!strcmp(mode, "set") || !strcmp(mode, "replace")) {
        // del_idx() が削除要素を解放するため、手動で put しない。
        while (json_object_array_length(tags_arr) > 0) {
            json_object_array_del_idx(tags_arr, 0, 1);
        }
    }
    
    // 新しいタグを追加（重複チェック）
    for (int i = 0; i < new_count; i++) {
        char normalized_tag[256];
        strncpy(normalized_tag, new_tags[i], sizeof(normalized_tag) - 1);
        normalized_tag[sizeof(normalized_tag) - 1] = '\0';
        normalize_tag_name(normalized_tag, strlen(normalized_tag));
        
        // 重複チェック
        int is_duplicate = 0;
        int existing_count = json_object_array_length(tags_arr);
        for (int j = 0; j < existing_count; j++) {
            const char *existing = json_object_get_string(json_object_array_get_idx(tags_arr, j));
            if (existing && strcmp(existing, normalized_tag) == 0) {
                is_duplicate = 1;
                break;
            }
        }
        
        if (!is_duplicate) {
            json_object_array_add(tags_arr, json_object_new_string(normalized_tag));
        }
    }
}

static int apply_tags_to_data(json_object *tag_data, const char *path, json_object *tags_obj,
                              const char *mode, char *abs_path, size_t abs_path_size) {
    int err = normalize_tag_path(abs_path, abs_path_size, path);
    if (err != 0) return err;

    int tag_count = json_object_array_length(tags_obj);
    const char **tag_names = tag_count ? malloc(sizeof(*tag_names) * (size_t)tag_count) : NULL;
    if (tag_count && !tag_names) return -32603;
    for (int i = 0; i < tag_count; i++) {
        tag_names[i] = json_object_get_string(json_object_array_get_idx(tags_obj, i));
    }

    json_object *files = json_object_object_get(tag_data, "files");
    json_object *file_obj = json_object_object_get(files, abs_path);
    if (!file_obj || !json_object_is_type(file_obj, json_type_object)) {
        file_obj = json_object_new_object();
        json_object_object_add(files, abs_path, file_obj);
    }
    json_object *tags_arr = json_object_object_get(file_obj, "tags");
    if (!tags_arr || !json_object_is_type(tags_arr, json_type_array)) {
        tags_arr = json_object_new_array();
        json_object_object_add(file_obj, "tags", tags_arr);
    }
    apply_tag_mode(tags_arr, tag_names, tag_count, mode);
    update_tag_metadata(file_obj);
    free(tag_names);
    return 0;
}

static char *join_string_array(json_object *array) {
    size_t len = 1;
    int count = json_object_is_type(array, json_type_array) ? json_object_array_length(array) : 0;
    for (int i = 0; i < count; i++) {
        const char *value = json_object_get_string(json_object_array_get_idx(array, i));
        if (value) len += strlen(value) + (i ? 2 : 0);
    }
    char *joined = malloc(len);
    if (!joined) return NULL;
    joined[0] = '\0';
    for (int i = 0; i < count; i++) {
        const char *value = json_object_get_string(json_object_array_get_idx(array, i));
        if (!value) continue;
        if (i) strcat(joined, ", ");
        strcat(joined, value);
    }
    return joined;
}

static json_object *new_tag_result(const char *text) {
    json_object *result = json_object_new_object();
    json_object *content = json_object_new_array();
    json_object *item = json_object_new_object();
    json_object_object_add(item, "type", json_object_new_string("text"));
    json_object_object_add(item, "text", json_object_new_string(text));
    json_object_array_add(content, item);
    json_object_object_add(result, "content", content);
    json_object_object_add(result, "isError", json_object_new_boolean(false));
    return result;
}

json_object* handle_tag_file(json_object *params, json_object *id) {
    json_object *path_obj = json_object_object_get(params, "path");
    json_object *tags_obj = json_object_object_get(params, "tags");
    json_object *mode_obj = json_object_object_get(params, "mode");
    
    // パラメータ検証
    if (!json_object_is_type(path_obj, json_type_string) || !validate_tags_array(tags_obj)) {
        send_json_rpc_error(id, -32602, "Invalid parameters: path and tags are required");
        return NULL;
    }
    const char *path = json_object_get_string(path_obj);
    if (mode_obj && !json_object_is_type(mode_obj, json_type_string)) {
        send_json_rpc_error(id, -32602, "Invalid parameter: mode must be a string");
        return NULL;
    }
    const char *mode_str = mode_obj ? json_object_get_string(mode_obj) : NULL;
    
    // mode のデフォルト値設定と検証
    const char *mode = mode_str ? mode_str : "add";
    if (!valid_tag_mode(mode)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Invalid mode: %s", mode);
        send_json_rpc_error(id, -32602, msg);
        return NULL;
    }
    
    // パス正規化
    char abs_path[MAX_PATH_LENGTH];
    int err = normalize_tag_path(abs_path, sizeof(abs_path), path);
    if (err != 0) {
        send_json_rpc_error(id, err, "Invalid path");
        return NULL;
    }
    
    // タグ配列からタグ名を取得
    int tag_count = json_object_array_length(tags_obj);
    const char **tag_names = tag_count > 0 ? malloc(sizeof(*tag_names) * (size_t)tag_count) : NULL;
    if (tag_count > 0 && !tag_names) {
        send_json_rpc_error(id, -32603, "Memory allocation failed");
        return NULL;
    }
    
    for (int i = 0; i < tag_count; i++) {
        json_object *tag = json_object_array_get_idx(tags_obj, i);
        if (!json_object_is_type(tag, json_type_string)) {
            free(tag_names);
            send_json_rpc_error(id, -32602, "Invalid parameter: every tag must be a string");
            return NULL;
        }
        tag_names[i] = json_object_get_string(tag);
    }
    
    // タグファイルの読み込み
    int load_err = 0;
    json_object *tag_data = load_tag_file(&load_err);
    if (!tag_data) {
        send_json_rpc_error(id, -32603, "Cannot read tag file");
        free(tag_names);
        return NULL;
    }
    
    // ファイルエントリの取得または作成
    json_object *files = json_object_object_get(tag_data, "files");
    json_object *file_obj = json_object_object_get(files, abs_path);
    
    if (!file_obj || !json_object_is_type(file_obj, json_type_object)) {
        file_obj = json_object_new_object();
        json_object_object_add(files, abs_path, file_obj);
        
        // タグ配列の初期化
        json_object *tags_arr = json_object_new_array();
        json_object_object_add(file_obj, "tags", tags_arr);
        
    }
    update_tag_metadata(file_obj);
    
    // タグ配列の取得
    json_object *tags_arr = json_object_object_get(file_obj, "tags");
    if (!tags_arr || !json_object_is_type(tags_arr, json_type_array)) {
        tags_arr = json_object_new_array();
        json_object_object_add(file_obj, "tags", tags_arr);
    }
    
    // モードに応じたタグ処理
    apply_tag_mode(tags_arr, tag_names, tag_count, mode);
    
    // インデックス再構築
    rebuild_tags_index(tag_data);
    
    // ファイル書き込み
    int save_err = save_tag_file(tag_data);
    if (save_err != 0) {
        json_object_put(tag_data);
        free(tag_names);
        send_json_rpc_error(id, -32603, "Cannot write tag file");
        return NULL;
    }
    
    // タグ名を元のケースで取得（表示用）
    size_t display_len = 1;
    for (int i = 0; i < tag_count; i++) {
        size_t tag_len = strlen(tag_names[i]);
        if (tag_len > SIZE_MAX - display_len - (i > 0 ? 2 : 0)) {
            json_object_put(tag_data);
            free(tag_names);
            send_json_rpc_error(id, -32603, "Tag list is too large");
            return NULL;
        }
        display_len += tag_len + (i > 0 ? 2 : 0);
    }
    char *tags_display = malloc(display_len);
    if (!tags_display) {
        json_object_put(tag_data);
        free(tag_names);
        send_json_rpc_error(id, -32603, "Memory allocation failed");
        return NULL;
    }
    tags_display[0] = '\0';
    for (int i = 0; i < tag_count; i++) {
        if (i > 0) strcat(tags_display, ", ");
        strcat(tags_display, tag_names[i]);
    }
    
    // MCP準拠のレスポンス生成
    json_object *result = json_object_new_object();
    json_object *content_arr = json_object_new_array();
    json_object *text_obj = json_object_new_object();
    
    const char *msg_prefix = "Tags added successfully: ";
    const char *msg_middle = " (tags: ";
    size_t msg_len = strlen(msg_prefix) + strlen(abs_path) + strlen(msg_middle) +
                     strlen(tags_display) + 2;
    char *msg = malloc(msg_len);
    if (!msg) {
        json_object_put(text_obj);
        json_object_put(content_arr);
        json_object_put(result);
        free(tag_names);
        free(tags_display);
        json_object_put(tag_data);
        send_json_rpc_error(id, -32603, "Memory allocation failed");
        return NULL;
    }
    snprintf(msg, msg_len, "%s%s%s%s)", msg_prefix, abs_path, msg_middle, tags_display);
    
    json_object_object_add(text_obj, "type", json_object_new_string("text"));
    json_object_object_add(text_obj, "text", json_object_new_string(msg));
    free(msg);
    json_object_array_add(content_arr, text_obj);
    
    json_object_object_add(result, "content", content_arr);
    json_object_object_add(result, "isError", json_object_new_boolean(false));
    
    // 解放
    free(tag_names);
    free(tags_display);
    json_object_put(tag_data);
    
    return result;
}

json_object* handle_batch_tag_file(json_object *params, json_object *id) {
    json_object *entries = json_object_object_get(params, "files");
    json_object *default_mode_obj = json_object_object_get(params, "default_mode");
    if (!json_object_is_type(entries, json_type_array)) {
        send_json_rpc_error(id, -32602, "Invalid parameters: files array is required");
        return NULL;
    }
    if (default_mode_obj && !json_object_is_type(default_mode_obj, json_type_string)) {
        send_json_rpc_error(id, -32602, "Invalid default_mode");
        return NULL;
    }
    const char *default_mode = default_mode_obj ? json_object_get_string(default_mode_obj) : "add";
    if (!valid_tag_mode(default_mode)) {
        send_json_rpc_error(id, -32602, "Invalid default_mode");
        return NULL;
    }

    int count = json_object_array_length(entries);
    for (int i = 0; i < count; i++) {
        json_object *entry = json_object_array_get_idx(entries, i);
        json_object *path = json_object_is_type(entry, json_type_object)
            ? json_object_object_get(entry, "path") : NULL;
        json_object *tags = json_object_is_type(entry, json_type_object)
            ? json_object_object_get(entry, "tags") : NULL;
        json_object *mode = json_object_is_type(entry, json_type_object)
            ? json_object_object_get(entry, "mode") : NULL;
        if (!json_object_is_type(path, json_type_string) || !validate_tags_array(tags) ||
            (mode && (!json_object_is_type(mode, json_type_string) ||
                      !valid_tag_mode(json_object_get_string(mode))))) {
            send_json_rpc_error(id, -32602, "files[].path and tags are required for each entry");
            return NULL;
        }
    }

    int load_err = 0;
    json_object *tag_data = load_tag_file(&load_err);
    if (!tag_data) {
        send_json_rpc_error(id, -32603, "Cannot read tag file");
        return NULL;
    }
    json_object *results = json_object_new_array();
    int succeeded = 0;
    for (int i = 0; i < count; i++) {
        json_object *entry = json_object_array_get_idx(entries, i);
        const char *path = json_object_get_string(json_object_object_get(entry, "path"));
        json_object *tags = json_object_object_get(entry, "tags");
        json_object *mode_obj = json_object_object_get(entry, "mode");
        const char *mode = mode_obj ? json_object_get_string(mode_obj) : default_mode;
        char abs_path[MAX_PATH_LENGTH];
        int err = apply_tags_to_data(tag_data, path, tags, mode, abs_path, sizeof(abs_path));

        json_object *entry_result = json_object_new_object();
        json_object_object_add(entry_result, "path", json_object_new_string(err == 0 ? abs_path : path));
        json_object_object_add(entry_result, "success", json_object_new_boolean(err == 0));
        if (err == 0) {
            json_object *applied = json_object_new_array();
            int tag_count = json_object_array_length(tags);
            for (int j = 0; j < tag_count; j++) {
                char normalized[256];
                snprintf(normalized, sizeof(normalized), "%s",
                         json_object_get_string(json_object_array_get_idx(tags, j)));
                normalize_tag_name(normalized, strlen(normalized));
                json_object_array_add(applied, json_object_new_string(normalized));
            }
            json_object_object_add(entry_result, "tags_applied", applied);
            json_object_object_add(entry_result, "mode_used", json_object_new_string(mode));
            succeeded++;
        } else {
            json_object_object_add(entry_result, "error", json_object_new_string("Invalid path"));
        }
        json_object_array_add(results, entry_result);
    }

    rebuild_tags_index(tag_data);
    if (save_tag_file(tag_data) != 0) {
        json_object_put(results);
        json_object_put(tag_data);
        send_json_rpc_error(id, -32603, "Cannot write tag file");
        return NULL;
    }
    json_object_put(tag_data);

    char summary[160];
    snprintf(summary, sizeof(summary), "Batch tagging completed: %d/%d files succeeded, %d failed",
             succeeded, count, count - succeeded);
    json_object *result = new_tag_result(summary);
    json_object_object_add(result, "results", results);
    return result;
}

json_object* handle_get_tags(json_object *params, json_object *id) {
    json_object *type_obj = json_object_object_get(params, "type");
    if (!json_object_is_type(type_obj, json_type_string)) {
        send_json_rpc_error(id, -32602, "Invalid parameters: type is required");
        return NULL;
    }
    const char *type = json_object_get_string(type_obj);
    int load_err = 0;
    json_object *tag_data = NULL;
    char abs_path[MAX_PATH_LENGTH];
    char normalized_tag[256];

    if (!strcmp(type, "file")) {
        json_object *path_obj = json_object_object_get(params, "path");
        if (!json_object_is_type(path_obj, json_type_string)) {
            send_json_rpc_error(id, -32602, "path is required when type=file");
            return NULL;
        }
        const char *path = json_object_get_string(path_obj);
        if (normalize_tag_path(abs_path, sizeof(abs_path), path) != 0) {
            send_json_rpc_error(id, -32602, "Invalid path");
            return NULL;
        }
        tag_data = load_tag_file(&load_err);
        if (!tag_data) {
            send_json_rpc_error(id, -32603, "Cannot read tag file");
            return NULL;
        }
        if (load_err == 1 && save_tag_file(tag_data) != 0) {
            json_object_put(tag_data);
            send_json_rpc_error(id, -32603, "Cannot write tag file");
            return NULL;
        }
        json_object *files = json_object_object_get(tag_data, "files");
        json_object *file_obj = json_object_object_get(files, abs_path);
        json_object *tags = file_obj ? json_object_object_get(file_obj, "tags") : NULL;
        if (!json_object_is_type(tags, json_type_array)) tags = NULL;
        json_object *returned_tags = tags ? json_object_get(tags) : json_object_new_array();
        char *joined = join_string_array(returned_tags);
        if (!joined) {
            json_object_put(returned_tags);
            json_object_put(tag_data);
            send_json_rpc_error(id, -32603, "Memory allocation failed");
            return NULL;
        }
        size_t msg_len = strlen("Tags for : ") + strlen(abs_path) + strlen(joined) + 1;
        char *msg = malloc(msg_len);
        if (!msg) {
            free(joined);
            json_object_put(returned_tags);
            json_object_put(tag_data);
            send_json_rpc_error(id, -32603, "Memory allocation failed");
            return NULL;
        }
        snprintf(msg, msg_len, "Tags for %s: %s", abs_path, joined);
        json_object *result = new_tag_result(msg);
        json_object_object_add(result, "tags", returned_tags);
        free(msg);
        free(joined);
        json_object_put(tag_data);
        return result;
    }

    if (!strcmp(type, "tag")) {
        json_object *name_obj = json_object_object_get(params, "tag_name");
        if (!json_object_is_type(name_obj, json_type_string) ||
            !valid_tag_name(json_object_get_string(name_obj))) {
            send_json_rpc_error(id, -32602, "tag_name is required when type=tag");
            return NULL;
        }
        snprintf(normalized_tag, sizeof(normalized_tag), "%s", json_object_get_string(name_obj));
        normalize_tag_name(normalized_tag, strlen(normalized_tag));
        tag_data = load_tag_file(&load_err);
        if (!tag_data) {
            send_json_rpc_error(id, -32603, "Cannot read tag file");
            return NULL;
        }
        if (load_err == 1 && save_tag_file(tag_data) != 0) {
            json_object_put(tag_data);
            send_json_rpc_error(id, -32603, "Cannot write tag file");
            return NULL;
        }
        json_object *index = json_object_object_get(tag_data, "_tags_index");
        json_object *files = json_object_object_get(index, normalized_tag);
        if (!json_object_is_type(files, json_type_array)) files = NULL;
        json_object *returned_files = files ? json_object_get(files) : json_object_new_array();
        char *joined = join_string_array(returned_files);
        if (!joined) {
            json_object_put(returned_files);
            json_object_put(tag_data);
            send_json_rpc_error(id, -32603, "Memory allocation failed");
            return NULL;
        }
        size_t msg_len = strlen("Files with tag '': ") + strlen(normalized_tag) + strlen(joined) + 1;
        char *msg = malloc(msg_len);
        if (!msg) {
            free(joined);
            json_object_put(returned_files);
            json_object_put(tag_data);
            send_json_rpc_error(id, -32603, "Memory allocation failed");
            return NULL;
        }
        snprintf(msg, msg_len, "Files with tag '%s': %s", normalized_tag, joined);
        json_object *result = new_tag_result(msg);
        json_object_object_add(result, "files", returned_files);
        free(msg);
        free(joined);
        json_object_put(tag_data);
        return result;
    }

    send_json_rpc_error(id, -32602, "Invalid type: expected file or tag");
    return NULL;
}

json_object* handle_delete_tag(json_object *params, json_object *id) {
    json_object *type_obj = json_object_object_get(params, "type");
    if (!json_object_is_type(type_obj, json_type_string)) {
        send_json_rpc_error(id, -32602, "Invalid parameters: type is required");
        return NULL;
    }
    const char *type = json_object_get_string(type_obj);
    if (strcmp(type, "file") && strcmp(type, "all_tags")) {
        send_json_rpc_error(id, -32602, "Invalid type: expected file or all_tags");
        return NULL;
    }
    json_object *path_obj = json_object_object_get(params, "path");
    if (!json_object_is_type(path_obj, json_type_string)) {
        send_json_rpc_error(id, -32602, "path is required");
        return NULL;
    }
    json_object *remove = json_object_object_get(params, "tags_to_remove");
    if (!strcmp(type, "file") && !validate_tags_array(remove)) {
        send_json_rpc_error(id, -32602, "tags_to_remove is required when type=file");
        return NULL;
    }
    char abs_path[MAX_PATH_LENGTH];
    if (normalize_tag_path(abs_path, sizeof(abs_path), json_object_get_string(path_obj)) != 0) {
        send_json_rpc_error(id, -32602, "Invalid path");
        return NULL;
    }
    int load_err = 0;
    json_object *tag_data = load_tag_file(&load_err);
    if (!tag_data) {
        send_json_rpc_error(id, -32603, "Cannot read tag file");
        return NULL;
    }
    json_object *files = json_object_object_get(tag_data, "files");
    json_object *file_obj = json_object_object_get(files, abs_path);
    json_object *tags = file_obj ? json_object_object_get(file_obj, "tags") : NULL;

    if (!strcmp(type, "all_tags")) {
        if (file_obj) json_object_object_del(files, abs_path);
    } else if (json_object_is_type(tags, json_type_array)) {
        for (int i = json_object_array_length(tags) - 1; i >= 0; i--) {
            const char *existing = json_object_get_string(json_object_array_get_idx(tags, i));
            int remove_count = json_object_array_length(remove);
            for (int j = 0; j < remove_count; j++) {
                char normalized[256];
                snprintf(normalized, sizeof(normalized), "%s",
                         json_object_get_string(json_object_array_get_idx(remove, j)));
                normalize_tag_name(normalized, strlen(normalized));
                if (existing && !strcmp(existing, normalized)) {
                    json_object_array_del_idx(tags, (size_t)i, 1);
                    break;
                }
            }
        }
        if (json_object_array_length(tags) == 0) json_object_object_del(files, abs_path);
        else update_tag_metadata(file_obj);
    }
    rebuild_tags_index(tag_data);
    if (save_tag_file(tag_data) != 0) {
        json_object_put(tag_data);
        send_json_rpc_error(id, -32603, "Cannot write tag file");
        return NULL;
    }

    char *removed = !strcmp(type, "file") ? join_string_array(remove) : NULL;
    json_object *remaining = json_object_new_array();
    file_obj = json_object_object_get(files, abs_path);
    tags = file_obj ? json_object_object_get(file_obj, "tags") : NULL;
    if (json_object_is_type(tags, json_type_array)) {
        json_object_put(remaining);
        remaining = json_object_get(tags);
    }
    char *remaining_text = join_string_array(remaining);
    size_t msg_len = strlen(abs_path) + (removed ? strlen(removed) : 0) +
                     (remaining_text ? strlen(remaining_text) : 0) + 80;
    char *msg = malloc(msg_len);
    if (!msg || (!strcmp(type, "file") && !removed) || !remaining_text) {
        free(msg); free(removed); free(remaining_text);
        json_object_put(remaining); json_object_put(tag_data);
        send_json_rpc_error(id, -32603, "Memory allocation failed");
        return NULL;
    }
    if (!strcmp(type, "all_tags")) snprintf(msg, msg_len, "All tags removed from %s", abs_path);
    else snprintf(msg, msg_len, "Tags removed from %s: %s (remaining: %s)",
                  abs_path, removed, remaining_text);
    json_object *result = new_tag_result(msg);
    json_object_object_add(result, "remaining_tags", remaining);
    free(msg); free(removed); free(remaining_text);
    json_object_put(tag_data);
    return result;
}

typedef struct {
    FILE *fp;
    uint64_t written;
    uint64_t limit;
    int too_large;
} DownloadWriteState;

static size_t write_download_chunk(void *data, size_t size, size_t count, void *userdata) {
    DownloadWriteState *state = userdata;
    if (size != 0 && count > SIZE_MAX / size) return 0;
    size_t bytes = size * count;
    if ((uint64_t)bytes > state->limit - state->written) {
        state->too_large = 1;
        return 0;
    }
    size_t stored = fwrite(data, 1, bytes, state->fp);
    state->written += stored;
    return stored;
}

// download_file: インターネットからファイル取得（MCP準拠）
json_object* handle_download_file(json_object *params, json_object *id) {
    const char *url = json_object_get_string(json_object_object_get(params, "url"));
    const char *filename = json_object_get_string(json_object_object_get(params, "filename"));
    
    if (!url || !filename) {
        send_json_rpc_error(id, -32602, "Invalid parameters: url and filename are required");
        return NULL;
    }
    
    // timeout_seconds is request-selectable within the configured ceiling.
    json_object *timeout_obj = json_object_object_get(params, "timeout_seconds");
    int timeout_seconds = g_download_timeout_seconds;
    if (timeout_obj && json_object_is_type(timeout_obj, json_type_int)) {
        timeout_seconds = json_object_get_int(timeout_obj);
        if (timeout_seconds <= 0 || timeout_seconds > g_max_download_timeout_seconds) {
            send_json_rpc_error(id, -32602, "timeout_seconds is outside the configured range");
            return NULL;
        }
    }
    
    char abs_path[MAX_PATH_LENGTH];
    if (normalize_new_path(abs_path, sizeof(abs_path), filename) != 0) {
        send_json_rpc_error(id, -32603, "Destination directory does not exist or path is invalid");
        return NULL;
    }

    char ca_bundle_native[MAX_PATH_LENGTH];
    if (g_ca_bundle_path[0]) {
        struct stat ca_stat;
        if (resolve_mcp_path(ca_bundle_native, sizeof(ca_bundle_native),
                             g_ca_bundle_path) != 0 ||
            mcp_file_stat(ca_bundle_native, &ca_stat) != 0 ||
            !S_ISREG(ca_stat.st_mode) || access(ca_bundle_native, R_OK) != 0) {
            char err_msg[MAX_PATH_LENGTH + 96];
            snprintf(err_msg, sizeof(err_msg),
                     "Configured CA bundle is missing, not a regular file, or unreadable: %s",
                     g_ca_bundle_path);
            send_json_rpc_error(id, -32603, err_msg);
            return NULL;
        }
    }
    
    // ファイルサイズ制限チェック（既存ファイルの場合）
    struct stat st;
    if (mcp_file_stat(abs_path, &st) == 0 && S_ISREG(st.st_mode)) {
        if ((uint64_t)st.st_size > g_max_download_file_size) {
            char err_msg[256];
            snprintf(err_msg, sizeof(err_msg), "File size exceeds limit: %ld bytes", st.st_size);
            send_json_rpc_error(id, -32603, err_msg);
            return NULL;
        }
    }
    
    // curlでダウンロード
    CURL *curl = curl_easy_init();
    if (curl) {
        char temp_path[MAX_PATH_LENGTH];
        char *slash = strrchr(abs_path, '/');
        size_t dir_len = slash == abs_path ? 1 : (size_t)(slash - abs_path);
        if (!slash || dir_len + sizeof("/.mcp-download-XXXXXX") > sizeof(temp_path)) {
            curl_easy_cleanup(curl);
            send_json_rpc_error(id, -32603, "Destination path is too long");
            return NULL;
        }
        memcpy(temp_path, abs_path, dir_len);
        temp_path[dir_len] = '\0';
        strcat(temp_path, "/.mcp-download-XXXXXX");
        int temp_fd = mkstemp(temp_path);
        FILE *fp = temp_fd >= 0 ? fdopen(temp_fd, "wb") : NULL;
        if (!fp) {
            if (temp_fd >= 0) close(temp_fd);
            curl_easy_cleanup(curl);
            send_json_rpc_error(id, -32603, "Cannot create destination file");
            return NULL;
        }
        
        DownloadWriteState write_state = {fp, 0, g_max_download_file_size, 0};
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_download_chunk);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_state);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_seconds);
        curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)g_max_download_file_size);
        if (g_ca_bundle_path[0]) {
            CURLcode ca_result = curl_easy_setopt(curl, CURLOPT_CAINFO,
                                                  ca_bundle_native);
            if (ca_result != CURLE_OK) {
                fclose(fp);
                unlink(temp_path);
                curl_easy_cleanup(curl);
                char err_msg[MAX_PATH_LENGTH + 96];
                snprintf(err_msg, sizeof(err_msg),
                         "Cannot configure CA bundle %s: %s",
                         g_ca_bundle_path, curl_easy_strerror(ca_result));
                send_json_rpc_error(id, -32603, err_msg);
                return NULL;
            }
        }
        // User-Agent偽装（一般的なブラウザ風）
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
        
        CURLcode res = curl_easy_perform(curl);
        int io_failed = fflush(fp) != 0 || fsync(temp_fd) != 0;
        if (fclose(fp) != 0) io_failed = 1;
        curl_easy_cleanup(curl);
        
        if (res != CURLE_OK || io_failed) {
            unlink(temp_path);
            char err_msg[256];
            snprintf(err_msg, sizeof(err_msg), "%s%s",
                     write_state.too_large ? "File size exceeds configured download limit" : "Download failed: ",
                     write_state.too_large ? "" : (res != CURLE_OK ? curl_easy_strerror(res) : "file write error"));
            send_json_rpc_error(id, -32603, err_msg);
            return NULL;
        }
        
        // ダウンロード後のファイルサイズチェック
        if (mcp_file_stat(temp_path, &st) == 0 && S_ISREG(st.st_mode)) {
            if ((uint64_t)st.st_size > g_max_download_file_size) {
                unlink(temp_path);
                char err_msg[256];
                snprintf(err_msg, sizeof(err_msg), "File size exceeds limit: %ld bytes", st.st_size);
                send_json_rpc_error(id, -32603, err_msg);
                return NULL;
            }
        }
        struct stat old_stat;
        if (mcp_file_stat(abs_path, &old_stat) == 0) {
            (void)chmod(temp_path, old_stat.st_mode & 07777);
        } else {
            mode_t mask = umask(0);
            umask(mask);
            (void)chmod(temp_path, 0666 & ~mask);
        }
        if (mcp_file_replace(temp_path, abs_path) != 0) {
            unlink(temp_path);
            send_json_rpc_error(id, -32603, "Cannot replace destination file");
            return NULL;
        }
    } else {
        send_json_rpc_error(id, -32603, "Failed to initialize curl");
        return NULL;
    }
    
    // MCP準拠のレスポンス形式（content配列 + isError）
    json_object *result = json_object_new_object();
    json_object *content_array = json_object_new_array();
    
    // ファイルサイズを取得
    long file_size = 0;
    if (mcp_file_stat(abs_path, &st) == 0 && S_ISREG(st.st_mode)) {
        file_size = st.st_size;
    }
    
    char visible_path[MAX_PATH_LENGTH];
    const char *display_path = virtualize_mcp_path(visible_path, sizeof(visible_path),
                                                   abs_path) == 0
        ? visible_path : filename;
    size_t success_len = strlen(display_path) + 96;
    char *success_msg = malloc(success_len);
    if (!success_msg) {
        json_object_put(content_array);
        json_object_put(result);
        send_json_rpc_error(id, -32603, "Memory allocation failed");
        return NULL;
    }
    snprintf(success_msg, success_len, "File downloaded successfully: %s (size: %ld bytes)", display_path, file_size);
    
    json_object *text_content = json_object_new_object();
    json_object_object_add(text_content, "type", json_object_new_string("text"));
    json_object_object_add(text_content, "text", json_object_new_string(success_msg));
    free(success_msg);
    
    json_object_array_add(content_array, text_content);
    json_object_object_add(result, "content", content_array);
    json_object_object_add(result, "isError", json_object_new_boolean(false));
    
    return result;
}

// search_string_in_file: ファイル内の文字列を検索し、開始行番号と終了行番号を返す
json_object* handle_search_string_in_file(json_object *params, json_object *id) {
    const char *path = json_object_get_string(json_object_object_get(params, "path"));
    const char *search_string = json_object_get_string(json_object_object_get(params, "search_string"));
    
    if (!path || !search_string) {
        send_json_rpc_error(id, -32602, "Invalid parameters: path and search_string are required");
        return NULL;
    }
    char resolved_path[MAX_PATH_LENGTH];
    if (normalize_path(resolved_path, sizeof(resolved_path), (char *)path) != 0) {
        send_json_rpc_error(id, -32603, "Invalid file path");
        return NULL;
    }
    
    // ファイルを開く
    FILE *fp = fopen(resolved_path, "r");
    if (!fp) {
        send_json_rpc_error(id, -32603, "Cannot open file");
        return NULL;
    }
    
    // ファイルサイズを取得
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (file_size <= 0 || (uint64_t)file_size > g_max_read_file_size) {
        fclose(fp);
        send_json_rpc_error(id, -32603, "File size is invalid or too large. Otherwise, if you want to search in directories, use search_files");
        return NULL;
    }
    
    // ファイル全体をメモリに読み込む
    char *file_content = malloc(file_size + 1);
    if (!file_content) {
        fclose(fp);
        send_json_rpc_error(id, -32603, "Memory allocation failed");
        return NULL;
    }
    
    size_t bytes_read = fread(file_content, 1, file_size, fp);
    file_content[bytes_read] = '\0';
    fclose(fp);
    
    // 検索文字列の長さを取得
    size_t search_len = strlen(search_string);
    
    // 空文字列の場合はエラーを返す（無限ループ防止）
    if (search_len == 0) {
        free(file_content);
        send_json_rpc_error(id, -32602, "search_string cannot be empty");
        return NULL;
    }
    
    // 結果を格納する配列
    json_object *results = json_object_new_array();
    int results_truncated = 0;
    
    // ファイル内容内で検索文字列を検索（複数ヒット対応）
    char *search_pos = file_content;
    size_t line_scan_offset = 0;
    int current_line = 1;
    while ((search_pos = strstr(search_pos, search_string)) != NULL) {
        if ((int)json_object_array_length(results) >= g_search_max_results) {
            results_truncated = 1;
            break;
        }
        // ヒット位置までのバイト数を計算
        size_t hit_offset = (size_t)(search_pos - file_content);
        
        // 前回のヒット位置から増分で行番号を計算する
        while (line_scan_offset < hit_offset) {
            if (file_content[line_scan_offset] == '\n') {
                current_line++;
            }
            line_scan_offset++;
        }
        int start_line = current_line;
        
        // 終了行番号を計算（検索文字列の終端位置）
        int end_line = start_line;
        size_t match_length = search_len <= bytes_read - hit_offset
                            ? search_len : bytes_read - hit_offset;
        for (size_t i = hit_offset; i < hit_offset + match_length; i++) {
            if (file_content[i] == '\n') {
                end_line++;
            }
        }
        
        // 結果をJSONオブジェクトとして追加
        json_object *match = json_object_new_object();
        json_object_object_add(match, "start_line", json_object_new_int(start_line));
        json_object_object_add(match, "end_line", json_object_new_int(end_line));
        json_object_array_add(results, match);
        
        // 次の検索位置に移動（重複ヒット防止のため1バイト進める）
        search_pos++;
    }
    
    free(file_content);
    
    // 検索結果をテキスト形式に変換
    char *result_text = malloc(g_search_max_output_size);
    if (!result_text) {
        json_object_put(results);
        send_json_rpc_error(id, -32603, "Memory allocation failed");
        return NULL;
    }
    result_text[0] = '\0';
    
    size_t text_len = 0;
    int match_count = json_object_array_length(results);
    
    if (match_count == 0) {
        snprintf(result_text, g_search_max_output_size, "No matches found.");
    } else {
        for (int i = 0; i < match_count; i++) {
            json_object *match = json_object_array_get_idx(results, i);
            int start = json_object_get_int(json_object_object_get(match, "start_line"));
            int end = json_object_get_int(json_object_object_get(match, "end_line"));
            char record[128];
            int written = snprintf(record, sizeof(record),
                                   "Match %d: start_line=%d, end_line=%d\n", i + 1, start, end);
            if (written < 0 || (size_t)written >= sizeof(record) ||
                !append_search_result(result_text, g_search_max_output_size, &text_len, record)) {
                results_truncated = 1;
                break;
            }
        }
        if (results_truncated && !strstr(result_text, SEARCH_TRUNCATION_MARKER)) {
            (void)append_search_result(result_text, g_search_max_output_size, &text_len,
                                       SEARCH_TRUNCATION_MARKER);
        }
    }
    
    // MCP仕様に従ってcontentフィールドを含む結果を返す
    json_object *result = json_object_new_object();
    json_object *content_array = json_object_new_array();
    
    json_object *text_content = json_object_new_object();
    json_object_object_add(text_content, "type", json_object_new_string("text"));
    json_object_object_add(text_content, "text", json_object_new_string(result_text));
    
    json_object_array_add(content_array, text_content);
    json_object_object_add(result, "content", content_array);
    
    free(result_text);
    json_object_put(results);  // resultsは不要なので解放
    
    return result;
}
