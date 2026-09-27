#include "mcp_common.h"

typedef struct {
    int entries;
    int truncated;
    size_t estimated_bytes;
} TreeBuildState;

static int compare_tree_items(const void *left, const void *right) {
    const json_object *a = *(const json_object * const *)left;
    const json_object *b = *(const json_object * const *)right;
    const char *a_name = json_object_get_string(json_object_object_get(a, "name"));
    const char *b_name = json_object_get_string(json_object_object_get(b, "name"));
    return strcmp(a_name ? a_name : "", b_name ? b_name : "");
}

/**
 * @brief ディレクトリツリーを構築する再帰ヘルパー関数
 */
static json_object* build_directory_tree(const char *path, int current_depth, int max_depth,
                                         int include_hidden_directories, TreeBuildState *state) {
    DIR *dir = opendir(path);
    if (!dir) {
        return NULL; 
    }

    json_object *root_array = json_object_new_array();
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        // "." や ".." は除外する
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[PATH_MAX];
        int written = !strcmp(path, "/")
            ? snprintf(full_path, sizeof(full_path), "/%s", entry->d_name)
            : snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(full_path)) {
            state->truncated = 1;
            continue;
        }

        if (mcp_path_is_link(full_path) != 0) {
            /* Never recurse through a symlink or Windows reparse point. */
            continue;
        }
        struct stat st;
        if (mcp_file_stat(full_path, &st) != 0) {
            // ステータス取得失敗時はスキップ（権限エラーなど）
            continue; 
        }
        if (S_ISDIR(st.st_mode) && entry->d_name[0] == '.' && !include_hidden_directories) {
            continue;
        }
        size_t entry_cost = strlen(path) + strlen(entry->d_name) + 96;
        if (state->entries >= g_tree_max_entries ||
            state->estimated_bytes >= g_tree_max_output_size ||
            entry_cost > g_tree_max_output_size - state->estimated_bytes) {
            state->truncated = 1;
            break;
        }

        json_object *item = json_object_new_object();
        state->entries++;
        state->estimated_bytes += entry_cost;
        
        // キー "name" に値を設定
        //json_object_object_add(item, "name", json_object_new_string(entry->d_name));
        char visible_path[MAX_PATH_LENGTH];
        const char *item_path = virtualize_mcp_path(visible_path, sizeof(visible_path),
                                                    full_path) == 0
            ? visible_path : full_path;
        json_object_object_add(item, "name", json_object_new_string(item_path));

        if (S_ISDIR(st.st_mode)) {
            // キー "is_dir" に true を設定
            json_object_object_add(item, "is_dir", json_object_new_boolean(true));
            
            // 深さの制限チェック
            int next_depth = current_depth + 1;
            if (next_depth <= max_depth) {
                // 再帰呼び出しして子要素を取得
                json_object *children = build_directory_tree(full_path, next_depth, max_depth,
                                                             include_hidden_directories, state);
                // キー "children" に配列を設定
                json_object_object_add(item, "children", children ? children : json_object_new_array());
            } else {
                // 深さが上限に達した場合は空配列を設定
                json_object_object_add(item, "children", json_object_new_array());
            }
        } else {
            // キー "is_dir" に false を設定
            json_object_object_add(item, "is_dir", json_object_new_boolean(false));
            // ファイルの場合は空配列を設定（または children キー自体を省略しても可）
            json_object_object_add(item, "children", json_object_new_array());
        }

        json_object_array_add(root_array, item);
        
        // デバッグログ: 読み込んだファイル名を出力
#ifdef _DEBUG
        fprintf(stderr, "[DEBUG] Read entry: %s (is_dir=%d)\n", full_path, S_ISDIR(st.st_mode));
#endif
    }

    closedir(dir);
    json_object_array_sort(root_array, compare_tree_items);
    return root_array;
}

/**
 * @brief MCPツール: get_directory_tree の実装
 */
