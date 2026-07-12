#!/usr/bin/env python3
"""Generate ggaze loader test fixtures (run manually; outputs are committed).

Produces, under the directory passed as argv[1] (or beside this script):
  plain.jpg   6x3 JPEG, EXIF Orientation = 1   -> loader yields 6x3
  rot6.jpg    8x4 JPEG, EXIF Orientation = 6   -> loader yields 4x8
  small.png   5x2 PNG (no orientation)          -> loader yields 5x2

Uses only the Python stdlib + cjpeg + exiftool.
"""
import os
import struct
import subprocess
import sys
import zlib


def write_ppm(path, w, h):
    # Distinct-ish RGB so frames are not blank.
    data = bytearray()
    for y in range(h):
        for x in range(w):
            data += bytes((x * 30 % 256, y * 60 % 256, (x + y) * 17 % 256))
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(bytes(data))


def write_png(path, w, h, alpha=False):
    sig = b"\x89PNG\r\n\x1a\n"
    color_type = 6 if alpha else 2   # 6 = RGBA, 2 = RGB
    channels = 4 if alpha else 3

    def chunk(typ, data):
        return (struct.pack(">I", len(data)) + typ + data
                + struct.pack(">I", zlib.crc32(typ + data) & 0xffffffff))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, color_type, 0, 0, 0)
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter: none
        for x in range(w):
            r = (x * 50) % 256
            g = (y * 90) % 256
            b = (x * 7 + y * 11) % 256
            if alpha:
                a = (255 - (x + y) * 30) % 256   # varied alpha
                raw += bytes((r, g, b, a))
            else:
                raw += bytes((r, g, b))
    idat = zlib.compress(bytes(raw))
    with open(path, "wb") as f:
        f.write(sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat)
                + chunk(b"IEND", b""))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(
        os.path.abspath(__file__))

    # plain.jpg: 6x3, orientation 1
    write_ppm(os.path.join(out, "_plain.ppm"), 6, 3)
    subprocess.run(["cjpeg", "-quality", "90", "-outfile",
                    os.path.join(out, "plain.jpg"),
                    os.path.join(out, "_plain.ppm")], check=True)
    subprocess.run(["exiftool", "-overwrite_original", "-Orientation#=1",
                    os.path.join(out, "plain.jpg")], check=True)

    # rot6.jpg: 8x4, orientation 6 (rotate 90 CW -> displayed 4x8)
    write_ppm(os.path.join(out, "_rot6.ppm"), 8, 4)
    subprocess.run(["cjpeg", "-quality", "90", "-outfile",
                    os.path.join(out, "rot6.jpg"),
                    os.path.join(out, "_rot6.ppm")], check=True)
    subprocess.run(["exiftool", "-overwrite_original", "-Orientation#=6",
                    os.path.join(out, "rot6.jpg")], check=True)

    # small.png: 5x2 RGB
    write_png(os.path.join(out, "small.png"), 5, 2, alpha=False)

    # rgba.png: 5x2 RGBA (exercises the has-alpha branch of texture_from_pixbuf)
    write_png(os.path.join(out, "rgba.png"), 5, 2, alpha=True)

    # tidy intermediates
    for p in ("_plain.ppm", "_rot6.ppm"):
        try:
            os.remove(os.path.join(out, p))
        except FileNotFoundError:
            pass
    print("fixtures generated in", out)


if __name__ == "__main__":
    main()