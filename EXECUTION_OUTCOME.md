# Execution outcome extension

Identifier: `io.github.stshm.mcp-file/execution-outcome`

This optional extension adds `pending` and `unknown` to the MCP tool result
discriminator. It lets a client distinguish a request that can be corrected and
retried without duplicating an earlier operation from one that needs inspection
before retry. It does not introduce asynchronous tasks, polling, or automatic retry.

The extension follows the capability advertisement and fallback rules in
[MCP ResultType](https://modelcontextprotocol.io/specification/2026-07-28/basic#resulttype)
and [Extension Negotiation](https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning#extension-negotiation).
These are application-specific meanings, not additional core MCP result types.

## Advertisement and opt-in

The server advertises support in `server/discover.result.capabilities.extensions`.
Both server and client use an object as the settings value; no settings are
currently defined. Unknown settings are ignored.

```json
{
  "capabilities": {
    "extensions": {
      "io.github.stshm.mcp-file/execution-outcome": {}
    }
  }
}
```

For protocol revision `2026-07-28`, each request supplies its own capabilities.
No extension support is inferred from previous requests or a legacy session:

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "method": "tools/call",
  "params": {
    "name": "edit_file",
    "arguments": {
      "path": "/sample.txt",
      "start_line": 16,
      "end_line": 16,
      "old_content": "before\n",
      "new_content": "after\n"
    },
    "_meta": {
      "io.modelcontextprotocol/protocolVersion": "2026-07-28",
      "io.modelcontextprotocol/clientCapabilities": {
        "extensions": {
          "io.github.stshm.mcp-file/execution-outcome": {}
        }
      }
    }
  }
}
```

For existing clients using the legacy initialization flow, this server also
advertises the extension in `initialize.result.capabilities.extensions` and
accepts the same opt-in in `initialize.params.capabilities.extensions`.
That opt-in applies to subsequent legacy requests on the stdio session only.

## Tool results

`resultType` is a string directly inside the JSON-RPC `result` object, alongside
`content`, `structuredContent` when present, and `isError` when present.
It is independent of `isError`: a completed operation can still have an error
in response formatting, while a command timeout can have an ordinary command
summary with an uncertain execution outcome.

| Value | Meaning with this extension enabled | Client action |
| --- | --- | --- |
| `complete` | Processing finished and its outcome is known. Includes reported nonzero command exits and known per-item batch failures. It does not mean every operation succeeded. | Consume the result. Inspect `isError` and tool-specific fields. |
| `pending` | The requested operation has not committed a persistent change to its target, or a failed new-file creation was successfully rolled back. It is no longer executing. | Let the LLM correct the request or resolve the cause, then retry if appropriate. |
| `unknown` | Execution/publication was attempted and the outcome cannot be guaranteed. A change may already have happened, or execution may have been interrupted after some side effects. | Inspect the target before retrying; do not blindly repeat a mutation. |

Example opted-in tool error:

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "result": {
    "resultType": "pending",
    "content": [
      {
        "type": "text",
        "text": "old_content trailing newline mismatch at lines 16-16."
      }
    ],
    "isError": true
  }
}
```

## Classification rules

- Validation, edit-content/hash verification, and read failures use `pending`.
  They do not apply the requested mutation.
- Atomic writes and copies use `pending` for failures while preparing the
  replacement, before publishing it. Publication failure is conservatively
  `unknown`, since an OS or network-filesystem error may not prove the target
  stayed unchanged. A definite exclusive-create conflict is `pending`.
- A failed new-file write uses `pending` only if removing the new file succeeds;
  otherwise it uses `unknown`. Ambiguous creation errors also use `unknown`.
- A directory creation rejected for a definite precondition, such as an existing
  path or denied permission, is `pending`. Other creation errors and failed
  delete/move system calls are conservatively `unknown`.
- Tag DB writes use the same preparation/publication distinction. If saving
  succeeded but building the response fails, the outcome is `complete`, with
  `isError: true`. Download destination replacement follows the same rule.
- A command that fails before launch is `pending`. Once execution starts,
  timeout, interrupted execution, or failure to recover its termination result
  is `unknown`. A known normal exit is `complete`, including nonzero exit codes.
  The existing command summary and `isError` behavior are preserved.
- Best-effort edit-history failure does not undo an already completed edit.
- A batch with known individual failures is `complete`; callers inspect its
  per-item results. An uncertain batch DB publication is `unknown`.

The guarantee concerns the requested target operation. It does not imply that
temporary files were never created, no network request was sent, or no filesystem
metadata was read. `pending` does not promise a retry will succeed, and is not an
instruction to retry indefinitely.

Protocol errors before tool dispatch retain JSON-RPC `error` responses, without
`result` or `resultType`. Numerical internal tool-error codes are not added to
tool results by this extension.

## Fallback

If the client does not advertise this extension, the server never sends
`pending` or `unknown`. It returns core `resultType: "complete"`, retaining the
existing `content`, `isError`, and tool-specific result fields.

Without a negotiated extension, `complete` only denotes a final protocol result;
it supplies no extra guarantee that a failed mutation had no effects. Clients
must not use it as a safe-retry signal. An unrecognized `resultType` is invalid
under the core protocol, so this fallback is required.
