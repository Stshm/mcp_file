# MCP Server - Tag File 機能仕様書

## 1. 概要

ファイルにタグを付与・取得・削除するためのMCPツール群の仕様。
AIモデルがコード構造を理解し、メタデータとしてファイルを整理・検索するために使用する。

### 1.1 目的
- プロジェクト内のファイルを論理的にグループ化（モジュール単位、機能単位等）
- タグベースのファイル検索・フィルタリング
- AIによる自動タグ付けと手動タグ付けの両方に対応

### 1.2 保存形式
タグ情報はプロジェクトルート直下に `mcp_tags.json` としてJSON形式で保存される。

---

## 2. データ構造

### 2.1 mcp_tags.json フォーマット

```json
{
  "version": 1,
  "files": {
    "/absolute/path/to/file.c": {
      "tags": ["tag1", "tag2"],
      "metadata": {
        "last_updated": "2024-01-01T00:00:00Z",
        "updated_by": "ai"
      }
    },
    "/absolute/path/to/another.py": {
      "tags": ["tag3"],
      "metadata": {
        "last_updated": "2024-01-02T00:00:00Z",
        "updated_by": "user"
      }
    }
  },
  "_tags_index": {
    "tag1": ["/absolute/path/to/file.c"],
    "tag2": ["/absolute/path/to/file.c"],
    "tag3": ["/absolute/path/to/another.py"]
  }
}
```

### 2.2 フィールド説明

| キー | 型 | 説明 |
|------|------|------|
| `version` | integer | スキーマバージョン（現在: 1） |
| `files` | object | ファイルパスをキーとするタグ情報マップ |
| `files.<path>.tags` | string[] | 付与されたタグの配列 |
| `files.<path>.metadata.last_updated` | string (ISO8601) | 最終更新時刻 |
| `files.<path>.metadata.updated_by` | string | 更新元（"ai" または "user"） |
| `_tags_index` | object | タグ→ファイルパスの逆引きインデックス（内部使用、APIで直接操作不可） |

### 2.3 タグ名のルール
- 英数字、ハイフン(`-`)、アンダースコア(`_`)のみ使用可能
- 先頭は英字または数字
- 大文字小文字を区別しない（内部では小文字に正規化して保存）
- 推奨フォーマット: `category:name` （例: `module:auth`, `bugfix:login-error`）

---

## 3. API仕様

### 3.1 handle_tag_file - ファイルへのタグ付与

#### リクエストパラメータ
```json
{
  "path": "/absolute/path/to/file.c",
  "tags": ["tag1", "tag2"],
  "mode": "add"
}
```

| パラメータ | 型 | 必須 | デフォルト | 説明 |
|------------|------|------|------------|------|
| `path` | string | はい | - | タグを付与するファイルの絶対パス |
| `tags` | string[] | はい | - | 付与するタグの配列 |
| `mode` | string | いいえ | `"add"` | タグ操作モード: `"add"`, `"set"`, `"replace"` |

#### mode の詳細

| モード | 説明 |
|--------|------|
| `"add"` | 既存タグに追加（重複タグは自動除外） |
| `"set"` | 指定されたタグのみをセット（既存タグをクリア後、設定） |
| `"replace"` | 既存タグを新しいタグで置換（`"set"` と同等だが明示的） |

#### レスポンス形式（MCP準拠）
```json
{
  "content": [
    {
      "type": "text",
      "text": "Tags added successfully: /absolute/path/to/file.c (tags: tag1, tag2)"
    }
  ],
  "isError": false
}
```

#### エラーケース
| コード | メッセージ | 原因 |
|--------|------------|------|
| -32602 | `Invalid parameters: path and tags are required` | パラメータ不足または型エラー |
| -32602 | `Invalid mode: <value>` | mode が add/set/replace のいずれでもない |
| -32603 | `Cannot read tag file` | mcp_tags.json の読み込み失敗 |
| -32603 | `Cannot write tag file` | mcp_tags.json の書き込み失敗 |

---

### 3.2 handle_batch_tag_file - バッチタグ付け（複数ファイル）

#### リクエストパラメータ
```json
{
  "files": [
    {
      "path": "/absolute/path/to/file1.c",
      "tags": ["tag1", "tag2"]
    },
    {
      "path": "/absolute/path/to/file2.py",
      "tags": ["tag3"],
      "mode": "set"
    }
  ],
  "default_mode": "add"
}
```

