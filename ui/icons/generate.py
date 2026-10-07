#!/usr/bin/env python3
"""Render the canonical SVG into the checked-in desktop icon formats."""
from pathlib import Path
import struct
import subprocess


root = Path(__file__).resolve().parent
sizes = (16, 32, 48, 64, 128, 256, 512, 1024)
images = {}
for size in sizes:
    images[size] = subprocess.run(
        ["rsvg-convert", "--width", str(size), "--height", str(size), str(root / "app.svg")],
        stdout=subprocess.PIPE, check=True).stdout
    if images[size][:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("The SVG renderer did not produce a PNG.")
(root / "app.png").write_bytes(images[256])

chunks = []
for size, kind in ((16, b"icp4"), (32, b"icp5"), (64, b"icp6"), (128, b"ic07"),
                   (256, b"ic08"), (512, b"ic09"), (1024, b"ic10")):
    data = images[size]
    chunks.append(kind + struct.pack(">I", len(data) + 8) + data)
body = b"".join(chunks)
(root / "app.icns").write_bytes(b"icns" + struct.pack(">I", len(body) + 8) + body)

windows_sizes = sizes[:6]
header = struct.pack("<HHH", 0, 1, len(windows_sizes))
entries, payload = [], []
offset = len(header) + 16 * len(windows_sizes)
for size in windows_sizes:
    data = images[size]
    entries.append(struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset))
    payload.append(data)
    offset += len(data)
(root / "app.ico").write_bytes(header + b"".join(entries + payload))
