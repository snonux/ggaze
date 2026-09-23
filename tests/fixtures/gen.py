#!/usr/bin/env python3
"""Generate ggaze loader test fixtures (run manually; outputs are committed).

Produces, under the directory passed as argv[1] (or beside this script):
  plain.jpg   6x3 JPEG, EXIF Orientation = 1   -> loader yields 6x3
  rot6.jpg    8x4 JPEG, EXIF Orientation = 6   -> loader yields 4x8
  small.png   5x2 PNG (no orientation)          -> loader yields 5x2
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

The still fixtures use only the Python stdlib + cjpeg + exiftool; the
animated ones need Pillow (12.3 at generation time; the frame colours are
ANIM_FRAME_RGB below, which tests/test_animation.c and
tests/test_loader_pixbuf.c assert against, so keep both in step).
"""
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