| パラメータ | 型 | 必須 | デフォルト | 説明 |
|------------|------|------|------------|------|
| `files` | object[] | はい | - | タグ付け対象のファイルリスト |
| `files[].path` | string | はい | - | ファイルパス |
| `files[].tags` | string[] | はい | - | 付与するタグ |
| `files[].mode` | string | いいえ | `"add"` | このファイルのモード（省略時はdefault_mode） |
| `default_mode` | string | いいえ | `"add"` | デフォルト操作モード |

#### レスポンス形式
```json
{
  "content": [
    {
      "type": "text",
      "text": "Batch tagging completed: 2/2 files succeeded, 0 failed"
    }
  ],
  "results": [
    {
      "path": "/absolute/path/to/file1.c",
      "success": true,
      "tags_applied": ["tag1", "tag2"]
    },
    {
      "path": "/absolute/path/to/file2.py",
      "success": true,
      "tags_applied": ["tag3"],
      "mode_used": "set"
    }
  ],
  "isError": false
}
```

#### エラーケース
| コード | メッセージ | 原因 |
|--------|------------|------|
| -32602 | `Invalid parameters: files array is required` | パラメータ不足 |
| -32602 | `files[].path and tags are required for each entry` | エントリのパラメータ不足 |

---

### 3.3 handle_get_tags - タグの取得

#### リクエストパラメータ（パターンA: ファイルからタグを取得）
```json
{
  "type": "file",
  "path": "/absolute/path/to/file.c"
}
```

| パラメータ | 型 | 必須 | 説明 |
|------------|------|------|------|
| `type` | string | はい | `"file"` または `"tag"` |
| `path` | string | type=file の場合必須 | タグを取得するファイルのパス |

#### レスポンス形式（type=file）
```json
{
  "content": [
    {
      "type": "text",
      "text": "Tags for /absolute/path/to/file.c: tag1, tag2"
    }
  ],
  "tags": ["tag1", "tag2"],
  "isError": false
}
```

#### リクエストパラメータ（パターンB: タグからファイルを取得）
```json
{
  "type": "tag",
  "tag_name": "module:auth"
}
```

| パラメータ | 型 | 必須 | 説明 |
|------------|------|------|------|
| `type` | string | はい | `"file"` または `"tag"` |
| `tag_name` | string | type=tag の場合必須 | タグ名（大文字小文字非区別） |

#### レスポンス形式（type=tag）
```json
{
  "content": [
    {
      "type": "text",
      "text": "Files with tag 'module:auth': /path/to/file1.c, /path/to/file2.c"
    }
  ],
  "files": ["/absolute/path/to/file1.c", "/absolute/path/to/file2.c"],
  "isError": false
}
```

#### エラーケース
| コード | メッセージ | 原因 |
|--------|------------|------|
| -32602 | `Invalid parameters: type is required` | パラメータ不足 |
| -32602 | `path is required when type=file` | type=file で path が未指定 |
| -32602 | `tag_name is required when type=tag` | type=tag で tag_name が未指定 |
| -32603 | `Cannot read tag file` | mcp_tags.json の読み込み失敗 |

---

### 3.4 handle_delete_tag - タグの削除

#### リクエストパラメータ（パターンA: ファイルからタグを削除）
```json
{
  "type": "file",
  "path": "/absolute/path/to/file.c",
  "tags_to_remove": ["tag1"]
}
```

| パラメータ | 型 | 必須 | 説明 |
|------------|------|------|------|
| `type` | string | はい | `"file"` または `"all_tags"` |
| `path` | string | type=file の場合必須 | タグを削除するファイルのパス |
| `tags_to_remove` | string[] | type=file の場合必須 | 削除するタグの配列 |

#### レスポンス形式（type=file）
```json
{
  "content": [
    {
      "type": "text",
      "text": "Tags removed from /absolute/path/to/file.c: tag1 (remaining: tag2)"
    }
  ],
  "isError": false
}
```

#### リクエストパラメータ（パターンB: タグをすべて削除）
```json
{
  "type": "all_tags",
  "path": "/absolute/path/to/file.c"
}
```

| パラメータ | 型 | 必須 | 説明 |
|------------|------|------|------|
| `type` | string | はい | `"file"` または `"all_tags"` |
| `path` | string | type=all_tags の場合必須 | タグを削除するファイルのパス |

#### レスポンス形式（type=all_tags）
```json
{
  "content": [
    {
      "type": "text",
      "text": "All tags removed from /absolute/path/to/file.c"
    }
  ],
  "isError": false
}
```

