# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial==3.5"]
# ///
"""The knob's relay: bridge state down to the knob, the knob's commands up to the bridge.

Lines on the wire (one JSON object per line, UTF-8):
  relay → knob   S {"worktrees": [...]}    the state, trimmed to what the knob shows; sent when it changes
  relay → knob   K {"now": ms}             keepalive, every second (the knob drops a link after 3 s of silence);
                                           "now" (also in S) sets the knob's clock: Wi-Fi NTP is off on BLE
  relay → knob   D {...AgentDetail...}     the agent the knob's agent page shows (protocol.md § AgentDetail)
  relay → knob   A {"cmd", "ok", "note"}   the bridge's answer to a command
  relay → knob   M {volume, muted, output, source, title, battery, charging, cpu, tz, cs, cw, csr, cwr, mi, mm, mv}
                                           the Mac (GET /api/mac) for Clock and the Mac remote, plus tz (the Mac's UTC
                                           offset, minutes); cs / cw: Claude 5 h / week limit %; csr / cwr: minutes to
                                           their reset; mi / mm / mv: the microphone's name, muted, input level %;
                                           every MAC_S, on change
  relay → knob   X {"text"} | {"error"}    the transcript of a dictation (V lines)
  relay → knob   Y {now, wa, wo, r:[[name, [[from, to, 0|1], …]], …]}   today's Agent Day Timeline (GET /api/day):
                                           minutes of the day; 0 working, 1 needs you; every DAY_S, on change
  relay → knob   W {n:[[ssid, pass], …], lan:[url, …]}   the knob's Wi-Fi networks and the bridge's LAN URLs
                                           (GET /api/wifi, localhost only); every WIFI_S, on change; never logged
  relay → knob   P {g:[[name, [prompt, …]], …]}   the Prompt Palette (GET /api/prompts); every PROMPTS_S, on change
  relay → knob   R {e, l, p:[[key, title, checks, mine, review, rerun], …]}   PR Radar (GET /api/prs): checks
                                           ok|fail|run|none, review approved|changes|""; e: the bridge's gh error,
                                           l: loaded once; every PRS_S, on change
  relay → knob   G {a, h, s, m, mt, al, an, bd, bc, pg:[{n, i:[{k, t, d, c, b}], hd, tp, e, ht, to, last}]}   one feed app
                                           (GET /api/feeds, bridge/feeds.lua): one line per app, on change; every
                                           FEEDS_S, and at once after an "app" command
  knob → relay   V {"start":true} · V <base64 IMA-ADPCM, 16 kHz mono> · V {"end":true} | {"cancel":true}
                                           dictation audio; on end the relay asks OpenAI (gpt-4o-transcribe, key in
                                           ~/.config/orca-deck/openai.key) and answers with X
  knob → relay   C {"cmd": ...}            a command for /api/cmd; {"cmd": "detail", "id"} stays here and
                                           picks the agent whose detail the relay polls ("" stops it);
                                           {"cmd": "hello"} (KnobRelay.app, on a BLE connect) resends S and D
Any other line from the knob is its debug log.

Transports, both at once:
  --serial   the knob on USB (Espressif VID 0x303A), found and re-found by itself (default on)
  --stdio    stdin/stdout — KnobRelay.app carries these lines over BLE (knob/mac)
Logs go to stderr: with --stdio, stdout is the wire.

Run: uv run knob/relay.py [--stdio] [--no-serial]   (stop it before flashing: it holds the serial port)
"""
import argparse
import base64
import io
import json
import os
import pathlib
import sys
import re
import threading
import time
import unicodedata
import urllib.parse
import urllib.request
import uuid
import wave

import serial
from serial.tools import list_ports

BRIDGE = "http://localhost:7352"
TOKEN = json.loads((pathlib.Path.home() / ".config/orca-deck/bridge.json").read_text())["token"]
ESPRESSIF_VID = 0x303A
POLL_S = 1.0
FULL_STATE_S = 30.0  # a full S even without a change, in case a line was lost
TEXT_CHARS = 100
MESSAGE_CHARS = 1200
MAC_S = 2.0
WIFI_S = 15.0  # GET /api/wifi: a network added on the Mac reaches the knob soon; a localhost call is cheap
PROMPTS_S = 30.0  # GET /api/prompts: the file is read on every call, so an edit shows up within this
PRS_S = 30.0  # GET /api/prs: the bridge asks GitHub every 60 s
FEEDS_S = 3.0  # GET /api/feeds: the bridge reads caches only; an app command asks again at once
FEEDS_ITEMS = 30
DAY_S = 60.0  # GET /api/day: the timeline moves a minute at a time  # GET /api/mac: CPU is measured between two calls, so not much faster


