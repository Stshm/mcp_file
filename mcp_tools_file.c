#include "mcp_common.h"
#include <stdarg.h>


// read_file: ファイル内容取得（行数指定による部分的読み出し対応）
json_object* handle_read_file(json_object *params, json_object *id) {
    const char *path = json_object_get_string(json_object_object_get(params, "path"));
    json_object *j_start_line = json_object_object_get(params, "start_line");
    json_object *j_end_line = json_object_object_get(params, "end_line");
    json_object *j_include_line_numbers = json_object_object_get(params, "include_line_numbers");
    json_object *j_include_trailing_newline = json_object_object_get(params, "include_trailing_newline");

    int has_line_range = 0;
    int start_line = 1;
    int end_line = INT_MAX;

    if (json_object_is_type(j_start_line, json_type_int) && 
        json_object_is_type(j_end_line, json_type_int)) {
        has_line_range = 1;
        start_line = json_object_get_int(j_start_line);
        end_line = json_object_get_int(j_end_line);

        if (start_line < 1 || end_line < start_line) {
            send_json_rpc_error(id, -32602, "Invalid parameters: start_line must be >= 1 and end_line >= start_line");
            return NULL;
        }
    } else if ((json_object_is_type(j_start_line, json_type_int)) ^ 
               (json_object_is_type(j_end_line, json_type_int))) {
        // 片方のみ指定された場合はエラー
        send_json_rpc_error(id, -32602, "Invalid parameters: both start_line and end_line must be specified together");
        return NULL;
    }

    int include_line_numbers = 0;
    if (json_object_is_type(j_include_line_numbers, json_type_boolean)) {
        include_line_numbers = json_object_get_boolean(j_include_line_numbers);
    }

    // 末尾改行の含有不含有フラグ（デフォルト=1:含む）
    int flag_last_lf = EOB_LINE_BREAK_INCLUDE;
    if (json_object_is_type(j_include_trailing_newline, json_type_boolean)) {
        flag_last_lf = json_object_get_boolean(j_include_trailing_newline) ? EOB_LINE_BREAK_INCLUDE : EOB_LINE_BREAK_EXCLUDE;
    }

    char abs_path[MAX_PATH_LENGTH];

    int err = normalize_path(abs_path,sizeof(abs_path),(char *)path);
    if( err != 0 )  {
         handle_error(id, err);
         return NULL;
    }
   
    FILE *fp = fopen(abs_path, "rb"); // バイナリモードで開く
    if (!fp) {
        send_json_rpc_error(id, -32603, "Cannot read file: permission denied or file not found");
        return NULL;
    }
    
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        send_json_rpc_error(id, -32603, "Cannot determine file size");
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        send_json_rpc_error(id, -32603, "Cannot determine file size");
        return NULL;
    }
    if ((uint64_t)size > g_max_read_file_size) {
        char msg[192];
        snprintf(msg, sizeof(msg), "File size exceeds configured read limit: %ld > %llu bytes",
                 size, (unsigned long long)g_max_read_file_size);
        fclose(fp);
        send_json_rpc_error(id, -32603, msg);
        return NULL;
    }
    if ((uint64_t)size > SIZE_MAX - 1) {
        fclose(fp);
        send_json_rpc_error(id, -32603, "File is too large to read");
        return NULL;
    }
    
    char *content = malloc(size + 1);
    if (!content) {
        fclose(fp);
        send_json_rpc_error(id, -32603, "Memory allocation failed");
        return NULL;
    }
    
    size_t bytes_read = fread(content, 1, (size_t)size, fp);
    if (bytes_read != (size_t)size || ferror(fp)) {
        free(content);
        fclose(fp);
        send_json_rpc_error(id, -32603, "Failed while reading file");
        return NULL;
    }
    content[size] = '\0';
    fclose(fp);

    // ファイルタイプ判定（determine_file_type関数を使用）
    const char *ext = strrchr(path, '.');
    
    // filenameを取得（パスの最後の/以降）
    const char *filename = strrchr(abs_path, '/');
    if (filename) {
        filename++; // skip '/'
    } else {
        filename = abs_path;
    }
    
    FileType file_type = determine_file_type(filename, abs_path);
    /* JSON text content is UTF-8. Other encodings are returned as Base64. */
    int is_text = file_type == FILE_TYPE_TEXT &&
                  is_valid_utf8((const unsigned char *)content, bytes_read);

    json_object *result = json_object_new_object();
    json_object *content_arr = json_object_new_array();

    // テキストファイルで行数指定がある場合、部分的に読み出す
    char *read_content = content;
    int partial_read = 0;
    
    if (is_text && has_line_range) {
        //read_content = extract_lines(content, start_line, end_line);
        size_t len = 0;
        char *p = extract_text_by_line_range(content, &len, start_line, end_line, flag_last_lf);  //改行含む = 1, 改行含めない = 0
        if (!p) {
            free(content);
            send_json_rpc_error(id, -32602, "Specified line range is out of file bounds");
            return NULL;
        }
        read_content = malloc(len + 1);
        if (!read_content) {
            free(content);
            send_json_rpc_error(id, -32603, "Memory allocation failed");
            return NULL;
        }
        memcpy(read_content, p, len);
        read_content[len] = '\0';
#ifdef _DEBUG
        fprintf(stderr,"[DEBUG] read_content ------------- \n%s\n------- \n",read_content);
#endif
        partial_read = 1;
    }

    
    if (is_text) {
        // テキストファイルの場合（行数指定あり/なしに関わらず）
        json_object *text_obj = json_object_new_object();
        json_object_object_add(text_obj, "type", json_object_new_string("text"));
        
        // 行番号付加が必要な場合
        char *output_content = read_content;
        if (include_line_numbers) {
            size_t content_len = strlen(read_content);
            
            // 改行数をカウントして必要なメモリ量を計算
            int line_count = 1;
            for (size_t i = 0; i < content_len; i++) {
                if (read_content[i] == '\n') {
                    line_count++;
                }
            }
            
            // 行番号を付加した文字列を作成
            size_t output_size = content_len + line_count * 16; // 最大で"99999999: " = 10文字
            char *line_numbered_content = malloc(output_size);
            if (!line_numbered_content) {
                free(content);
                if (partial_read) free(read_content);
                send_json_rpc_error(id, -32603, "Memory allocation failed for line numbers");
                return NULL;
            }
            
            int current_line = has_line_range ? start_line : 1;
            size_t out_idx = 0;
            const char *ptr = read_content;
            
            while (*ptr) {
                // 行番号を付加
                int num_len = snprintf(line_numbered_content + out_idx, output_size - out_idx, "%d:", current_line);
                out_idx += num_len;
                
                // 行の内容をコピー（改行まで）
                const char *newline_pos = strchr(ptr, '\n');
                if (newline_pos) {
                    size_t line_len = newline_pos - ptr + 1;
                    memcpy(line_numbered_content + out_idx, ptr, line_len);
                    out_idx += line_len;
                    ptr = newline_pos + 1;
                    current_line++;
                } else {
                    // 最後の行（改行なし）
                    size_t remaining = strlen(ptr);
                    if (remaining > 0) {
                        memcpy(line_numbered_content + out_idx, ptr, remaining);
                        out_idx += remaining;
                    }
                    break;
                }
            }
            line_numbered_content[out_idx] = '\0';
            
            output_content = line_numbered_content;
        }
        
        json_object_object_add(text_obj, "text", json_object_new_string(output_content));
        
        // 行数指定があった場合、メタ情報を追加（別テキストとして）
        if (has_line_range) {
            json_object *meta = json_object_new_object();
            json_object_object_add(meta, "start_line", json_object_new_int(start_line));
            json_object_object_add(meta, "end_line", json_object_new_int(end_line));
            
            // ハッシュ値の計算（部分読み出しの場合）
            // edit_file関数と同じ方式で\r\n -> \n変換後のコンテンツに対してハッシュを計算
            if (partial_read) {
                // read_contentから\r\n -> \n変換后的コンテンツを作成
                size_t content_len = strlen(read_content);
                char *normalized_for_hash = malloc(content_len + 2);
                uint8_t hash[32] = {0};
                char hash_b64[48] = {0};
                char hash_str[64] = {0};
                
                if (normalized_for_hash) {
                    size_t n_idx = 0;
                    for (size_t i = 0; i < content_len; i++) {
                        if (read_content[i] == '\r' && i + 1 < content_len && read_content[i+1] == '\n') {
                            normalized_for_hash[n_idx++] = '\n';
                            i++; // Skip \n
                        } else {
                            normalized_for_hash[n_idx++] = read_content[i];
                        }
                    }
                    normalized_for_hash[n_idx] = '\0';
                    
                    _sha256_fips((const uint8_t*)normalized_for_hash, strlen(normalized_for_hash), hash);
                    base64_encode((const char*)hash, sizeof(hash), hash_b64);
                    
                    free(normalized_for_hash);
                } else {
                    // 元のread_contentに対してハッシュ計算（フォールバック）
                    _sha256_fips((const uint8_t*)read_content, strlen(read_content), hash);
                    base64_encode((const char*)hash, sizeof(hash), hash_b64);
                }
                
                snprintf(hash_str, sizeof(hash_str), "sha256:%s", hash_b64);
                json_object_object_add(meta, "content_hash", json_object_new_string(hash_str));
            }
            
            // メタ情報をJSON文字列に変換して別オブジェクトとして追加
            const char *meta_json = json_object_to_json_string_ext(meta, JSON_C_TO_STRING_PRETTY);
            json_object *meta_obj = json_object_new_object();
            json_object_object_add(meta_obj, "type", json_object_new_string("text"));
            json_object_object_add(meta_obj, "text", json_object_new_string(meta_json));
            json_object_array_add(content_arr, meta_obj);
            
            // 解放（json_object_putでも可）
            //free((void*)meta_json);
            json_object_put(meta);
        }
        
        // 行番号付加で割り当てたメモリを解放
        if (include_line_numbers && output_content != read_content) {
            free(output_content);
        }
        
        json_object_array_add(content_arr, text_obj);
    } else {
        // イメージやオーディオの場合（Base64エンコード）
        // 行数指定はテキストファイルのみ有効なので、バイナリファイルでは無視
        char *base64_data = malloc(size * 4 / 3 + 4); // Base64 size estimation
        if (!base64_data) {
            free(content);
            send_json_rpc_error(id, -32603, "Memory allocation failed for base64");
            return NULL;
        }

        const char *mime_type = "application/octet-stream";
        json_object *file_obj = json_object_new_object();
        int is_media = 0;

        if (ext && strcasecmp(ext, ".png") == 0) {
            mime_type = "image/png";
            json_object_object_add(file_obj, "type", json_object_new_string("image"));
            is_media = 1;
        } else if (ext && (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0)) {
            mime_type = "image/jpeg";
            json_object_object_add(file_obj, "type", json_object_new_string("image"));
            is_media = 1;
        } else if (ext && strcasecmp(ext, ".wav") == 0) {
            mime_type = "audio/wav";
            json_object_object_add(file_obj, "type", json_object_new_string("audio"));
            is_media = 1;
        } else {
            json_object_object_add(file_obj, "type", json_object_new_string("text"));
        }

        base64_encode((const char*)content, size, base64_data);
        
        if (is_media) {
            json_object_object_add(file_obj, "data", json_object_new_string(base64_data));
            json_object_object_add(file_obj, "mimeType", json_object_new_string(mime_type));
        } else {
            json_object_object_add(file_obj, "text", json_object_new_string(base64_data));
        }
        
        // annotations (optional)
        json_object *annotations = json_object_new_object();
        json_object *audience_arr = json_object_new_array(); 
        json_object_array_add(audience_arr, json_object_new_string("user"));
        json_object_object_add(annotations, "audience", audience_arr);
        json_object_object_add(file_obj, "annotations", annotations);

        json_object_array_add(content_arr, file_obj);
        free(base64_data);
    }

    if (partial_read) {
        free(read_content);
    }

    json_object_object_add(result, "content", content_arr);
    json_object_object_add(result, "isError", json_object_new_boolean(false));
    
    free(content);
    
    return result;
}



