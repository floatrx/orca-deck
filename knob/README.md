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

The knob boots into the **hub**, a watchOS-style list of apps: the crown moves the list, a tap opens the app,
**swipe down in any app goes back to the hub**. Apps: orca-deck, Timer, Mac, Clock, Settings (`hub.cpp` holds the list; a new app is
an `App` in `deck.h` plus one line). Plan: `.work/plans/knob-hub.md` (local).

Behind the list a dotted Earth turns, edge to edge (`globe.cpp`; land mask from Natural Earth via
`knob/tools/earth_mask.py`). After 12 s without input in the hub
or Settings, **Clawd** (the Claude Code banner's pixel creature, `buddy.cpp`) takes the screen as the screensaver and
acts the loudest state out (idle, working, needs you, done, asleep without a link); a tap, the crown or a swipe goes
back to the hub. Never over orca-deck: that page is the one glanced at.

The link status line shows only when something is wrong (offline, a failed or dropped command); "bridge online"
stays hidden. A tap that ends a swipe is not a tap: LVGL sends a click on release even after a gesture.

**Timer** works on the knob alone and keeps counting in the background: crown sets the minutes (1–90), tap
starts / pauses, hold resets, tap twice switches Pomodoro (25 min focus → a 5 min break starts by itself). The end
buzzes 3× and shows a banner wherever you are; a tap on it opens the Timer.

**Mac** (remote): the crown sets the Mac's output volume (±2 % per detent; the knob moves at once, the steps go
in one command every 120 ms), tap = play / pause, swipe left / right = next / previous track, hold = mute; the
output device's name under the dial. No track title (macOS 15+ closed MediaRemote).

**Boot**: the Earth turns with a spinner and "connecting…" until the Mac answers (5 s at most), then the list fades
in from the centre. **Settings → Screensaver**: off / 5 / 10 / 15 (default) / 20 / 25 / 30 s / 1 min.

**Clock**: the Mac's local time and date, a seconds sweep round the rim, and the Mac's battery and CPU while the
link is alive (stale numbers say how old they are). The time works offline: the relay sends the Mac's UTC offset
and the knob keeps it in NVS.

When an agent turns needs-you or done while orca-deck is not on screen, a **banner** slides in at the top for 3 s
(project + state, in the state's colour) next to the usual buzz; a tap on it opens orca-deck on that agent (and
Orca follows), a tap below it goes to the app under it.

## orca-deck pages and gestures (as Apex: its settings · main · agent)

| | Main | Agent page | orca-deck settings |
|---|---|---|---|
| crown | workspace (Orca follows after 1 s) | row | row |
| tap | `enter` | tick a variant / Send / answer | change the value |
| hold | next agent of the workspace | — | — |
| double tap | `send` (Enter in Orca) | — | — |
| swipe right | its settings | back | — |
| swipe left | agent page | — | back |
| swipe down | hub | hub | hub |

- **Main page:** a ring of dots (one per worktree, coloured by its lead agent), the project big, the branch
  small, the agent's last message. A comet runs round the rim while the agent works; the halo pulses while it
  needs you. Buzz + sound: 2 for needs-you, 1 for done.
- **Agent page:** the full last message, the Next variants with ticks (the first one ticked, marked •), one Send
  (`say`, several ticked go as a numbered list); a pending question shows its options and a tap answers it. No
  variants while the agent works. After Send / an answer: back to the main page, turned to the next waiting
  agent.
- **orca-deck settings** (NVS): Hide inactive, Hide completed (off / 1 h / 1 day).
- **Settings app** (NVS): Connection (**hold 2 s**: forget the paired relay), Wi-Fi off / fallback, Haptics 0–3,
  Sound off / quiet / on, Brightness 40–100 %. The fonts carry `→` but no `←` / `↓`: write directions in words.

## Links

Priority **BLE → USB → Wi-Fi**. Wi-Fi turns on only after 10 s without BLE and USB, and off when one returns
(one radio for both). The status line names the live link. Lines on BLE / USB (`relay.py` docstring):
`S` state · `K` keepalive · `D` agent detail · `A` ack (down), `C` command (up).

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

Verbs: `turn N` · `tap` · `hold [ms]` · `swipe left|right|down` · `open <app>` · `home` · `alert` (the banner for the agent on
screen) · `tap banner` · `saver off|on|now` (off until the next reboot: keeps the screensaver out of a test) · `haptics` (level and
DRV2605 write count: a silent knob with `level=0` is Settings → Haptics off) · `select <worktree id>|<agent id>` · `sound tick|buzz|test` ·
`ble-drop` · `report` (page, selection, agent, link, status, age; the agent page's rows / focus / ticks), and
`wait <s>`. Opening the port resets the S3, hence the first `wait 9`. Send tests go to a scratch chat session only.

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
- **Never touch NVS (Preferences) from a link task — not even a read**: their stacks are in PSRAM, any flash access
  turns the cache and PSRAM off, and `cache_utils.c:127` asserts — the knob rebooted every 3 s. Load at boot, flag
  changes, save from the LVGL task.
- macOS ignores a BLE security request from the knob: the app reads TX (READ_ENC) to start pairing.

## Third-party code

- `lcd_bsp.*`, `cst816.*`, `lcd_bl_pwm_bsp.*`, `lcd_config.h`, `esp_lcd_sh8601.*`, `bidi_switch_knob.*` come from the
  Waveshare ESP32-S3-Knob-Touch-LCD-1.8 demo package; `lcd_bsp.c` calls `ui_init()` instead of the LVGL demo.
  Audio pins and I2S setup in `sound.c` follow the demo's `07_Audio_Test`.
- `deck_*.c` are Montserrat (SIL Open Font License 1.1) converted with `lv_font_conv` 1.5.2: Latin + Cyrillic.