json_object* handle_get_directory_tree(json_object *params, json_object *id) {
    const char *path = "/"; // デフォルトパス
    int max_depth = g_tree_default_depth;
    int include_hidden_directories = 0;
    
    if (params != NULL) {
        json_object *p_obj = json_object_object_get(params, "path");
        if (p_obj && json_object_get_type(p_obj) == json_type_string) {
            path = json_object_get_string(p_obj);
        } else {
            // path がない場合はデフォルト "/" を使う
            path = "/";
        }
        
        json_object *depth_obj = json_object_object_get(params, "depth");
        if (depth_obj) {
            if (!json_object_is_type(depth_obj, json_type_int)) {
                send_json_rpc_error(id, -32602, "depth must be an integer");
                return NULL;
            }
            max_depth = json_object_get_int(depth_obj);
            if (max_depth < 0 || max_depth > g_tree_max_depth) {
                char message[128];
                snprintf(message, sizeof(message), "depth must be between 0 and %d", g_tree_max_depth);
                send_json_rpc_error(id, -32602, message);
                return NULL;
            }
        }
        json_object *hidden_obj = json_object_object_get(params, "include_hidden_directories");
        if (hidden_obj) {
            if (!json_object_is_type(hidden_obj, json_type_boolean)) {
                send_json_rpc_error(id, -32602, "include_hidden_directories must be a boolean");
                return NULL;
            }
            include_hidden_directories = json_object_get_boolean(hidden_obj);
        }
    }

    // パスが空の場合は "/" にする
    if (!path || strlen(path) == 0) {
        path = "/";
    }

    char native_path[MAX_PATH_LENGTH];
    if (normalize_path(native_path, sizeof(native_path), (char *)path) != 0) {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg),
                 "Invalid directory path '%s': %s (errno %d)",
                 path, strerror(errno), errno);
        send_json_rpc_error(id, -32603, err_msg);
        return NULL;
    }

    struct stat st;
    // stat() の失敗時に具体的なエラーを返す
    if (mcp_file_stat(native_path, &st) != 0) {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Failed to access path '%s': %s", path, strerror(errno));
        send_json_rpc_error(id, -32603, err_msg);
        return NULL;
    }

    if (!S_ISDIR(st.st_mode)) {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Path '%s' is not a directory", path);
        send_json_rpc_error(id, -32603, err_msg);
        return NULL;
    }

    // 再帰呼び出し (current_depth=0 で開始)
    TreeBuildState state = {0};
    json_object *tree = build_directory_tree(native_path, 0, max_depth,
                                             include_hidden_directories, &state);
    
    if (!tree) {
        send_json_rpc_error(id, -32603, "Failed to read directory contents");
        return NULL;
    }

    // MCP 仕様に合わせて content キーを持つオブジェクトでラップ
    json_object *content_item = json_object_new_object();
    json_object_object_add(content_item, "type", json_object_new_string("text"));
    
    // ツリー配列を JSON 文字列に変換して text キーに設定
    if (state.truncated) {
        json_object *marker = json_object_new_object();
        json_object_object_add(marker, "truncated", json_object_new_boolean(true));
        json_object_object_add(marker, "entries_returned", json_object_new_int(state.entries));
        json_object_array_add(tree, marker);
    }
    const char *tree_json_str = json_object_to_json_string(tree);
    json_object_object_add(content_item, "text", json_object_new_string(tree_json_str));
    json_object_put(tree);

    json_object *result = json_object_new_object();
    json_object_object_add(result, "content", json_object_new_array());
    json_object_array_add(json_object_object_get(result, "content"), content_item);

    return result;
}


// create_directory: ディレクトリ作成（親ディレクトリ検証版）
json_object* handle_create_directory(json_object *params, json_object *id) {
    const char *path = json_object_get_string(json_object_object_get(params, "path"));
    
    if (!path || strlen(path) == 0) {
        send_json_rpc_error(id, -32602, "Invalid parameters: path is required");
        return NULL;
    }

    char target_path[MAX_PATH_LENGTH];
    if (normalize_new_path(target_path, sizeof(target_path), path) != 0) {
        send_json_rpc_error(id, -32603, "Parent directory does not exist or path is invalid");
        return NULL;
    }

    if (mkdir(target_path, 0755) != 0) {
        int saved_errno = errno;
        char message[256];
        if (saved_errno == EEXIST) {
            snprintf(message, sizeof(message),
                     "Cannot create directory: path already exists");
        } else {
            snprintf(message, sizeof(message),
                     "Cannot create directory: %s", strerror(saved_errno));
        }
        send_json_rpc_error(id, -32603, message);
        return NULL;
    }

    // MCP仕様に準拠した結果生成
    json_object *result = json_object_new_object();
    json_object *content_arr = json_object_new_array();
    json_object *text_obj = json_object_new_object();
    
    json_object_object_add(text_obj, "type", json_object_new_string("text"));
    
    char msg[256];
    snprintf(msg, sizeof(msg), "Directory created successfully: %s", path);
    json_object_object_add(text_obj, "text", json_object_new_string(msg));
    
    json_object_array_add(content_arr, text_obj);
    json_object_object_add(result, "content", content_arr);

    return result;
 
}

// move_file: ファイル/フォルダ移動（MCP仕様準拠版）
json_object* handle_move_file(json_object *params, json_object *id) {
    const char *source = json_object_get_string(json_object_object_get(params, "source"));
    const char *destination = json_object_get_string(json_object_object_get(params, "destination"));
    
    if (!source || !destination) {
        send_json_rpc_error(id, -32602, "Invalid parameters: source and destination are required");
        return NULL;
    }
    
    // パス検証（ソース）
    char resolved_source[MAX_PATH_LENGTH];
    int err = normalize_path(resolved_source,sizeof(resolved_source),(char *)source);
    if( err != 0 ) {
        handle_error(id, err);
        return NULL;
    }

    char resolved_destination[MAX_PATH_LENGTH];
    if (normalize_new_path(resolved_destination, sizeof(resolved_destination), destination) != 0) {
        send_json_rpc_error(id, -32603, "Destination directory does not exist or path is invalid");
        return NULL;
    }
    struct stat destination_stat;
    if (mcp_path_is_link(resolved_destination) == 1 ||
        mcp_file_stat(resolved_destination, &destination_stat) == 0) {
        send_json_rpc_error(id, -32603, "Destination already exists");
        return NULL;
    }
    
    // ファイル/ディレクトリ移動
    if (rename(resolved_source, resolved_destination) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Move failed: %s", strerror(errno));
        send_json_rpc_error(id, -32603, msg);
        return NULL;
    }
    
    // MCP仕様に準拠した結果生成（content配列内にtext型を配置）
    json_object *result = json_object_new_object();
    json_object *content_arr = json_object_new_array();
    json_object *text_obj = json_object_new_object();
    
    json_object_object_add(text_obj, "type", json_object_new_string("text"));
    
    char msg[256];
    snprintf(msg, sizeof(msg), "File/Directory moved successfully: %s -> %s", source, destination);
    json_object_object_add(text_obj, "text", json_object_new_string(msg));
    
    json_object_array_add(content_arr, text_obj);
    json_object_object_add(result, "content", content_arr);

    return result;
}