/* Validate UTF-8 across read-buffer boundaries while counting LF-terminated
 * lines. Keep the same valid-Unicode rules as is_valid_utf8(). */
static int count_utf8_text_lines(FILE *fp, int64_t *line_count) {
    unsigned char buffer[65536];
    int remaining = 0;
    uint32_t codepoint = 0;
    uint32_t minimum = 0;
    int64_t newlines = 0;
    int saw_byte = 0;
    int final_lf = 0;
    size_t count;

    while ((count = fread(buffer, 1, sizeof(buffer), fp)) != 0) {
        for (size_t i = 0; i < count; i++) {
            unsigned char c = buffer[i];
            saw_byte = 1;
            final_lf = c == '\n';
            if (c == '\n') newlines++;
            if (remaining) {
                if ((c & 0xc0) != 0x80) return 0;
                codepoint = (codepoint << 6) | (c & 0x3f);
                if (--remaining == 0 &&
                    (codepoint < minimum ||
                     (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
                     codepoint > 0x10ffff)) return 0;
            } else if (c == 0) {
                return 0;
            } else if (c <= 0x7f) {
                continue;
            } else if (c >= 0xc2 && c <= 0xdf) {
                remaining = 1;
                codepoint = c & 0x1f;
                minimum = 0x80;
            } else if (c >= 0xe0 && c <= 0xef) {
                remaining = 2;
                codepoint = c & 0x0f;
                minimum = 0x800;
            } else if (c >= 0xf0 && c <= 0xf4) {
                remaining = 3;
                codepoint = c & 0x07;
                minimum = 0x10000;
            } else {
                return 0;
            }
        }
    }
    if (ferror(fp)) return -1;
    if (remaining) return 0;
    *line_count = newlines + (saw_byte && !final_lf);
    return 1;
}

static json_object *stat_file_content(json_object *metadata) {
    json_object *result = json_object_new_object();
    json_object *content = json_object_new_array();
    json_object *item = json_object_new_object();
    json_object_object_add(item, "type", json_object_new_string("text"));
    json_object_object_add(item, "text", json_object_new_string(
        json_object_to_json_string_ext(metadata, JSON_C_TO_STRING_PLAIN)));
    json_object_array_add(content, item);
    json_object_object_add(result, "content", content);
    json_object_put(metadata);
    return result;
}

static json_object *stat_file_unavailable(const char *path, int error_number) {
    json_object *metadata = json_object_new_object();
    json_object_object_add(metadata, "path", json_object_new_string(path));
    if (error_number == ENOENT) {
        json_object_object_add(metadata, "status", json_object_new_string("not_found"));
        json_object_object_add(metadata, "exists", json_object_new_boolean(0));
        json_object_object_add(metadata, "message", json_object_new_string("No such file"));
    } else {
        json_object_object_add(metadata, "status", json_object_new_string("permission_denied"));
        json_object_object_add(metadata, "message", json_object_new_string("Permission denied"));
    }
    return stat_file_content(metadata);
}

static int stat_file_is_unavailable(int error_number) {
    return error_number == ENOENT || error_number == EACCES || error_number == EPERM;
}

json_object* handle_stat_file(json_object *params, json_object *id) {
    const char *path = json_object_get_string(json_object_object_get(params, "path"));
    if (!path || !*path) {
        send_json_rpc_error(id, -32602, "Invalid parameters: path is required");
        return NULL;
    }

    char abs_path[MAX_PATH_LENGTH];
    int path_error = normalize_path(abs_path, sizeof(abs_path), (char *)path);
    if (path_error != 0) {
        int path_errno = errno;
        if (stat_file_is_unavailable(path_errno))
            return stat_file_unavailable(path, path_errno);
        send_json_rpc_error(id, path_error, "Invalid file path");
        return NULL;
    }
    struct stat st;
    if (mcp_file_stat(abs_path, &st) != 0) {
        int stat_errno = errno;
        if (stat_file_is_unavailable(stat_errno))
            return stat_file_unavailable(path, stat_errno);
        send_json_rpc_error(id, -32603, "Cannot stat file");
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        send_json_rpc_error(id, -32603, "Path is not a regular file");
        return NULL;
    }
    int64_t size_bytes = 0;
    if (mcp_file_size_bytes(abs_path, &size_bytes) != 0) {
        int size_errno = errno;
        if (stat_file_is_unavailable(size_errno))
            return stat_file_unavailable(path, size_errno);
        send_json_rpc_error(id, -32603, "Cannot determine file size");
        return NULL;
    }

    struct tm utc;
    char modified_at[32];
    if (mcp_time_utc(&st.st_mtime, &utc) != 0 ||
        strftime(modified_at, sizeof(modified_at), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        send_json_rpc_error(id, -32603, "Cannot format modification time");
        return NULL;
    }

    const char *filename = strrchr(abs_path, '/');
    filename = filename ? filename + 1 : abs_path;
    FILE *fp = fopen(abs_path, "rb");
    if (!fp) {
        int open_errno = errno;
        if (stat_file_is_unavailable(open_errno))
            return stat_file_unavailable(path, open_errno);
        send_json_rpc_error(id, -32603, "Cannot read file to count lines");
        return NULL;
    }
    int is_text = determine_file_type(filename, abs_path) == FILE_TYPE_TEXT;
    int64_t line_count = 0;
    if (is_text) {
        int counted = count_utf8_text_lines(fp, &line_count);
        if (counted < 0) {
            fclose(fp);
            send_json_rpc_error(id, -32603, "Cannot read file to count lines");
            return NULL;
        }
        is_text = counted > 0;
    }
    fclose(fp);

    json_object *metadata = json_object_new_object();
    json_object_object_add(metadata, "path", json_object_new_string(path));
    json_object_object_add(metadata, "status", json_object_new_string("ok"));
    json_object_object_add(metadata, "exists", json_object_new_boolean(1));
    json_object_object_add(metadata, "size_bytes", json_object_new_int64(size_bytes));
    json_object_object_add(metadata, "modified_at", json_object_new_string(modified_at));
    json_object_object_add(metadata, "is_text", json_object_new_boolean(is_text));
    if (is_text)
        json_object_object_add(metadata, "line_count", json_object_new_int64(line_count));

    return stat_file_content(metadata);
}

// handle_create_file: ファイル新規作成（既存ファイルがない場合のみ）
json_object* handle_create_file(json_object *params, json_object *id) {
    const char *path = json_object_get_string(json_object_object_get(params, "path"));
    const char *content = json_object_get_string(json_object_object_get(params, "content"));
    
    if (!path || !content) {
        send_json_rpc_error(id, -32602, "Invalid parameters: path and content are required");
        return NULL;
    }
    
    char abs_path[MAX_PATH_LENGTH];
    if (normalize_new_path(abs_path, sizeof(abs_path), path) != 0) {
        send_json_rpc_error(id, -32603, "Parent directory does not exist or path is invalid");
        return NULL;
    }
    
    // ファイル書き込み（新規作成）
    int fd = open(abs_path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        char msg[MAX_PATH_LENGTH + 64];
        snprintf(msg, sizeof(msg), "Cannot create file: %s", strerror(errno));
        send_json_rpc_error(id, errno == EEXIST ? -32001 : -32603, msg);
        return NULL;
    }
    size_t remaining = strlen(content);
    const char *cursor = content;
    int failed = 0;
    while (remaining > 0) {
        ssize_t n = write(fd, cursor, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            failed = 1;
            break;
        }
        cursor += (size_t)n;
        remaining -= (size_t)n;
    }
    if (!failed && fsync(fd) != 0) failed = 1;
    if (close(fd) != 0) failed = 1;
    if (failed) {
        unlink(abs_path);
        send_json_rpc_error(id, -32603, "Failed while writing new file");
        return NULL;
    }
    
    // MCP 仕様に合わせて content キーでラップ
    json_object *result = json_object_new_object();
    json_object *content_arr = json_object_new_array();
    json_object *text_obj = json_object_new_object();
    json_object_object_add(text_obj, "type", json_object_new_string("text"));
    
    char msg[MAX_PATH_LENGTH + 64];
    snprintf(msg, sizeof(msg), "File created successfully: %s", path);
    json_object_object_add(text_obj, "text", json_object_new_string(msg));
    
    json_object_array_add(content_arr, text_obj);
    json_object_object_add(result, "content", content_arr);

    return result;
}
// write_file: ファイル作成・上書き
json_object* handle_write_file(json_object *params, json_object *id) {
    const char *path = json_object_get_string(json_object_object_get(params, "path"));
    const char *content = json_object_get_string(json_object_object_get(params, "content"));
    
    if (!path || !content) {
        send_json_rpc_error(id, -32602, "Invalid parameters: path and content are required");
        return NULL;
    }
    
    char abs_path[MAX_PATH_LENGTH];
    if (normalize_new_path(abs_path, sizeof(abs_path), path) != 0) {
        send_json_rpc_error(id, -32603, "Directory does not exist or path is invalid");
        return NULL;
    }
    if (atomic_write_file(abs_path, content, strlen(content)) != 0) {
        send_json_rpc_error(id, -32603, "Cannot write file atomically");
        return NULL;
    }
    
   // MCP 仕様に合わせて content キーでラップ
    json_object *result = json_object_new_object();
    json_object *content_arr = json_object_new_array();
    json_object *text_obj = json_object_new_object();
    json_object_object_add(text_obj, "type", json_object_new_string("text"));
    
    // 成功メッセージを生成
    char msg[MAX_PATH_LENGTH + 64];
    snprintf(msg, sizeof(msg), "File written successfully: %s", path);
    json_object_object_add(text_obj, "text", json_object_new_string(msg));
    
    json_object_array_add(content_arr, text_obj);
    json_object_object_add(result, "content", content_arr);

    return result;

}



typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} HistoryBuffer;

static int history_buffer_reserve(HistoryBuffer *buffer, size_t extra) {
    if (extra > SIZE_MAX - buffer->length - 1) return -1;
    size_t needed = buffer->length + extra + 1;
    if (needed <= buffer->capacity) return 0;
    size_t capacity = buffer->capacity ? buffer->capacity : 1024;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
        capacity *= 2;
    }
    char *grown = realloc(buffer->data, capacity);
    if (!grown) return -1;
    buffer->data = grown;
    buffer->capacity = capacity;
    return 0;
}

