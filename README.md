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

## Apex — the watch app

`watch/` holds **Apex**, the knob mock adapted to the watch's rounded-rectangle display: worktree dots ride a
track that follows the screen's shape, the chosen one at top centre. The crown moves them one worktree per
detent (one haptic tick each, wrapping round); the
centre shows the chosen worktree's state, name and what its agent is doing. A tap or Double Tap focuses it
in Orca; a long press shows the worktree's next agent. Events buzz the wrist: 1 haptic for `done`, 2 for
`needs you`, plus a shake of the dial.

**Swipe left** for the agent page: the agent's full last message (the crown scrolls it) and the **Next card**
pinned at the bottom — the user's own `Next:` suggestions (from the Stop hook's pending file). **Send** waits
3 s with an Undo, then delivers the prompt through Orca's own `agentSession.send` (`bridge/agent.cjs`): no key
presses, Orca's focus stays put. See `protocol.md` § Agent screen.

The app is alive: the background glows in the agent's colour, a comet runs the track while it works, sonar
rings leave the chosen dot while it needs you. Every motion follows the track: a crown step slides the dots
along it frame by frame (`Dial.slide`), never in a straight line. There is no system clock — the whole display
belongs to the app; the track's corners are concentric with the display (`GeometryProxy.concentricCornerRadii`,
watchOS 27; transient readings are ignored).

**Swipe right** for Settings: the main page's **Layout** (Knob or Cards — a scrolling list of worktree cards),
**Hide inactive** (worktrees without agents; on by default), bridge status, a picker for the "needs you"
haptic, haptic tests, and **Keep watching** — a mindfulness extended runtime session that keeps Apex awake with
the wrist down for up to an hour, so it can still buzz. watchOS mutes an app's haptics with the wrist down:
without Keep watching, an event that came then buzzes once on the wrist raise ("missed · …"), and ntfy is the
wrist-down beacon.

On each launch Apex writes two lines to the bridge trail (`Apex 0.3 online · …` and the knob's corner radii
with the canvas size), so the Mac can tell the watch got through.

Transport: **direct LAN** — the watch sends plain HTTP to `http://<Mac LAN IP>:7353`; with the iPhone
nearby, watchOS proxies it through the iPhone (measured: p50 67–83 ms). The watch alone, without the iPhone,
does not reach the bridge (a known watchOS failover issue). Details: `.work/notes/transport-probe.md`.

```bash
# 1. the bridge must expose its remote API on the LAN:  "interface": "lan" in ~/.config/orca-deck/bridge.json,
#    then reload the bridge
# 2. configure, build, install and launch on the paired watch, then wait for its hello in the bridge trail
#    (first time only: open watch/Apex.xcodeproj once, Run, Always Allow for the signing key)
watch/install.sh                   # or: watch/install.sh http://<host>:7353
```

A free Personal Team signs for **7 days**: run `watch/install.sh` again each week (it renews the profile).
Settings and the hello line show "signed until …".

Apex opens on the agent calling you — the latest one that needs you — and when a "needs you" came with the
wrist down, the wrist raise turns the knob to it: one swipe left shows its message and Next.
The icon source is `watch/Icon/apex.svg`; `watch/Icon/render.sh` regenerates the PNG.

The pure logic (protocol models, event cursor, beacon, selection, dial geometry) lives in the `watch/ApexCore`
Swift package and is tested on macOS. The crown, swipes, taps and settings are tested with XCUITest on the
watchOS simulator against the running bridge (`watch/configure.sh http://localhost:7353` first):

```bash
cd watch/ApexCore && swift test
cd watch && TEST_RUNNER_APEX_SCRATCH_SESSION=<scratch chat session id> xcodebuild test -project Apex.xcodeproj \
  -scheme Apex -destination 'platform=watchOS Simulator,name=<simulator>'
```

Launch arguments override a setting for one run: `-apex.layout cards`, `-apex.hideInactive NO`.

## ntfy

1. Read the topic: `jq -r .ntfyTopic ~/.config/orca-deck/bridge.json`.
2. In the ntfy iOS app, subscribe to that topic on `ntfy.sh`.
3. Allow ntfy notifications on the watch (Watch app → Notifications → ntfy → Mirror my iPhone).

Notifications carry only the worktree name and `done` / `needs you` — never prompt text. The topic
name is the only protection, so keep it secret.

> **An unlocked iPhone takes the notification instead of the watch.** That is Apple's routing rule:
> a notification goes to the watch only while the iPhone is locked.
