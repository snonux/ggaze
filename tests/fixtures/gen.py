#!/usr/bin/env python3
"""Generate ggaze loader test fixtures (run manually; outputs are committed).

Produces, under the directory passed as argv[1] (or beside this script):
  plain.jpg   6x3 JPEG, EXIF Orientation = 1   -> loader yields 6x3
  rot6.jpg    8x4 JPEG, EXIF Orientation = 6   -> loader yields 4x8
  small.png   5x2 PNG (no orientation)          -> loader yields 5x2
  rgba.png    5x2 RGBA PNG                       -> has-alpha branch
  anim.gif    8x6 GIF, 4 solid frames, 100 ms each, loops forever (yb2)
  anim.webp   8x6 WebP, the same 4 frames, lossless, loops forever (yb2)
  zerodelay.gif anim.gif with every frame delay patched to 0 ms (yb2: both
              decoders turn that into 100 ms before ggaze sees it)
  fastdelay.gif anim.gif with every frame delay patched to 10 ms (yb2: the
              delay that does reach ggaze's 20 ms clamp -- as 10 ms on
              glycin, already raised to 20 ms by gdk-pixbuf 2.42's io-gif.c)
  once.gif    the frames of anim.gif without a NETSCAPE2.0 loop extension:
              plays once and holds its last frame (yb2)
  once.webp   the frames of anim.webp with an ANIM loop count of 1: plays
              once and holds its last frame (yb2)
  slow.gif    the frames of anim.gif at 500 ms each, looping forever (yb2:
              the viewer ticks only near a frame change, not every vblank)
  manyframes.gif 1001 frames of 8x6 alternating the first two colours, one
              over the playback frame cap (yb2: shown as its first frame)

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
  srgb-icc.jpg 8x8 JPEG, the same sRGB profile in APP2 (the JPEG side of
               that guard: byte-identical to the file without it).
  swapped-rot6.jpg 16x8 JPEG under the swapped profile, left half stored
               red and right half green, EXIF Orientation 6: upright 8x16,
               managed top half BLUE, bottom half green (the orientation
               permutation on the managed path).
  grey-icc.png 4x2 grey PNG, every pixel stored 128, under a grey profile
               with a LINEAR (gamma 1.0) curve: managed it displays ~188
               in sRGB, unmanaged 128 (grey -> sRGB working space).
  grey-icc.jpg 8x8 greyscale JPEG of the same, the profile in APP2.
  swapped-prog.jpg 16x16 PROGRESSIVE JPEG of a noise pattern under the
               swapped profile: every scan carries entropy data, so a test
               can cut the file between or inside scans (xb2 review 2).
  cmyk-icc.jpg 8x8 CMYK JPEG (ImageMagick) of C=0 M=Y=100% K=0 (red),
               under a hand-built lut8 CMYK -> Lab printer profile
               (write_cmyk_icc) that maps it to BLUE: managed through
               babl's LCMS it displays blue, unmanaged red; the chain runs
               in sRGB (CMYK -> sRGB working space).

The still fixtures use only the Python stdlib + cjpeg + exiftool, and
ImageMagick (magick) for the one CMYK JPEG, which cjpeg cannot write; the
animated ones need Pillow (12.3 at generation time; the frame colours are
ANIM_FRAME_RGB below, which tests/test_animation.c and
tests/test_loader_pixbuf.c assert against, so keep both in step).
"""
import math
import os
import struct
import subprocess
import sys
import zlib

# Frame i of the animated fixtures is solid ANIM_FRAME_RGB[i]. Distinct
# enough that a test can tell "the frame advanced" from one pixel, and each
# a palette entry of its own so the GIF frames are exact.
ANIM_FRAME_RGB = [(0, 255, 0), (60, 195, 40), (120, 135, 80), (180, 75, 120)]
ANIM_W, ANIM_H = 8, 6


def gif_color_table_len(packed):
    return 3 * (1 << ((packed & 7) + 1)) if packed & 0x80 else 0