static int history_buffer_append(HistoryBuffer *buffer, const char *text, size_t length) {
    if (history_buffer_reserve(buffer, length) != 0) return -1;
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 0;
}

static int history_buffer_format(HistoryBuffer *buffer, const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0 || history_buffer_reserve(buffer, (size_t)length) != 0) {
        va_end(args);
        return -1;
    }
    vsnprintf(buffer->data + buffer->length,
              buffer->capacity - buffer->length, format, args);
    va_end(args);
    buffer->length += (size_t)length;
    return 0;
}

static int history_buffer_prefixed_lines(HistoryBuffer *buffer, const char *text,
                                         const char *prefix) {
    const char *cursor = text;
    size_t prefix_length = strlen(prefix);
    if (history_buffer_append(buffer, prefix, prefix_length) != 0) return -1;
    for (;;) {
        const char *newline = strchr(cursor, '\n');
        if (!newline) {
            return history_buffer_append(buffer, cursor, strlen(cursor)) == 0 &&
                   history_buffer_append(buffer, "\n", 1) == 0 ? 0 : -1;
        }
        size_t line_length = (size_t)(newline - cursor) + 1;
        if (history_buffer_append(buffer, cursor, line_length) != 0 ||
            history_buffer_append(buffer, prefix, prefix_length) != 0) return -1;
        cursor = newline + 1;
    }
}

