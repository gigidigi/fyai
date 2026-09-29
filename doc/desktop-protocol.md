# Desktop session control protocol

`fyai -b BRANCH desktop` serves one normal, arena-backed FYAI branch for the
life of that invocation. Its standard input and output carry newline-framed
JSON-RPC 2.0. Standard error carries diagnostics. The invocation is the live
session owner; the arena remains the only durable conversation store.

Requests from the desktop:

| Method | Parameters | Result |
| --- | --- | --- |
| `initialize` | `{}` | Protocol `1`, agent ID `fyai-desktop`, capabilities. |
| `session/history` | `{}` | `{messages:[...],documents:[...],documentsComplete:boolean}` in stored order. Messages contain canonical metadata. Documents contain the saved display transcript. |
| `session/prompt` | `{prompt:string}` | Deferred until the turn ends: `{status,published}`. Status is `published`, `cancelled`, or `failed`. |
| `session/cancel` | `{}` | `{accepted:boolean}`. Interrupts the active turn through FYAI's turn cancellation path. |
| `shutdown` | `{}` | `null`; the process flushes replies and exits. |

`session/update` notifications carry `{kind,text}` for `text`, `reasoning`, or
`tool_output`. Tool updates carry `{kind:"tool",name,callId,state,ok}` with
`state` `started` or `completed`. A tool update describes the tool's execution;
the stored transcript remains authoritative for complete output and replay.
If `documentsComplete` is false, an older branch lacks saved display records;
the client can use the existing `history --raw` renderer for that branch.

For `ask_user`, FYAI sends a `user/ask` request with the tool's `question`,
`options`, and optional `from`. The desktop responds on the same channel with
`{answer:string}`. An empty or absent answer leaves the question unanswered.

The protocol is invocation-local and does not change FYAI's arena format. A
desktop may reopen the same branch in a new invocation to select another model
or effort. Only one prompt runs at a time on a connection. The desktop must
keep numeric IDs sent by FYAI distinct from its own request IDs.

An explicit `--config` file can set `arena_dir` before FYAI reads branch
configuration. This permits a separate arena for the same working directory
when another FYAI revision cannot read the old arena format. `fyai init` uses
the selected arena directory.
