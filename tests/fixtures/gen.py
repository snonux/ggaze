#!/usr/bin/env python3
"""Generate ggaze loader test fixtures (run manually; outputs are committed).

Produces, under the directory passed as argv[1] (or beside this script):
  plain.jpg   6x3 JPEG, EXIF Orientation = 1   -> loader yields 6x3
  rot6.jpg    8x4 JPEG, EXIF Orientation = 6   -> loader yields 4x8
  small.png   5x2 PNG (no orientation)          -> loader yields 5x2
  rgba.png    5x2 RGBA PNG                       -> has-alpha branch

ICC colour-management fixtures (task xb2, decision #45). The profiles are
hand-built ICC v2 matrix/TRC profiles (write_icc below) so nothing outside
the stdlib is needed and the files stay tiny (~700 bytes each):
  swapped.png  6x3 PNG, every pixel stored (255, 0, 0), iCCP profile
               "ggaze swapped RGB" whose red and blue primaries are sRGB's
               SWAPPED: managed, it displays pure BLUE; unmanaged, red.
  swapped.jpg  8x8 JPEG of the same pixels with the same profile in APP2.
  badicc.png   6x3 PNG, pixels (255, 0, 0), iCCP that inflates to garbage
               (not an ICC profile): must fall back to sRGB, never fail.
  srgb-icc.png 6x3 PNG, pixels (255, 0, 0), an embedded profile that IS
               sRGB (primaries + parametric curve): must behave like an
               untagged file (regression guard for "sRGB stays as today").

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


def _s15f16(v):
    """ICC s15Fixed16Number (big-endian)."""
    return struct.pack(">i", int(round(v * 65536)))


def _tag_xyz(x, y, z):
    return b"XYZ " + b"\0" * 4 + _s15f16(x) + _s15f16(y) + _s15f16(z)


def _tag_desc(text):
    """ICC v2 textDescriptionType: ASCII + empty Unicode + empty ScriptCode."""
    ascii_ = text.encode("ascii") + b"\0"
    return (b"desc" + b"\0" * 4 + struct.pack(">I", len(ascii_)) + ascii_
            + struct.pack(">II", 0, 0) + struct.pack(">HB", 0, 0)
            + b"\0" * 67)


def _tag_text(text):
    return b"text" + b"\0" * 4 + text.encode("ascii") + b"\0"


def _tag_curv_gamma(gamma):
    """curveType with one u8Fixed8 entry = a plain gamma."""
    return (b"curv" + b"\0" * 4 + struct.pack(">I", 1)
            + struct.pack(">H", int(round(gamma * 256))))


def _tag_para_srgb():
    """parametricCurveType function 3: the sRGB piecewise curve."""
    g, a, b, c, d = 2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045
    return (b"para" + b"\0" * 4 + struct.pack(">HH", 3, 0)
            + b"".join(_s15f16(v) for v in (g, a, b, c, d)))


# sRGB primaries adapted to D50 (the values every sRGB ICC profile carries).
SRGB_R = (0.4361, 0.2225, 0.0139)
SRGB_G = (0.3851, 0.7169, 0.0971)
SRGB_B = (0.1431, 0.0606, 0.7141)
D50 = (0.9642, 1.0, 0.8249)


def write_icc(desc, r_xyz, g_xyz, b_xyz, trc):
    """A minimal ICC v2.1 RGB display profile: header + 9 tags."""
    tags = [
        (b"desc", _tag_desc(desc)),
        (b"cprt", _tag_text("ggaze test fixture, public domain")),
        (b"wtpt", _tag_xyz(*D50)),
        (b"rXYZ", _tag_xyz(*r_xyz)),
        (b"gXYZ", _tag_xyz(*g_xyz)),
        (b"bXYZ", _tag_xyz(*b_xyz)),
        (b"rTRC", trc),
        (b"gTRC", trc),
        (b"bTRC", trc),
    ]
    table = b""
    body = b""
    offset = 128 + 4 + 12 * len(tags)
    for sig, data in tags:
        padded = data + b"\0" * (-len(data) % 4)
        table += sig + struct.pack(">II", offset + len(body), len(data))
        body += padded
    size = offset + len(body)
    header = (struct.pack(">I", size) + b"\0" * 4
              + struct.pack(">I", 0x02100000)          # version 2.1
              + b"mntr" + b"RGB " + b"XYZ "
              + struct.pack(">6H", 2026, 1, 1, 0, 0, 0)
              + b"acsp" + b"\0" * 4 + struct.pack(">I", 0)
              + b"\0" * 4 + b"\0" * 4 + b"\0" * 8
              + struct.pack(">I", 0)                   # intent: perceptual
              + _s15f16(D50[0]) + _s15f16(D50[1]) + _s15f16(D50[2])
              + b"\0" * 4 + b"\0" * 16 + b"\0" * 28)
    assert len(header) == 128
    return header + struct.pack(">I", len(tags)) + table + body


def _png_chunk(typ, data):
    return (struct.pack(">I", len(data)) + typ + data
            + struct.pack(">I", zlib.crc32(typ + data) & 0xffffffff))


def write_png_icc(path, w, h, iccp_payload):
    """RGB PNG of solid (255, 0, 0) pixels with an iCCP chunk whose
    zlib-compressed payload is iccp_payload (a profile, or garbage)."""
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    raw = b"".join(b"\0" + b"\xff\x00\x00" * w for _ in range(h))
    iccp = b"ggaze\0" + b"\0" + zlib.compress(iccp_payload)
    with open(path, "wb") as f:
        f.write(sig + _png_chunk(b"IHDR", ihdr) + _png_chunk(b"iCCP", iccp)
                + _png_chunk(b"IDAT", zlib.compress(raw))
                + _png_chunk(b"IEND", b""))


def write_jpg_icc(path, w, h, icc):
    """Solid (255, 0, 0) JPEG via cjpeg, then the profile spliced in as one
    APP2 "ICC_PROFILE" segment right after SOI (where every writer puts
    it; the profile is far below the 64 KiB segment limit)."""
    ppm = path + ".ppm"
    with open(ppm, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h) + b"\xff\x00\x00" * (w * h))
    subprocess.run(["cjpeg", "-quality", "100", "-outfile", path, ppm],
                   check=True)
    os.remove(ppm)
    with open(path, "rb") as f:
        jpg = f.read()
    assert jpg[:2] == b"\xff\xd8"
    payload = b"ICC_PROFILE\0" + bytes((1, 1)) + icc
    app2 = b"\xff\xe2" + struct.pack(">H", len(payload) + 2) + payload
    with open(path, "wb") as f:
        f.write(jpg[:2] + app2 + jpg[2:])


def write_icc_fixtures(out):
    swapped = write_icc("ggaze swapped RGB", SRGB_B, SRGB_G, SRGB_R,
                        _tag_curv_gamma(2.2))
    write_png_icc(os.path.join(out, "swapped.png"), 6, 3, swapped)
    write_jpg_icc(os.path.join(out, "swapped.jpg"), 8, 8, swapped)
    write_png_icc(os.path.join(out, "badicc.png"), 6, 3,
                  b"this is not an ICC profile, only zlib-valid bytes " * 4)
    srgb = write_icc("ggaze sRGB test", SRGB_R, SRGB_G, SRGB_B,
                     _tag_para_srgb())
    write_png_icc(os.path.join(out, "srgb-icc.png"), 6, 3, srgb)


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

    # ICC colour-management fixtures (xb2)
    write_icc_fixtures(out)

    # tidy intermediates
    for p in ("_plain.ppm", "_rot6.ppm"):
        try:
            os.remove(os.path.join(out, p))
        except FileNotFoundError:
            pass
    print("fixtures generated in", out)


if __name__ == "__main__":
    main()