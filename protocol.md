# orca-deck protocol — v1

The contract between the bridge (`bridge/`, Hammerspoon on the Mac) and every client: the watch app,
the browser mock, the future ESP32 knob. **This file is the single source of truth.** When the bridge
and this file disagree, one of them is a bug.

Every message carries `v: 1`. A client that sees a higher `v` should show "update the app" instead of
guessing.

## Transports

| Transport | Address | Auth | Clients |
|---|---|---|---|
| WebSocket | `ws://localhost:7352/ws` | none — localhost only | browser mock, future knob |
| HTTP | `http://<host>:7352/api/*` | `Authorization: Bearer <token>` on every request | the watch (WebSocket is not allowed on watchOS — TN3135) |
| ntfy | `https://ntfy.sh/<topic>` | the secret topic name | the ntfy iOS app → mirrored to the watch |

The bridge binds `localhost` by default. `interface: "lan"` in the config binds every interface; in that
mode the WebSocket refuses every command except `hello`, because the WebSocket has no auth.

## Shared shapes

### Worktree

```jsonc
{
  "id": "…",              // Orca worktreeId
  "name": "main",         // Orca displayName, else the repo name
  "repo": "orca-deck",
  "branch": "main",       // without refs/heads/
  "active": true,         // the worktree Orca shows now
  "status": "…",          // Orca's worktree status, passed through
  "lead": Agent | null,   // the agent that most needs the user (see below); null when there are no agents
  "agents": [Agent, …]
}
```

Worktrees come in `orca worktree ps` order (the CLI does not expose the sidebar order).

### Agent

```jsonc
{
  "id": "…",              // Orca paneKey
  "kind": "claude",       // Orca agentType
  "chat": true,           // a native chat pane (paneKey starts with structured-agent-session)
  "state": "working",     // Orca's raw state, passed through
  "bucket": "working",    // normalised — see Buckets
  "interrupted": false,   // the user stopped the turn (Esc)
  "tool": "Bash",         // current tool, or null
  "prompt": "…",          // the last user prompt, one line, ≤ 90 characters
  "last": "…",            // the last assistant message, one line, ≤ 140 characters
  "since": 1761160000000  // when the agent entered `state`, epoch ms
}
```

### Buckets

| Orca `state` | `bucket` | Meaning |
|---|---|---|
| `working` | `working` | the agent is running |
| `blocked`, `waiting` | `attention` | the agent needs the user — a permission prompt or a question |
| `done` | `done` | the turn finished |
| anything else, or missing | `idle` | |

Source: Orca's `AGENT_STATUS_STATES = ['working', 'blocked', 'waiting', 'done']`
(`/Applications/Orca.app/Contents/Resources/app.asar.unpacked/out/shared/agent-status-types.js:29`);
`agent-session-attention.js:110` reports `blocked` when the agent awaits the user.
**Open check:** only `working` and `done` have been observed in `orca worktree ps --json` so far
(2026-10-22). Record the first observed `blocked` / `waiting` here.

### Lead agent

The lead is the agent with the highest urgency: `attention` > `working` > `done` > `idle`. Ties keep
Orca's order. Clients colour a worktree by `lead.bucket`.

### Event

```jsonc
{
  "type": "event", "v": 1,
  "seq": 1761160000042,   // see Sequence numbers
  "kind": "done",         // "done" | "attention"
  "worktreeId": "…",
  "worktree": "main",     // the worktree name, so a client can show it without a lookup
  "agentId": "…",
  "at": 1761160000000     // epoch ms
}
```

The bridge computes events, not the clients, so every client pulses for the same transitions.
It compares each agent's bucket with the bucket from the previous poll:

| Transition | Event | Beacon |
|---|---|---|
| any bucket → `attention` | `attention` | 2 haptics |
| `working` → `done`, and `interrupted` is false | `done` | 1 haptic |
| everything else, an agent's first appearance, an agent that disappears | none | |

An interrupted turn gives no `done`: the user pressed Esc, so the user is already at the Mac.

