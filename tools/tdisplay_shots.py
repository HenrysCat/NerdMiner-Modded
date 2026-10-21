"""Fetch the T-Display Pulse screenshots from a dev build and save them as PNGs.

Build with -D TDISPLAY_UI_SCREENSHOT=1: the driver renders every screen once
at boot and serves each one, run-length encoded RGB332, at /shot?i=<n>.

    python tools/tdisplay_shots.py <miner ip> [out_dir] [scale]
"""
import os
import struct
import sys
import urllib.request
import zlib

W, H = 240, 135


def rgb332(v):
    r = (v >> 5) & 7
    g = (v >> 2) & 7
    b = v & 3
    return (r * 255 // 7, g * 255 // 7, b * 255 // 3)


def png(path, rows):
    h = len(rows)
    w = len(rows[0]) // 3
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    ip = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "."
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    os.makedirs(out, exist_ok=True)
    i = 0
    while True:
        try:
            r = urllib.request.urlopen("http://%s/shot?i=%d" % (ip, i), timeout=10)
        except urllib.error.HTTPError:
            break
        name = r.headers.get("X-Shot-Name", "shot%d" % i)
        data = r.read()
        px = bytearray()
        for k in range(0, len(data) - 1, 2):
            px += bytes([data[k + 1]]) * data[k]
        if len(px) != W * H:
            print("%s: %d pixels, expected %d" % (name, len(px), W * H))
            px = px[: W * H].ljust(W * H, b"\x00")
        rows = []
        for y in range(H):
            row = bytearray()
            for x in range(W):
                row += bytes(rgb332(px[y * W + x])) * scale
            for _ in range(scale):
                rows.append(row)
        path = os.path.join(out, "%s.png" % name)
        png(path, rows)
        print(path)
        i += 1


if __name__ == "__main__":
    main()