static int save_edit_history(const char *abs_path, const char *display_path, int start_line,
                             int end_line, const char *old_content, const char *new_content) {
    char dir[MAX_PATH_LENGTH];
    int written = snprintf(dir, sizeof(dir), "%s", abs_path);
    if (written < 0 || (size_t)written >= sizeof(dir)) return -1;
    char *slash = strrchr(dir, '/');
    const char *basename = slash ? slash + 1 : abs_path;
    if (!slash) snprintf(dir, sizeof(dir), ".");
    else if (slash == dir) slash[1] = '\0';
    else *slash = '\0';

    char history_dir[MAX_PATH_LENGTH];
    written = !strcmp(dir, "/")
        ? snprintf(history_dir, sizeof(history_dir), "/.history")
        : snprintf(history_dir, sizeof(history_dir), "%s/.history", dir);
    if (written < 0 || (size_t)written >= sizeof(history_dir)) return -1;
    if (mkdir(history_dir, 0755) != 0 && errno != EEXIST) return -1;

    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return -1;
    struct tm tm_info;
    if (mcp_time_utc(&now.tv_sec, &tm_info) != 0) return -1;
    char timestamp[48];
    if (!strftime(timestamp, sizeof(timestamp), "%Y-%m-%d_%H-%M-%S", &tm_info)) return -1;
    char stem[MAX_PATH_LENGTH];
    written = snprintf(stem, sizeof(stem), "%s/%s.%09ld.%ld.%s",
                       history_dir, timestamp, now.tv_nsec, (long)getpid(), basename);
    if (written < 0 || (size_t)written >= sizeof(stem) - 7) return -1;

    const char *old_text = old_content ? old_content : "";
    const char *new_text = new_content ? new_content : "";
    HistoryBuffer patch = {0};
    int new_lines = 0;
    if (*new_text) {
        new_lines = 1;
        for (const char *p = new_text; *p; p++) if (*p == '\n') new_lines++;
    }
    if (history_buffer_format(&patch, "--- %s.old\n+++ %s\n@@ -%d,%d +%d,%d @@\n",
                              display_path, display_path, start_line,
                              end_line - start_line + 1, start_line, new_lines) != 0 ||
        history_buffer_prefixed_lines(&patch, old_text, "-") != 0 ||
        history_buffer_prefixed_lines(&patch, new_text, "+") != 0) {
        free(patch.data);
        return -1;
    }
    char patch_path[MAX_PATH_LENGTH];
    written = snprintf(patch_path, sizeof(patch_path), "%s.patch", stem);
    if (written < 0 || (size_t)written >= sizeof(patch_path)) { free(patch.data); return -1; }
    int ok = atomic_write_file(patch_path, patch.data, patch.length) == 0;
    free(patch.data);
    if (!ok) return -1;

    uint8_t old_hash[32], new_hash[32];
    char old_hash_b64[48] = {0}, new_hash_b64[48] = {0};
    _sha256_fips((const uint8_t *)old_text, strlen(old_text), old_hash);
    _sha256_fips((const uint8_t *)new_text, strlen(new_text), new_hash);
    base64_encode((const char *)old_hash, sizeof(old_hash), old_hash_b64);
    base64_encode((const char *)new_hash, sizeof(new_hash), new_hash_b64);
    json_object *meta = json_object_new_object();
    json_object_object_add(meta, "timestamp", json_object_new_int64((int64_t)now.tv_sec));
    json_object_object_add(meta, "path", json_object_new_string(display_path));
    json_object_object_add(meta, "start_line", json_object_new_int(start_line));
    json_object_object_add(meta, "end_line", json_object_new_int(end_line));
    char hash_string[64];
    snprintf(hash_string, sizeof(hash_string), "sha256:%s", old_hash_b64);
    json_object_object_add(meta, "old_hash", json_object_new_string(hash_string));
    snprintf(hash_string, sizeof(hash_string), "sha256:%s", new_hash_b64);
    json_object_object_add(meta, "new_hash", json_object_new_string(hash_string));
    const char *meta_text = json_object_to_json_string_ext(meta, JSON_C_TO_STRING_PRETTY);
    char json_path[MAX_PATH_LENGTH];
    written = snprintf(json_path, sizeof(json_path), "%s.json", stem);
    if (written < 0 || (size_t)written >= sizeof(json_path)) { json_object_put(meta); return -1; }
    ok = atomic_write_file(json_path, meta_text, strlen(meta_text)) == 0;
    json_object_put(meta);
    return ok ? 0 : -1;
}

