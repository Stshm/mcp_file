#include "mcp_common.h"

#define MCP_VIRTUAL_PATH_RULE \
    "Use an MCP virtual path rooted at '/'; '/' is the configured root. " \
    "Use forward slashes even on Windows. Do not use drive letters, UNC paths, " \
    "native OS paths, or wildcard characters."

static int g_initialize_responded = 0;
static int g_initialized = 0;

static json_object *make_server_instructions(void) {
    const char *native_root = g_native_root_dir[0] ? g_native_root_dir : g_root_dir;
    char instructions[MAX_PATH_LENGTH + 768];
    int written = snprintf(
        instructions, sizeof(instructions),
        "The configured native filesystem root is \"%s\". It is exposed by this "
        "server as MCP virtual root \"/\". For every path-like tool argument, use "
        "only an MCP virtual path beginning with '/' and use forward slashes on all "
        "operating systems. To translate a native path below the configured root, "
        "remove the native root prefix and replace path separators with '/'. For "
        "example, the relative child project/file.txt is /project/file.txt. Never "
        "pass a drive letter, UNC path, native absolute path, or wildcard in a path "
        "argument. Paths outside the configured root are unavailable. For run_cmd, "
        "path follows the same MCP virtual-path rule; use paths relative to that "
        "working directory inside command text for portable Windows/Linux behavior.",
        native_root);
    if (written < 0 || (size_t)written >= sizeof(instructions)) return NULL;
    return json_object_new_string(instructions);
}

static void add_required(json_object *schema, const char *const *fields) {
    json_object *required = json_object_new_array();
    for (size_t i = 0; fields[i] != NULL; i++) {
        json_object_array_add(required, json_object_new_string(fields[i]));
    }
    json_object_object_add(schema, "required", required);
}

static int required_string(json_object *args, const char *name) {
    json_object *v = json_object_object_get(args, name);
    return v && json_object_is_type(v, json_type_string) && json_object_get_string(v)[0] != '\0';
}

static int required_int(json_object *args, const char *name) {
    json_object *v = json_object_object_get(args, name);
    return v && json_object_is_type(v, json_type_int);
}

static int required_string_allow_empty(json_object *args, const char *name) {
    return json_object_is_type(json_object_object_get(args, name), json_type_string);
}

static int validate_tool_arguments(const char *name, json_object *args) {
    if (!args || !json_object_is_type(args, json_type_object)) return 0;
    if (!strcmp(name, "read_file") || !strcmp(name, "stat_file") ||
        !strcmp(name, "delete_file") || !strcmp(name, "create_directory"))
        return required_string(args, "path");
    if (!strcmp(name, "write_file") || !strcmp(name, "create_file"))
        return required_string(args, "path") && required_string_allow_empty(args, "content");
    if (!strcmp(name, "edit_file"))
        return required_string(args, "path") && required_int(args, "start_line") &&
               required_int(args, "end_line") && required_string_allow_empty(args, "new_content");
    if (!strcmp(name, "move_file") || !strcmp(name, "copy_file"))
        return required_string(args, "source") && required_string(args, "destination");
    if (!strcmp(name, "search_files"))
        return required_string(args, "path") && required_string(args, "pattern");
    if (!strcmp(name, "search_string_in_file"))
        return required_string(args, "path") && required_string(args, "search_string");
    if (!strcmp(name, "tag_file"))
        return required_string(args, "path") && json_object_is_type(json_object_object_get(args, "tags"), json_type_array);
    if (!strcmp(name, "batch_tag_file"))
        return json_object_is_type(json_object_object_get(args, "files"), json_type_array);
    if (!strcmp(name, "get_tags"))
        return required_string(args, "type");
    if (!strcmp(name, "delete_tag"))
        return required_string(args, "type") && required_string(args, "path");
    if (!strcmp(name, "download_file"))
        return required_string(args, "url") && required_string(args, "filename");
    if (!strcmp(name, "run_cmd"))
        return required_string(args, "command");
    return 1;
}

static json_object *string_schema(const char *description) {
    json_object *schema = json_object_new_object();
    json_object_object_add(schema, "type", json_object_new_string("string"));
    json_object_object_add(schema, "description", json_object_new_string(description));
    return schema;
}

static json_object *string_array_schema(const char *description) {
    json_object *schema = json_object_new_object();
    json_object_object_add(schema, "type", json_object_new_string("array"));
    json_object_object_add(schema, "description", json_object_new_string(description));
    json_object *items = json_object_new_object();
    json_object_object_add(items, "type", json_object_new_string("string"));
    json_object_object_add(items, "pattern", json_object_new_string("^[A-Za-z0-9][A-Za-z0-9:_-]*$"));
    json_object_object_add(items, "maxLength", json_object_new_int(255));
    json_object_object_add(schema, "items", items);
    return schema;
}

static json_object *new_tool_schema(const char *name, const char *title, const char *description,
                                    json_object *properties, const char *const *required) {
    json_object *tool = json_object_new_object();
    json_object_object_add(tool, "name", json_object_new_string(name));
    json_object_object_add(tool, "title", json_object_new_string(title));
    json_object_object_add(tool, "description", json_object_new_string(description));
    json_object *schema = json_object_new_object();
    json_object_object_add(schema, "$schema", json_object_new_string("https://json-schema.org/draft/2020-12/schema#"));
    json_object_object_add(schema, "type", json_object_new_string("object"));
    json_object_object_add(schema, "properties", properties);
    add_required(schema, required);
    json_object_object_add(tool, "inputSchema", schema);
    return tool;
}

