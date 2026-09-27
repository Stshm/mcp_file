# MCP サーバ（mcp_file / mcp_lib）コードレビュー

レビュー対象: /home/pub/workai/sandbox/devel/ 配下
  - mcp_file/: mcp_main.c, mcp_server.c, mcp_json_rpc.c, mcp_utils.c,
               mcp_tools_file.c, mcp_tools_dir.c, mcp_tools_misc.c, mcp_common.h
  - mcp_lib/:  mcp_sandbox.c, mcp_transport.c, mcp_core.h
日付: 2026-08-26（行番号・検証結果は 2026-08-28 付のソース現状で再確認・改訂）
検証: make test 10/10 合格 / ASan ハーネスで H-1 実証（2026-08-28 再実行）/
      スモークテスト (initialize, tools/list, tools/call) 正常

## 0. 設計概観と評価

- stdin/stdout 経由の NDJSON (new-line-delimited JSON) JSON-RPC 2.0。
  mcp_transport.c:37-38 が getline で 1 行 1 メッセージ読み込み、
  mcp_json_rpc.c:39-41, 73-75 がレスポンスを 1 行で出力。
  MCP の stdio transport 仕様に準拠しており、実装は整合している。
- mcp_main.c:134 で mcp_sandbox_enter() により user/mount namespace + pivot_root
  でルートディレクトリを「/」に見せかけたサンドボックス化を行う
  (mcp_sandbox.c:38-101)。ツール実装がすべて絶対パス "/" 前提で動くため、
  実装が簡潔になる意図的な設計。
- ツール 16 種（read_file, write_file, create_file, edit_file, delete_file,
  copy_file, move_file, get_directory_tree, create_directory, search_files,
  search_string_in_file, tag_file, get_tags, batch_tag_file, delete_tag,
  download_file）。
- 全体として防御的コーディング（snprintf 長さチェック、malloc チェック、
  EINTR 対処、原子書き込み mcp_utils.c:446-467）が徹底されており品質は高い。
  以下は残りの指摘事項。

## 1. 重大 (High)

### H-1: append_search_result の size_t 下位オーバーフローで
      バッファオーバーライト（ASan で実証済み）
ファイル: mcp_file/mcp_tools_misc.c:6-20
  static int append_search_result(char *result_text, size_t capacity,
                                 size_t *text_len, const char *record)
  {
      size_t record_len = strlen(record);
      size_t marker_len = sizeof(SEARCH_TRUNCATION_MARKER) - 1;  // = 40

      if (*text_len >= capacity || marker_len >= capacity ||
          record_len > capacity - 1 - marker_len - *text_len) {  // 行11
          memcpy(result_text + *text_len, SEARCH_TRUNCATION_MARKER, marker_len + 1);
          *text_len += marker_len;
          return 0;
      }
      memcpy(result_text + *text_len, record, record_len + 1);   // 行17
      ...
  }
問題:
  行11 の `capacity - 1 - marker_len - *text_len` は size_t の減算であり、
  `1 + marker_len + *text_len > capacity` のときに 0 を下回って SIZE_MAX 側に
  ラップする。このときガード条件 `record_len > SIZE_MAX...` が偽となり、
  行17 の memcpy(record_len+1 バイト) が capacity を超えて書き込まれる。
根拠（ASan ハーネス /tmp/mcp_review/harness.c、2026-08-28 再実行で再現）:
  capacity=100, marker_len=40, text_len=60, record_len=49 のとき
  ガード値 = 100-1-40-60 = -1 (SIZE_MAX) → record 経路で memcpy 50 バイト
  がオフセット 60 から書き込まれ、末尾 10 バイトが overflow。
  ASan 出力（実測）:
    ==PID==ERROR: AddressSanitizer: stack-buffer-overflow
    WRITE of size 50 ... in append_search_result (harness.c:19)
    [176, 276) 'buf' <== Memory access at offset 276 overflows this variable
リーチ可能性（重要）:
  実コードでは capacity は g_search_max_output_size（既定 1MiB、
  mcp_common.h:35。設定値は最低 1024 バイト、mcp_utils.c:378, 385-395 の
  size_settings 検証）であり、marker_len=40 が固定的なため、overflow は
  `text_len > capacity - 41` の領域で、かつ record が残量に収まらない
  タイミング、つまり「出力上限の直前でちょうど切り詰めが起きた瞬間」に
  発生する。呼び出し元は search_files (mcp_tools_misc.c:84, 140, 151) と
  search_string_in_file (mcp_tools_misc.c:1198, 1204, 1177) の 2 系統。
  1MiB 上限の検索でトリuncate 発生時のみという確率は低いが、
  search_max_output_size_bytes を設定で小さくすれば（最小 1024）
  ほぼ確実に再現可能。