static void matched_text_line_range(const char *file_content, const char *match_pos,
                                    const char *matched_text, int *start_line, int *end_line) {
    int start = 1;
    for (const char *p = file_content; p < match_pos; p++) {
        if (*p == '\n') start++;
    }

    size_t matched_len = strlen(matched_text);
    int line_breaks = 0;
    for (size_t i = 0; i < matched_len; i++) {
        if (matched_text[i] == '\n') line_breaks++;
    }
    /* A final newline terminates the preceding line; it does not add a line to the range. */
    int end = start + line_breaks;
    if (matched_len > 0 && matched_text[matched_len - 1] == '\n') end--;

    *start_line = start;
    *end_line = end;
}

typedef enum {
    EDIT_NEWLINE_NONE,
    EDIT_NEWLINE_LF,
    EDIT_NEWLINE_CRLF
} EditNewlineStyle;

static EditNewlineStyle detect_edit_newline_style(const char *text, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (text[i] != '\n') continue;
        return i > 0 && text[i - 1] == '\r' ? EDIT_NEWLINE_CRLF : EDIT_NEWLINE_LF;
    }
    return EDIT_NEWLINE_NONE;
}

static char *normalize_edit_text(const char *text, size_t length,
                                 size_t *normalized_length) {
    if (length == SIZE_MAX) return NULL;
    char *normalized = malloc(length + 1);
    if (!normalized) return NULL;

    size_t input = 0;
    size_t output = 0;
    while (input < length) {
        if (text[input] == '\r' && input + 1 < length && text[input + 1] == '\n') {
            normalized[output++] = '\n';
            input += 2;
        } else {
            normalized[output++] = text[input++];
        }
    }
    normalized[output] = '\0';
    *normalized_length = output;
    return normalized;
}