### Sequence numbers

`seq` starts at the bridge's boot time in epoch ms and grows by 1 per event. It therefore keeps
growing across bridge restarts. The bridge keeps the last 100 events.

## WebSocket

| Direction | Message |
|---|---|
| bridge → client | `{type:"state", v:1, seq, worktrees:[Worktree…]}` — pushed whenever the payload changes |
| bridge → client | `Event` — pushed when it happens |
| client → bridge | `{cmd:"hello"}` → the current `state` |
| client → bridge | `{cmd:"go", id}` · `{cmd:"turn", step:±1}` · `{cmd:"enter"}` · `{cmd:"tab"}` · `{cmd:"next"}` · `{cmd:"send"}` → `Ack` |

`Ack` = `{type:"ack", v:1, cmd, ok, note}`. `note` explains a failure (`"Orca is not running"`).

| Command | What Orca does |
|---|---|
| `go {id}` | jumps to that worktree with Cmd+1…9; the bridge learns which slot opens which worktree. The ack comes at once; the jump finishes asynchronously — watch `active` in the state |
| `turn {step}` | Next/Previous worktree (Ctrl+Opt+Cmd+↓/↑, remapped in Orca) |
| `enter` | Orca comes forward; focus returns to its composer |
| `tab` | Next tab (Cmd+Shift+]) — the next agent in the worktree |
| `next` | a → press (accepts the Next prefill) |
| `send` | a Return press (sends the composer) |

## HTTP

Every `/api/*` request needs `Authorization: Bearer <token>`. Without it, or with a wrong token:
`401 {"error":"unauthorized"}`. Responses are `application/json` with `Cache-Control: no-store`.

| Request | Response |
|---|---|
| `GET /api/ping` | `{v:1, ok:true, at}` — for latency probes |
| `GET /api/state` | `{v:1, seq, worktrees:[…], events:[]}` |
| `GET /api/state?since=<seq>` | `{v:1, seq, worktrees:[…], events:[Event… with seq > since]}` |
| `POST /api/cmd` body `{cmd, id?, step?}` | `Ack`, as on the WebSocket; `hello` is not a command here |
| anything else under `/api/` | `404 {"error":"not found"}` |

**The request returns at once — there is no long-poll.** Hammerspoon's `hs.httpserver` callback must
return its response synchronously, so holding a request open would freeze the bridge. Clients poll
about once a second.

Client loop:

1. First request without `since`: draw `worktrees`, store `seq`. No events — a fresh client does not
   replay old pulses.
2. Every next request sends `since=<stored seq>`: pulse for each event, draw `worktrees`, store the new
   `seq`.
3. If `since` is greater than the bridge's `seq` (the client's clock ran ahead of a restarted bridge),
   the bridge returns no events and its own `seq`; the client stores that `seq` and continues.

## ntfy

On each event the bridge publishes one message (JSON publishing, so UTF-8 names survive):

```jsonc
POST https://ntfy.sh/
{ "topic": "<secret topic>", "title": "main · orca-deck", "message": "needs you",
  "tags": ["warning"], "priority": 4 }
```

| Event | `message` | `tags` | `priority` |
|---|---|---|---|
| `attention` | `needs you` | `warning` | 4 (high) |
| `done` | `done` | `white_check_mark` | 3 (default) |

`title` is the worktree name, plus ` · <repo>` when the two differ. **No prompt or assistant text ever
goes to ntfy** — the topic is public infrastructure, and its secret name is the only protection.

## Config

`~/.config/orca-deck/bridge.json`, mode 600, created by the bridge on its first load:

```jsonc
{
  "token": "<48 hex characters>",     // the HTTP bearer token
  "ntfyTopic": "orca-deck-<24 hex>",  // subscribe to it in the ntfy iOS app
  "ntfy": true,                       // false = never publish
  "interface": "localhost"            // "localhost" | "lan"
}
```

The bridge reads it on every load. Change a value, then reload the bridge.