def gif_skip_sub_blocks(data, pos):
    while pos < len(data):
        n = data[pos]
        pos += 1
        if n == 0:
            return pos
        pos += n
    return pos


def gif_set_delays(data, centis):
    """Return `data` (a GIF) with every Graphic Control Extension's delay set
    to `centis` (10 ms units). Pillow cannot write either fixture's delay
    itself: it drops the GCE entirely for duration=0, which a decoder reads
    as "no delay given" rather than the 0 ms delay real encoders write, and
    rounds 10 ms the same way; so the fixtures are anim.gif with its delays
    patched by this walk (the same block walk animation.c does in C)."""
    out = bytearray(data)
    pos = 13 + gif_color_table_len(out[10])
    while pos < len(out):
        block = out[pos]
        if block == 0x21:                       # extension
            if out[pos + 1] == 0xF9:            # GCE: size, packed, delay(2)
                out[pos + 4] = centis & 0xFF
                out[pos + 5] = centis >> 8
            pos = gif_skip_sub_blocks(out, pos + 2)
        elif block == 0x2C:                     # image descriptor
            pos += 10 + gif_color_table_len(out[pos + 9]) + 1
            pos = gif_skip_sub_blocks(out, pos)
        else:                                   # trailer
            break
    return bytes(out)


def write_animations(out):
    """The animated fixtures (see the module docstring). Pillow writes a
    NETSCAPE2.0 loop extension only when `loop` is given, and an ANIM chunk
    with the `loop` it is given (0 = for ever) for WebP."""
    from PIL import Image

    frames = [Image.new("RGB", (ANIM_W, ANIM_H), rgb) for rgb in ANIM_FRAME_RGB]

    def save_gif(name, **kw):
        frames[0].save(os.path.join(out, name), save_all=True,
                       append_images=frames[1:], **kw)

    def save_webp(name, loop):
        frames[0].save(os.path.join(out, name), save_all=True,
                       append_images=frames[1:], duration=100, loop=loop,
                       lossless=True)

    save_gif("anim.gif", duration=100, loop=0)
    save_gif("once.gif", duration=100)
    save_gif("slow.gif", duration=500, loop=0)
    # One frame over GGAZE_ANIM_MAX_FRAMES (1000): the over-budget still
    # path on a file that costs next to nothing to decode. Alternating
    # colours, so Pillow cannot merge neighbouring frames into one.
    many = [frames[i % 2] for i in range(1001)]
    many[0].save(os.path.join(out, "manyframes.gif"), save_all=True,
                 append_images=many[1:], duration=100, loop=0)
    save_webp("anim.webp", 0)
    save_webp("once.webp", 1)
    with open(os.path.join(out, "anim.gif"), "rb") as f:
        data = f.read()
    for name, centis in (("zerodelay.gif", 0), ("fastdelay.gif", 1)):
        with open(os.path.join(out, name), "wb") as f:
            f.write(gif_set_delays(data, centis))


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


def _icc_from_tags(cls, space, pcs, tags):
    """An ICC v2.1 profile of device class cls, colour space space and PCS
    pcs from (signature, data) tags: header, tag table, 4-byte aligned
    data."""
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
              + cls + space + pcs
              + struct.pack(">6H", 2026, 1, 1, 0, 0, 0)
              + b"acsp" + b"\0" * 4 + struct.pack(">I", 0)
              + b"\0" * 4 + b"\0" * 4 + b"\0" * 8
              + struct.pack(">I", 0)                   # intent: perceptual
              + _s15f16(D50[0]) + _s15f16(D50[1]) + _s15f16(D50[2])
              + b"\0" * 4 + b"\0" * 16 + b"\0" * 28)
    assert len(header) == 128
    return header + struct.pack(">I", len(tags)) + table + body


