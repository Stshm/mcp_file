#include "mcp_common.h"


// 右回転 (Rotate Right)
#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

// SHA-256 特有の論理関数
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

// 64個の初期化定数 (K)
static const uint32_t k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

void _sha256_fips(const uint8_t *data, size_t length, uint8_t hash[32]) {
    // ハッシュの初期値 (H)
    uint32_t h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
    uint32_t h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;

    // パディング処理の計算
    uint64_t bit_len = length * 8;
    size_t pad_len = (length % 64 < 56) ? (64 - (length % 64)) : (128 - (length % 64));
    size_t total_len = length + pad_len;

    uint8_t *padded = calloc(total_len, 1);
    memcpy(padded, data, length);
    padded[length] = 0x80; // 最初のパディングビット '1' を立てる

    // 末尾の8バイトに元のビット長をビッグエンディアンで格納
    for (int i = 0; i < 8; i++) {
        padded[total_len - 1 - i] = (bit_len >> (i * 8)) & 0xFF;
    }

    // 512ビット（64バイト）のブロックごとに処理
    for (size_t chunk = 0; chunk < total_len; chunk += 64) {
        uint32_t w[64];
        uint8_t *p = padded + chunk;

        // メッセージスケジュールの初期化 (最初の16個)
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | 
                   ((uint32_t)p[i*4+2] << 8)  | ((uint32_t)p[i*4+3]);
        }
        // 残り48個を拡張
        for (int i = 16; i < 64; i++) {
            w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];
        }

        // ワーキング変数の初期化
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;

        // メインループ (64ラウンド)
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = h + EP1(e) + CH(e, f, g) + k[i] + w[i];
            uint32_t t2 = EP0(a) + MAJ(a, b, c);
            h = g; g = f; f = e;
            e = d + t1;
            d = c; c = b; b = a;
            a = t1 + t2;
        }

        // ハッシュ値の更新
        h0 += a; h1 += b; h2 += c; h3 += d;
        h4 += e; h5 += f; h6 += g; h7 += h;
    }

    free(padded);

    // 出力バッファへビッグエンディアンで書き出し
    uint32_t states[8] = {h0, h1, h2, h3, h4, h5, h6, h7};
    for (int i = 0; i < 8; i++) {
        hash[i*4]   = (states[i] >> 24) & 0xFF;
        hash[i*4+1] = (states[i] >> 16) & 0xFF;
        hash[i*4+2] = (states[i] >> 8)  & 0xFF;
        hash[i*4+3] = states[i]         & 0xFF;
    }
}

const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Base64デコード用変換テーブル
const int kBase64Inv[256] = {
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,62, -1,-1,-1,63, // + (62), / (63)
    52,53,54,55, 56,57,58,59, 60,61,-1,-1, -1,-1,-1,-1, // 0-9
    -1, 0, 1, 2,  3, 4, 5, 6,  7, 8, 9,10, 11,12,13,14, // A-O
    15,16,17,18, 19,20,21,22, 23,24,25,-1, -1,-1,-1,-1, // P-Z
    -1,26,27,28, 29,30,31,32, 33,34,35,36, 37,38,39,40, // a-o
    41,42,43,44, 45,46,47,48, 49,50,51,-1, -1,-1,-1,-1, // p-z
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1
};

