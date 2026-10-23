# orca-deck protocol — v1

The contract between the bridge (`bridge/`, Hammerspoon on the Mac) and every client: the watch app,
the browser mock, the future ESP32 knob. **This file is the single source of truth.** When the bridge
and this file disagree, one of them is a bug.

Every message carries `v: 1`. A client that sees a higher `v` should show "update the app" instead of
guessing.

**Absent means null.** The bridge is Lua, and Lua drops `nil` keys: a field written `X | null` below
may be missing from the JSON. Decode every such field as optional. Empty lists are always `[]`.

## Transports

| Transport | Address | Auth | Clients |
|---|---|---|---|
| WebSocket | `ws://localhost:7352/ws` | none — localhost only | browser mock, future knob |
| HTTP, local | `http://localhost:7352/api/*` | `Authorization: Bearer <token>` on every request | `curl`, local tools |
| HTTP, remote | `http://<host>:7353/api/*` | `Authorization: Bearer <token>` on every request | the watch (WebSocket is not allowed on watchOS — TN3135) |
| ntfy | `https://ntfy.sh/<topic>` | the secret topic name | the ntfy iOS app → mirrored to the watch |

**Two listeners.** Port **7352** always binds `localhost` and serves the mock page, the WebSocket and
`/api/*`. Port **7353** serves `/api/*` and nothing else — no WebSocket, no mock. Only 7353 may be
reached from outside the Mac: `interface: "lan"` in the config binds 7353 on every interface, and a
tunnel (`cloudflared tunnel --url http://localhost:7353`) points at 7353. The WebSocket has no auth and
presses keys, so it must never leave localhost.

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
  "lead": Agent | null,   // the agent that most needs the user (see below); absent when there are no agents
  "agents": [Agent, …]
}
```

Worktrees come in `orca worktree ps` order (the CLI does not expose the sidebar order). `name` falls back
to the repo, then to the id, so it is always present. An agent Orca reports without a paneKey is left out —
a client could not address it.

### Agent

```jsonc
{
  "id": "…",              // Orca paneKey
  "kind": "claude",       // Orca agentType
  "chat": true,           // a native chat pane (paneKey starts with structured-agent-session)
  "state": "working",     // Orca's raw state, passed through
  "bucket": "working",    // normalised — see Buckets
  "interrupted": false,   // the user stopped the turn (Esc)
  "tool": "Bash",         // current tool, or null (absent)
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
**Observed live:** `blocked` — first at 2026-10-22 23:12, 9 times by 00:35, e.g. while a Claude session
waited on a question for the user (bridge trail: `event attention (orca state blocked)`). `waiting` has
not been observed yet; it stays in the `attention` bucket on the strength of Orca's source.

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
| client → bridge | `{cmd:"hello", since}` → the current `state` plus `events:[Event… with seq > since]` |
| client → bridge | `{cmd:"go", id, pane?}` · `{cmd:"turn", step:±1}` · `{cmd:"enter"}` · `{cmd:"tab"}` · `{cmd:"next"}` · `{cmd:"send"}` → `Ack` |

**A push reaches one WebSocket client only** (`hs.httpserver:send()`; observed by the spike, see
`bridge/index.html` above `connect()`). So every WebSocket client also sends `hello {since}` about twice
a second, keeps the newest event `seq` it has handled, and skips events with a `seq` it has already seen.

`Ack` = `{type:"ack", v:1, cmd, ok, note}`. `note` explains a failure (`"Orca is not running"`).

| Command | What Orca does |
|---|---|
| `go {id, pane}` | shows that agent's tab — Orca's own `terminal.reveal` RPC (`bridge/reveal.cjs`): exact, any worktree, no key press, focus stays put. `pane` is the agent `id` (Orca paneKey `<tabId>:<leafId>`). Clients should send the lead's id |
| `go {id}` | without `pane` (a worktree with no agents): Orca's `worktree.activate` RPC — the worktree as it was last left. Same properties: exact, no key press, focus stays put |

Both `go` forms ack at once; the jump finishes asynchronously — watch `active` in the state.

| Command | What Orca does |
|---|---|
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
| `POST /api/cmd` body `{cmd, id?, pane?, step?}` | `Ack`, as on the WebSocket; `hello` is not a command here |
| `POST /api/cmd` with a body that is not a command | `400 {"error":"bad command"}` |
| `GET /api/agent?id=<agent id>` | `AgentDetail` (below); `404 {"error":"unknown agent"}` |
| `POST /api/cmd {cmd:"say", id:<agent id>, text}` | `Ack` with `note:"sending"` — the send runs in the background; its outcome appears as `send` in `AgentDetail` |
| `POST /api/log` body `{text}` | `{v:1, ok:true}` — appends `text` (≤ 4000 bytes) to the bridge trail. A diagnostic aid for clients without a console, such as the transport probe |
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

## Agent screen (Apex 0.2)

### AgentDetail — `GET /api/agent?id=<agent id>`

```jsonc
{
  "v": 1,
  "id": "structured-agent-session-claude_…:…",   // the agent id from the state
  "worktreeId": "…", "worktree": "main",
  "kind": "claude", "chat": true, "state": "done", "bucket": "done", "since": 1792708619802,
  "message": "…",       // the full last assistant message, its trailing `Next: ` lines removed
  "full": true,         // false: `message` is still Orca's 200-character cut, the full one is being fetched
  "next": ["…", "…"],   // the pending Next variants (≤ 5); [] when none or already used
  "send": { "at": 1792708619802, "ok": true, "note": "…" }   // the last say; absent before the first;
                                                            // ok absent while it is still sending
}
```

- The full message comes from Orca's `agentSession.history` (`bridge/agent.cjs history`). The bridge
  answers at once from a cache and refreshes it in the background (fresh for 3 s, and again whenever
  `since` changes), so the first request after a change may carry `full: false`. Poll again.
- `next` is read from `~/.claude/hooks/next-pending/<session id>`, the file the user's Stop hook writes
  per Orca chat session (`<session id>` is the part of the agent id between `structured-agent-session-`
  and `:`). Terminal agents have no such file: `next` is `[]`.

### say — `POST /api/cmd {cmd:"say", id, text}`

- Sends `text` (≤ 2000 characters) to that chat agent as a user prompt through Orca's own
  `agentSession.send` (`bridge/agent.cjs say`, text on stdin): no key presses, Orca's focus stays put.
- Refused at once with a note: no text, unknown agent, a terminal agent, Orca not running — and **over the
  WebSocket**: `say` needs the bearer token (`POST /api/cmd`), because any web page can open
  `ws://localhost:7352/ws` and the agents run with bypassed permissions.
- When the sent text equals a pending Next variant, the bridge deletes that pending file — the same thing
  Hammerspoon does when → inserts it, so the Mac does not offer a used Next again.
- The trail records `say <session> (<n> chars) → sent in <ms> ms | failed: <reason>` — never the text.

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
  "interface": "localhost"            // where port 7353 listens: "localhost" | "lan" (all interfaces)
}
```

The bridge reads it on every load. Change a value, then reload the bridge.