def log(text):
    print(time.strftime("%H:%M:%S"), text, file=sys.stderr, flush=True)


def bridge(method, path, body=None):
    request = urllib.request.Request(BRIDGE + path, method=method,
                                     data=json.dumps(body).encode() if body is not None else None,
                                     headers={"Authorization": "Bearer " + TOKEN, "Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=5) as response:
        return json.loads(response.read())


OPENAI_KEY_FILE = pathlib.Path.home() / ".config/orca-deck/openai.key"
TRANSCRIBE_MODEL = "gpt-4o-transcribe"
# Without a hint gpt-4o-transcribe heard Ukrainian as Czech ("Okej, testujeme"). The hint must not read like a
# sentence: a list of terms or a sample phrase came back as the transcript itself (measured 2026-11-14).
TRANSCRIBE_PROMPT = "Українська мова."

STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
         118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
         1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
         6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
         32767]
INDEX_SHIFT = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def until_minutes(iso):
    """Minutes from now until an ISO time ("2026-11-15T18:00:00.14+00:00"); None when there is none."""
    if not iso:
        return None
    try:
        from datetime import datetime, timezone
        return max(0, int((datetime.fromisoformat(iso.replace("Z", "+00:00")) - datetime.now(timezone.utc)).total_seconds() // 60))
    except ValueError:
        return None


def adpcm_to_wav(data):
    """The knob's IMA ADPCM (two 4-bit codes per byte, low nibble first, one stream from predictor 0) → WAV bytes."""
    predictor, index, pcm = 0, 0, bytearray()
    for byte in data:
        for code in (byte & 0x0F, byte >> 4):
            step = STEPS[index]
            delta = step >> 3
            if code & 4: delta += step
            if code & 2: delta += step >> 1
            if code & 1: delta += step >> 2
            predictor = max(-32768, min(32767, predictor - delta if code & 8 else predictor + delta))
            index = max(0, min(88, index + INDEX_SHIFT[code]))
            pcm += predictor.to_bytes(2, "little", signed=True)
    # Peak-normalise: the knob's fixed +18 dB still left quiet speech that gpt-4o-transcribe answered with its own
    # prompt (a known failure on faint audio, seen 2026-11-14).
    samples = memoryview(bytes(pcm)).cast("h")
    peak = max((abs(v) for v in samples), default=0)
    if peak:
        gain = min(16.0, 0.9 * 32767 / peak)
        pcm = bytearray(b"".join(max(-32768, min(32767, int(v * gain))).to_bytes(2, "little", signed=True) for v in samples))
    out = io.BytesIO()
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(bytes(pcm))
    return out.getvalue()


def transcribe(wav):
    """OpenAI speech to text. Returns (text, error)."""
    try:
        key = OPENAI_KEY_FILE.read_text().strip()
    except OSError:
        return "", "no OpenAI key on the Mac"
    boundary = uuid.uuid4().hex
    fields = {"model": TRANSCRIBE_MODEL, "prompt": TRANSCRIBE_PROMPT}
    body = b"".join(f"--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n{v}\r\n".encode()
                    for k, v in fields.items())
    body += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"knob.wav\"\r\n"
             "Content-Type: audio/wav\r\n\r\n").encode() + wav + f"\r\n--{boundary}--\r\n".encode()
    request = urllib.request.Request("https://api.openai.com/v1/audio/transcriptions", data=body, method="POST",
                                     headers={"Authorization": "Bearer " + key,
                                              "Content-Type": f"multipart/form-data; boundary={boundary}"})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            text = json.loads(response.read()).get("text", "")
        # On silence the model answers with its own prompt ("Українська мова." after 4 s of a quiet room).
        if text.strip(" .!?").lower() == TRANSCRIBE_PROMPT.strip(" .!?").lower():
            return "", "heard nothing"
        return text, ""
    except urllib.error.HTTPError as error:
        return "", f"OpenAI HTTP {error.code}"
    except Exception as error:
        return "", str(error)[:80]


def trim(state):
    """Keeps what the knob shows, so a line stays a few KB."""
    keep_agent = ("id", "kind", "chat", "bucket", "tool", "since")
    worktrees = []
    for w in state.get("worktrees", []):
        agents = []
        for a in w.get("agents", []):
            agent = {k: a[k] for k in keep_agent if k in a}
            agent["last"] = (a.get("last") or "")[:TEXT_CHARS]
            agent["prompt"] = (a.get("prompt") or "")[:TEXT_CHARS]
            agents.append(agent)
        worktrees.append({"id": w.get("id"), "name": w.get("name"), "repo": w.get("repo"),
                          "active": w.get("active", False), "agents": agents})
    return {"worktrees": worktrees}


def trim_detail(detail):
    keep = {k: detail[k] for k in ("id", "bucket", "next", "question") if k in detail}
    keep["message"] = (detail.get("message") or "")[:MESSAGE_CHARS]
    return keep


# The knob's fonts (knob/deck/deck_*.c, Montserrat + a few symbols) have ~300 glyphs; anything else draws as a box
# (user, 2026-11-16: «замість символа тупо квадратик»). Every text going down is mapped to what they have: the
# glyph list is read from the font file itself, so it cannot drift from the firmware.
FONT_FILE = pathlib.Path(__file__).resolve().parent / "deck" / "deck_16.c"
try:
    KNOB_GLYPHS = {int(h, 16) for h in re.findall(r"/\* U\+([0-9A-F]+) ", FONT_FILE.read_text())}
except OSError:
    KNOB_GLYPHS = set(range(0x20, 0x7F))
GLYPH_MAP = {
    "✅": "✓", "✔": "✓", "☑": "✓", "🟢": "●", "❌": "✗", "✖": "✗", "❎": "✗", "⛔": "✗", "🚫": "✗", "🔴": "●",
    "🟡": "●", "🟠": "●", "⚪": "●", "⚫": "●", "🔵": "●", "⚠": "!", "❗": "!", "❕": "!", "‼": "!!", "❓": "?",
    "❔": "?", "⏸": "||", "⏯": "▶", "▶": "▶", "⏹": "■", "⏺": "●", "⭐": "*", "★": "*", "☆": "*", "➜": "→",
    "➡": "→", "⇒": "→", "⟶": "→", "↪": "→", "←": "<-", "⇐": "<-", "⬅": "<-", "↔": "<->", "⬆": "↑", "⬇": "↓",
    "↗": "↗", "≈": "~", "≤": "<=", "≥": ">=", "≠": "!=", "×": "×", "÷": "/", "…": "…", "─": "-", "━": "-",
    "│": "|", "┃": "|", "├": "|", "└": "'", "┌": ",", "═": "=", "▪": "•", "◦": "•", "∙": "•", "‣": "•", "⁃": "-",
    "‐": "-", "‑": "-", "′": "’", "″": "”", "\u00a0": " ", "\u2009": " ", "\u200a": " ", "\u202f": " ",
}
DROP = re.compile("[\ufe0e\ufe0f\u200b-\u200d\u2060\U0001f3fb-\U0001f3ff]")


def drawable(text):
    return all(ord(c) < 0x7F or ord(c) in KNOB_GLYPHS for c in text)


def knob_text(text):
    """`text` with only what the knob's fonts can draw: known symbols mapped, emoji and other strays dropped."""
    if not isinstance(text, str) or drawable(text):
        return text
    out = []
    for ch in DROP.sub("", text):
        if ord(ch) < 0x7F or ord(ch) in KNOB_GLYPHS:
            out.append(ch)
        elif ch in GLYPH_MAP and drawable(GLYPH_MAP[ch]):
            out.append(GLYPH_MAP[ch])
        else:
            plain = "".join(c for c in unicodedata.normalize("NFKD", ch) if not unicodedata.combining(c))  # ǵ → g, ﬁ → fi
            if plain and drawable(plain) and unicodedata.category(ch)[0] == "L":
                out.append(plain)
            elif unicodedata.category(ch)[0] in "LN":
                out.append("?")  # a letter the knob cannot draw: a visible gap, not a box
            # symbols and emoji: gone
    return re.sub(r"(^|\n) +| {2,}", lambda m: m.group(1) if m.group(1) or m.start() == 0 else " ", "".join(out))


# Fields that the knob sends back as they are (ids, keys, paths): never rewritten.
KEEP_FIELDS = {"id", "k", "key", "worktreeId", "agentId", "itemId"}


def knob_safe(value, field=""):
    if isinstance(value, str):
        return value if field in KEEP_FIELDS else knob_text(value)
    if isinstance(value, list):
        return [knob_safe(v, field) for v in value]
    if isinstance(value, dict):
        return {k: knob_safe(v, k) for k, v in value.items()}
    return value


FEEDS_DUE = threading.Event()  # an "app" command ran: the feeds change now, not in FEEDS_S


def feed_lines(feeds):
    """One G line per feed app, trimmed so a line stays a few KB."""
    out = []
    for f in feeds:
        pages = []
        for p in (f.get("pg") or [])[:6]:
            page = {k: p[k] for k in ("n", "hd", "tp", "e", "ht", "to", "last") if p.get(k) not in (None, "")}
            page["i"] = [{k: v for k, v in i.items() if v not in (None, "")} for i in (p.get("i") or [])[:FEEDS_ITEMS]]
            pages.append(page)
        line = {"a": f["a"], "h": (f.get("h") or "")[:40], "s": (f.get("s") or "")[:60], "pg": pages}
        if f.get("m"):
            line["m"], line["mt"] = f["m"][:120], f.get("mt", 0)
        if f.get("bd"):
            line["bd"], line["bc"] = int(f["bd"]), int(f.get("bc") or 0xFF7A1A)
        if f.get("al"):
            line["al"], line["an"] = f["al"][:80], f.get("an", 0)
        out.append(encode("G", line))
    return out


def encode(kind, payload):
    if kind != "W":  # Wi-Fi names and passwords go exactly as they are
        payload = knob_safe(payload)
    return (kind + " " + json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n").encode()


class Link:
    """One way to reach the knob. Subclasses provide write() and feed lines to on_line()."""

    name = "link"

    def __init__(self):
        self.lock = threading.Lock()
        self.alive = True
        self.watch = ""          # the agent whose detail the knob wants
        self.last_detail = None
        self.last_state = None
        self.last_full = 0.0
        self.last_extra = {}     # the last M / Y / P / R line sent, by letter (G: by app): each goes again only on change
        self.voice = None        # dictation audio (ADPCM bytes) between V start and V end

    def send(self, data):
        with self.lock:
            self.write(data)

    def on_voice(self, body):
        if body.startswith("{"):
            event = json.loads(body)
            if event.get("start"):
                self.voice = bytearray()
            elif event.get("cancel"):
                self.voice = None
                log(f"{self.name}: dictation cancelled")
            elif event.get("end") and self.voice is not None:
                audio, self.voice = bytes(self.voice), None
                threading.Thread(target=self.finish_dictation, args=(audio,), daemon=True).start()
        elif self.voice is not None:
            self.voice += base64.b64decode(body)

    def finish_dictation(self, audio):
        started = time.time()
        text, error = transcribe(adpcm_to_wav(audio))
        log(f"{self.name}: dictation {len(audio) * 2 / 16000:.1f} s → {'error: ' + error if error else f'{len(text)} chars'}"
            f" in {time.time() - started:.1f} s")  # the text itself stays out of the log
        self.send(encode("X", {"error": error} if error else {"text": text}))

    def on_line(self, line):
        if line.startswith("V "):
            try:
                self.on_voice(line[2:])
            except Exception as error:  # a damaged line: the transcript may lose a word, the stream goes on
                log(f"{self.name}: voice line dropped: {error}")
            return
        if not line.startswith("C "):
            if line:
                log(f"knob ({self.name}) │ {line}")
            return
        try:
            command = json.loads(line[2:])
        except json.JSONDecodeError:
            log(f"{self.name}: garbled command line")
            return
        if command.get("cmd") == "detail":
            self.watch = command.get("id") or ""
            self.last_detail = None
            return
        if command.get("cmd") == "hello":  # a fresh BLE connection: send everything again at once
            self.last_state = self.last_detail = None
            self.last_extra = {}
            return
        try:
            ack = bridge("POST", "/api/cmd", command)
        except Exception as error:  # the bridge is down
            ack = {"cmd": command.get("cmd", "?"), "ok": False, "note": str(error)[:80]}
        log(f"{self.name}: cmd {command.get('cmd')} → ok={ack.get('ok')} {ack.get('note') or ''}")
        if command.get("cmd") == "app":
            FEEDS_DUE.set()
        self.send(encode("A", ack))

    def tick(self, state_line, extra_lines=()):
        """Called once per poll with the encoded state and the extra lines (M, Y, P, R) there are."""
        now = time.time()
        for line in extra_lines:
            key = line[:line.find(b",")] if line and line[:1] == b"G" else line[:1] if line else None
            if line and line != self.last_extra.get(key):
                self.send(line)
                self.last_extra[key] = line
        if state_line != self.last_state or now - self.last_full >= FULL_STATE_S:
            # "now" goes in after the comparison: the clock alone must not count as a change.
            self.send(state_line[:-2] + f',"now":{int(now * 1000)}}}\n'.encode())
            self.last_state, self.last_full = state_line, now
        else:
            self.send(encode("K", {"now": int(time.time() * 1000)}))
        if self.watch:
            try:
                detail = encode("D", trim_detail(bridge("GET", "/api/agent?id=" + urllib.parse.quote(self.watch))))
            except Exception as error:
                log(f"{self.name}: detail failed: {error}")
                return
            if detail != self.last_detail:
                self.send(detail)
                self.last_detail = detail


class SerialLink(Link):
    name = "usb"

    def __init__(self, device):
        super().__init__()
        # DTR/RTS stay low: toggling them through the USB-Serial-JTAG unit resets the ESP32-S3.
        self.port = serial.Serial()
        self.port.port, self.port.baudrate, self.port.timeout = device, 115200, 0.1
        self.port.dtr = self.port.rts = False
        self.port.open()
        threading.Thread(target=self.reader, daemon=True).start()

    def write(self, data):
        self.port.write(data)

    def reader(self):
        buffer = b""
        while self.alive:
            try:
                buffer += self.port.read(4096)
            except serial.SerialException as error:
                log(f"usb read failed: {error}")
                self.alive = False
                return
            while b"\n" in buffer:
                raw, buffer = buffer.split(b"\n", 1)
                self.on_line(raw.decode("utf-8", "replace").rstrip("\r"))

    def close(self):
        self.alive = False
        try:
            self.port.close()
        except Exception:
            pass


class StdioLink(Link):
    name = "ble"

    def __init__(self):
        super().__init__()
        threading.Thread(target=self.reader, daemon=True).start()

    def write(self, data):
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()

    def reader(self):
        for raw in sys.stdin.buffer:
            self.on_line(raw.decode("utf-8", "replace").rstrip("\r\n"))
        log("stdin closed: the app is gone")
        self.alive = False


def find_port():
    for port in list_ports.comports():
        if port.vid == ESPRESSIF_VID:
            return port.device
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stdio", action="store_true", help="carry lines over stdin/stdout (KnobRelay.app)")
    parser.add_argument("--no-serial", action="store_true", help="do not look for the knob on USB")
    args = parser.parse_args()
    # KNOB_RELAY_NO_SERIAL=1 (the LaunchAgent's environment) leaves the USB port to a test driver (knob/test).
    args.no_serial = args.no_serial or os.environ.get("KNOB_RELAY_NO_SERIAL") == "1"

    links = []
    if args.stdio:
        links.append(StdioLink())
    usb = None
    last_scan = 0.0
    mac_line, mac_at = None, 0.0
    day_line, day_at = None, -DAY_S
    prompts_line, prompts_at = None, -PROMPTS_S
    wifi_line, wifi_at = None, -WIFI_S
    prs_line, prs_at = None, -PRS_S
    feeds_lines, feeds_at = [], -FEEDS_S
    while True:
        if args.stdio and not links[0].alive:
            sys.exit(0)  # the app closed our stdin: it restarts us with the BLE link
        if not args.no_serial and usb is None and time.time() - last_scan >= 2:
            last_scan = time.time()
            device = find_port()
            if device:
                try:
                    usb = SerialLink(device)
                    links.append(usb)
                    log(f"knob on {device}")
                except serial.SerialException as error:
                    log(f"cannot open {device}: {error}")
        try:
            state = trim(bridge("GET", "/api/state"))
            state_line = encode("S", state)
        except Exception as error:
            log(f"bridge: {error}")
            time.sleep(POLL_S)
            continue
        if links and time.time() - mac_at >= MAC_S:
            mac_at = time.time()
            try:
                mac = bridge("GET", "/api/mac")
                payload = {k: mac[k] for k in ("volume", "muted", "battery", "charging", "cpu") if k in mac}
                payload["output"] = (mac.get("output") or "")[:40]
                playing = mac.get("playing") or {}
                payload["source"] = playing.get("source", "")
                payload["title"] = (playing.get("title") or "")[:100]
                payload["fe"] = (mac.get("focusError") or "")[:80]  # the Focus Shortcut failed: say why
                claude = mac.get("claude") or {}
                for key, field in (("session", "cs"), ("week", "cw")):  # Claude plan limits, %
                    if claude.get(key) is not None:
                        payload[field] = claude[key]
                payload["csv"] = claude.get("weekSeverity") or ""  # normal / warning / critical
                payload["cmod"] = "|".join(f"{m['name']} {m['percent']}" for m in claude.get("models") or [])
                payload["cbd"] = "|".join(f"{r['name']} {r['percent']}" for r in claude.get("breakdown") or [])
                for key, field, unit in (("sessionResets", "csr", 60), ("weekResets", "cwr", 60)):  # minutes left
                    left = until_minutes(claude.get(key))
                    if left is not None:
                        payload[field] = left
                payload["mi"] = (mac.get("mic") or "")[:40]  # the Mic Mute app
                payload["mm"] = bool(mac.get("micMuted"))
                if mac.get("micVolume") is not None:
                    payload["mv"] = mac["micVolume"]
                payload["tz"] = time.localtime().tm_gmtoff // 60  # the knob keeps UTC; its Clock shows the Mac's time
                mac_line = encode("M", payload)
            except Exception as error:  # an older bridge without /api/mac: the knob's Clock shows the time only
                log(f"mac: {error}")
                mac_line = None
        if links and time.time() - day_at >= DAY_S:
            day_at = time.time()
            try:
                day = bridge("GET", "/api/day")
                rows = [[r["name"][:24], [[s[0], s[1], 1 if s[2] == "a" else 0] for s in r["spans"][-60:]]]
                        for r in day.get("rows", [])]
                day_line = encode("Y", {"now": day["now"], "wa": day["waited"], "wo": day["worked"], "r": rows})
            except Exception as error:  # an older bridge: no timeline
                log(f"day: {error}")
                day_line = None
        if links and time.time() - wifi_at >= WIFI_S:
            wifi_at = time.time()
            try:
                wifi = bridge("GET", "/api/wifi")
                nets = wifi.get("networks") if isinstance(wifi.get("networks"), list) else []
                lan = wifi.get("lan") if isinstance(wifi.get("lan"), list) else []
                wifi_line = encode("W", {"n": [[n["ssid"], n.get("pass", "")] for n in nets[:8]], "lan": lan[:4]})
            except Exception as error:  # an older bridge: the knob keeps what it has
                log(f"wifi: {type(error).__name__}")  # never the body: it carries passwords
                wifi_line = None
        if links and time.time() - prompts_at >= PROMPTS_S:
            prompts_at = time.time()
            try:
                groups = bridge("GET", "/api/prompts").get("groups") or []
                prompts_line = encode("P", {"g": [[g["name"][:24], [p[:200] for p in g["prompts"][:12]]] for g in groups[:6]]})
            except Exception as error:  # an older bridge: no palette
                log(f"prompts: {error}")
                prompts_line = None
        if links and time.time() - prs_at >= PRS_S:
            prs_at = time.time()
            try:
                radar = bridge("GET", "/api/prs")
                prs_line = encode("R", {"e": (radar.get("error") or "")[:60], "l": bool(radar.get("loaded")), "p": [
                    [p["key"][:60], p["title"][:60], p["checks"], bool(p["mine"]), p.get("review") or "", bool(p["rerun"])]
                    for p in (radar["prs"] if isinstance(radar.get("prs"), list) else [])[:10]]})
            except Exception as error:  # an older bridge: no radar
                log(f"prs: {error}")
                prs_line = None
        if links and (time.time() - feeds_at >= FEEDS_S or FEEDS_DUE.is_set()):
            if FEEDS_DUE.is_set():
                time.sleep(0.4)  # the action's own result (a note, a mark in todo.md) lands first
                FEEDS_DUE.clear()
            feeds_at = time.time()
            try:
                feeds_lines = feed_lines(bridge("GET", "/api/feeds").get("feeds") or [])
            except Exception as error:  # an older bridge: no feed apps
                log(f"feeds: {error}")
                feeds_lines = []
        for link in list(links):
            try:
                link.tick(state_line, (mac_line, day_line, prompts_line, prs_line, wifi_line, *feeds_lines))
            except Exception as error:  # a write failed: the cable is out
                log(f"{link.name}: {error}")
                link.alive = False
            if not link.alive and link is usb:
                usb.close()
                links.remove(usb)
                usb = None
                log("knob gone from usb")
        FEEDS_DUE.wait(POLL_S)  # an app command cuts the wait short


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