// Base64 エンコード関数
void base64_encode(const char *input, size_t input_len, char *output) {
    const unsigned char *src = (const unsigned char *)input;
    size_t i = 0, j = 0;
    while (i < input_len) {
        size_t remaining = input_len - i;
        uint32_t octet_a = src[i++];
        uint32_t octet_b = (remaining > 1) ? src[i++] : 0;
        uint32_t octet_c = (remaining > 2) ? src[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        output[j++] = base64_table[(triple >> 18) & 0x3F];
        output[j++] = base64_table[(triple >> 12) & 0x3F];
        output[j++] = (remaining > 1) ? base64_table[(triple >> 6) & 0x3F] : '=';
        output[j++] = (remaining > 2) ? base64_table[triple & 0x3F] : '=';
    }
    output[j] = '\0';
}

// Base64デコード関数
unsigned char* base64_decode(const char *src, size_t *out_len) {
    if (!src || !out_len) return NULL;
    size_t len = strlen(src);
    if (len == 0) {
        *out_len = 0;
        unsigned char *empty = malloc(1);
        if (empty) *empty = '\0';
        return empty;
    }
    if (len % 4 != 0) return NULL;

    size_t pad = 0;
    if (src[len - 1] == '=') pad++;
    if (src[len - 2] == '=') pad++;
    // 末尾以外に '=' がある、または '=' が3個以上の場合はエラー
    if (pad == 1 && src[len - 2] == '=') return NULL;

    *out_len = (len / 4) * 3 - pad;
    if (*out_len == SIZE_MAX) return NULL;
    unsigned char *decoded = malloc(*out_len + 1);
    if (!decoded) return NULL;

    size_t i = 0, j = 0;
    while (i < len) {
        int a = kBase64Inv[(unsigned char)src[i]];
        int b = kBase64Inv[(unsigned char)src[i+1]];
        int c = kBase64Inv[(unsigned char)src[i+2]];
        int d = kBase64Inv[(unsigned char)src[i+3]];

        int is_last_chunk = (i + 4 == len);

        if (a < 0 || b < 0) {
            free(decoded);
            return NULL;
        }

        // パディング位置の厳密チェック
        if (src[i+2] == '=') {
            if (!is_last_chunk || src[i+3] != '=') {
                free(decoded);
                return NULL;
            }
        } else if (c < 0) {
            free(decoded);
            return NULL;
        }

        if (src[i+3] == '=') {
            if (!is_last_chunk) {
                free(decoded);
                return NULL;
            }
        } else if (d < 0) {
            free(decoded);
            return NULL;
        }

        uint32_t val_c = (c >= 0) ? (uint32_t)c : 0;
        uint32_t val_d = (d >= 0) ? (uint32_t)d : 0;
        uint32_t triple = ((uint32_t)a << 18) | ((uint32_t)b << 12) | (val_c << 6) | val_d;

        if (j < *out_len) decoded[j++] = (triple >> 16) & 0xFF;
        if (j < *out_len) decoded[j++] = (triple >> 8) & 0xFF;
        if (j < *out_len) decoded[j++] = triple & 0xFF;

        i += 4;
    }
    decoded[*out_len] = '\0';
    return decoded;
}

int normalize_path(char *out_abs_path, int out_len, char *in_path) {
    if (!out_abs_path || out_len <= 0 || !in_path || !*in_path) return -32602;
    const char *root = g_root_dir[0] ? g_root_dir : "/";
    return mcp_path_resolve(root, in_path, 0, out_abs_path,
                            (size_t)out_len) == 0 ? 0 :
           (errno == EINVAL || errno == EACCES || errno == ENAMETOOLONG
                ? -32602 : -32603);
}

int is_valid_utf8(const unsigned char *data, size_t len) {
    if (!data && len != 0) return 0;

    for (size_t i = 0; i < len;) {
        unsigned char c = data[i];
        if (c <= 0x7f) {
            i++;
            continue;
        }

        size_t continuation_count;
        uint32_t codepoint;
        if (c >= 0xc2 && c <= 0xdf) {
            continuation_count = 1;
            codepoint = c & 0x1f;
        } else if (c >= 0xe0 && c <= 0xef) {
            continuation_count = 2;
            codepoint = c & 0x0f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            continuation_count = 3;
            codepoint = c & 0x07;
        } else {
            return 0;
        }

        if (continuation_count > len - i - 1) return 0;
        for (size_t j = 1; j <= continuation_count; j++) {
            unsigned char next = data[i + j];
            if ((next & 0xc0) != 0x80) return 0;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }

        if ((continuation_count == 2 && codepoint < 0x800) ||
            (continuation_count == 3 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            codepoint > 0x10ffff) {
            return 0;
        }
        i += continuation_count + 1;
    }
    return 1;
}

int normalize_new_path(char *out_abs_path, int out_len, const char *in_path) {
    if (!out_abs_path || out_len <= 0 || !in_path || !*in_path) return -32602;
    const char *root = g_root_dir[0] ? g_root_dir : "/";
    return mcp_path_resolve(root, in_path, 1, out_abs_path,
                            (size_t)out_len) == 0 ? 0 :
           (errno == EINVAL || errno == EACCES || errno == ENAMETOOLONG
                ? -32602 : -32603);
}

/* Resolve a client-visible path below the configured MCP root.
 *
 * Normally the server has already pivoted g_root_dir to "/" before tools are
 * called.  Keeping the root explicit here also makes configured internal paths
 * work correctly before/without the pivot (for example in tests), and the
 * containment check prevents a parent symlink from escaping the MCP root.
 * The final component may be absent so callers can use this for new files.
 */
int resolve_mcp_path(char *out_abs_path, int out_len, const char *virtual_path) {
    if (!out_abs_path || out_len <= 0 || !virtual_path || !*virtual_path ||
        !g_root_dir[0]) return -32602;

    return mcp_path_resolve(g_root_dir, virtual_path, 1, out_abs_path,
                            (size_t)out_len) == 0 ? 0 :
           (errno == EINVAL || errno == EACCES || errno == ENAMETOOLONG
                ? -32602 : -32603);
}

int virtualize_mcp_path(char *out_path, int out_len, const char *native_path) {
    if (!out_path || out_len <= 0 || !native_path || !g_root_dir[0]) return -1;
    return mcp_path_to_virtual(g_root_dir, native_path, out_path,
                               (size_t)out_len);
}

static int is_valid_config_virtual_path(const char *value) {
    if (!value || value[0] != '/' || value[1] == '\0' ||
        strlen(value) >= MAX_PATH_LENGTH || strchr(value, '\\') ||
        strchr(value, ':')) {
        return 0;
    }

    for (const char *part = value + 1; part; ) {
        const char *slash = strchr(part, '/');
        size_t part_len = slash ? (size_t)(slash - part) : strlen(part);
        if (part_len == 0 || (part_len == 1 && part[0] == '.') ||
            (part_len == 2 && part[0] == '.' && part[1] == '.')) {
            return 0;
        }
        part = slash ? slash + 1 : NULL;
    }
    return 1;
}

int load_server_config(const char *config_path) {
    g_max_read_file_size = DEFAULT_MAX_READ_FILE_SIZE;
    g_max_download_file_size = DEFAULT_MAX_DOWNLOAD_FILE_SIZE;
    g_download_timeout_seconds = DEFAULT_DOWNLOAD_TIMEOUT_SECONDS;
    g_max_download_timeout_seconds = DEFAULT_MAX_DOWNLOAD_TIMEOUT_SECONDS;
    g_search_timeout_seconds = DEFAULT_SEARCH_TIMEOUT_SECONDS;
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
    snprintf(g_tag_file_path, sizeof(g_tag_file_path), "/%s", TAGS_FILE);
    if (!config_path || !*config_path) return 0;
    FILE *fp = fopen(config_path, "rb");
    if (!fp) {
        if (errno != ENOENT) fprintf(stderr, "Warning: cannot open %s: %s\n", config_path, strerror(errno));
        return 0;
    }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    long size = ftell(fp);
    if (size < 0 || size > MAX_BUFFER_SIZE || fseek(fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "Warning: invalid or oversized %s; using defaults\n", config_path);
        fclose(fp);
        return -1;
    }
    char *data = malloc((size_t)size + 1);
    if (!data) { fclose(fp); return -1; }
    size_t got = fread(data, 1, (size_t)size, fp);
    int read_failed = got != (size_t)size || ferror(fp);
    fclose(fp);
    data[got] = '\0';
    if (read_failed) { free(data); return -1; }

    json_object *config = json_tokener_parse(data);
    free(data);
    if (!json_object_is_type(config, json_type_object)) {
        if (config) json_object_put(config);
        fprintf(stderr, "Warning: invalid %s; using defaults\n", config_path);
        return -1;
    }
    json_object *limit = json_object_object_get(config, "max_read_file_size_bytes");
    if (limit) {
        int64_t value = json_object_is_type(limit, json_type_int) ? json_object_get_int64(limit) : -1;
        if (value <= 0) {
            fprintf(stderr, "Warning: max_read_file_size_bytes must be a positive integer; using default\n");
        } else {
            g_max_read_file_size = (uint64_t)value;
        }
    }
    struct {
        const char *name;
        uint64_t *target;
        uint64_t maximum;
    } byte_settings[] = {
        {"max_download_file_size_bytes", &g_max_download_file_size, (uint64_t)LLONG_MAX},
    };
    for (size_t i = 0; i < sizeof(byte_settings) / sizeof(byte_settings[0]); i++) {
        json_object *item = json_object_object_get(config, byte_settings[i].name);
        if (!item) continue;
        int64_t value = json_object_is_type(item, json_type_int) ? json_object_get_int64(item) : -1;
        if (value <= 0 || (uint64_t)value > byte_settings[i].maximum) {
            fprintf(stderr, "Warning: %s must be a positive integer; using default\n", byte_settings[i].name);
        } else {
            *byte_settings[i].target = (uint64_t)value;
        }
    }
    struct {
        const char *name;
        int *target;
    } int_settings[] = {
        {"download_timeout_seconds", &g_download_timeout_seconds},
        {"max_download_timeout_seconds", &g_max_download_timeout_seconds},
        {"search_timeout_seconds", &g_search_timeout_seconds},
        {"search_max_results", &g_search_max_results},
        {"search_max_results_limit", &g_search_max_results_limit},
        {"tree_default_depth", &g_tree_default_depth},
        {"tree_max_depth", &g_tree_max_depth},
        {"tree_max_entries", &g_tree_max_entries},
        {"run_cmd_timeout_seconds", &g_run_cmd_timeout_seconds},
        {"max_run_cmd_timeout_seconds", &g_max_run_cmd_timeout_seconds},
    };
    for (size_t i = 0; i < sizeof(int_settings) / sizeof(int_settings[0]); i++) {
        json_object *item = json_object_object_get(config, int_settings[i].name);
        if (!item) continue;
        int64_t value = json_object_is_type(item, json_type_int) ? json_object_get_int64(item) : -1;
        int zero_allowed = int_settings[i].target == &g_tree_default_depth;
        if (value < (zero_allowed ? 0 : 1) || value > INT_MAX) {
            fprintf(stderr, "Warning: %s is outside its valid integer range; using default\n", int_settings[i].name);
        } else {
            *int_settings[i].target = (int)value;
        }
    }
    struct {
        const char *name;
        size_t *target;
    } size_settings[] = {
        {"search_max_output_size_bytes", &g_search_max_output_size},
        {"tree_max_output_size_bytes", &g_tree_max_output_size},
        {"max_run_cmd_output_size_bytes", &g_max_run_cmd_output_size},
    };
    for (size_t i = 0; i < sizeof(size_settings) / sizeof(size_settings[0]); i++) {
        json_object *item = json_object_object_get(config, size_settings[i].name);
        if (!item) continue;
        int64_t value = json_object_is_type(item, json_type_int) ? json_object_get_int64(item) : -1;
        int exceeds_run_cmd_limit = size_settings[i].target == &g_max_run_cmd_output_size &&
                                    value > INT_MAX;
        if (value < 1024 || (uint64_t)value > SIZE_MAX || exceeds_run_cmd_limit) {
            fprintf(stderr, "Warning: %s must be an integer of at least 1024; using default\n", size_settings[i].name);
        } else {
            *size_settings[i].target = (size_t)value;
        }
    }
    json_object *tag_file = json_object_object_get(config, "tag_file_path");
    if (tag_file) {
        const char *value = json_object_is_type(tag_file, json_type_string)
            ? json_object_get_string(tag_file) : NULL;
        int valid = value && *value && value[0] != '/' && strlen(value) < MAX_PATH_LENGTH - 1;
        for (const char *part = value; valid && part; ) {
            const char *slash = strchr(part, '/');
            size_t part_len = slash ? (size_t)(slash - part) : strlen(part);
            if (part_len == 0 || (part_len == 1 && part[0] == '.') ||
                (part_len == 2 && part[0] == '.' && part[1] == '.')) {
                valid = 0;
                break;
            }
            part = slash ? slash + 1 : NULL;
        }
        if (!valid) {
            fprintf(stderr, "Warning: tag_file_path must be a non-empty path relative to the MCP root; using default\n");
        } else {
            snprintf(g_tag_file_path, sizeof(g_tag_file_path), "/%s", value);
        }
    }
    json_object *ca_bundle = json_object_object_get(config, "ca_bundle_path");
    if (ca_bundle) {
        const char *value = json_object_is_type(ca_bundle, json_type_string)
            ? json_object_get_string(ca_bundle) : NULL;
        if (!is_valid_config_virtual_path(value)) {
            fprintf(stderr, "Warning: ca_bundle_path must be an absolute MCP path without dot segments; ignoring it\n");
        } else {
            snprintf(g_ca_bundle_path, sizeof(g_ca_bundle_path), "%s", value);
        }
    }
    if (g_download_timeout_seconds > g_max_download_timeout_seconds) {
        fprintf(stderr, "Warning: download_timeout_seconds exceeds its maximum; clamping it\n");
        g_download_timeout_seconds = g_max_download_timeout_seconds;
    }
    if (g_search_max_results > g_search_max_results_limit) {
        fprintf(stderr, "Warning: search_max_results exceeds its limit; clamping it\n");
        g_search_max_results = g_search_max_results_limit;
    }
    if (g_tree_default_depth > g_tree_max_depth) {
        fprintf(stderr, "Warning: tree_default_depth exceeds tree_max_depth; clamping it\n");
        g_tree_default_depth = g_tree_max_depth;
    }
    if (g_run_cmd_timeout_seconds > g_max_run_cmd_timeout_seconds) {
        fprintf(stderr, "Warning: run_cmd_timeout_seconds exceeds its maximum; clamping it\n");
        g_run_cmd_timeout_seconds = g_max_run_cmd_timeout_seconds;
    }
    json_object_put(config);
    return 0;
}

int atomic_write_file(const char *path, const void *data, size_t len) {
    if (!path || (!data && len != 0)) { errno = EINVAL; return -1; }
    char dir[MAX_PATH_LENGTH];
    int written = snprintf(dir, sizeof(dir), "%s", path);
    if (written < 0 || (size_t)written >= sizeof(dir)) { errno = ENAMETOOLONG; return -1; }
    char *slash = strrchr(dir, '/');
    if (!slash) snprintf(dir, sizeof(dir), ".");
    else if (slash == dir) slash[1] = '\0';
    else *slash = '\0';

    char temp[MAX_PATH_LENGTH];
    written = snprintf(temp, sizeof(temp), "%s/.mcp-write-XXXXXX", dir);
    if (written < 0 || (size_t)written >= sizeof(temp)) { errno = ENAMETOOLONG; return -1; }
    int fd = mkstemp(temp);
    if (fd < 0) return -1;

    struct stat st;
    if (mcp_file_stat(path, &st) == 0) {
        if (mcp_file_set_descriptor_mode(fd, st.st_mode & 07777) != 0) { close(fd); unlink(temp); return -1; }
    } else {
        mode_t mask = umask(0);
        umask(mask);
        if (mcp_file_set_descriptor_mode(fd, 0666 & ~mask) != 0) { close(fd); unlink(temp); return -1; }
    }

    const unsigned char *p = data;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd); unlink(temp); return -1;
        }
        p += (size_t)n;
        remaining -= (size_t)n;
    }
    int failed = fsync(fd) != 0;
    if (close(fd) != 0) failed = 1;
    if (failed) { unlink(temp); return -1; }
    if (mcp_file_replace(temp, path) != 0) { unlink(temp); return -1; }

    int dir_fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) { (void)fsync(dir_fd); close(dir_fd); }
    return 0;
}

