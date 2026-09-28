# MCP File Server

[日本語](README.ja.md)

A local Model Context Protocol (MCP) server for reading, editing, searching, organizing, and downloading files inside a directory you choose. It communicates over standard input and output and builds on Linux and Windows.

The server presents paths as slash-separated MCP paths such as /docs/notes.md on every platform. File operations are resolved inside the configured root directory.

## Features

- Read and write UTF-8 text; binary and non-UTF-8 file contents are returned as Base64.
- Create, edit, copy, move, and delete files; create directories and inspect directory trees.
- Search file names and file contents.
- Add, query, and remove tags stored in a JSON file.
- Download a URL to a file inside the root directory.
- Run a command with a timeout and bounded output.
- Use a Linux user and mount namespace sandbox, or a Windows AppContainer child process.

Available MCP tools:

- Files: read_file, stat_file, write_file, create_file, edit_file, delete_file, copy_file, move_file, create_directory, get_directory_tree
- Search: search_files, search_string_in_file
- Tags: tag_file, batch_tag_file, get_tags, delete_tag
- Network and commands: download_file, run_cmd

## Build

The repository includes the MCP core library source under mcp_lib, so it can be built from a standalone checkout.

### Linux

Install a C compiler, make, pkg-config, and the json-c and libcurl development packages. For Debian or Ubuntu:

    sudo apt-get install build-essential pkg-config libjson-c-dev libcurl4-openssl-dev

Build from the repository root:

    make

The executable is mcpsv_file.

Linux sandbox startup uses unprivileged user and mount namespaces. Some distributions or host security policies disable unprivileged user namespaces; the host administrator may need to configure an appropriate policy before the server can start.

### Windows

Build in an MSYS2 MINGW64 or UCRT64 shell. Install the matching toolchain and libraries.

MINGW64:

    pacman -S --needed make mingw-w64-x86_64-{gcc,json-c,curl,pkgconf}

UCRT64:

    pacman -S --needed make mingw-w64-ucrt-x86_64-{gcc,json-c,curl,pkgconf}

Then build:

    make -f Makefile.windows

The Windows target is x64 and requires Windows 10 or later. Keep mcpsv_file.exe and its MinGW DLL dependencies on a local disk. Place these DLLs beside the executable:

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

UNC paths and mapped network drives are not supported as the sandbox root.

## Run

Choose a directory the server is allowed to access:

    ./mcpsv_file --root_dir /path/to/mcp-root

You can also use the short option:

    ./mcpsv_file -r /path/to/mcp-root

If root_dir is omitted, MCP_ROOT_DIR is used; if that variable is unset, the current directory is used.

A generic MCP client configuration can use the executable path and root directory:

    {
      "mcpServers": {
        "localFile": {
          "command": "/absolute/path/to/mcpsv_file",
          "args": ["--root_dir", "/path/to/mcp-root"]
        }
      }
    }

Use an absolute path for command in your client configuration. On Windows, use the absolute path to mcpsv_file.exe and a local drive path for the root.

## Configuration

The server loads configuration from mcp_config.json in the root directory. If that file is absent, it checks beside the executable. You may specify another file with --config FILE.

Start with the example configuration:

    cp mcp_config.example.json /path/to/mcp-root/mcp_config.json
    mkdir -p /path/to/mcp-root/.mcp

The second command creates the parent directory for the example tag database path. Configuration files are limited to 1 MiB. Missing or invalid values fall back to defaults.

| Setting | Default | Purpose |
| --- | ---: | --- |
| tag_file_path | .mcp/mcp_tags.json | Root-relative path for the tag database. Create its parent directory first. |
| max_read_file_size_bytes | 104857600 | Maximum file size read_file and edit_file load into memory (100 MiB). |
| max_download_file_size_bytes | 209715200 | Maximum downloaded file size (200 MiB). |
| download_timeout_seconds | 30 | Default download timeout. |
| max_download_timeout_seconds | 300 | Maximum timeout accepted by download_file. |
| ca_bundle_path | unset | Optional CA bundle path inside the root, written as an MCP absolute path. |
| search_timeout_seconds | 25 | Search time limit in seconds; stops traversal and returns incomplete results with retry guidance. |
| search_max_results | 100 | Default result limit for search_files. |
| search_max_results_limit | 1000 | Maximum result limit accepted by search_files. |
| search_max_output_size_bytes | 1048576 | Maximum search response size (1 MiB). |
| tree_default_depth | 3 | Default depth for get_directory_tree. |
| tree_max_depth | 32 | Maximum tree depth. |
| tree_max_entries | 5000 | Maximum tree entries. |
| tree_max_output_size_bytes | 131072 | Maximum tree response size (128 KiB). |
| run_cmd_timeout_seconds | 30 | Default run_cmd timeout. |
| max_run_cmd_timeout_seconds | 300 | Maximum run_cmd timeout. |
| max_run_cmd_output_size_bytes | 1048576 | Maximum combined command output (1 MiB). |

For TLS downloads, ca_bundle_path must point to a certificate bundle available inside the root. For example, /etc/ssl/certs/ca-certificates.crt refers to the file at that MCP path below the root. If unset, libcurl's default certificate discovery is used.

## Path and security model

- Use MCP paths beginning with /, for example /docs/notes.md. Native absolute paths, UNC paths, drive-letter paths, and paths that escape the root are not accepted as file paths.
- On Linux, the server enters a user and mount namespace and pivots the process root to the selected directory.
- On Windows, the server launches an AppContainer child with access to the selected root and the executable directory. The ACL entries granted to the root-specific AppContainer profile persist after the server exits.
- Windows roots must be on a local drive. Network shares and mapped network drives are unsupported.
- download_file requires outbound Internet access. The Windows AppContainer includes the outbound Internet capability for this tool.
- run_cmd executes shell commands inside the sandbox with a timeout and output limit. Only use this server with MCP clients and users you trust.

## Search limits and results

`search_files` accepts `depth` (default 3, root depth 0, capped by `tree_max_depth`). `recursive: false` overrides depth and searches only the selected directory. Dot-prefixed directories remain excluded by default. Response metadata reports effective depth and completion status.

The deadline is checked between directory entries and file-read chunks. Traversal closes its files and directories before returning; no background search remains. A blocking OS filesystem call can delay the deadline check.

Search metadata is returned in `structuredContent` and as JSON text in the second `content` item; the first item contains matching lines. `status` is `complete` or `limit_exceeded`, with `complete`, `depth`, and `timeout_seconds`. Limited results include `reason` (`timeout`, `output_limit`, or `max_results`) and retry guidance. Completion is relative to the selected search depth. Clients that display only matching text or shorten tool history may hide this metadata; inspect the full tool response when diagnosing limits.

Set `search_timeout_seconds` below the client tool timeout, allowing time for response delivery. The default remains 25 seconds; clients with shorter limits may need a value such as 10 seconds.
