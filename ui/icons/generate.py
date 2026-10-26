#!/usr/bin/env python3
"""Render the canonical artwork into the checked-in application icon formats."""
from pathlib import Path
import base64
import struct
import subprocess


root = Path(__file__).resolve().parent
source = (root / "app-source.png").read_bytes()
if source[:8] != b"\x89PNG\r\n\x1a\n":
    raise ValueError("The source artwork must be a PNG.")
width, height = struct.unpack(">II", source[16:24])
if width != height or width < 1024:
    raise ValueError("The source artwork must be square and at least 1024 pixels.")
svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}">'
       f'<image width="{width}" height="{height}" href="data:image/png;base64,'
       + base64.b64encode(source).decode("ascii") + '"/></svg>').encode("utf-8")
sizes = (16, 32, 48, 64, 128, 256, 512, 1024)
images = {}
for size in sizes:
    images[size] = subprocess.run(
        ["rsvg-convert", "--width", str(size), "--height", str(size)],
        input=svg, stdout=subprocess.PIPE, check=True).stdout
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

# Keep provider marks independent of optional runtime SVG plugins.
subprocess.run(["rsvg-convert", "--width", "64", "--height", "64", "--keep-aspect-ratio",
                "--output", str(root / "teamspeak.png"), str(root / "teamspeak.svg")], check=True)