修正方針:
  減算を unsigned 前提で書く:
  if (*text_len + marker_len + 1 >= capacity ||
      record_len + 1 > capacity - *text_len - (marker_len + 1)) { ... }
  または record 経路でも「record が残量に収まるか」を別途判定し、
  収まらなければ marker 経路に落とす。

### H-2: create_directory が mkdir 失敗しても成功レスポンスを返す
ファイル: mcp_file/mcp_tools_dir.c:204-223（handle_create_directory は 188 行〜）
    if(mkdir(target_path, 0755)){          // 行204
       perror("mkdir");
       send_json_rpc_error(id, -32603, "Cannot create directory: The directory
                                          aleady exist or permission denied."); // 行206
    }                                       // ← ここに return が無い（行207）
    ...
    json_object_array_add(content_arr, text_obj);
    json_object_object_add(result, "content", content_arr);
    return result;    // 行223: 失敗時でも必ず到達
問題:
  mkdir が失敗するとエラーメッセージ（JSON-RPC error 枠）を送出した後、
  fall-through で「Directory created successfully: ...」の result が組み立てられ、
  tools/call 側 (mcp_server.c:823-824: result が非 NULL なので
  send_json_rpc_response) が success レスポンスも送出する。
  つまり 1 リクエストに対して「error 1 行 + result 1 行」の 2 行が返り、
  JSON-RPC の 1 要求 1 応答が破れる（クライアントは 2 件目の行を
  予期しない id 付きメッセージとして扱う/エラー後に成功と誤認）。
  さらに「aleady exist」のタイポ（already の誤り、行206）。
  成功メッセージにも「created successfully」という断定が入るため、
  既存ディレクトリ（EEXIST）でも成功扱いになる。
修正: 行204-207 のブロック末尾に `return NULL;` を追加。
      EEXIST を他の失敗と区別して「already exists」を明示するのが望ましい。

## 2. 中程度 (Medium)

### M-1: search_string_in_file の O(N^2) 行番号計算
ファイル: mcp_file/mcp_tools_misc.c:1139-1150（handle_search_string_in_file は 1079 行〜）
  while ((search_pos = strstr(search_pos, search_string)) != NULL) {   // 行1139
      ...
      int start_line = 1;
      for (size_t i = 0; i < hit_offset; i++)        // 行1149
          if (file_content[i] == '\n') start_line++;
  }
  各ヒットごとにファイル先頭から '\n' を数え直しており、最大 1MiB ファイルで
  大量ヒット（上限 g_search_max_results_limit=1000 本まで）だと 10^9 近い
  比較になり得る。前回のヒット位置から増分計算（またはヒット前に走査済み
  行番号の保持）で O(N + M) にできる。機能面は正しい。

### M-2: search_string_in_file が NUL バイト以降を検索できない
ファイル: mcp_file/mcp_tools_misc.c:1112-1116, 1139
  ファイル全体を malloc(file_size+1)（行1112）に入れ '\0'（行1116）を
  付けた後に strstr を使うため、先頭 NUL バイトまでの部分文字列として
  扱われる。バイナリ混じりのテキストを検索すると NUL 以降のヒットが
  見逃される（仕様として許容なら OK。ただし read_file のツール説明
  "Supports text and binary files"（mcp_server.c:243）との差異に注意。
  search_string_in_file 自体のツール説明ではこの挙動が明記されていない）。

### M-3: search_files が d_type でファイルを判定し、DT_UNKNOWN を取りこぼす
ファイル: mcp_file/mcp_tools_misc.c:42-47
        if (entry->d_type == DT_DIR) { ... }          // 行42
        else if (entry->d_type == DT_REG) { ... }     // 行47
  一部のファイルシステム（旧 NFS、一部の overlayfs 等）で d_type が
  DT_UNKNOWN になるとファイル・ディレクトリとも無視される。
  安全策: DT_UNKNOWN の場合 lstat() で種別を確定する。

### M-4: get_tags のメッセージ長計算でリテラルが実フォーマットと不一致
      （現状はちょうどフィットのため overflow せず、軽微な整合性問題）
ファイル: mcp_file/mcp_tools_misc.c:751/760, 799/808
  行751: msg_len = strlen("Tags for : ") + strlen(abs_path) + strlen(joined) + 1;
  行760: snprintf(msg, msg_len, "Tags for %s: %s", abs_path, joined);
  リテラル "Tags for : "（11 文字）は実フォーマットの固定部分
  "Tags for " + ": "（11 文字）と文字数は一致しているが、空欄位置が
  異なり（"for : " vs "for " + ": "）意図と実装が食い違っている。
  行799/808 も同型: strlen("Files with tag '': ")（19 文字）に対し
  実フォーマット "Files with tag '%s': %s" の固定部分
  "Files with tag '" + "': "（19 文字）。
  両所とも「リテラル長 = 実固定部 + 1（NUL）」でちょうどフィットしており、
  現行コードでは overflow しない（検証済み）。ただし +1 が「NUL のみ」を
  意図しているならリテラルはフォーマットの固定部そのものを使うべきで、
  現在の書き方は改行・スペースの配置ミスが潜在化している。
  修正: リテラルをフォーマットの固定部と揃えるか、+1 を +2 にして
  明示的な余裕を持つ。

### M-5: edit_file の normalize 失敗時のエラー送出方式が他ツールと不統一
ファイル: mcp_file/mcp_tools_file.c:564-568
  int path_err = normalize_path(abs_path, sizeof(abs_path), (char *)path);
  if (path_err != 0) {
      send_json_rpc_error(id, path_err, "Invalid path");
  }
  normalize_path は -32602 / -32603 を返す（mcp_utils.c:203-216）。
  他ツール群では normalize 失敗は handle_error()（mcp_json_rpc.c:79-85）
  経由（例 mcp_tools_file.c:46-49, mcp_tools_dir.c:239-242 の
  handle_move_file）。edit_file のみ send_json_rpc_error を直出ししており、
  メッセージも「Invalid path」のみで原因（存在しない/長すぎ）が伝わらない。
  挙動自体は妥当だが一貫性の観点で handle_error 経由に統一するのが望ましい。

### M-6: mcp_main.c の config 探索で実行ファイル隣フォールバック時に
      どの config を使ったかのログがない
ファイル: mcp_file/mcp_main.c:104-131
  ROOT 側に mcp_config.json がないとき、可搬実行ファイルの隣にフォールバック
  するが（行114-126）、どちらを使っても「どの config を使用した」の
  出力はない（load_server_config 内で open 失敗時の Warning が出る程度、
  mcp_utils.c:301-304）。デプロイ時の切り分けに不便。軽微。

## 3. MCP 仕様との突合

### 3.1 合致している点
- JSON-RPC 2.0: "jsonrpc":"2.0" 必須チェック (mcp_server.c:157-163)。
  id なし（通知）はレスポンスしない (mcp_server.c:178-187,
  mcp_json_rpc.c:45-49)。method 欠落は -32600 (mcp_server.c:166-171)。
- initialize: protocolVersion / capabilities / serverInfo を返す
  (mcp_server.c:188-227)。バージョンネゴシエーションは negotiate_version
  (mcp_server.c:127-152)、サポート表 (mcp_main.c:6-11) にクライアント要求が
  含まれれば採用、含まれなければ最新 (2026-07-28) にフォールバック。
  2024-11-05 以降の仕様では「クライアントが要求したバージョンをサーバが
  理解しているならそれ、そうでなければサーバが使用するもの」が返される
  べきであり、実装はこれに合致。
- capabilities: tools / resources が空オブジェクトとして宣言
  (mcp_server.c:207-209)。MCP 仕様では capability はオブジェクト型であり、
  実装もオブジェクト（{}）で宣言しており正しく、コメントもその意図を明記
  (mcp_server.c:199-206)。
- tools/list: {"tools":[...]} 形式 (mcp_server.c:234-)、各ツールに
  inputSchema（JSON Schema draft 2020-12 の $schema 付き、例
  mcp_server.c:247）。2025-03-26 以降の仕様で追加された任意の title
  フィールドも new_tool_schema で出力している (mcp_server.c:75-80、
  リテラルツールでは mcp_server.c:242 等) → 新仕様対応。
- tools/call の結果が {"content":[{"type":"text","text":...}], "isError":...}
  形式（各ハンドラ共通、例 mcp_tools_file.c:379-388）。ツール実行中の失敗は
  isError=true の content を返し（JSON-RPC レベルでは成功、
  g_tool_call_active で分岐、mcp_json_rpc.c:51-62、
  mcp_server.c:782, 816, 822）、プロトコルレベルの誤りは JSON-RPC error として
  返す、という 2025-06-18 以降の「tool execution error と protocol error を
  分離する」仕様方針に沿っている。
- ping に対応 (mcp_server.c:230-231)。
- notifications/initialized を受信して初期化完了を認識
  (mcp_server.c:178-187, 183, 227)。
- resources/list / resources/read に対応 (mcp_server.c:828-888)。
  tree://project 単一リソースでディレクトリツリーを公開。
  capabilities.resources を宣言しているため整合。
  resources/read の tree_content 取得時、content 配列が空のときは
  text="" で返す (mcp_server.c:871-875) → 妥当。
- prompts/list が空配列を返す (mcp_server.c:889-892)。
  capabilities に prompts を宣言していないため、厳密に読めば
  「宣言していない capability のメソッド」になるが、MCP 仕様では
  prompts を使わないサーバが prompts/list をサポートする必要はなく、
  空配列で応答するのは実用上無害。
  推奨: capabilities に "prompts": {} を追加するか、prompts/list には
  未サポートエラーを返す、のどちらかに統一。

### 3.2 不整合・留意点
- P-1: 「initialize 前に ping 以外のメソッド」に -32002 "Server is not
  initialized" を返す (mcp_server.c:232-233)。MCP 仕様は initialize 前に
  ping 以外のリクエストは拒否してよく、ping は例外。実装は ping のみを
  例外にしており仕様通り。initialize 自体が 2 回送られた場合の
  -32600 "already completed" (mcp_server.c:189-192) は仕様の
  「server MUST NOT respond to a second initialize」に合致。OK。
- P-2: 通知メソッドの判定に "initialized" / "cancelled" という
  notifications/ プレフィックスなしの別名を許容している
  (mcp_server.c:179-181)。MCP 仕様の正規名は notifications/initialized
  のみで、プレフィックスなし版は仕様には存在しない。寛容さとしては
  無害だが、正規名のみを認めてもよい。
- P-3: tools/call で unknown tool に対し -32602 を返す
  (mcp_server.c:817)。MCP 仕様では unknown tool name は -32602
  （無効なパラメータ）として扱うことが想定されており、実装はこれに
  合致。OK。
- P-4: serverInfo に "supported_versions" 配列を載せている
  (mcp_server.c:217-221)。これは MCP 標準フィールドではない
  （標準は name / version のみ）。拡張情報として無害だが、厳格な
  バリデータが unknown field を警告する可能性。任意の追加フィールドは
  JSON としては合法なので問題なし。
- P-5: read_file ツールの inputSchema に "encoding" を宣言している
  (mcp_server.c:259-262) が、handle_read_file (mcp_tools_file.c:5-327)
  は encoding パラメータを一切読んでいない（grep でヒットなし）。
  宣言されているのに無視されるため、クライアントが "encoding":"base64"
  等を渡すと何の効果がなく（テキスト判定はホワイトリスト・マジック
  ナンバー・先頭 1KB の NUL スキャンのみ、mcp_utils.c:521-565 の
  determine_file_type）、期待外れになり得る。
  → 実装するか、schema から除去するか、description に無視され得る旨を
    明記する。
- P-6: 2025-11-25 系の仕様では list 系メソッドの cursor による
  分割応答（pagination）が規定されているが、本サーバは
  tools/list, resources/list, prompts/list いずれも cursor / nextCursor
  を返さない（全量 1 応答）。ツール 16 個・リソース 1 個・プロンプト 0 個
  という規模では実用上問題ないが、仕様準拠の厳密なクライアントが
  nextCursor を期待する可能性。（MCP では cursor は任意なので、
  未実装でも違反ではない。）
- P-7: 通知の完了イベント（notifications/progress、
  notifications/tools/list_changed 等）を送信しない。
  capabilities に listChanged 等のフラグも宣言していないため整合。
  download_file 等の長時間ツールに progressNotification を
  付加しない設計（_meta.progressToken は未サポート）。
  仕様上の必須ではないが、大規模ダウンロードの進捗が
  クライアントに伝わない点は UX 上の留意点。

## 4. 軽微 (Low)

- L-1: mcp_tools_dir.c:202 の fprintf(stderr,"target_path=%s\n",...) が
      成功パスでも必ず出力されるデバッグログ（_DEBUG ガードなし）。
      handle_create_directory のみ。他関数には無い。
- L-2: mcp_tools_dir.c:206 の "aleady" タイポ（already）。
- L-3: mcp_json_rpc.c:4-23 の JsonRpcError / JsonRpcRequest /
      JsonRpcResponse 構造体が定義されているが未使用（dead code、
      使用箇所 grep 0 件）。send_json_rpc_response / send_json_rpc_error は
      直接使用。
- L-4: mcp_tools_file.c:816-818 の edit_file 成功メッセージ
      "History saved to .history/." の末尾が「/.」で読みにくい。
- L-5: mcp_tools_misc.c:497-505 の tag_names 取得ループで、
      json_object_get_string の戻り値を NULL チェックせず tag_names[] へ
      格納（行504）。直前の文字列型検証（行498-503）で安全。
- L-6: mcp_utils.c:521-565 determine_file_type のテキスト判定は
      先頭 1KB（CHECK_BUFFER_SIZE=1024、mcp_common.h:46）の
      NUL スキャンのみ。1KB 以降に NUL があってもテキスト扱い。
      意図的な簡略化として妥当。
- L-7: mcp_main.c:28 の g_tag_file_path 初期値 "/" TAGS_FILE は
      load_server_config (mcp_utils.c:284-297) で上書きされるため実質
      初期値のみ。問題なし。
- L-8: mcp_tools_misc.c:1194 の char record[128] は
      "Match %d: start_line=%d, end_line=%d\n" に対して十分
      （INT_MAX 級でも約 50 文字）。snprintf の書き込み量チェックあり
      （行1195-1197）。OK。

## 5. 検証結果（2026-08-28 再確認）

- make test: 10/10 PASS（base64 4件, sha256 2件, config 3件,
  resolve_mcp_path 1件。tests/test_utils.c）。
- ASan ハーネス (/tmp/mcp_review/harness_asan): H-1 の
  stack-buffer-overflow を再現（WRITE of size 50 @ append_search_result,
  2026-08-28 実行、capacity=100/text_len=60/record_len=49 で末尾 10 バイト
  overflow）。
- スモーク: curl ヘッダ stub + 実 libjson-c でビルド成功、
  initialize → protocolVersion 応答、tools/list → 16 ツール、
  tools/call（read_file / search_files / edit_file / tag_file 系）正常
  （2026-08-26 実施）。
- デプロイ制約: mcp_sandbox_enter はルート配下の tmp/.old を単一 mkdir で
  作成するため（mcp_sandbox.c:82: mkdir(old_root, 0700)、EEXIST 以外の
  失敗で返却）、ルート直下に tmp/ が無いと起動失敗。
  実運用ではルート配下の tmp ディレクトリ存在を確認するか、
  単一 mkdir を mkdirp 相当に強化するのが望ましい。

## 6. 優先度まとめ

  修正必須  H-1 (size_t 下位 overflow、ASan 実証) /
            H-2 (create_directory の二重応答・fall-through)
  推奨      M-1 (O(N^2) 行番号計算) / M-3 (DT_UNKNOWN 取りこぼし) /
            P-5 (encoding 宣言と無視の不一致)
  任意      M-2, M-4, M-5, M-6, P-2, P-4, P-6, P-7, L 系

## 7. 修正結果（2026-08-28）

- H-1: `append_search_result` を残量ベースの判定に変更し、marker + NUL
  自体が収まらない状態でも書き込まないよう修正。現行呼び出しは
  `text_len <= capacity - marker_len - 1` を保つため、当初のASan
  ハーネスは通常呼び出しでは生成されない状態だが、防御的に解消済み。
- H-2: `mkdir` 失敗後に `return NULL` し、ツールエラー1応答のみを
  返すよう修正。EEXIST メッセージと無条件デバッグログも整理。
- M-1: 前回ヒット位置からの増分改行計数に変更し、行番号計算を
  O(N + M) に線形化。
- M-3: `DT_UNKNOWN` の場合のみ `lstat` で種別を確定。シンボリック
  リンクは追跡しない。
- P-5: `encoding` を inputSchema から削除。UTF-8テキストのみを
  サポートし、不正UTF-8/バイナリはBase64で返す契約をschemaとREADMEに
  明記。UTF-8検証を実装して実動作も契約に合わせた。
- 追加修正: 既存テストで文字列として扱われる `base64_decode`
  返却値をNUL終端し、ヒープ境界外読みの未定義動作を解消。

検証:

- `make test`: 19/19 PASS。検索境界、行番号、DT_UNKNOWN、二重応答、
  UTF-8 schema/実動作の回帰テストを追加。
- `make all`: 警告なしで成功。
- AddressSanitizer + UndefinedBehaviorSanitizer: 19/19 PASS。実行環境の
  ptrace制約のためLeakSanitizerのみ `detect_leaks=0` で無効化。
