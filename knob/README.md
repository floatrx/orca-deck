# knob — orca-deck firmware for the Waveshare ESP32-S3 Knob (1.8")

The spike panel, grown into Apex's behaviour, as firmware for the ESP32-S3-Knob-Touch-LCD-1.8 (board label
"TAIJI KNOB 32"). It talks to the bridge over **BLE** through `KnobRelay.app` on the Mac; USB and Wi-Fi are
fallbacks.

| Folder / file | What |
|---|---|
| `deck/` | the firmware (below) |
| `deck/build.sh` | build, and `build.sh upload [port]` to flash (takes the relay agent down and back up) |
| `relay.py` | the relay: bridge ⇄ knob lines, over USB (pyserial) and over stdin/stdout for the app |
| `mac/` | `KnobRelay.app` (Swift, CoreBluetooth) + `install.sh` → LaunchAgent `com.floatrx.knob-relay` |
| `hello/` | the first test firmware (ring + crown + haptics) |
| `ble-spike/` | Phase 1 spike: python cannot own Bluetooth on macOS, an app bundle can |
| `libraries/lv_conf.h` | LVGL 8.4 config (pool in PSRAM; Montserrat 20 / 28 / 48 for symbols) |

## Hub and apps

The knob boots into the **hub**, a watchOS-style list over a dotted Earth turning edge to edge: the crown moves the
list, a tap opens the app, **swipe down in any app goes home** (its own vibration: a ramp down that lands on a
click). An app is an `App` in `deck.h` plus one line in `hub.cpp`. Plan: `.work/plans/knob-hub.md` (local).

| App | What it is for | Crown · tap · hold · swipe | Data |
|---|---|---|---|
| **Claude** | the agents: who works, who needs you, their Next prompts | workspace · show it in Orca · dictate · ← agent page / → its settings / ↑ next agent | bridge state |
| **Usage** | Claude's plan limits | — | bridge `oauth/usage` |
| **Day** | today on a 24 h dial: who worked, who waited on you | a cursor, 15 min a detent | bridge day log |
| **Focus** | one tap into deep work: Mac Focus + Pomodoro + quiet banners | — · on / off | Shortcuts |
| **Timer** | timer / Pomodoro, counts in the background | minutes · start/pause · reset · double tap: Pomodoro | knob only |
| **Mac** | volume and media keys, what plays | volume · play/pause · mute · ←/→ track | bridge `/api/mac` |
| **Jog** | the crown as a precise wheel for the Mac's front app | act · reset · — · ←/→ mode | bridge `jog` |
| **Wheel** | pick lunch / a break / yes-no by spinning | spin · spin · — · ←/→ list | knob only |
| **Clock** | the Mac's time, battery, CPU | — | bridge `/api/mac` |
| **Settings** | Connection, Wi-Fi, Screensaver, Haptics, Sound, Brightness | row · change · Connection: forget (2 s) | NVS |