def write_icc(desc, r_xyz, g_xyz, b_xyz, trc):
    """A minimal ICC v2.1 RGB display profile: header + 9 tags."""
    return _icc_from_tags(b"mntr", b"RGB ", b"XYZ ", [
        (b"desc", _tag_desc(desc)),
        (b"cprt", _tag_text("ggaze test fixture, public domain")),
        (b"wtpt", _tag_xyz(*D50)),
        (b"rXYZ", _tag_xyz(*r_xyz)),
        (b"gXYZ", _tag_xyz(*g_xyz)),
        (b"bXYZ", _tag_xyz(*b_xyz)),
        (b"rTRC", trc),
        (b"gTRC", trc),
        (b"bTRC", trc),
    ])


def write_gray_icc(desc, gamma):
    """A minimal ICC v2.1 grey display profile: desc, wtpt, kTRC."""
    return _icc_from_tags(b"mntr", b"GRAY", b"XYZ ", [
        (b"desc", _tag_desc(desc)),
        (b"cprt", _tag_text("ggaze test fixture, public domain")),
        (b"wtpt", _tag_xyz(*D50)),
        (b"kTRC", _tag_curv_gamma(gamma)),
    ])


def _srgb_to_lab(r, g, b):
    """sRGB (0..1) -> CIE Lab (D50-adapted via the sRGB D50 primaries)."""
    lin = [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
           for c in (r, g, b)]
    xyz = [sum(m[i] * lin[j] for j, m in enumerate((SRGB_R, SRGB_G, SRGB_B)))
           for i in range(3)]

    def f(t):
        return t ** (1 / 3) if t > 216 / 24389 else (24389 / 27 * t + 16) / 116
    fx, fy, fz = (f(xyz[i] / D50[i]) for i in range(3))
    return 116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)


def _tag_lut8_cmyk_to_lab():
    """lut8Type (mft1) A2B0: 4 in, 3 out, a 2-point grid whose 16 corners
    are the Lab of the naive CMYK -> RGB of each corner with red and blue
    SWAPPED (as swapped.png's primaries are, so managed and unmanaged
    decodes tell apart: M+Y displays blue, not red); identity input and
    output tables, identity matrix (unused with a Lab PCS)."""
    ident = bytes(range(256))
    clut = bytearray()
    for c in (0, 1):
        for m in (0, 1):
            for y in (0, 1):
                for k in (0, 1):
                    L, a, b = _srgb_to_lab((1 - y) * (1 - k), (1 - m) * (1 - k),
                                           (1 - c) * (1 - k))
                    clut += bytes((round(L * 255 / 100),
                                   max(0, min(255, round(a + 128))),
                                   max(0, min(255, round(b + 128)))))
    matrix = b"".join(_s15f16(v) for v in (1, 0, 0, 0, 1, 0, 0, 0, 1))
    return (b"mft1" + b"\0" * 4 + bytes((4, 3, 2, 0)) + matrix
            + ident * 4 + bytes(clut) + ident * 3)


def write_cmyk_icc(desc):
    """A minimal ICC v2.1 CMYK output profile: desc, wtpt, A2B0 (lut8)."""
    return _icc_from_tags(b"prtr", b"CMYK", b"Lab ", [
        (b"desc", _tag_desc(desc)),
        (b"cprt", _tag_text("ggaze test fixture, public domain")),
        (b"wtpt", _tag_xyz(*D50)),
        (b"A2B0", _tag_lut8_cmyk_to_lab()),
    ])


def _png_chunk(typ, data):
    return (struct.pack(">I", len(data)) + typ + data
            + struct.pack(">I", zlib.crc32(typ + data) & 0xffffffff))


def write_png_icc(path, w, h, iccp_payload, pixel=b"\xff\x00\x00"):
    """PNG of solid pixels (RGB (255, 0, 0) by default; a one-byte pixel
    makes it greyscale) with an iCCP chunk whose zlib-compressed payload is
    iccp_payload (a profile, or garbage)."""
    sig = b"\x89PNG\r\n\x1a\n"
    ctype = 0 if len(pixel) == 1 else 2
    ihdr = struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0)
    raw = b"".join(b"\0" + pixel * w for _ in range(h))
    iccp = b"ggaze\0" + b"\0" + zlib.compress(iccp_payload)
    with open(path, "wb") as f:
        f.write(sig + _png_chunk(b"IHDR", ihdr) + _png_chunk(b"iCCP", iccp)
                + _png_chunk(b"IDAT", zlib.compress(raw))
                + _png_chunk(b"IEND", b""))


