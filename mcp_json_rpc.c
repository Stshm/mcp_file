#include "mcp_common.h"

// JSON-RPC エラー構造体
typedef struct {
    int code;
    char *message;
    void *data;
} JsonRpcError;

// JSON-RPC メッセージ構造体
typedef struct {
    char *jsonrpc;
    char *method;
    json_object *params;
    char *id;
} JsonRpcRequest;

typedef struct {
    char *jsonrpc;
    char *id;
    json_object *result;
    JsonRpcError *error;
} JsonRpcResponse;

// JSON-RPC レスポンス生成
void send_json_rpc_response(json_object *id, json_object *result) {
    json_object *response = json_object_new_object();
    json_object_object_add(response, "jsonrpc", json_object_new_string("2.0"));
    
    // idがnullの場合の対応
    if (id != NULL) {
        json_object_object_add(response, "id", json_object_get(id));
    } else {
        json_object_object_add(response, "id", json_object_new_null());
    }
    
    json_object_object_add(response, "result", result);
    
    const char *str = json_object_to_json_string_ext(response, JSON_C_TO_STRING_PLAIN);
    printf("%s\n", str);
    fflush(stdout);
    json_object_put(response);
}

void send_json_rpc_error(json_object *id, int code, const char *message) {
    // idがNULL（通知）の場合はレスポンスを返さない
    if (id == NULL) {
        return;
    }

    if (g_tool_call_active) {
        json_object *tool_result = json_object_new_object();
        json_object *content = json_object_new_array();
        json_object *item = json_object_new_object();
        json_object_object_add(item, "type", json_object_new_string("text"));
        json_object_object_add(item, "text", json_object_new_string(message ? message : "Tool execution failed"));
        json_object_array_add(content, item);
        json_object_object_add(tool_result, "content", content);
        json_object_object_add(tool_result, "isError", json_object_new_boolean(true));
        send_json_rpc_response(id, tool_result);
        return;
    }

    json_object *response = json_object_new_object();
    json_object_object_add(response, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(response, "id", json_object_get(id));
    
    json_object *error_obj = json_object_new_object();
    json_object_object_add(error_obj, "code", json_object_new_int(code));
    json_object_object_add(error_obj, "message", json_object_new_string(message));
    json_object_object_add(response, "error", error_obj);
    
    const char *str = json_object_to_json_string_ext(response, JSON_C_TO_STRING_PLAIN);
    printf("%s\n", str);
    fflush(stdout);
    json_object_put(response);
}

void handle_error(json_object *id, int err){
    if( err == -32602) {
        send_json_rpc_error(id, err, "Invalid parameters: path is required");
    }else if (err == -32603){
        send_json_rpc_error(id, err, "File not found or invalid path");
    }    
}
