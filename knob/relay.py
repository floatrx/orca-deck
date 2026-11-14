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
  relay → knob   M {volume, muted, battery, charging, cpu, tz}   the Mac (GET /api/mac) for Clock and the Mac
                                           remote, plus tz (the Mac's UTC offset, minutes); every MAC_S, on change
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
import json
import os
import pathlib
import sys
import threading
import time
import urllib.parse
import urllib.request

import serial
from serial.tools import list_ports

BRIDGE = "http://localhost:7352"
TOKEN = json.loads((pathlib.Path.home() / ".config/orca-deck/bridge.json").read_text())["token"]
ESPRESSIF_VID = 0x303A
POLL_S = 1.0
FULL_STATE_S = 30.0  # a full S even without a change, in case a line was lost
TEXT_CHARS = 100
MESSAGE_CHARS = 1200
MAC_S = 2.0  # GET /api/mac: CPU is measured between two calls, so not much faster


def log(text):
    print(time.strftime("%H:%M:%S"), text, file=sys.stderr, flush=True)


def bridge(method, path, body=None):
    request = urllib.request.Request(BRIDGE + path, method=method,
                                     data=json.dumps(body).encode() if body is not None else None,
                                     headers={"Authorization": "Bearer " + TOKEN, "Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=5) as response:
        return json.loads(response.read())


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


def encode(kind, payload):
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
        self.last_mac = None

    def send(self, data):
        with self.lock:
            self.write(data)

    def on_line(self, line):
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
            self.last_state = self.last_detail = self.last_mac = None
            return
        try:
            ack = bridge("POST", "/api/cmd", command)
        except Exception as error:  # the bridge is down
            ack = {"cmd": command.get("cmd", "?"), "ok": False, "note": str(error)[:80]}
        log(f"{self.name}: cmd {command.get('cmd')} → ok={ack.get('ok')} {ack.get('note') or ''}")
        self.send(encode("A", ack))

    def tick(self, state_line, mac_line=None):
        """Called once per poll with the encoded state (and the Mac line, when there is one)."""
        now = time.time()
        if mac_line and mac_line != self.last_mac:
            self.send(mac_line)
            self.last_mac = mac_line
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
                payload["tz"] = time.localtime().tm_gmtoff // 60  # the knob keeps UTC; its Clock shows the Mac's time
                mac_line = encode("M", payload)
            except Exception as error:  # an older bridge without /api/mac: the knob's Clock shows the time only
                log(f"mac: {error}")
                mac_line = None
        for link in list(links):
            try:
                link.tick(state_line, mac_line)
            except Exception as error:  # a write failed: the cable is out
                log(f"{link.name}: {error}")
                link.alive = False
            if not link.alive and link is usb:
                usb.close()
                links.remove(usb)
                usb = None
                log("knob gone from usb")
        time.sleep(POLL_S)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
