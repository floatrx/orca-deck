# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial==3.5", "numpy"]
# ///
"""Records the knob's real UI animations at an exact 30 fps (the firmware's demo recorder, `T rec`).

  uv run knob/test/rec.py <out dir> <seconds> "<t>:<verb>|<t>:<verb>" [steps before…]
  uv run knob/test/rec.py clips/hub 6 "1.0:turn 1|2.0:turn 1" "wait 10" "home"

The steps run first (as drive.py: T verbs, "wait <s>"), then `T rec`. While recording, the knob holds time: each frame
is one 1/30 s of UI time, rendered and sent before time moves on. Writes <out>/0000.png … (round-cropped, alpha
outside the screen) and <out>/events.json (haptic events: [ms, kind]) for the edit's clicks and vibration rings.
"""
import base64
import json
import os
import struct
import sys
import time
import zlib

import numpy as np
import serial
from serial.tools import list_ports

out_dir, seconds, script, steps = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
os.makedirs(out_dir, exist_ok=True)
port = next((p.device for p in list_ports.comports() if p.vid == 0x303A), None)
if not port:
    sys.exit("no knob on USB")
link = serial.Serial()
link.port, link.baudrate, link.timeout = port, 115200, 0.2
link.dtr = link.rts = False
link.open()
time.sleep(0.3)
link.reset_input_buffer()
for step in steps:
    if step.startswith("wait "):
        end = time.time() + float(step.split()[1])
        while time.time() < end:
            link.read(65536)
        continue
    link.write(f"T {step}\n".encode())
    time.sleep(0.4)
link.reset_input_buffer()
link.write(f"T rec {seconds} {script}\n".encode())


def png(path, rgba, w, h):
    raw = b"".join(b"\x00" + rgba[y].tobytes() for y in range(h))

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(raw, 3)) + chunk(b"IEND", b"")
    open(path, "wb").write(data)


buffer, size, events, frames, started = b"", None, [], 0, time.time()
mask = None
done = False
while not done and time.time() - started < 1800:
    buffer += link.read(1 << 20)
    *lines, buffer = buffer.split(b"\n")
    for line in lines:
        line = line.strip()
        if line.startswith(b"REC END"):
            done = True
        elif line.startswith(b"REC failed"):
            sys.exit(line.decode())
        elif line.startswith(b"REC "):
            w, h, fps, total = map(int, line.split()[1:5])
            size = (w, h)
            yy, xx = np.mgrid[0:h, 0:w]
            mask = ((xx - (w - 1) / 2) ** 2 + (yy - (h - 1) / 2) ** 2 <= (w / 2) ** 2).astype(np.uint8) * 255
            print(f"recording {total} frames at {fps} fps…", flush=True)
        elif line.startswith(b"EVT "):
            _, ms, kind = line.decode().split()[:3]
            events.append([int(ms), kind])
        elif line.startswith(b"FRM ") and size:
            _, idx, b64 = line.split(b" ", 2)
            rle = np.frombuffer(base64.b64decode(b64), dtype="<u2").reshape(-1, 2)
            px = np.repeat(rle[:, 1], rle[:, 0])
            w, h = size
            if px.size != w * h:
                print(f"frame {int(idx)}: {px.size} px, skipped", flush=True)
                continue
            v = px.byteswap().reshape(h, w).astype(np.uint32)  # the snapshot holds RGB565 big-endian
            r, g, b = (v >> 11) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3
            rgba = np.dstack([r | r >> 5, g | g >> 6, b | b >> 5, mask]).astype(np.uint8)
            png(os.path.join(out_dir, f"{int(idx):04d}.png"), rgba, w, h)
            frames += 1
            if frames % 30 == 0:
                print(f"  {frames} frames · {time.time() - started:.0f} s", flush=True)
json.dump(events, open(os.path.join(out_dir, "events.json"), "w"))
print(f"{out_dir}: {frames} frames, {len(events)} events, {time.time() - started:.0f} s")