int atomic_copy_file(const char *source, const char *destination) {
    struct stat source_stat;
    if (mcp_file_stat(source, &source_stat) != 0 || !S_ISREG(source_stat.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    if (access(destination, F_OK) == 0) { errno = EEXIST; return -1; }

    char dir[MAX_PATH_LENGTH];
    int written = snprintf(dir, sizeof(dir), "%s", destination);
    if (written < 0 || (size_t)written >= sizeof(dir)) { errno = ENAMETOOLONG; return -1; }
    char *slash = strrchr(dir, '/');
    if (!slash) snprintf(dir, sizeof(dir), ".");
    else if (slash == dir) slash[1] = '\0';
    else *slash = '\0';
    char temp[MAX_PATH_LENGTH];
    written = snprintf(temp, sizeof(temp), "%s/.mcp-copy-XXXXXX", dir);
    if (written < 0 || (size_t)written >= sizeof(temp)) { errno = ENAMETOOLONG; return -1; }

    int in_fd = open(source, O_RDONLY);
    if (in_fd < 0) return -1;
    int out_fd = mkstemp(temp);
    if (out_fd < 0) { close(in_fd); return -1; }
    int failed = mcp_file_set_descriptor_mode(out_fd, source_stat.st_mode & 07777) != 0;
    unsigned char buffer[64 * 1024];
    while (!failed) {
        ssize_t got = read(in_fd, buffer, sizeof(buffer));
        if (got == 0) break;
        if (got < 0) { if (errno == EINTR) continue; failed = 1; break; }
        size_t offset = 0;
        while (offset < (size_t)got) {
            ssize_t put = write(out_fd, buffer + offset, (size_t)got - offset);
            if (put < 0) { if (errno == EINTR) continue; failed = 1; break; }
            offset += (size_t)put;
        }
    }
    if (close(in_fd) != 0) failed = 1;
    if (!failed && fsync(out_fd) != 0) failed = 1;
    if (close(out_fd) != 0) failed = 1;
    if (!failed && mcp_file_publish_exclusive(temp, destination) != 0) failed = 1;
    if (failed) { unlink(temp); return -1; }
    (void)unlink(temp);
    int dir_fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) { (void)fsync(dir_fd); close(dir_fd); }
    return 0;
}

FileType determine_file_type(const char *filename, const char *filepath) {
    // 1. 既知のテキストファイル名のホワイトリストチェック
    if (strcmp(filename, "Makefile") == 0 || 
        strcmp(filename, "README") == 0 || 
        strstr(filename, "Kconfig") != NULL) {
        return FILE_TYPE_TEXT;
    }

    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        return FILE_TYPE_BINARY; // 開けない場合は安全のためバイナリ扱い
    }

    unsigned char buffer[CHECK_BUFFER_SIZE];
    size_t bytes_read = fread(buffer, 1, sizeof(buffer), fp);
    fclose(fp);

    if (bytes_read == 0) {
        return FILE_TYPE_TEXT; // 空ファイルはテキスト扱い
    }

    // 2. マジックナンバーによる実行ファイルの即時判定
    if (bytes_read >= 4) {
        // Linux 実行ファイル (ELF)
        if (memcmp(buffer, "\x7f\x45\x4c\x46", 4) == 0) return FILE_TYPE_BINARY;
        // Windows 実行ファイル (PE/EXE)
        if (memcmp(buffer, "MZ", 2) == 0) return FILE_TYPE_BINARY;
        // macOS 実行ファイル (Mach-O)
        if (memcmp(buffer, "\xfe\xed\xfa\xce", 4) == 0 || 
            memcmp(buffer, "\xce\xfa\xed\xfe", 4) == 0 ||
            memcmp(buffer, "\xfe\xed\xfa\xcf", 4) == 0 || 
            memcmp(buffer, "\xcf\xfa\xed\xfe", 4) == 0) {
            return FILE_TYPE_BINARY;
        }
    }

    // 3. ヌルバイト(制御文字)スキャン（Gitやfileコマンドでも使われる手法）
    // UTF-8やASCIIでは、通常のテキストに \0 が含まれることは絶対にありません
    for (size_t i = 0; i < bytes_read; i++) {
        if (buffer[i] == '\0') {
            return FILE_TYPE_BINARY; // \0 が見つかったら確実にバイナリ
        }
    }

    return FILE_TYPE_TEXT;
}