static void add_extended_tag_tools(json_object *tools) {
    json_object *batch_props = json_object_new_object();
    json_object *files = json_object_new_object();
    json_object_object_add(files, "type", json_object_new_string("array"));
    json_object *entry = json_object_new_object();
    json_object_object_add(entry, "type", json_object_new_string("object"));
    json_object *entry_props = json_object_new_object();
    json_object_object_add(entry_props, "path", string_schema("File path. " MCP_VIRTUAL_PATH_RULE));
    json_object_object_add(entry_props, "tags", string_array_schema("Tags to apply"));
    json_object_object_add(entry_props, "mode", string_schema("add, set, or replace"));
    json_object_object_add(entry, "properties", entry_props);
    { const char *required[] = {"path", "tags", NULL}; add_required(entry, required); }
    json_object_object_add(files, "items", entry);
    json_object_object_add(batch_props, "files", files);
    json_object_object_add(batch_props, "default_mode", string_schema("Default mode: add, set, or replace"));
    { const char *required[] = {"files", NULL};
      json_object_array_add(tools, new_tool_schema("batch_tag_file", "Tag multiple files",
          "Adds or replaces tags for multiple files in one operation.", batch_props, required)); }

    json_object *get_props = json_object_new_object();
    json_object_object_add(get_props, "type", string_schema("Lookup type: file or tag"));
    json_object_object_add(get_props, "path", string_schema("File path when type=file. " MCP_VIRTUAL_PATH_RULE));
    json_object_object_add(get_props, "tag_name", string_schema("Tag name when type=tag"));
    { const char *required[] = {"type", NULL};
      json_object_array_add(tools, new_tool_schema("get_tags", "Get tags or tagged files",
          "Gets tags for a file or files associated with a tag.", get_props, required)); }

    json_object *delete_props = json_object_new_object();
    json_object_object_add(delete_props, "type", string_schema("Delete type: file or all_tags"));
    json_object_object_add(delete_props, "path", string_schema("File path. " MCP_VIRTUAL_PATH_RULE));
    json_object_object_add(delete_props, "tags_to_remove", string_array_schema("Tags to remove when type=file"));
    { const char *required[] = {"type", "path", NULL};
      json_object_array_add(tools, new_tool_schema("delete_tag", "Remove tags from a file",
          "Removes selected tags or all tags from a file.", delete_props, required)); }
}

// クライアントが要求したバージョンを判定
static const char *negotiate_version(json_object *client_info) {
    if (!client_info || !json_object_is_type(client_info, json_type_object)) {
        fprintf(stderr, "No client capabilities provided, using default version\n");
        return supported_versions[0];  // デフォルトは最新バージョン
    }
    
    json_object *protocol_version = json_object_object_get(client_info, "protocolVersion");

    if (protocol_version && json_object_is_type(protocol_version, json_type_string)) {
        const char *requested = json_object_get_string(protocol_version);

        // クライアントが指定したバージョンがサポート範囲内か確認
        for (int i = 0; supported_versions[i] != NULL; i++) {
            if (!strcmp(requested, supported_versions[i])) {
                fprintf(stderr, "Negotiated version: %s\n", requested);
                return requested;
            }
        }
        
        // サポート範囲外の場合は最新バージョンを返す（フォールバック）
        fprintf(stderr, "Client requested unsupported version '%s', falling back to latest\n", requested);
    }
    
    // デフォルトは最新バージョン
    return supported_versions[0];
}

/* ==================== メインハンドラ ==================== */