static int map_edit_raw_range(const char *raw_text, size_t raw_length,
                              size_t normalized_start, size_t normalized_end,
                              size_t *raw_start, size_t *raw_end) {
    size_t raw = 0;
    size_t normalized = 0;
    int found_start = 0;
    while (1) {
        if (!found_start && normalized == normalized_start) {
            *raw_start = raw;
            found_start = 1;
        }
        if (normalized == normalized_end) {
            *raw_end = raw;
            return found_start ? 0 : -1;
        }
        if (raw >= raw_length) return -1;
        if (raw_text[raw] == '\r' && raw + 1 < raw_length && raw_text[raw + 1] == '\n') {
            raw += 2;
        } else {
            raw++;
        }
        normalized++;
    }
}

static char *convert_edit_replacement(const char *text, EditNewlineStyle style,
                                      size_t *converted_length) {
    size_t input_length = strlen(text);
    if (style == EDIT_NEWLINE_NONE) {
        char *copy = malloc(input_length + 1);
        if (!copy) return NULL;
        memcpy(copy, text, input_length + 1);
        *converted_length = input_length;
        return copy;
    }

    size_t normalized_length = 0;
    char *normalized = normalize_edit_text(text, input_length, &normalized_length);
    if (!normalized) return NULL;
    if (style == EDIT_NEWLINE_LF) {
        *converted_length = normalized_length;
        return normalized;
    }

    size_t line_breaks = 0;
    for (size_t i = 0; i < normalized_length; i++) {
        if (normalized[i] == '\n') line_breaks++;
    }
    if (normalized_length > SIZE_MAX - line_breaks - 1) {
        free(normalized);
        return NULL;
    }
    char *converted = malloc(normalized_length + line_breaks + 1);
    if (!converted) {
        free(normalized);
        return NULL;
    }
    size_t output = 0;
    for (size_t i = 0; i < normalized_length; i++) {
        if (normalized[i] == '\n') converted[output++] = '\r';
        converted[output++] = normalized[i];
    }
    converted[output] = '\0';
    free(normalized);
    *converted_length = output;
    return converted;
}