#### エラーケース
| コード | メッセージ | 原因 |
|--------|------------|------|
| -32602 | `Invalid parameters: type is required` | パラメータ不足 |
| -32602 | `path is required` | path が未指定 |

---

## 4. mcp_tags.json の操作ルール

### 4.1 ファイル読み書きフロー

```
┌─────────────┐     ┌──────────────┐     ┌─────────────┐
│   MCP Client │────▶│ handle_tag_* │────▶│  mcp_tags   │
│              │◀────│   (C)        │◀────│   .json     │
└─────────────┘     └──────────────┘     └─────────────┘
```

1. **読み込み**: `mcp_tags.json` をJSONとしてパース
2. **操作**: 指定されたファイルのタグ情報を更新
3. **インデックス再構築**: `_tags_index` を再生成（変更があった場合）
4. **書き込み**: 変更後のデータをJSONとして保存

### 4.2 ファイルが存在しない場合の扱い
- `handle_tag_file`: タグ情報を作成（ファイルの実体は存在しなくてもよい）
- `handle_get_tags (type=file)`: タグなしを返す（エラーではない）
- `handle_delete_tag`: 何もしない（エラーではない）

### 4.3 mcp_tags.json が存在しない場合
- 新規に作成して保存する
- 読み込み時は空のオブジェクトとして扱う

---

## 5. エラーコード一覧

| コード | 説明 | 使用箇所 |
|--------|------|----------|
| -32600 | Invalid Request | 全般 |
| -32602 | Invalid params | パラメータ不足・型エラー |
| -32603 | Internal error | ファイルI/O失敗、メモリ確保失敗 |

---

## 6. 使用例

### 6.1 AIによる自動タグ付け（バッチ）
```json
// リクエスト
{
  "files": [
    {"path": "/project/src/auth/login.c", "tags": ["module:auth", "feature:login"]},
    {"path": "/project/src/auth/logout.c", "tags": ["module:auth", "feature:logout"]},
    {"path": "/project/src/api/user.py", "tags": ["module:api", "tech:python"]}
  ],
  "default_mode": "add"
}

// レスポンス
{
  "content": [{"type": "text", "text": "Batch tagging completed: 3/3 files succeeded, 0 failed"}],
  "results": [
    {"path": "/project/src/auth/login.c", "success": true, "tags_applied": ["module:auth", "feature:login"]},
    {"path": "/project/src/auth/logout.c", "success": true, "tags_applied": ["module:auth", "feature:logout"]},
    {"path": "/project/src/api/user.py", "success": true, "tags_applied": ["tech:python"]}
  ],
  "isError": false
}
```

### 6.2 タグベースのファイル検索
```json
// リクエスト
{
  "type": "tag",
  "tag_name": "module:auth"
}

// レスポンス
{
  "content": [{"type": "text", "text": "Files with tag 'module:auth': /project/src/auth/login.c, /project/src/auth/logout.c"}],
  "files": ["/project/src/auth/login.c", "/project/src/auth/logout.c"],
  "isError": false
}
```

### 6.3 タグの追加（既存タグを保持）
```json
// リクエスト
{
  "path": "/project/src/auth/login.c",
  "tags": ["bugfix:session-timeout"],
  "mode": "add"
}

// mcp_tags.json 更新後
{
  "files": {
    "/project/src/auth/login.c": {
      "tags": ["module:auth", "feature:login", "bugfix:session-timeout"]
    }
  }
}
```

---

## 7. 実装上の注意点

### 7.1 パス正規化
- 入力されたパスが既存の場合は `normalize_path()` で絶対パスに変換して保存
- ファイルの新規作成時など、まだ現存しないパスの場合は、親ディレクトリを`normalize_path()` で絶対パスに変換する

### 7.2 タグ名の正規化
- 内部では小文字に統一して管理
- 表示時は元のケースを保持（または小文字で統一）

### 7.3 JSONのフォーマット
- 読み込み: `json_object_from_file()` または同等関数を使用
- 書き込み: `JSON_C_TO_STRING_PRETTY` で人間可読形式で保存

### 7.4 同時アクセス
- 単一スレッドでの実行を想定（MCPサーバの特性上）
- 必要に応じてファイルロックを追加

---

## 8. 変更履歴

| 日付 | バージョン | 変更内容 |
|------|------------|----------|
| 2024-XX-XX | v1 | 初版作成 |
