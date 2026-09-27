# MCP File Server

[English](README.md)

指定したディレクトリ内のファイルを読み書き、検索、整理、ダウンロードするためのローカル Model Context Protocol（MCP）サーバーです。標準入力・標準出力で通信し、LinuxとWindowsに対応します。

どのOSでも、ファイルパスは /docs/notes.md のようなスラッシュ区切りのMCPパスで表します。ファイル操作は指定したルートディレクトリ内に解決されます。

## 機能

- UTF-8テキストの読み書き。バイナリおよびUTF-8でない内容はBase64で返します。
- ファイルの作成、編集、コピー、移動、削除、ディレクトリの作成と一覧表示。
- ファイル名とファイル内容の検索。
- JSONファイルに保存するタグの追加、検索、削除。
- ルートディレクトリ内へのURLダウンロード。
- タイムアウトと出力上限を指定したコマンド実行。
- Linuxのuser/mount namespace sandbox、またはWindows AppContainerによる隔離。

提供するMCPツール:

- ファイル: read_file、stat_file、write_file、create_file、edit_file、delete_file、copy_file、move_file、create_directory、get_directory_tree
- 検索: search_files、search_string_in_file
- タグ: tag_file、batch_tag_file、get_tags、delete_tag
- ネットワークとコマンド: download_file、run_cmd

## ビルド

MCP共通ライブラリのソースをmcp_lib/に同梱しているため、このリポジトリ単独でビルドできます。

### Linux

Cコンパイラー、make、pkg-config、json-cとlibcurlの開発パッケージが必要です。DebianまたはUbuntuでは次のように導入できます。

    sudo apt-get install build-essential pkg-config libjson-c-dev libcurl4-openssl-dev

リポジトリのルートでビルドします。

    make

実行ファイルはmcpsv_fileです。

Linuxでは非特権user/mount namespaceを使用します。ディストリビューションやホストのセキュリティ設定が非特権user namespaceを無効にしている場合、サーバーの起動前に管理者によるポリシー設定が必要になることがあります。

### Windows

MSYS2のMINGW64またはUCRT64シェルでビルドします。利用するシェルに対応するツールチェーンとライブラリを導入してください。

MINGW64:

    pacman -S --needed make mingw-w64-x86_64-{gcc,json-c,curl,pkgconf}

UCRT64:

    pacman -S --needed make mingw-w64-ucrt-x86_64-{gcc,json-c,curl,pkgconf}

その後、次のコマンドでビルドします。

    make -f Makefile.windows

Windows版はx64、Windows 10以降が対象です。mcpsv_file.exeと以下のMinGW依存DLLをローカルディスクに配置し、DLLは実行ファイルと同じフォルダーに置いてください。

    libbrotlicommon.dll
    libbrotlidec.dll
    libcrypto-3-x64.dll
    libcurl-4.dll
    libiconv-2.dll
    libidn2-0.dll
    libintl-8.dll
    libjson-c-5.dll
    libnghttp2-14.dll
    libnghttp3-9.dll
    libngtcp2-16.dll
    libngtcp2_crypto_ossl-0.dll
    libpsl-5.dll
    libssh2-1.dll
    libssl-3-x64.dll
    libunistring-5.dll
    libwinpthread-1.dll
    libzstd.dll
    zlib1.dll

UNCパスとネットワークドライブはsandbox rootとして使えません。

## 起動

サーバーにアクセスさせるディレクトリを指定して起動します。

    ./mcpsv_file --root_dir /path/to/mcp-root

短縮形も使えます。

    ./mcpsv_file -r /path/to/mcp-root

root_dirを省略するとMCP_ROOT_DIR環境変数を使い、環境変数もない場合はカレントディレクトリを使います。

一般的なMCPクライアントには、実行ファイルとルートディレクトリを次のように設定できます。

    {
      "mcpServers": {
        "localFile": {
          "command": "/absolute/path/to/mcpsv_file",
          "args": ["--root_dir", "/path/to/mcp-root"]
        }
      }
    }

クライアント設定のcommandには絶対パスを指定してください。Windowsではmcpsv_file.exeの絶対パスと、ローカルドライブ上のrootを指定します。

## 設定

ルートディレクトリ内のmcp_config.jsonを読み込みます。存在しない場合は実行ファイルと同じディレクトリを確認します。--config FILEで別の設定ファイルを指定できます。

同梱の例を使う場合:

    cp mcp_config.example.json /path/to/mcp-root/mcp_config.json
    mkdir -p /path/to/mcp-root/.mcp

2つ目のコマンドは、例のタグデータベース用ディレクトリを作成します。設定ファイルの上限は1 MiBです。項目が未指定または不正な場合は既定値を使います。

| 設定項目 | 既定値 | 説明 |
| --- | ---: | --- |
| tag_file_path | .mcp/mcp_tags.json | タグデータベースのルート相対パス。親ディレクトリを先に作成してください。 |
| max_read_file_size_bytes | 104857600 | read_fileとedit_fileがメモリに読み込む最大サイズ（100 MiB）。 |
| max_download_file_size_bytes | 209715200 | ダウンロード可能な最大サイズ（200 MiB）。 |
| download_timeout_seconds | 30 | ダウンロードの既定タイムアウト。 |
| max_download_timeout_seconds | 300 | download_fileに指定できるタイムアウト上限。 |
| ca_bundle_path | 未設定 | ルート内にあるCA証明書バンドル。MCP絶対パスで指定します。 |
| search_max_results | 100 | search_filesの既定結果数。 |
| search_max_results_limit | 1000 | search_filesに指定できる結果数の上限。 |
| search_max_output_size_bytes | 1048576 | 検索応答の最大サイズ（1 MiB）。 |
| tree_default_depth | 3 | get_directory_treeの既定の深さ。 |
| tree_max_depth | 32 | ツリーの最大深さ。 |
| tree_max_entries | 5000 | ツリーの最大エントリー数。 |
| tree_max_output_size_bytes | 131072 | ツリー応答の最大サイズ（128 KiB）。 |
| run_cmd_timeout_seconds | 30 | run_cmdの既定タイムアウト。 |
| max_run_cmd_timeout_seconds | 300 | run_cmdのタイムアウト上限。 |
| max_run_cmd_output_size_bytes | 1048576 | コマンド出力の最大サイズ（1 MiB）。 |

TLSダウンロードでca_bundle_pathを指定する場合、証明書バンドルをルート内に配置してください。たとえば /etc/ssl/certs/ca-certificates.crt は、ルート配下の同じMCPパスにあるファイルを指します。未設定の場合はlibcurlの既定の証明書検索を使います。

## パスとセキュリティ

- MCPパスは /docs/notes.md のように / から始めます。ネイティブの絶対パス、UNCパス、ドライブ文字形式、ルート外へ抜けるパスはファイル操作に使えません。
- Linuxではuser/mount namespaceへ入り、選択したディレクトリをプロセスのrootにします。
- Windowsでは選択したルートと実行ファイル配置先へアクセスできるAppContainer child processを起動します。ルート固有のAppContainer profileに追加されるACLエントリーは、サーバー終了後も残ります。
- Windowsのrootはローカルドライブ上に置いてください。ネットワーク共有とネットワークドライブは非対応です。
- download_fileには外部へのインターネット接続が必要です。Windows AppContainerにはこのツール用のoutbound Internet capabilityを付与します。
- run_cmdはsandbox内でコマンドを実行し、タイムアウトと出力上限を適用します。信頼できるMCPクライアントと利用者に対してのみ使用してください。