// edit_file: 差分適用（仕様準拠版）
json_object* handle_edit_file(json_object *params, json_object *id) {
    // 1. パラメータ検証
    const char *path = json_object_get_string(json_object_object_get(params, "path"));
    json_object *j_start_line = json_object_object_get(params, "start_line");
    json_object *j_end_line = json_object_object_get(params, "end_line");
    json_object *j_old_content = json_object_object_get(params, "old_content");
    json_object *j_new_content = json_object_object_get(params, "new_content");
    json_object *j_old_content_hash = json_object_object_get(params, "old_content_hash");
    json_object *j_include_trailing_newline = json_object_object_get(params, "include_trailing_newline");

    if (!path || !json_object_is_type(j_start_line, json_type_int) || 
        !json_object_is_type(j_end_line, json_type_int) || 
        !json_object_is_type(j_new_content, json_type_string) ||
        (j_old_content && !json_object_is_type(j_old_content, json_type_string)) ||
        (j_old_content_hash && !json_object_is_type(j_old_content_hash, json_type_string)) ||
        (j_include_trailing_newline &&
         !json_object_is_type(j_include_trailing_newline, json_type_boolean))) {
        send_json_rpc_error(id, -32602, "Missing or invalid parameters");
        return NULL;
    }

    // old_content_hashが文字列型の場合のみ検証（オプション）
    int has_old_content_hash = (j_old_content_hash != NULL && json_object_is_type(j_old_content_hash, json_type_string));

    int start_line = json_object_get_int(j_start_line);
    int end_line = json_object_get_int(j_end_line);
    if (start_line < 1 || end_line < start_line || (!j_old_content && !has_old_content_hash)) {
        send_json_rpc_error(id, -32602, "Invalid line range or missing old_content verification");
        return NULL;
    }
    const char *old_content_str = (j_old_content != NULL) ? json_object_get_string(j_old_content) : NULL;
    const char *new_content_str = json_object_get_string(j_new_content);

    // 2. Normalize once and use the resolved path for all file I/O.
    char abs_path[MAX_PATH_LENGTH];
    int path_err = normalize_path(abs_path, sizeof(abs_path), (char *)path);
    if (path_err != 0) {
        send_json_rpc_error(id, path_err, "Invalid path");
        return NULL;
    }

    // ファイル読み込み & コンテンツ検証
    FILE *fp = fopen(abs_path, "rb");
    if (!fp) {
        send_json_rpc_error(id, -32001, "File does not exist");
        perror("fopen");
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        send_json_rpc_error(id, -32603, "Cannot determine file size");
        return NULL;
    }
    long file_size = ftell(fp);
    if (file_size < 0 || (uint64_t)file_size > g_max_read_file_size ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        send_json_rpc_error(id, -32603, "File is too large or cannot be read");
        return NULL;
    }

    char *file_content = malloc(file_size + 1);
    if (!file_content) {
        fclose(fp);
        send_json_rpc_error(id, -32603, "Memory allocation failed");
        return NULL;
    }
    
    size_t bytes_read = fread(file_content, 1, (size_t)file_size, fp);
    if (bytes_read != (size_t)file_size || ferror(fp)) {
        free(file_content);
        fclose(fp);
        send_json_rpc_error(id, -32603, "Failed while reading file");
        return NULL;
    }
    file_content[file_size] = '\0';
    fclose(fp);

    EditNewlineStyle newline_style =
        detect_edit_newline_style(file_content, (size_t)file_size);
    size_t normalized_length = 0;
    char *normalized_content = normalize_edit_text(file_content, (size_t)file_size,
                                                   &normalized_length);
    if (!normalized_content) {
        free(file_content);
        send_json_rpc_error(id, -32603, "Memory allocation failed");
        return NULL;
    }

    size_t old_len = 0;
    char *normalized_old_content = NULL;
    if (old_content_str != NULL) {
        normalized_old_content = normalize_edit_text(old_content_str, strlen(old_content_str),
                                                     &old_len);
        if (!normalized_old_content) {
            free(normalized_content);
            free(file_content);
            send_json_rpc_error(id, -32603, "Memory allocation failed");
            return NULL;
        }
    }
    
    // 行番号に基づいたコンテンツ検証（まず行番号で指定された範囲を検証）
    //char *temp_ptr = normalized_content;
    
    // 抽出対象の開始位置と長さを計算
    char *target_start = NULL;
    size_t target_len = 0;

    int last_lf;
    if (has_old_content_hash) {
        // Match read_file's default and allow callers to reproduce either hash policy.
        last_lf = (!j_include_trailing_newline ||
                   json_object_get_boolean(j_include_trailing_newline))
            ? EOB_LINE_BREAK_INCLUDE : EOB_LINE_BREAK_EXCLUDE;
    } else {
        // Preserve the existing text-verification behavior.
        last_lf = (old_len > 0 && normalized_old_content[old_len - 1] == '\n')
            ? EOB_LINE_BREAK_INCLUDE : EOB_LINE_BREAK_EXCLUDE;
    }
    
    target_start = extract_text_by_line_range(normalized_content,&target_len,start_line,end_line,last_lf);

#ifdef _DEBUG
    fprintf(stderr,"[DEBUG] content_len = %d / ",target_len); 
    fprintf(stderr,"input_len = %d / ",old_len);  
    fprintf(stderr,"last_lf = %d\n",last_lf);  
#endif

    // コンテンツマッチ検証（行番号指定で）
    if (has_old_content_hash && target_start && target_len > 0) {
        // ハッシュ値による検証
        uint8_t file_hash[32] = {0};
        _sha256_fips((const uint8_t*)target_start, target_len, file_hash);
        
        // 計算したハッシュ値をBase64エンコード
        char file_hash_b64[48] = {0};
        base64_encode((const char*)file_hash, sizeof(file_hash), file_hash_b64);
        
        // 受信したハッシュ値を解析（"sha256:"プレフィックス除去）
        const char *hash_str = json_object_get_string(j_old_content_hash);
        if (strncmp(hash_str, "sha256:", 7) == 0) {
            hash_str += 7; // プレフィックスをスキップ
        }
        
        // Base64エンコードしたハッシュ値と比較
        if (strcmp(file_hash_b64, hash_str) != 0) {
            // ハッシュ不一致
            char msg[256];
            snprintf(msg, sizeof(msg), "old_content hash mismatch at lines %d-%d", start_line, end_line);
            free(normalized_old_content);
            free(normalized_content);
            free(file_content);
            send_json_rpc_error(id, -32002, msg);
            return NULL;
        }
    } else if (target_start && normalized_old_content != NULL &&
               old_len == target_len &&
               strncmp(target_start, normalized_old_content, target_len) == 0) {
        // File and request line endings are compared in normalized LF form.
    } else if (target_start && normalized_old_content != NULL && old_len == 0) {
        // old_contentが空文字列の場合はマッチとみなす
    } else if (target_start && normalized_old_content != NULL && old_len > 0) {
        char *match_pos = strstr(normalized_content, normalized_old_content);
        if (match_pos) {
            // マッチしたが、行番号が正しくない場合はエラーを返す（正しい行範囲を計算）
            int correct_start_line;
            int correct_end_line;
            matched_text_line_range(normalized_content, match_pos, normalized_old_content,
                                    &correct_start_line, &correct_end_line);
            
            char msg[MAX_PATH_LENGTH + 128];
            snprintf(msg, sizeof(msg), 
                     "old_content matches file content but at different line range. "
                     "Expected lines: %d-%d, Actual correct lines: %d-%d",
                     start_line, end_line, correct_start_line, correct_end_line);

            free(normalized_old_content);
            free(normalized_content);
            free(file_content);
            send_json_rpc_error(id, -32002, msg);
            return NULL;
        } else {
            char msg[MAX_PATH_LENGTH + 64];
            snprintf(msg, sizeof(msg), 
                     "old_content does not match file content. Line numbers: %d-%d", 
                     start_line, end_line);

            free(normalized_old_content);
            free(normalized_content);
            free(file_content);
            send_json_rpc_error(id, -32002, msg);
            return NULL;
        }
    } else {
        // target_startがない場合
        char msg[MAX_PATH_LENGTH + 64];
        snprintf(msg, sizeof(msg), 
                 "old_content does not match file content. Line numbers: %d-%d", 
                 start_line, end_line);

        free(normalized_old_content);
        free(normalized_content);
        free(file_content);
        send_json_rpc_error(id, -32002, msg);
        return NULL;
    }

    // 3. Replace the corresponding raw byte range so untouched line endings
    // remain byte-for-byte intact. New text follows the file's newline style.
    size_t normalized_start = (size_t)(target_start - normalized_content);
    if (normalized_start > normalized_length ||
        target_len > normalized_length - normalized_start) {
        free(normalized_old_content);
        free(normalized_content);
        free(file_content);
        send_json_rpc_error(id, -32603, "Invalid normalized line range");
        return NULL;
    }
    size_t raw_start = 0;
    size_t raw_end = 0;
    if (map_edit_raw_range(file_content, (size_t)file_size, normalized_start,
                           normalized_start + target_len, &raw_start, &raw_end) != 0) {
        free(normalized_old_content);
        free(normalized_content);
        free(file_content);
        send_json_rpc_error(id, -32603, "Invalid normalized line range");
        return NULL;
    }
    size_t replacement_len = 0;
    char *replacement = convert_edit_replacement(new_content_str, newline_style,
                                                 &replacement_len);
    size_t suffix_len = (size_t)file_size - raw_end;
    if (!replacement || raw_start > SIZE_MAX - replacement_len ||
        raw_start + replacement_len > SIZE_MAX - suffix_len - 1) {
        free(replacement);
        free(normalized_old_content);
        free(normalized_content);
        free(file_content);
        send_json_rpc_error(id, -32603, "Memory allocation failed");
        return NULL;
    }
    size_t new_file_length = raw_start + replacement_len + suffix_len;
    char *new_file_content = malloc(new_file_length + 1);
    if (!new_file_content) {
        free(replacement);
        free(normalized_old_content);
        free(normalized_content);
        free(file_content);
        send_json_rpc_error(id, -32603, "Memory allocation failed");
        return NULL;
    }
    memcpy(new_file_content, file_content, raw_start);
    memcpy(new_file_content + raw_start, replacement, replacement_len);
    memcpy(new_file_content + raw_start + replacement_len,
           file_content + raw_end, suffix_len);
    new_file_content[new_file_length] = '\0';

    free(replacement);
    free(normalized_old_content);
    free(normalized_content);
    free(file_content);

    if (atomic_write_file(abs_path, new_file_content, new_file_length) != 0) {
        free(new_file_content);
        send_json_rpc_error(id, -32003, "Failed to apply file patch");
        return NULL;
    }
    free(new_file_content);

    // History is best-effort: the primary edit has already committed atomically.
    int history_saved = save_edit_history(abs_path, path, start_line, end_line,
                                          old_content_str, new_content_str) == 0;

    // 5. MCP-compliant Response
    json_object *result = json_object_new_object();
    json_object *content_arr = json_object_new_array();
    json_object *text_obj = json_object_new_object();
    
    char msg[256];
    snprintf(msg, sizeof(msg), history_saved
             ? "File %s edited successfully. History saved to .history/."
             : "File %s edited successfully, but history could not be saved.", path);
    
    json_object_object_add(text_obj, "type", json_object_new_string("text"));
    json_object_object_add(text_obj, "text", json_object_new_string(msg));
    
    json_object_array_add(content_arr, text_obj);
    json_object_object_add(result, "content", content_arr);

    return result;
}