**Everywhere:**
- **Boot**: the Earth with a spinner and "connecting…" until the Mac answers (5 s at most), then the list fades in.
- **Banner**: an agent turns needs-you or done while Claude is not on screen → a pill at the top for 3 s and a buzz;
  a tap on it opens Claude on that agent. Apps use it too (the Timer's end, Focus off).
- **Screensaver**: after Settings → Screensaver (off / 5 … 30 s / 1 min, default 15 s) without input in the hub or
  Settings, **Clawd** (the Claude Code banner's pixel creature) acts out the loudest agent state — idle, working,
  needs you, done, asleep without a link — with the plan limits round the rim (outer: the week, orange past 80 %;
  inner: 5 h) and "wk 80% · 1d 0h" under him. Any touch goes back to the hub. Never over Claude, Clock or the apps
  that are glanced at.
- **Every gesture vibrates**: the hub ticks for a gesture its app left silent. A silent knob is first Settings →
  Haptics = off (one tap from 3).
- The status line shows only problems (offline, a failed or dropped command). Fonts carry `→` but no `←` / `↓`:
  write directions in words.

**The apps in short:**
- **Claude** — "Claude" everywhere on the knob (orca-deck is the repo's name). Its hub badge is Clawd on black
  acting out the loudest state; its row reads "2 done · wk 80%". Pages below.
- **Dictation** (Claude, hold on an agent): speak freely; a round **equalizer** (64 bars, mirrored, magenta → blue
  → cyan) follows the voice; **tap when done** — the relay has it transcribed (OpenAI `gpt-4o-transcribe`, key in
  `~/.config/orca-deck/openai.key`, mode 600, never in the repo) and the text **goes to the agent at once**, shown as
  SENT for 1.2 s. A swipe while listening cancels — the only way not to send. Audio streams as IMA ADPCM (8 KB/s)
  while you speak, so the text is back ~1 s after the tap. The transcript never goes to a log.
- **Usage** — the week's dial in its severity's colour (blue / orange warning / red critical), % and time to the
  reset, an inner dial for the 5 h window (or a model limit such as Fable), and a stacked bar of what the week went
  on (Code / Cowork / Chats). From the bridge's `oauth/usage` call, the source of Orca's status bar.
- **Day** — midnight at the top, one ring per worktree (busiest outside), blue worked / orange waited, a white hand
  for now; the centre sums waited / worked. The bridge keeps the spans in `~/.config/orca-deck/day/<date>.json`
  (from when it started recording; `done` is not kept — it lingers until the next prompt).
- **Focus** — needs two Shortcuts, made once: "Knob Focus On" (*Set Focus → Do Not Disturb → On, until turned
  off*) and "Knob Focus Off" (*Off*). While on, a fresh done neither buzzes nor banners (needs-you still does);
  the end of the Pomodoro's break turns it off. A missing Shortcut shows in red.
- **Timer** — the end buzzes 3× and banners wherever you are.
- **Mac** — what plays comes from Music, Spotify or a YouTube tab in Chrome / Safari (macOS 15+ closed
  MediaRemote; macOS asks once to let Hammerspoon control the browser).
- **Jog** — Scroll, Undo scrub (⌘Z / ⇧⌘Z), Tabs (⌃Tab), Zoom (⌘= / ⌘−, tap ⌘0), Frame (← / →); a ruler turns one
  tick a detent, the centre counts ("-14 undos"). **It acts on whatever app is in front** — test with Scroll only.
- **Wheel** — friction slows the spin, a haptic tick per segment slows with it, the stop lands on a click with the
  winner. Lists (Ukrainian — content, not UI): Так / Ні, Магічна куля, Мерджимо?, Монетка, Обід, Перерва, Кубик.
  A segment "Покрути ще" spins the wheel again by itself when it wins.
- **Clock** — the time works offline (the relay sends the Mac's UTC offset, kept in NVS); its hub badge is a live
  analog face.

## Claude pages and gestures (as Apex: its settings · main · agent)

| | Main | Agent page | Claude settings |
|---|---|---|---|
| crown | workspace (Orca follows after 1 s) | row; the message when there is nothing to pick | row |
| tap | Orca shows this agent (`go` + `enter`) | tick a variant / Send / answer | change the value |
| hold | **dictate** to the agent | — | — |
| swipe up | next agent of the workspace | — | — |
| double tap | `send` (Enter in Orca) | — | — |
| swipe right | its settings | back | — |
| swipe left | agent page | — | back |
| swipe down | hub | hub | hub |

- **Main page:** a ring of dots (one per worktree, coloured by its lead agent), the project big, the branch
  small, the agent's last message. A comet runs round the rim while the agent works; the halo pulses while it
  needs you. Buzz + sound: 2 for needs-you, 1 for done.
- **Agent page:** the full last message, the Next variants with ticks (the first one ticked, marked •), one Send
  (`say`, several ticked go as a numbered list); a pending question shows its options and a tap answers it. While
  the agent works: no variants, a grey "Working…" with a spinner. After Send / an answer: back to the main page,
  turned to the next waiting agent.
- **Claude settings** (NVS): Hide inactive, Hide completed (off / 1 h / 1 day).

## Links

Priority **BLE → USB → Wi-Fi**. Wi-Fi turns on only after 10 s without BLE and USB, and off when one returns
(one radio for both). Lines on BLE / USB (`relay.py` docstring, `protocol.md` § Knob transports):
down `S` state · `K` keepalive · `D` agent detail · `A` ack · `M` the Mac and Claude's limits · `X` a transcript ·
`Y` the day; up `C` command · `V` dictation audio.

BLE: Nordic UART service, encrypted and bonded (Just Works). The knob refuses a new central once it has a bond,
unless Settings → Connection → forget opened pairing (2 min). The app talks only to the knob it pinned in
`~/.config/orca-deck/knob.json`.

## Setup

1. `brew install arduino-cli`; add the Espressif index; `arduino-cli core install esp32:esp32` (≥ 3.2.0);
   `arduino-cli lib install ArduinoJson@7.4.2 NimBLE-Arduino@2.5.2`.
2. Put LVGL 8.4 at `libraries/lvgl/` (Waveshare demo zip, `Arduino/libraries/lvgl`).
3. `cp deck/secrets.example.h deck/secrets.h` and fill it in (Wi-Fi is only the fallback).
4. Plug the USB-C so the Mac sees `USB JTAG/serial debug unit` (`/dev/cu.usbmodem*`) — the other orientation
   reaches the board's second chip. macOS asks "Allow accessory to connect?" the first time.
5. `knob/deck/build.sh upload`.
6. `knob/mac/install.sh --pair` the first time; macOS asks to allow Bluetooth for KnobRelay and shows
   "Connection Request from: orca-deck-knob" → Connect. Then `knob/mac/install.sh` (no `--pair`) keeps the pin.
   Every rebuild of the app changes its ad-hoc signature, so macOS asks for Bluetooth again.

Log: `~/Library/Logs/knob-relay.log` (the app and relay.py; with `install.sh --usb` also the knob's USB log).

## Testing on the device

`knob/test/drive.py` drives the UI over USB with `T …` lines (the firmware accepts them only on USB), while the
data rides BLE — the same as the cable being out for the bridge link:

```bash
uv run knob/test/drive.py "wait 9" "report" "turn 1" "turn -1" "wait 2" "tap" "swipe left" "wait 3" "report"
```

Verbs:

| Verb | Does |
|---|---|
| `turn N` · `tap` · `hold [ms]` · `swipe left\|right\|up\|down` | the hands (`hold 2500` = a 2.5 s hold) |
| `open <app>` · `home` | open an app by its hub name / go home |
| `report` | page (and the open app's own state), selection, agent, link, status |
| `select <worktree id>\|<agent id>` | the Claude page's choice, without `go` |
| `alert` · `tap banner` | the banner for the agent on screen / a tap on it |
| `dictate` · `subtitle` | the dictation screen's state / the Claude row and the limits |
| `saver off\|on\|now` | keep the screensaver out of a test (until the next reboot) / show it |
| `haptics [level]` | the level and the DRV2605 writes / set the level (saved) |
| `mic <s>` | record and stream the samples as `MIC` lines (`.work` scratch `miccap.py` makes a WAV) |
| `sound tick\|buzz\|test` · `ble-drop` · `wait <s>` | sounds / drop the BLE link / wait | Opening the port resets the S3, hence the first `wait 9`. Send tests go to a scratch chat session only.

**`turn` on Claude's main page moves Orca** (a `go` after 1 s): wait for the state (`wait 8` after boot) before
`swipe left`, or the crown meant for the agent page switches the user's active worktree.

**A dictation test cancels with `swipe left` while listening — never `tap`**: since the auto-send, the tap that
ends a dictation sends the transcript to the agent on screen for real (2026-11-14: a test tap sent «Перевіряємо.»
into a live session). To test the send, select a scratch chat session first (`select <worktree>|<agent>`).

**`hold 2500` on Settings → Connection forgets the bond for real** — the Mac keeps its key and every connect fails
with "Peer removed pairing information". Re-pair without rebuilding the app (a rebuild makes macOS ask for
Bluetooth again): System Settings → Bluetooth → orca-deck-knob → Forget; add `--pair` to the LaunchAgent's
`ProgramArguments` (`PlistBuddy -c "Add :ProgramArguments: string --pair"`), `launchctl bootout` + `bootstrap`,
click Connect, then delete the `--pair` entry and restart the agent again.

**A boot loop hides the port from esptool** ("No serial data received"): power the knob off, unplug USB-C for 5 s,
plug it back and power on, then upload at once.

`knob/test/stranger/` is a BLE central for the board's second chip (ESP32-U4WDH, the other USB-C orientation): it
tries to connect and write a command — the knob must drop it.

**Sound needs a 3.5 mm headphone or speaker:** the board has no speaker (Waveshare wiki: "You need to connect an
external 3.5mm headphone or speaker to achieve playback"). The haptics carry the feedback without one.

## Gotchas

- Internal RAM is tight with BLE + LVGL: the LVGL pool, the link tasks' stacks, NimBLE and every `malloc` over
  256 bytes live in PSRAM. Before that, internal heap was 92 bytes and `time()` aborted.
- `Wire` must not be used (the touch driver uses the legacy I2C driver; mixing aborts on core 3.x): the DRV2605
  is written directly.
- ArduinoJson `serializeJson(doc, someString)` replaces the String — build prefixes after.
- **`shared_mux` exists before any page is built** (`ui_init`'s first line): a page that paints in its create() reads
  `shared`. When link_init made it, boot looped on `xQueueSemaphoreTake queue.c:1709` three times.
- **Never touch NVS (Preferences) from a link task — not even a read**: their stacks are in PSRAM, any flash access
  turns the cache and PSRAM off, and `cache_utils.c:127` asserts — the knob rebooted every 3 s. Load at boot, flag
  changes, save from the LVGL task.
- macOS ignores a BLE security request from the knob: the app reads TX (READ_ENC) to start pairing.

## Third-party code

- `lcd_bsp.*`, `cst816.*`, `lcd_bl_pwm_bsp.*`, `lcd_config.h`, `esp_lcd_sh8601.*`, `bidi_switch_knob.*` come from the
  Waveshare ESP32-S3-Knob-Touch-LCD-1.8 demo package; `lcd_bsp.c` calls `ui_init()` instead of the LVGL demo.
  Audio pins and I2S setup in `sound.c` follow the demo's `07_Audio_Test`.
- `deck_*.c` are Montserrat (SIL Open Font License 1.1) converted with `lv_font_conv` 1.5.2: Latin + Cyrillic.