void add_text_first_line_fprintf(FILE *fp, const char *data, const char *add_text) {

    const char *p = data;
    fputs(add_text, fp);
    while(1) {  
        const char *line_end = strstr(p,"\n"); 
        if(line_end == NULL) {
            fprintf(fp,"%s\n",p);
            break;
        }
        int  size = line_end - p + 1;
        fwrite(p,sizeof(char), size, fp);
        fputs(add_text, fp);
        p += size;
    } 

}

char *extract_text_by_line_range(const char *in_text, size_t *out_len, int start_line, int end_line, int flag_last_lf){
    *out_len = 0;
    if (!in_text || start_line < 1 || end_line < start_line) return NULL;

    const char *start = in_text;
    int line = 1;
    while (line < start_line && *start) {
        const char *newline = strchr(start, '\n');
        if (!newline) return NULL;
        start = newline + 1;
        line++;
    }
    if (line != start_line || !*start) return NULL;

    const char *end = start;
    while (line <= end_line && *end) {
        const char *newline = strchr(end, '\n');
        if (!newline) {
            end += strlen(end);
            break;
        }
        end = newline + 1;
        line++;
    }
    if (flag_last_lf == EOB_LINE_BREAK_EXCLUDE && end > start && end[-1] == '\n') end--;
    *out_len = (size_t)(end - start);
    return (char *)start;
}


// Removes /r from the string. The buffer is updated to the string with /r removed.
void condense_crlf2lf(char* buf, size_t len){
	while(1){
		char *p = strstr(buf, "\r\n");
		if(p)	memmove(p, p + 1, len - (p - buf)); //Including the terminating character
		else	break;
	}
}
