# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial==3.5"]
# ///
"""USB transport for the knob: bridge state down the serial line, knob commands up to the bridge.

Lines on the wire (one JSON object per line, UTF-8):
  relay → knob   S {"worktrees": [...]}   the state, trimmed to the fields the knob shows
  knob → relay   C {"cmd": "go", ...}     a command, posted to /api/cmd
  relay → knob   A {"cmd": ..., "ok": ..., "note": ...}   the bridge's answer
Any other line from the knob is its debug log and is printed as is.

Run: uv run knob/relay.py   (stop it before flashing: it holds the serial port)
"""
import json
import pathlib
import sys
import threading
import time
import urllib.request

import serial
from serial.tools import list_ports

BRIDGE = "http://localhost:7352"
TOKEN = json.loads((pathlib.Path.home() / ".config/orca-deck/bridge.json").read_text())["token"]
ESPRESSIF_VID = 0x303A
POLL_S = 1.0
RESEND_S = 2.0  # the knob treats the link as gone after 3 s without a state line
LAST_CHARS = 140


def log(text):
    print(time.strftime("%H:%M:%S"), text, flush=True)


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
            agent["last"] = (a.get("last") or "")[:LAST_CHARS]
            agent["prompt"] = (a.get("prompt") or "")[:LAST_CHARS]
            agents.append(agent)
        worktrees.append({"id": w.get("id"), "name": w.get("name"), "repo": w.get("repo"),
                          "active": w.get("active", False), "agents": agents})
    return {"worktrees": worktrees}


def find_port():
    for port in list_ports.comports():
        if port.vid == ESPRESSIF_VID:
            return port.device
    return None


def open_port(device):
    # DTR/RTS stay low: toggling them through the USB-Serial-JTAG unit resets the ESP32-S3.
    link = serial.Serial()
    link.port, link.baudrate, link.timeout = device, 115200, 0.1
    link.dtr = link.rts = False
    link.open()
    return link


def reader(link, lock, stop):
    buffer = b""
    while not stop.is_set():
        try:
            chunk = link.read(4096)
        except serial.SerialException as error:
            log(f"serial read failed: {error}")
            stop.set()
            return
        buffer += chunk
        while b"\n" in buffer:
            raw, buffer = buffer.split(b"\n", 1)
            line = raw.decode("utf-8", "replace").rstrip("\r")
            if not line.startswith("C "):
                if line:
                    print("  knob │", line, flush=True)
                continue
            try:
                command = json.loads(line[2:])
                ack = bridge("POST", "/api/cmd", command)
            except Exception as error:  # the bridge is down or the line is garbled
                ack = {"cmd": "?", "ok": False, "note": str(error)[:80]}
            log(f"cmd {command.get('cmd')} → ok={ack.get('ok')} {ack.get('note') or ''}")
            with lock:
                link.write(b"A " + json.dumps(ack, ensure_ascii=False).encode() + b"\n")


def serve(device):
    link = open_port(device)
    log(f"knob on {device}")
    lock, stop = threading.Lock(), threading.Event()
    threading.Thread(target=reader, args=(link, lock, stop), daemon=True).start()
    last_line, last_sent = None, 0.0
    try:
        while not stop.is_set():
            try:
                line = json.dumps(trim(bridge("GET", "/api/state")), ensure_ascii=False, separators=(",", ":"))
            except Exception as error:
                log(f"bridge: {error}")
                time.sleep(POLL_S)
                continue
            if line != last_line or time.time() - last_sent >= RESEND_S:
                with lock:
                    link.write(b"S " + line.encode() + b"\n")
                last_line, last_sent = line, time.time()
            time.sleep(POLL_S)
    except serial.SerialException as error:
        log(f"serial write failed: {error}")
    finally:
        stop.set()
        link.close()
        log("knob gone")


def main():
    while True:
        device = find_port()
        if device:
            try:
                serve(device)
            except serial.SerialException as error:
                log(f"cannot open {device}: {error}")
        time.sleep(2)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
