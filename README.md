# orca-deck

A physical remote for Orca agents. The Mac runs several Claude Code agent
sessions in Orca; the remote shows them, selects one, focuses it in Orca, and buzzes when an agent
finishes or needs you.

The first remote is an **Apple Watch**: the Digital Crown scrolls the agents with one haptic tick per
row, a tap focuses that worktree in Orca, and the Taptic Engine is the beacon — 1 haptic for `done`,
2 for `needs you`.

```
orca-deck/
├── protocol.md   the contract between the bridge and every client — read this first
├── bridge/       Hammerspoon Lua bridge on the Mac (local 7352: WebSocket + mock; remote API 7353) and the browser mock
├── watch/        SwiftUI watchOS app
├── probe/        throwaway transport probe for the real watch
└── knob/         later — ESP32 firmware
```

## How it fits together

```
 Orca (Mac) ──orca worktree ps──▶ bridge (Hammerspoon) ──HTTP /api/*──▶ watch app
      ▲                             │  │                 ──WebSocket──▶ browser mock, knob
      └──── key presses ────────────┘  └──ntfy.sh──▶ ntfy iOS app ──▶ watch notification
```

- The **bridge** polls `orca worktree ps --json`, normalises agent states into buckets
  (`working | done | attention | idle`), and emits `done` / `attention` events.
- Clients send commands (`go`, `enter`, …); the bridge turns them into Orca's own key presses.
- With the wrist down, a watch app can neither poll nor play haptics, so the bridge also publishes each
  event to **ntfy**. The ntfy iOS app mirrors that notification to the watch.

## Requirements

- macOS with [Hammerspoon](https://www.hammerspoon.org) (it holds the Accessibility permission the key
  presses need) and its `hs` CLI.
- Orca with its `orca` CLI at `/opt/homebrew/bin/orca`.
- In Orca → Settings → Keyboard: **Next/Previous worktree remapped to Ctrl+Opt+Cmd+↓/↑** (the chat
  composer keeps the default Cmd+Shift+↓/↑ for text selection).
- For the watch app: Xcode, an Apple ID signed into Xcode (a free Personal Team is enough), the iPhone
  paired with Xcode, and Developer Mode on the iPhone and the watch. A free Personal Team build expires
  after 7 days — reinstall from Xcode weekly.
- For the wrist-down beacon: the [ntfy](https://ntfy.sh) iOS app, subscribed to your topic.

## Bridge

```bash
# load / reload
hs -c 'dofile("/Users/floatrx/Projects/Floatrx/orca-deck/bridge/bridge.lua")' </dev/null
# stop
hs -c 'deckBridge.stop()' </dev/null
# the browser mock
open http://localhost:7352/
```

On its first load the bridge writes `~/.config/orca-deck/bridge.json` (mode 600) with a random bearer
token and a random ntfy topic. See `protocol.md` § Config.

Port 7352 (mock, WebSocket, API) always stays on localhost. Port 7353 serves the token-protected API
only; it is the only port the watch, the LAN or a tunnel may reach. Never point a tunnel at 7352 — its
WebSocket has no auth and presses keys.

```bash
TOKEN=$(jq -r .token ~/.config/orca-deck/bridge.json)
curl -s -H "Authorization: Bearer $TOKEN" http://localhost:7352/api/state | jq
```

A synthetic event checks ntfy and the clients' haptics without waiting for an agent:

```bash
hs -c 'return deckBridge.testEvent("attention")' </dev/null   # or "done"
```

Trail of commands, events (with Orca's raw state) and active-worktree changes: `/tmp/orca-deck-bridge.log`.

Tests for the pure logic (`bridge/core.lua`), run inside Hammerspoon's Lua 5.4 without starting a server:

```bash
bridge/test/run.sh
```

### The spike bridge and this bridge — never both at once

This bridge grew from a throwaway spike (`…/nursehound/spike/orca-deck/bridge.lua`, port 7351, global
`orcaDeck`). Both press keys in Orca, so **only one may run**. This bridge refuses to start while the
spike's server runs. To switch:

```bash
hs -c 'orcaDeck.stop()' </dev/null      # stop the spike (port 7351)
hs -c 'dofile("/Users/floatrx/Projects/Floatrx/orca-deck/bridge/bridge.lua")' </dev/null

hs -c 'deckBridge.stop()' </dev/null    # and back
```

The spike does not know about this bridge — do not start the spike while this one runs.

## ntfy

1. Read the topic: `jq -r .ntfyTopic ~/.config/orca-deck/bridge.json`.
2. In the ntfy iOS app, subscribe to that topic on `ntfy.sh`.
3. Allow ntfy notifications on the watch (Watch app → Notifications → ntfy → Mirror my iPhone).

Notifications carry only the worktree name and `done` / `needs you` — never prompt text. The topic
name is the only protection, so keep it secret.

> **An unlocked iPhone takes the notification instead of the watch.** That is Apple's routing rule:
> a notification goes to the watch only while the iPhone is locked.