// delete_file: ファイルまたはディレクトリの削除
json_object* handle_delete_file(json_object *params, json_object *id) {
    const char *path = json_object_get_string(json_object_object_get(params, "path"));
    
    if (!path || strlen(path) == 0) {
        send_json_rpc_error(id, -32602, "Invalid parameters: path is required");
        return NULL;
    }

    // パス検証（既存のヘルパー関数を利用）
    char abs_path[MAX_PATH_LENGTH];
    int err = normalize_path(abs_path, sizeof(abs_path), (char *)path);
    if (err != 0) {
        send_json_rpc_error(id, err, "Invalid path");
        return NULL;
    }

    // ファイルまたはディレクトリ削除（removeは空ディレクトリも対象）
    if (remove(abs_path) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Delete failed: %s", strerror(errno));
        send_json_rpc_error(id, -32603, msg);
        return NULL;
    }

    // MCP仕様に準拠した結果生成（content配列内にtext型を配置）
    json_object *result = json_object_new_object();
    json_object *content_arr = json_object_new_array();
    json_object *text_obj = json_object_new_object();
    
    json_object_object_add(text_obj, "type", json_object_new_string("text"));
    
    char msg[256];
    snprintf(msg, sizeof(msg), "File/Directory deleted successfully: %s", path);
    json_object_object_add(text_obj, "text", json_object_new_string(msg));
    
    json_object_array_add(content_arr, text_obj);
    json_object_object_add(result, "content", content_arr);

    return result;
    
}

// copy_file: ファイルコピー（MCP仕様準拠版）
json_object* handle_copy_file(json_object *params, json_object *id) {
    const char *source = json_object_get_string(json_object_object_get(params, "source"));
    const char *destination = json_object_get_string(json_object_object_get(params, "destination"));
    
    if (!source || !destination) {
        send_json_rpc_error(id, -32602, "Invalid parameters: source and destination are required");
        return NULL;
    }
    
    char resolved_source[MAX_PATH_LENGTH];
    if (normalize_path(resolved_source, sizeof(resolved_source), (char *)source) != 0) {
        send_json_rpc_error(id, -32603, "Source file does not exist");
        return NULL;
    }

    struct stat src_stat;
    if (mcp_file_stat(resolved_source, &src_stat) != 0 || !S_ISREG(src_stat.st_mode)) {
        char msg[MAX_PATH_LENGTH + 64];
        snprintf(msg, sizeof(msg), "Source file does not exist: %s", source);
        send_json_rpc_error(id, -32603, msg);
        return NULL;
    }
    
    char resolved_destination[MAX_PATH_LENGTH];
    if (normalize_new_path(resolved_destination, sizeof(resolved_destination), destination) != 0) {
        send_json_rpc_error(id, -32603, "Destination directory does not exist or path is invalid");
        return NULL;
    }
    if (atomic_copy_file(resolved_source, resolved_destination) != 0) {
        send_json_rpc_error(id, -32603, errno == EEXIST ? "Destination file already exists" : "File copy failed");
        return NULL;
    }
    
    // MCP仕様に準拠した結果生成（content配列内にtext型を配置）
    json_object *result = json_object_new_object();
    json_object *content_arr = json_object_new_array();
    json_object *text_obj = json_object_new_object();
    
    json_object_object_add(text_obj, "type", json_object_new_string("text"));
    
    char msg[256];
    snprintf(msg, sizeof(msg), "File copied successfully: %s -> %s", source, destination);
    json_object_object_add(text_obj, "text", json_object_new_string(msg));
    
    json_object_array_add(content_arr, text_obj);
    json_object_object_add(result, "content", content_arr);

    return result;
}
