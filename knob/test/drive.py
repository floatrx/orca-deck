# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial==3.5"]
# ///
"""Drives the knob's UI over USB with "T …" lines and prints what the knob reports (like Apex's DebugLaunch).

Run with the relay's USB side off (KNOB_RELAY_NO_SERIAL=1 in the LaunchAgent), so the data rides BLE only:
  uv run knob/test/drive.py "turn 1" "wait 1.5" "tap" "swipe left" "report"
Steps: any T verb (turn N · tap · hold · swipe left|right · select <wt>|<agent> · sound tick|buzz · ble-drop ·
report) or "wait <seconds>". Prints the knob's log lines.
"""
import sys
import time

import serial
from serial.tools import list_ports

port = next((p.device for p in list_ports.comports() if p.vid == 0x303A), None)
if not port:
    sys.exit("no knob on USB")
link = serial.Serial()
link.port, link.baudrate, link.timeout = port, 115200, 0.1
link.dtr = link.rts = False
link.open()
time.sleep(0.3)
link.reset_input_buffer()


def drain(seconds):
    end = time.time() + seconds
    buffer = b""
    while time.time() < end:
        buffer += link.read(4096)
    for line in buffer.decode("utf-8", "replace").splitlines():
        if not line.startswith(("ESP-ROM", "Build:", "rst:", "configsip", "SPIWP", "mode:", "load:", "entry", "Saved")):
            print(time.strftime("%H:%M:%S"), line, flush=True)


for step in sys.argv[1:]:
    if step.startswith("wait "):
        drain(float(step.split()[1]))
        continue
    link.write(f"T {step}\n".encode())
    drain(0.4)
drain(0.5)