void handle_request(json_object *request) {
    json_object *jsonrpc_obj = json_object_object_get(request, "jsonrpc");
    if (!jsonrpc_obj || !json_object_is_type(jsonrpc_obj, json_type_string) ||
        strcmp(json_object_get_string(jsonrpc_obj), "2.0") != 0) {
        json_object *bad_id = json_object_object_get(request, "id");
        send_json_rpc_error(bad_id, -32600, "Invalid Request: jsonrpc must be \"2.0\"");
        return;
    }
    json_object *method_obj = json_object_object_get(request, "method");
    const char *method = method_obj ? json_object_get_string(method_obj) : NULL;
    json_object *params = json_object_object_get(request, "params");
    json_object *id = json_object_object_get(request, "id");
    
    if (!method) {
        if (id) {
            send_json_rpc_error(id, -32600, "Invalid Request: missing method");
        }
        return;
    }

    fprintf(stderr, "[INFO] Client request: %s\n", method);

    // 通知（notifications/initialized, notifications/cancelled 等）にはレスポンスを返さない
    if (!strcmp(method, "notifications/initialized") || !strcmp(method, "initialized") ||
        !strcmp(method, "notifications/cancelled") || !strcmp(method, "cancelled") ||
        strncmp(method, "notifications/", 14) == 0) {
        if (!strcmp(method, "notifications/initialized") || !strcmp(method, "initialized")) {
            if (g_initialize_responded) g_initialized = 1;
        }
        return;
    }

    if (!strcmp(method, "initialize")) {
        if (g_initialize_responded) {
            send_json_rpc_error(id, -32600, "Invalid Request: initialize was already completed");
            return;
        }
        // バージョンネゴシエーション
        const char *version = negotiate_version(params);
        
        json_object *result = json_object_new_object();
        json_object_object_add(result, "protocolVersion", json_object_new_string(version));
        
        json_object *capabilities = json_object_new_object();

        /*
         * MCP capabilities are objects, not booleans.  Advertising these
         * capabilities lets the client request tools/list and resources/list.
         * This server does not emit list-changed notifications or implement
         * resource subscriptions, so no optional flags are declared.
         */
        json_object_object_add(capabilities, "tools", json_object_new_object());
        json_object_object_add(capabilities, "resources", json_object_new_object());
        json_object_object_add(result, "capabilities", capabilities);

        // サーバー情報にバージョンを含める
        json_object *server_info = json_object_new_object();
        json_object_object_add(server_info, "name", json_object_new_string("mcp-c-server"));
        json_object_object_add(server_info, "version", json_object_new_string("1.0.0"));
        
        // supported_versions配列の追加
        json_object *versions_array = json_object_new_array();
        for (int i = 0; supported_versions[i] != NULL; i++) {
            json_object_array_add(versions_array, json_object_new_string(supported_versions[i]));
        }
        json_object_object_add(server_info, "supported_versions", versions_array);
        
        json_object_object_add(result, "serverInfo", server_info);
        json_object *instructions = make_server_instructions();
        if (instructions) json_object_object_add(result, "instructions", instructions);
        
        fprintf(stderr, "MCP Server initialized with version: %s\n", version);
        g_initialize_responded = 1;
        g_initialized = 0;
        send_json_rpc_response(id, result);
        
    } else if (!strcmp(method, "ping")) {
        send_json_rpc_response(id, json_object_new_object());
    } else if (!g_initialized && strcmp(method, "ping") != 0) {
        send_json_rpc_error(id, -32002, "Server is not initialized");
    } else if (!strcmp(method, "tools/list")) {
        // MCP 2025-11-25 Spec: {"tools": [...]} 形式
        json_object *result = json_object_new_object();
        json_object *tools_array = json_object_new_array();
        char configured_description[192];

        // --- read_file ツール定義 ---
        json_object *read_tool = json_object_new_object();
        json_object_object_add(read_tool, "name", json_object_new_string("read_file"));
        json_object_object_add(read_tool, "title", json_object_new_string("Read file content from the filesystem"));
        json_object_object_add(read_tool, "description", json_object_new_string("Reads UTF-8 text files from the local filesystem. Other text encodings are not supported; non-UTF-8 or binary content is returned Base64-encoded."));

        // inputSchema 作成
        json_object *read_input_schema = json_object_new_object();
        json_object_object_add(read_input_schema, "$schema", json_object_new_string("https://json-schema.org/draft/2020-12/schema#"));
        json_object_object_add(read_input_schema, "type", json_object_new_string("object"));

        // properties 作成（まず空のオブジェクトを作る）
        json_object *read_props = json_object_new_object();
        
        // プロパティを追加
        json_object *p_path = json_object_new_object();
        json_object_object_add(p_path, "type", json_object_new_string("string"));
        json_object_object_add(p_path, "description", json_object_new_string("Path to the file. " MCP_VIRTUAL_PATH_RULE));
        json_object_object_add(read_props, "path", p_path);

        // start_line プロパティ（行数指定による部分的読み出し用）
        json_object *p_start = json_object_new_object();
        json_object_object_add(p_start, "type", json_object_new_string("integer"));
        json_object_object_add(p_start, "description", json_object_new_string("Start line number for partial read (1-based). Must be used together with end_line."));
        json_object_object_add(read_props, "start_line", p_start);

        // end_line プロパティ（行数指定による部分的読み出し用）
        json_object *p_end = json_object_new_object();
        json_object_object_add(p_end, "type", json_object_new_string("integer"));
        json_object_object_add(p_end, "description", json_object_new_string("End line number for partial read (1-based). Must be used together with start_line."));
        json_object_object_add(read_props, "end_line", p_end);

        // include_line_numbers プロパティ（行番号付加用）
        json_object *p_line_nums = json_object_new_object();
        json_object_object_add(p_line_nums, "type", json_object_new_string("boolean"));
        json_object_object_add(p_line_nums, "description", json_object_new_string("Include line numbers in the output (format: \"1:\"). Default is false."));
        json_object_object_add(read_props, "include_line_numbers", p_line_nums);

        // include_trailing_newline プロパティ（末尾改行の含有不含有用）
        json_object *p_trail_lf = json_object_new_object();
        json_object_object_add(p_trail_lf, "type", json_object_new_string("boolean"));
        json_object_object_add(p_trail_lf, "description", json_object_new_string("Include trailing newline in the output. Default is true."));
        json_object_object_add(read_props, "include_trailing_newline", p_trail_lf);

        // properties を inputSchema に追加
        json_object_object_add(read_input_schema, "properties", read_props);
        { const char *required[] = {"path", NULL}; add_required(read_input_schema, required); }
        
        // tool に inputSchema を設定
        json_object_object_add(read_tool, "inputSchema", read_input_schema);

        json_object_array_add(tools_array, read_tool);

        json_object *stat_properties = json_object_new_object();
        json_object_object_add(stat_properties, "path", string_schema(
            "Path to the file to inspect. " MCP_VIRTUAL_PATH_RULE));
        { const char *required[] = {"path", NULL};
          json_object_array_add(tools_array, new_tool_schema(
              "stat_file", "Get file size, modification time, and text line count; missing or inaccessible files return status",
              "Returns size in bytes and UTC modification time for a regular file. "
              "For UTF-8 text files, also returns the number of lines; use this "
              "before choosing a read_file line range. Empty text files have 0 "
              "lines, and a final newline does not add an empty line.",
              stat_properties, required)); }

        json_object *run_cmd_properties = json_object_new_object();
        json_object_object_add(run_cmd_properties, "command", string_schema(
            "Command line interpreted by cmd.exe on Windows or /bin/sh on Linux. "
            "Windows selects console code page 65001 (UTF-8) before execution."));
        json_object_object_add(run_cmd_properties, "path", string_schema(
            "Working directory inside the MCP root. Default: /. " MCP_VIRTUAL_PATH_RULE));
        json_object *run_cmd_timeout = json_object_new_object();
        json_object_object_add(run_cmd_timeout, "type", json_object_new_string("integer"));
        snprintf(configured_description, sizeof(configured_description),
                 "Timeout in seconds. Default: %d, maximum: %d.",
                 g_run_cmd_timeout_seconds, g_max_run_cmd_timeout_seconds);
        json_object_object_add(run_cmd_timeout, "description",
                               json_object_new_string(configured_description));
        json_object_object_add(run_cmd_timeout, "minimum", json_object_new_int(1));
        json_object_object_add(run_cmd_timeout, "maximum",
                               json_object_new_int(g_max_run_cmd_timeout_seconds));
        json_object_object_add(run_cmd_properties, "timeout_seconds", run_cmd_timeout);
        { const char *required[] = {"command", NULL};
          json_object_array_add(tools_array, new_tool_schema(
              "run_cmd", "Run a command inside the filesystem sandbox",
              "Runs a shell command with the selected MCP directory as its working "
              "directory. Standard output and standard error are combined in order. "
              "A non-zero exit code is a normal result. Commands inherit the server "
              "sandbox and receive no interactive standard input. Windows command "
              "output is normalized to UTF-8; legacy OEM text reports its original "
              "Windows code page in source_encoding.",
              run_cmd_properties, required)); }

        // --- write_file ツール定義---
        json_object *write_tool = json_object_new_object();
        json_object_object_add(write_tool, "name", json_object_new_string("write_file"));
        json_object_object_add(write_tool, "title", json_object_new_string("Create or overwrite a file"));
        json_object_object_add(write_tool, "description", json_object_new_string("Writes content to a file on the local filesystem."));

        json_object *write_input_schema = json_object_new_object();
        json_object_object_add(write_input_schema, "$schema", json_object_new_string("https://json-schema.org/draft/2020-12/schema#"));
        json_object_object_add(write_input_schema, "type", json_object_new_string("object"));

        json_object *write_props = json_object_new_object();
        
        json_object *w_path = json_object_new_object();
        json_object_object_add(w_path, "type", json_object_new_string("string"));
        json_object_object_add(w_path, "description", json_object_new_string("Path to the file. " MCP_VIRTUAL_PATH_RULE));
        json_object_object_add(write_props, "path", w_path);

        json_object *w_content = json_object_new_object();
        json_object_object_add(w_content, "type", json_object_new_string("string"));
        json_object_object_add(w_content, "description", json_object_new_string("Content to write"));
        json_object_object_add(write_props, "content", w_content);

        json_object_object_add(write_input_schema, "properties", write_props);
        { const char *required[] = {"path", "content", NULL}; add_required(write_input_schema, required); }
        json_object_object_add(write_tool, "inputSchema", write_input_schema);

        json_object_array_add(tools_array, write_tool);

        // --- create_file ツール定義 ---
        json_object *create_file_tool = json_object_new_object();
        json_object_object_add(create_file_tool, "name", json_object_new_string("create_file"));
        json_object_object_add(create_file_tool, "title", json_object_new_string("Create a new file (fails if file already exists)"));
        json_object_object_add(create_file_tool, "description", json_object_new_string("Creates a new file on the local filesystem. Fails if the file already exists."));

        json_object *create_file_input_schema = json_object_new_object();
        json_object_object_add(create_file_input_schema, "$schema", json_object_new_string("https://json-schema.org/draft/2020-12/schema#"));
        json_object_object_add(create_file_input_schema, "type", json_object_new_string("object"));

        json_object *create_file_props = json_object_new_object();
        json_object *cf_path = json_object_new_object();
        json_object_object_add(cf_path, "type", json_object_new_string("string"));
        json_object_object_add(cf_path, "description", json_object_new_string("Path to the new file. " MCP_VIRTUAL_PATH_RULE));
        json_object_object_add(create_file_props, "path", cf_path);

        json_object *cf_content = json_object_new_object();
        json_object_object_add(cf_content, "type", json_object_new_string("string"));
        json_object_object_add(cf_content, "description", json_object_new_string("Content to write"));
        json_object_object_add(create_file_props, "content", cf_content);

        json_object_object_add(create_file_input_schema, "properties", create_file_props);
        { const char *required[] = {"path", "content", NULL}; add_required(create_file_input_schema, required); }
        json_object_object_add(create_file_tool, "inputSchema", create_file_input_schema);

        json_object_array_add(tools_array, create_file_tool);

        // --- move_file ツール定義 ---
        // --- get_directory_tree ツール定義 ---
        json_object *tree_tool = json_object_new_object();
        json_object_object_add(tree_tool, "name", json_object_new_string("get_directory_tree"));
        json_object_object_add(tree_tool, "title", json_object_new_string("Get directory tree structure"));
        snprintf(configured_description, sizeof(configured_description),
                 "Returns a bounded tree structure of directory contents (default depth: %d).",
                 g_tree_default_depth);
        json_object_object_add(tree_tool, "description", json_object_new_string(configured_description));

        // inputSchema 作成
        json_object *tree_input_schema = json_object_new_object();
        json_object_object_add(tree_input_schema, "$schema", json_object_new_string("https://json-schema.org/draft/2020-12/schema#"));
        json_object_object_add(tree_input_schema, "type", json_object_new_string("object"));

        // properties 作成（まず空のオブジェクトを作る）
        json_object *tree_props = json_object_new_object();

        // プロパティ: path (必須)
        json_object *t_path = json_object_new_object();
        json_object_object_add(t_path, "type", json_object_new_string("string"));
        json_object_object_add(t_path, "description", json_object_new_string("Path to the directory. " MCP_VIRTUAL_PATH_RULE));
        json_object_object_add(tree_props, "path", t_path);

        // プロパティ: depth (任意)
        json_object *t_depth = json_object_new_object();
        json_object_object_add(t_depth, "type", json_object_new_string("integer"));
        snprintf(configured_description, sizeof(configured_description),
                 "Maximum recursion depth from 0 to %d. Default: %d.",
                 g_tree_max_depth, g_tree_default_depth);
        json_object_object_add(t_depth, "description", json_object_new_string(configured_description));
        json_object_object_add(t_depth, "minimum", json_object_new_int(0));
        json_object_object_add(t_depth, "maximum", json_object_new_int(g_tree_max_depth));
        json_object_object_add(tree_props, "depth", t_depth);

        json_object *t_hidden = json_object_new_object();
        json_object_object_add(t_hidden, "type", json_object_new_string("boolean"));
        json_object_object_add(t_hidden, "description", json_object_new_string("Include directories whose names start with '.' (such as .history). Default: false. An explicitly selected path remains accessible."));
        json_object_object_add(tree_props, "include_hidden_directories", t_hidden);

        // properties を inputSchema に追加
        json_object_object_add(tree_input_schema, "properties", tree_props);
        // tool に inputSchema を設定
        json_object_object_add(tree_tool, "inputSchema", tree_input_schema);

        // tools_array に追加
        json_object_array_add(tools_array, tree_tool);

        // --- delete_file ツール定義 ---
        json_object *delete_tool = json_object_new_object();
        json_object_object_add(delete_tool, "name", json_object_new_string("delete_file"));
        json_object_object_add(delete_tool, "title", json_object_new_string("Delete a file or directory"));
        json_object_object_add(delete_tool, "description", json_object_new_string("Deletes a file or empty directory from the local filesystem."));

        json_object *delete_input_schema = json_object_new_object();
        json_object_object_add(delete_input_schema, "$schema", json_object_new_string("https://json-schema.org/draft/2020-12/schema#"));
        json_object_object_add(delete_input_schema, "type", json_object_new_string("object"));

        json_object *delete_props = json_object_new_object();
        json_object *d_path = json_object_new_object();
        json_object_object_add(d_path, "type", json_object_new_string("string"));
        json_object_object_add(d_path, "description", json_object_new_string("Path to the file or directory to delete. " MCP_VIRTUAL_PATH_RULE));
        json_object_object_add(delete_props, "path", d_path);

        json_object_object_add(delete_input_schema, "properties", delete_props);
        { const char *required[] = {"path", NULL}; add_required(delete_input_schema, required); }
        json_object_object_add(delete_tool, "inputSchema", delete_input_schema);

        json_object_array_add(tools_array, delete_tool);

        // --- create_directory ツール定義 ---
        json_object *create_dir_tool = json_object_new_object();
        json_object_object_add(create_dir_tool, "name", json_object_new_string("create_directory"));
        json_object_object_add(create_dir_tool, "title", json_object_new_string("Create a new directory"));
        json_object_object_add(create_dir_tool, "description", json_object_new_string("Creates a new directory"));

        json_object *create_dir_input_schema = json_object_new_object();
        json_object_object_add(create_dir_input_schema, "$schema", json_object_new_string("https://json-schema.org/draft/2020-12/schema#"));
        json_object_object_add(create_dir_input_schema, "type", json_object_new_string("object"));

        json_object *create_dir_props = json_object_new_object();
        json_object *c_path = json_object_new_object();
        json_object_object_add(c_path, "type", json_object_new_string("string"));
        json_object_object_add(c_path, "description", json_object_new_string("Path to the new directory. " MCP_VIRTUAL_PATH_RULE));
        json_object_object_add(create_dir_props, "path", c_path);

        json_object_object_add(create_dir_input_schema, "properties", create_dir_props);
        { const char *required[] = {"path", NULL}; add_required(create_dir_input_schema, required); }
        json_object_object_add(create_dir_tool, "inputSchema", create_dir_input_schema);

        json_object_array_add(tools_array, create_dir_tool);

        // --- move_file ツール定義 ---
        json_object *move_tool = json_object_new_object();
        json_object_object_add(move_tool, "name", json_object_new_string("move_file"));
        json_object_object_add(move_tool, "title", json_object_new_string("Move a file or directory"));
        json_object_object_add(move_tool, "description", json_object_new_string("Moves a file or directory from source to destination."));

        json_object *move_input_schema = json_object_new_object();
        json_object_object_add(move_input_schema, "$schema", json_object_new_string("https://json-schema.org/draft/2020-12/schema#"));
        json_object_object_add(move_input_schema, "type", json_object_new_string("object"));

        json_object *move_props = json_object_new_object();
        
        json_object *m_source = json_object_new_object();
        json_object_object_add(m_source, "type", json_object_new_string("string"));
        json_object_object_add(m_source, "description", json_object_new_string("Source path to move. " MCP_VIRTUAL_PATH_RULE));
        json_object_object_add(move_props, "source", m_source);

        json_object *m_dest = json_object_new_object();
        json_object_object_add(m_dest, "type", json_object_new_string("string"));
        json_object_object_add(m_dest, "description", json_object_new_string("Destination path to move to. " MCP_VIRTUAL_PATH_RULE));
        json_object_object_add(move_props, "destination", m_dest);

        json_object_object_add(move_input_schema, "properties", move_props);
        { const char *required[] = {"source", "destination", NULL}; add_required(move_input_schema, required); }
        json_object_object_add(move_tool, "inputSchema", move_input_schema);

        json_object_array_add(tools_array, move_tool);

        // --- copy_file ツール定義 ---
        json_object *copy_tool = json_object_new_object();
        json_object_object_add(copy_tool, "name", json_object_new_string("copy_file"));
        json_object_object_add(copy_tool, "title", json_object_new_string("Copy a file or directory"));
        json_object_object_add(copy_tool, "description", json_object_new_string("Copies a file from source to destination."));

        json_object *copy_input_schema = json_object_new_object();
        json_object_object_add(copy_input_schema, "$schema", json_object_new_string("https://json-schema.org/draft/2020-12/schema#"));
        json_object_object_add(copy_input_schema, "type", json_object_new_string("object"));

        json_object *copy_props = json_object_new_object();
        
        json_object *c_source = json_object_new_object();
        json_object_object_add(c_source, "type", json_object_new_string("string"));
        json_object_object_add(c_source, "description", json_object_new_string("Source path to copy. " MCP_VIRTUAL_PATH_RULE));
        json_object_object_add(copy_props, "source", c_source);

        json_object *c_dest = json_object_new_object();
        json_object_object_add(c_dest, "type", json_object_new_string("string"));
        json_object_object_add(c_dest, "description", json_object_new_string("Destination path to copy to. " MCP_VIRTUAL_PATH_RULE));
        json_object_object_add(copy_props, "destination", c_dest);

        json_object_object_add(copy_input_schema, "properties", copy_props);
        { const char *required[] = {"source", "destination", NULL}; add_required(copy_input_schema, required); }
        json_object_object_add(copy_tool, "inputSchema", copy_input_schema);

        json_object_array_add(tools_array, copy_tool);

        // --- edit_file ツール定義 ---
        json_object *edit_tool = json_object_new_object();
        json_object_object_add(edit_tool, "name", json_object_new_string("edit_file"));
        json_object_object_add(edit_tool, "title", json_object_new_string("Edit a file using diff+patch format"));
        json_object_object_add(edit_tool, "description", json_object_new_string("Edits a file using diff+patch format. Specify the location and content changes."));

        json_object *edit_input_schema = json_object_new_object();
        json_object_object_add(edit_input_schema, "$schema", json_object_new_string("https://json-schema.org/draft/2020-12/schema#"));
        json_object_object_add(edit_input_schema, "type", json_object_new_string("object"));

        json_object *edit_props = json_object_new_object();
        
        json_object *e_path = json_object_new_object();
        json_object_object_add(e_path, "type", json_object_new_string("string"));
        json_object_object_add(e_path, "description", json_object_new_string("Path to the target file. " MCP_VIRTUAL_PATH_RULE));
        json_object_object_add(edit_props, "path", e_path);

        json_object *e_start = json_object_new_object();
        json_object_object_add(e_start, "type", json_object_new_string("integer"));
        json_object_object_add(e_start, "description", json_object_new_string("Start line number (1-based index).The number of line breaks must match the total number of lines (start_line_number - end_line_number + 1)."));
        json_object_object_add(edit_props, "start_line", e_start);

        json_object *e_end = json_object_new_object();
        json_object_object_add(e_end, "type", json_object_new_string("integer"));
        json_object_object_add(e_end, "description", json_object_new_string("End line number (1-based index).The number of line breaks must match the total number of lines (start_line_number - end_line_number + 1)."));
        json_object_object_add(edit_props, "end_line", e_end);

        json_object *e_old = json_object_new_object();
        json_object_object_add(e_old, "type", json_object_new_string("string"));
        json_object_object_add(e_old, "description", json_object_new_string("Content before change (must match lines start_line to end_line). Required if old_content_hash is missing"));
        json_object_object_add(edit_props, "old_content", e_old);

        json_object *e_new = json_object_new_object();
        json_object_object_add(e_new, "type", json_object_new_string("string"));
        json_object_object_add(e_new, "description", json_object_new_string("Content after change"));
        json_object_object_add(edit_props, "new_content", e_new);

        // old_content_hash: オプションパラメータ（ハッシュ値による検証用）
        json_object *e_old_hash = json_object_new_object();
        json_object_object_add(e_old_hash, "type", json_object_new_string("string"));
        json_object_object_add(e_old_hash, "description", json_object_new_string("SHA-256 hash of old_content (format: sha256:<base64>). Required if `old_content` is empty. Used for content verification instead of text comparison."));
        json_object_object_add(edit_props, "old_content_hash", e_old_hash);

        json_object *e_trail_lf = json_object_new_object();
        json_object_object_add(e_trail_lf, "type", json_object_new_string("boolean"));
        json_object_object_add(e_trail_lf, "description", json_object_new_string("Include the target range's trailing newline when validating old_content_hash. Must match read_file; default: true. Ignored when validating with old_content."));
        json_object_object_add(edit_props, "include_trailing_newline", e_trail_lf);

        json_object_object_add(edit_input_schema, "properties", edit_props);
        { const char *required[] = {"path", "start_line", "end_line", "new_content", NULL}; add_required(edit_input_schema, required); }
        json_object_object_add(edit_tool, "inputSchema", edit_input_schema);

        json_object_array_add(tools_array, edit_tool);

        // --- search_string_in_file ツール定義 ---
        json_object *search_str_tool = json_object_new_object();
        json_object_object_add(search_str_tool, "name", json_object_new_string("search_string_in_file"));
        json_object_object_add(search_str_tool, "title", json_object_new_string("Search for a string in a file and return line numbers"));
        json_object_object_add(search_str_tool, "description", json_object_new_string("Searches for a specific string in a file and returns the start and end line numbers of each match. Useful for AI models to get exact line ranges when using read_file or edit_file."));

        json_object *search_str_input_schema = json_object_new_object();
        json_object_object_add(search_str_input_schema, "$schema", json_object_new_string("https://json-schema.org/draft/2020-12/schema#"));
        json_object_object_add(search_str_input_schema, "type", json_object_new_string("object"));

        json_object *search_str_props = json_object_new_object();
        
        json_object *ss_path = json_object_new_object();
        json_object_object_add(ss_path, "type", json_object_new_string("string"));
        json_object_object_add(ss_path, "description", json_object_new_string("Path to the file to search in. " MCP_VIRTUAL_PATH_RULE));
        json_object_object_add(search_str_props, "path", ss_path);

        json_object *ss_search = json_object_new_object();
        json_object_object_add(ss_search, "type", json_object_new_string("string"));
        json_object_object_add(ss_search, "description", json_object_new_string("String to search for (can include newlines)"));
        json_object_object_add(search_str_props, "search_string", ss_search);

        json_object_object_add(search_str_input_schema, "properties", search_str_props);
        { const char *required[] = {"path", "search_string", NULL}; add_required(search_str_input_schema, required); }
        json_object_object_add(search_str_tool, "inputSchema", search_str_input_schema);

        json_object_array_add(tools_array, search_str_tool);

        // --- search_files ツール定義 ---
        json_object *search_file_tool = json_object_new_object();
        json_object_object_add(search_file_tool, "name", json_object_new_string("search_files"));
        json_object_object_add(search_file_tool, "title", json_object_new_string("Search files in a directory (grep equivalent)"));
        json_object_object_add(search_file_tool, "description", json_object_new_string("Searches file names and file contents below a directory for a literal string. Put only the directory in path and the search text in pattern; this tool does not use glob syntax. Default depth is 3 (root is depth 0); recursive=false searches only the root. Dot-prefixed directories are excluded by default. Search stops at the configured search_timeout_seconds (default 25 seconds), returning incomplete results with retry guidance."));

        // inputSchema 作成
        json_object *search_file_input_schema = json_object_new_object();
        json_object_object_add(search_file_input_schema, "$schema", json_object_new_string("https://json-schema.org/draft/2020-12/schema#"));
        json_object_object_add(search_file_input_schema, "type", json_object_new_string("object"));

        // properties 作成
        json_object *search_file_props = json_object_new_object();

        // path (必須)
        json_object *sf_path = json_object_new_object();
        json_object_object_add(sf_path, "type", json_object_new_string("string"));
        json_object_object_add(sf_path, "description", json_object_new_string("Directory in which to start searching. Do not append a file name or pattern. " MCP_VIRTUAL_PATH_RULE));
        json_object_object_add(search_file_props, "path", sf_path);

        // pattern (必須)
        json_object *sf_pattern = json_object_new_object();
        json_object_object_add(sf_pattern, "type", json_object_new_string("string"));
        json_object_object_add(sf_pattern, "description", json_object_new_string("Literal search text used for file names and contents. For example, use 'test', not '*test*'. This is neither glob syntax nor a regular expression."));
        json_object_object_add(search_file_props, "pattern", sf_pattern);

        // recursive (任意)
        json_object *sf_recursive = json_object_new_object();
        json_object_object_add(sf_recursive, "type", json_object_new_string("boolean"));
        json_object_object_add(sf_recursive, "description", json_object_new_string("サブディレクトリも再帰的に検索するか。デフォルト: true。"));
        json_object_object_add(search_file_props, "recursive", sf_recursive);

        json_object *sf_depth = json_object_new_object();
        json_object_object_add(sf_depth, "type", json_object_new_string("integer"));
        json_object_object_add(sf_depth, "description", json_object_new_string("Maximum subdirectory depth, as in get_directory_tree. Default: 3 (capped by tree_max_depth). 0 searches only the selected directory. recursive=false overrides this option."));
        json_object_object_add(sf_depth, "default", json_object_new_int(g_tree_max_depth < DEFAULT_TREE_DEPTH ? g_tree_max_depth : DEFAULT_TREE_DEPTH));
        json_object_object_add(sf_depth, "minimum", json_object_new_int(0));
        json_object_object_add(sf_depth, "maximum", json_object_new_int(g_tree_max_depth));
        json_object_object_add(search_file_props, "depth", sf_depth);

        json_object *sf_hidden = json_object_new_object();
        json_object_object_add(sf_hidden, "type", json_object_new_string("boolean"));
        json_object_object_add(sf_hidden, "description", json_object_new_string("Search inside directories whose names start with '.' (such as .history). Default: false. An explicitly selected path remains searchable."));
        json_object_object_add(search_file_props, "include_hidden_directories", sf_hidden);

        // case_sensitive (任意)
        json_object *sf_case = json_object_new_object();
        json_object_object_add(sf_case, "type", json_object_new_string("boolean"));
        json_object_object_add(sf_case, "description", json_object_new_string("大文字小文字を区別するか。デフォルト: true。"));
        json_object_object_add(search_file_props, "case_sensitive", sf_case);

        // max_results (任意)
        json_object *sf_max = json_object_new_object();
        json_object_object_add(sf_max, "type", json_object_new_string("integer"));
        snprintf(configured_description, sizeof(configured_description),
                 "返却する最大マッチ行数。デフォルト: %d、上限: %d。",
                 g_search_max_results, g_search_max_results_limit);
        json_object_object_add(sf_max, "description", json_object_new_string(configured_description));
        json_object_object_add(sf_max, "minimum", json_object_new_int(1));
        json_object_object_add(sf_max, "maximum", json_object_new_int(g_search_max_results_limit));
        json_object_object_add(search_file_props, "max_results", sf_max);

        // required fields
        json_object *required_fields = json_object_new_array();
        json_object_array_add(required_fields, json_object_new_string("path"));
        json_object_array_add(required_fields, json_object_new_string("pattern"));
        json_object_object_add(search_file_input_schema, "required", required_fields);

        // properties を inputSchema に追加
        json_object_object_add(search_file_input_schema, "properties", search_file_props);
        { const char *required[] = {"path", "pattern", NULL}; add_required(search_file_input_schema, required); }
        // tool に inputSchema を設定
        json_object_object_add(search_file_tool, "inputSchema", search_file_input_schema);

        // tools_array に追加
        json_object_array_add(tools_array, search_file_tool);

        // --- tag_file ツール定義 ---
        json_object *tag_file_tool = json_object_new_object();
        json_object_object_add(tag_file_tool, "name", json_object_new_string("tag_file"));
        json_object_object_add(tag_file_tool, "title", json_object_new_string("Add tags to a file"));
        json_object_object_add(tag_file_tool, "description", json_object_new_string("Adds tags to a file for logical grouping and search."));

        // inputSchema 作成
        json_object *tag_file_input_schema = json_object_new_object();
        json_object_object_add(tag_file_input_schema, "$schema", json_object_new_string("https://json-schema.org/draft/2020-12/schema#"));
        json_object_object_add(tag_file_input_schema, "type", json_object_new_string("object"));

        // properties 作成
        json_object *tag_file_props = json_object_new_object();

        // path (必須)
        json_object *tf_path = json_object_new_object();
        json_object_object_add(tf_path, "type", json_object_new_string("string"));
        json_object_object_add(tf_path, "description", json_object_new_string("Path to the file to tag. " MCP_VIRTUAL_PATH_RULE));
        json_object_object_add(tag_file_props, "path", tf_path);

        // tags (必須)
        json_object *tf_tags = json_object_new_object();
        json_object_object_add(tf_tags, "type", json_object_new_string("array"));
        json_object_object_add(tf_tags, "description", json_object_new_string("Array of tags to add"));
        json_object *tf_tag_items = json_object_new_object();
        json_object_object_add(tf_tag_items, "type", json_object_new_string("string"));
        json_object_object_add(tf_tag_items, "pattern", json_object_new_string("^[A-Za-z0-9][A-Za-z0-9:_-]*$"));
        json_object_object_add(tf_tag_items, "maxLength", json_object_new_int(255));
        json_object_object_add(tf_tags, "items", tf_tag_items);
        json_object_object_add(tag_file_props, "tags", tf_tags);

        // mode (任意、デフォルト: "add")
        json_object *tf_mode = json_object_new_object();
        json_object_object_add(tf_mode, "type", json_object_new_string("string"));
        json_object_object_add(tf_mode, "description", json_object_new_string("Tag operation mode: \"add\", \"set\", or \"replace\". Default is \"add\"."));
        json_object *tf_mode_enum = json_object_new_array();
        json_object_array_add(tf_mode_enum, json_object_new_string("add"));
        json_object_array_add(tf_mode_enum, json_object_new_string("set"));
        json_object_array_add(tf_mode_enum, json_object_new_string("replace"));
        json_object_object_add(tf_mode, "enum", tf_mode_enum);
        json_object_object_add(tag_file_props, "mode", tf_mode);

        // required fields
        json_object *tf_required = json_object_new_array();
        json_object_array_add(tf_required, json_object_new_string("path"));
        json_object_array_add(tf_required, json_object_new_string("tags"));
        json_object_object_add(tag_file_input_schema, "required", tf_required);

        // properties を inputSchema に追加
        json_object_object_add(tag_file_input_schema, "properties", tag_file_props);
        // tool に inputSchema を設定
        json_object_object_add(tag_file_tool, "inputSchema", tag_file_input_schema);

        // tools_array に追加
        json_object_array_add(tools_array, tag_file_tool);
        add_extended_tag_tools(tools_array);

        // --- download_file ツール定義 ---
        json_object *download_tool = json_object_new_object();
        json_object_object_add(download_tool, "name", json_object_new_string("download_file"));
        json_object_object_add(download_tool, "title", json_object_new_string("Download file from URL"));
        json_object_object_add(download_tool, "description", json_object_new_string("Downloads a file from the specified URL and saves it to the local filesystem."));

        // inputSchema 作成
        json_object *download_input_schema = json_object_new_object();
        json_object_object_add(download_input_schema, "$schema", json_object_new_string("https://json-schema.org/draft/2020-12/schema#"));
        json_object_object_add(download_input_schema, "type", json_object_new_string("object"));

        // properties 作成
        json_object *download_props = json_object_new_object();

        // url (必須)
        json_object *dl_url = json_object_new_object();
        json_object_object_add(dl_url, "type", json_object_new_string("string"));
        json_object_object_add(dl_url, "description", json_object_new_string("Download URL"));
        json_object_object_add(download_props, "url", dl_url);

        // filename (必須)
        json_object *dl_filename = json_object_new_object();
        json_object_object_add(dl_filename, "type", json_object_new_string("string"));
        json_object_object_add(dl_filename, "description", json_object_new_string("Destination file path. " MCP_VIRTUAL_PATH_RULE));
        json_object_object_add(download_props, "filename", dl_filename);

        // timeout_seconds (任意、デフォルト30秒)
        json_object *dl_timeout = json_object_new_object();
        json_object_object_add(dl_timeout, "type", json_object_new_string("integer"));
        snprintf(configured_description, sizeof(configured_description),
                 "Download timeout in seconds. Default: %d, maximum: %d.",
                 g_download_timeout_seconds, g_max_download_timeout_seconds);
        json_object_object_add(dl_timeout, "description", json_object_new_string(configured_description));
        json_object_object_add(dl_timeout, "minimum", json_object_new_int(1));
        json_object_object_add(dl_timeout, "maximum", json_object_new_int(g_max_download_timeout_seconds));
        json_object_object_add(download_props, "timeout_seconds", dl_timeout);

        // required fields
        json_object *dl_required = json_object_new_array();
        json_object_array_add(dl_required, json_object_new_string("url"));
        json_object_array_add(dl_required, json_object_new_string("filename"));
        json_object_object_add(download_input_schema, "required", dl_required);

        // properties を inputSchema に追加
        json_object_object_add(download_input_schema, "properties", download_props);
        // tool に inputSchema を設定
        json_object_object_add(download_tool, "inputSchema", download_input_schema);

        // tools_array に追加
        json_object_array_add(tools_array, download_tool);
        
        // 最終的に tools キーでラップ
        json_object_object_add(result, "tools", tools_array);

        send_json_rpc_response(id, result);
        
    } else if (!strcmp(method, "tools/call")) {
        // ツール呼び出し
        if (!params || !json_object_is_type(params, json_type_object)) {
            send_json_rpc_error(id, -32602, "Invalid params: expected an object");
            return;
        }
        json_object *name_obj = json_object_object_get(params, "name");
        const char *tool_name = (name_obj && json_object_is_type(name_obj, json_type_string)) ? json_object_get_string(name_obj) : NULL;
        json_object *arguments = params ? json_object_object_get(params, "arguments") : NULL;
        
        json_object *result = NULL;
        
        if (!tool_name) {
            send_json_rpc_error(id, -32602, "Missing tool name in params");
            return;
        }
        int owns_arguments = 0;
        if (!arguments) {
            arguments = json_object_new_object();
            owns_arguments = 1;
        }
        if (!validate_tool_arguments(tool_name, arguments)) {
            if (arguments != json_object_object_get(params, "arguments")) json_object_put(arguments);
            send_json_rpc_error(id, -32602, "Invalid or missing tool arguments");
            return;
        }
        
        g_tool_call_active = 1;
        if (!strcmp(tool_name, "read_file")) {
            result = handle_read_file(arguments, id);
        } else if (!strcmp(tool_name, "stat_file")) {
            result = handle_stat_file(arguments, id);
        } else if (!strcmp(tool_name, "run_cmd")) {
            result = handle_run_cmd(arguments, id);
        } else if (!strcmp(tool_name, "write_file")) {
            result = handle_write_file(arguments, id);
        } else if (!strcmp(tool_name, "create_file")) {
            result = handle_create_file(arguments, id);
        } else if (!strcmp(tool_name, "edit_file")) {
            result = handle_edit_file(arguments, id);
        } else if (!strcmp(tool_name, "create_directory")) {
            result = handle_create_directory(arguments, id);
        } else if (!strcmp(tool_name, "move_file")) {
            result = handle_move_file(arguments, id);
        } else if (!strcmp(tool_name, "copy_file")) {
            result = handle_copy_file(arguments, id);
        } else if (!strcmp(tool_name, "delete_file")) {
            result = handle_delete_file(arguments, id);
        } else if (!strcmp(tool_name, "search_files")) {
            result = handle_search_files(arguments, id);
        } else if (!strcmp(tool_name, "tag_file")) {
            result = handle_tag_file(arguments, id);
        } else if (!strcmp(tool_name, "batch_tag_file")) {
            result = handle_batch_tag_file(arguments, id);
        } else if (!strcmp(tool_name, "get_tags")) {
            result = handle_get_tags(arguments, id);
        } else if (!strcmp(tool_name, "delete_tag")) {
            result = handle_delete_tag(arguments, id);
        } else if (!strcmp(tool_name, "download_file")) {
            result = handle_download_file(arguments, id);
        } else if (!strcmp(tool_name, "get_directory_tree")) {
            result = handle_get_directory_tree(arguments, id);
        } else if (!strcmp(tool_name, "search_string_in_file")) {
            result = handle_search_string_in_file(arguments, id);
        } else {
            g_tool_call_active = 0;
            send_json_rpc_error(id, -32602, "Unknown tool");
            if (arguments != json_object_object_get(params, "arguments")) json_object_put(arguments);
            return;
        }
        /* Handlers may emit a tool execution error through send_json_rpc_error. */
        g_tool_call_active = 0;
        if (result) {
            send_json_rpc_response(id, result);
        }
        if (owns_arguments) json_object_put(arguments);
        
    } else if (!strcmp(method, "resources/list")) {
        // リソース一覧
        json_object *result = json_object_new_object();
        json_object *resources_arr = json_object_new_array();
        
        json_object *resource_tree = json_object_new_object();
        json_object_object_add(resource_tree, "uri", json_object_new_string("tree://project"));
        json_object_object_add(resource_tree, "name", json_object_new_string("Project Directory Tree"));
        json_object_object_add(resource_tree, "description", json_object_new_string("Bounded project directory overview (depth 3)"));
        json_object_object_add(resource_tree, "mimeType", json_object_new_string("application/json"));
        
        json_object_array_add(resources_arr, resource_tree);
        json_object_object_add(result, "resources", resources_arr);
        
        send_json_rpc_response(id, result);
        
    } else if (!strcmp(method, "resources/read")) {
        // リソース読み取り
        json_object *uri_obj = params ? json_object_object_get(params, "uri") : NULL;
        const char *uri = json_object_is_type(uri_obj, json_type_string) ? json_object_get_string(uri_obj) : NULL;
        if (!uri || !*uri) {
            send_json_rpc_error(id, -32602, "Missing uri parameter");
            return;
        }

        json_object *result = json_object_new_object();
        json_object *contents_arr = json_object_new_array();
        json_object *content_item = json_object_new_object();
        json_object_object_add(content_item, "uri", json_object_new_string(uri));
        json_object_object_add(content_item, "mimeType", json_object_new_string("application/json"));
        
        if (!strcmp(uri, "tree://project")) {
            json_object *tree_params = json_object_new_object();
            json_object_object_add(tree_params, "path", json_object_new_string("/"));
            json_object_object_add(tree_params, "depth", json_object_new_int(3));
            json_object *tree_res = handle_get_directory_tree(tree_params, id);
            json_object_put(tree_params);
            if (!tree_res) {
                json_object_put(result);
                json_object_put(contents_arr);
                json_object_put(content_item);
                return;
            }
            json_object *tree_content = json_object_object_get(tree_res, "content");
            json_object *first = tree_content ? json_object_array_get_idx(tree_content, 0) : NULL;
            const char *text = first ? json_object_get_string(json_object_object_get(first, "text")) : "";
            json_object_object_add(content_item, "text", json_object_new_string(text ? text : ""));
            json_object_put(tree_res);
        } else {
            json_object_put(result);
            json_object_put(contents_arr);
            json_object_put(content_item);
            send_json_rpc_error(id, -32602, "Resource not found");
            return;
        }
        
        json_object_array_add(contents_arr, content_item);
        json_object_object_add(result, "contents", contents_arr);
        send_json_rpc_response(id, result);
        
    } else if (!strcmp(method, "prompts/list")) {
        json_object *result = json_object_new_object();
        json_object_object_add(result, "prompts", json_object_new_array());
        send_json_rpc_response(id, result);
        
    } else {
        if (id != NULL) {
            send_json_rpc_error(id, -32601, "Unknown method");
        }
    }
}