def _splice_app2(path, icc):
    """Put icc into the JPEG at path as one APP2 "ICC_PROFILE" segment right
    after SOI (where every writer puts it; the profiles here are far below
    the 64 KiB segment limit)."""
    with open(path, "rb") as f:
        jpg = f.read()
    assert jpg[:2] == b"\xff\xd8"
    payload = b"ICC_PROFILE\0" + bytes((1, 1)) + icc
    app2 = b"\xff\xe2" + struct.pack(">H", len(payload) + 2) + payload
    with open(path, "wb") as f:
        f.write(jpg[:2] + app2 + jpg[2:])


def write_jpg_icc(path, w, h, icc, pixels=None, grey=False, sample=None,
                  extra=()):
    """A JPEG via cjpeg of pixels (row-major RGB or grey bytes; solid
    (255, 0, 0) by default), with icc spliced in. sample="1x1" writes 4:4:4
    so neighbouring colours do not bleed (swapped.jpg predates the option
    and keeps cjpeg's default); extra holds further cjpeg options."""
    pnm = path + ".pnm"
    if pixels is None:
        pixels = b"\xff\x00\x00" * (w * h)
    with open(pnm, "wb") as f:
        f.write(b"P%d\n%d %d\n255\n" % (5 if grey else 6, w, h) + pixels)
    opts = (["-sample", sample] if sample else []) + list(extra)
    subprocess.run(["cjpeg", "-quality", "100"] + opts + ["-outfile", path,
                                                           pnm], check=True)
    os.remove(pnm)
    _splice_app2(path, icc)


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
    write_icc_fixtures_2(out, swapped, srgb)


def write_icc_fixtures_2(out, swapped, srgb):
    """The sRGB JPEG, orientation, grey and CMYK fixtures (xb2 review)."""
    write_jpg_icc(os.path.join(out, "srgb-icc.jpg"), 8, 8, srgb)
    rot = os.path.join(out, "swapped-rot6.jpg")
    halves = b"".join((b"\xff\x00\x00" if x < 8 else b"\x00\xff\x00")
                      for y in range(8) for x in range(16))
    write_jpg_icc(rot, 16, 8, swapped, pixels=halves, sample="1x1")
    subprocess.run(["exiftool", "-q", "-overwrite_original",
                    "-Orientation#=6", rot], check=True)
    linear = write_gray_icc("ggaze linear grey", 1.0)
    write_png_icc(os.path.join(out, "grey-icc.png"), 4, 2, linear,
                  pixel=b"\x80")
    write_jpg_icc(os.path.join(out, "grey-icc.jpg"), 8, 8, linear,
                  pixels=b"\x80" * 64, grey=True)
    cmyk = os.path.join(out, "cmyk-icc.jpg")
    subprocess.run(["magick", "-size", "8x8", "xc:cmyk(0,255,255,0)",
                    "-colorspace", "CMYK", "-quality", "100", "-strip",
                    cmyk], check=True)
    _splice_app2(cmyk, write_cmyk_icc("ggaze CMYK test"))
    # A progressive JPEG with real entropy data in every scan (a noise
    # pattern), for the crafted cut-between-scans cases (xb2 review 2).
    noise = bytes((x * 37 + y * 91 + c * 53) * 7 % 256
                  for y in range(16) for x in range(16) for c in range(3))
    write_jpg_icc(os.path.join(out, "swapped-prog.jpg"), 16, 16, swapped,
                  pixels=noise, sample="1x1", extra=("-progressive",))


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

    # the animated fixtures (Pillow)
    write_animations(out)

    # tidy intermediates
    for p in ("_plain.ppm", "_rot6.ppm"):
        try:
            os.remove(os.path.join(out, p))
        except FileNotFoundError:
            pass
    print("fixtures generated in", out)


if __name__ == "__main__":
    main()