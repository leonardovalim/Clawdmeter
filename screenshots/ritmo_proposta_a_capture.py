"""Capture Ritmo screenshot from COM4 -> RGB565 -> PNG (Windows-friendly, no ffmpeg)."""
import sys
import serial
from PIL import Image

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM4"
OUT = sys.argv[2] if len(sys.argv) > 2 else "ritmo_proposta_a.png"

port = serial.Serial(PORT, 115200, timeout=60)
port.reset_input_buffer()
port.write(b"screenshot\n")
port.flush()

w = h = raw_size = None
while True:
    line = port.readline().decode("utf-8", errors="replace").strip()
    if line.startswith("SCREENSHOT_START"):
        parts = line.split()
        w, h, raw_size = int(parts[1]), int(parts[2]), int(parts[3])
        break
    if line == "SCREENSHOT_ERR":
        print("device err", file=sys.stderr); sys.exit(1)

data = b""
while len(data) < raw_size:
    chunk = port.read(min(4096, raw_size - len(data)))
    if not chunk:
        print(f"timeout got {len(data)}/{raw_size}", file=sys.stderr); sys.exit(1)
    data += chunk

for _ in range(10):
    if port.readline().decode("utf-8", errors="replace").strip() == "SCREENSHOT_END":
        break
port.close()

img = Image.frombytes("RGB", (w, h), data, "raw", "BGR;16")
img.save(OUT)
print(f"saved {OUT} {w}x{h} ({len(data)} bytes)")
