# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial==3.5"]
# ///
"""A screenshot of the knob over USB: runs the steps (as drive.py), then "T shot", and writes a PNG.

  uv run knob/test/shot.py out.png "wait 10" "open Clock" "home" "saver now" "wait 2"

The knob answers SHOT <w> <h>, SHOTD <base64 RGB565 big-endian> lines, SHOT END. The PNG is cropped to the round
screen (outside the circle: transparent).
"""
import base64
import struct
import sys
import time
import zlib

import serial
from serial.tools import list_ports

out_path, steps = sys.argv[1], sys.argv[2:]
port = next((p.device for p in list_ports.comports() if p.vid == 0x303A), None)
if not port:
    sys.exit("no knob on USB")
link = serial.Serial()
link.port, link.baudrate, link.timeout = port, 115200, 0.1
link.dtr = link.rts = False
link.open()
time.sleep(0.3)
link.reset_input_buffer()
for step in steps:
    if step.startswith("wait "):
        end = time.time() + float(step.split()[1])
        while time.time() < end:
            link.read(4096)
        continue
    link.write(f"T {step}\n".encode())
    time.sleep(0.4)
link.reset_input_buffer()
link.write(b"T shot\n")

buffer, size, data, end = b"", None, bytearray(), time.time() + 30
while time.time() < end:
    buffer += link.read(65536)
    *lines, buffer = buffer.split(b"\n")
    done = False
    for line in lines:
        line = line.strip()
        if line.startswith(b"SHOT END"):
            done = True
        elif line.startswith(b"SHOTD "):
            data += base64.b64decode(line[6:])
        elif line.startswith(b"SHOT "):
            if b"failed" in line:
                sys.exit(line.decode())
            w, h = map(int, line.split()[1:3])
            size = (w, h)
    if done:
        break
if not size or len(data) < size[0] * size[1] * 2:
    sys.exit(f"incomplete shot: {len(data)} bytes")

w, h = size
rows = bytearray()
cx, cy, r2 = (w - 1) / 2, (h - 1) / 2, (w / 2) ** 2
for y in range(h):
    rows.append(0)
    for x in range(w):
        i = (y * w + x) * 2
        v = (data[i] << 8) | data[i + 1]
        red, green, blue = (v >> 11) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3
        rows += bytes((red | red >> 5, green | green >> 6, blue | blue >> 5, 255 if (x - cx) ** 2 + (y - cy) ** 2 <= r2 else 0))


def chunk(kind, payload):
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))


png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b"")
open(out_path, "wb").write(png)
print(f"{out_path}: {w}x{h}")
