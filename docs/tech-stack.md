# Tech Stack

## Core

| Concern        | Choice                                    | Notes                                   |
|----------------|-------------------------------------------|-----------------------------------------|
| Language       | C (C11)                                   | `-Wall -Wextra`, no glib-style boilerplate beyond what GTK needs |
| UI toolkit    | GTK4                                      | custom viewer widget; `GtkDropTarget` for file/folder drops |
| Native look   | libadwaita (decided)                     | GNOME styling; see decision #29 |
| Object/async  | GLib / GObject / GTask                    | bundled with GTK                         |
| Config        | GSettings + Gio                           | schema `org.buetow.ggaze` *(placeholder)* |
| Build          | Meson + Ninja                             | GNOME/Fedora standard                    |
| Packaging     | RPM (Fedora)                              | AppStream metainfo                       |
| Conventions   | c-best-practices skill                   | see [coding-conventions.md](coding-conventions.md) |
| Image proc.  | GEGL + babl *(optional, feature-gated)* | quick enhance, color mgmt, export copy; see [gegl.md](gegl.md) |
| Clipboard    | GdkClipboard + GdkContentProvider        | `image/png` (pixels) and `text/uri-list` (files) for `Ctrl+c` |
://| Folder watch | GFileMonitor (GIO)                       | live auto-refresh of the current dir (external adds/deletes) |
| Testing      | `meson test` + gcov/lcov                  | ≥80% on plain-C modules; see Testing below |
| Quality audit | `auditing-code-quality` skill            | at milestones (C: c-best-practices + find-code-bugs + SOLID + beyond-SOLID) |

## Image decode

Tiered: a sniffed-format dispatcher selects a backend; GdkPixbuf is the
fallback for anything common it already supports.

- **GdkPixbuf** (fallback) — PNG, JPEG, GIF, WebP, TIFF, ICO (first frame;
  animated GIF/WebP playback is planned).
- **libjpeg-turbo** (optional, direct) — faster JPEG + progressive first-scan
  low-res preview.
- **libjxl** — JPEG XL.
- **libavif** — AVIF.
- **libheif** — HEIF / HEIC (and AVIF via libheif if libavif absent).
- **GEGL loaders** (optional, if GEGL enabled) — `gegl:jpg/png/tiff/webp/…-load`
  can augment the enhance/export path and bring ICC-aware decode; JXL/AVIF/HEIF
  still need their own libs.
- **EXIF orientation** — every backend honors the EXIF Orientation tag so the
  decoded texture is upright (GdkPixbuf: `gdk_pixbuf_apply_embedded_orientation`;
  others read the tag via libexif and rotate/flip). Manual rotate/straighten
  compose on top.

Each backend behind a `GgazeLoaderBackend` struct:
`gboolean (*can_load)(const guint8 *head, gsize len);`
`GdkTexture *(*load)(GFile *file, GCancellable *, GError **);`
Backends compiled in conditionally via meson `feature` options so a minimal
build (GdkPixbuf only) is possible.

### The decode gate (task tb2)

**The glycin hazard.** On Fedora >= 41 gdk-pixbuf no longer decodes in
process: every format (JXL, AVIF, HEIF, and on Fedora 44 even JPEG/PNG) is
forwarded to a sandboxed `glycin` loader subprocess, and the pixbuf calls
that take a path or a `GdkPixbufLoader` (`gdk_pixbuf_new_from_file_at_scale`,
`gdk_pixbuf_get_file_info`, `gdk_pixbuf_loader_close`) block on it with **no
cancellable and no timeout** reachable from the caller. `glycin-jxl` was
measured to wait forever on *every* garbage or truncated JXL fed to it (an
8-byte codestream, a 60-byte one and a box-wrapped container alike);
`glycin-avif`/`-heif` and the raster loaders return promptly on bad input.
Because the large view, the thumbnail pool and the info worker all run
decodes on `GTask` threads that cannot be interrupted once gdk-pixbuf has the
file, one such file used to hang a worker for good.

Every loader entry point (`loader_load`, `loader_load_pixbuf_scaled`,
`loader_peek_dimensions`) therefore sniffs the first
`GGAZE_DETECT_SNIFF_LEN` (64) bytes -- read with `g_input_stream_read_all`,
so the sniff length is truly min(file, 64) even on a FIFO or GVFS stream --
and refuses, **in this order, before any decoder sees the file**:

1. an empty file -- `G_IO_ERROR_INVALID_DATA`;
2. a file shorter than the smallest complete file its signature's format
   allows (`detect_reject_truncated()`) -- `G_IO_ERROR_INVALID_DATA`;
3. any JXL when the `jxl` feature is off -- `G_IO_ERROR_NOT_SUPPORTED`,
   "JXL support is not built in (enable the jxl feature)".

1 and 2 are properties of the bytes and hold in every build; 3 is a property
of the build and only decides what happens to a file that could be complete.
The order is what keeps the truncated-file contract build-independent.

**How exact "before any decoder sees the file" is.** The gate is
`loader_sniff_bytes()`, a function of bytes; the entry points run it on the
first 64 bytes of one open. Where the gated bytes are the decoded bytes the
guarantee is exact: `loader_load()`'s pixbuf backend loads the whole file
once and gates *that* buffer before the `GdkPixbufLoader` sees it -- with
`loader_sniff_bytes_for_fallback()`, which is the gate plus the dispatch
rule: bytes that a format-specific backend of the build claims are refused
(`G_IO_ERROR_BUSY`, "<format> file changed while loading; try again" --
a status-line message, and a code a caller could tell from the gate's
`INVALID_DATA`/`NOT_SUPPORTED` and the backends' generic `FAILED`; no
caller matches it today, and `loader.c` notes why a future auto-reload
must not key on the code alone -- it is also GIO's mapping of `EBUSY`), since
the fallback can only be
holding them because the file changed between the dispatcher's sniff and
the backend's read. The dispatch rule is what makes the JXL refusal
build-independent rather than a property of the minimal build: with
libjxl the plain gate *admits* a JXL (the jxl backend decodes complete
ones), so a garbage JXL swapped in after the sniff would otherwise reach
the `GdkPixbufLoader` and hang glycin-jxl; with the rule it is refused in
every build (`tests/test_loader_pixbuf.c` calls `pixbuf_backend.load`
directly with garbage and with the smallest real JXLs and asserts the
refusal under both `GGAZE_HAVE_JXL` values). The thumbnail cache read
gates its entry the same way and decodes a PNG only (below). Where a
gdk-pixbuf call must take a **path** --
`gdk_pixbuf_new_from_file_at_scale()` in `loader_load_pixbuf_scaled()` and
`gdk_pixbuf_get_file_info()` in `loader_peek_dimensions()` -- the sniff is
one open and the decode another, and a file replaced in between (a rename
racing a thumbnail pass) reaches gdk-pixbuf unsniffed. That is a rename
away from the sniff on a local disk, not something an attacker steers, and
closing it would cost the at-scale JPEG path its 1/8 DCT decode; it is
documented, not closed.

**Per-signature minimum** (`detect.c`; signature plus the fixed-size
mandatory header structure, never entropy data, optional chunks or trailers,
so a valid file can never fall below it; every value <= 64 so a short sniff
buffer proves a short file):

| signature | minimum | counted structure |
|-----------|---------|-------------------|
| JPEG `FF D8 FF` | 25 | SOI + smallest SOF (13) + smallest SOS (10); no EOI |
| PNG | 33 | 8-byte signature + IHDR chunk (4+4+13+4) |
| GIF `GIF8` | 13 | header + logical screen descriptor |
| WebP `RIFF....WEBP` | 20 | RIFF/WEBP header + first chunk header |
| TIFF `II 2A 00` / `MM 00 2A` | 22 | header + entry count + one IFD entry |
| ICO `00 00 01 00` | 22 | ICONDIR + one ICONDIRENTRY |
| JXL codestream `FF 0A` | 8 | signature + bit-packed headers + 2 bytes of group data (smallest known real codestream is 12) |
| JXL container `....JXL ` | 44 | signature box + ftyp + jxlc box header + codestream minimum |
| AVIF/HEIF `ftyp` + `avif`/`avis`/`heic`/`heix`/`mif1` | 36 | ftyp + meta FullBox header + mdat header |

The gate is proven not to over-tighten against the smallest real 1x1 file of
every format (`tests/helpers/tiny_images.h`: lossless WebP 36 B, lossy WebP
42 B, GIF 43 B, JXL codestream 62 B, PNG 67 B, ICO 70 B, JXL container 102 B,
TIFF 123 B, JPEG 159 B), each of which must pass the gate and decode wherever
the build has a decoder.

**JXL without libjxl is refused, not attempted.** The minimum-length gate
closes only the truncated class; a JXL that is long enough but garbage still
hangs `glycin-jxl` forever, and a worker that can never be cancelled is worse
than a missing decode. So a minimal build (no `jxl` feature) never hands a
JXL -- bare codestream or container -- to gdk-pixbuf at all: a valid one
costs a "not built in" message, a garbage one costs nothing. AVIF/HEIF stay
on the gdk-pixbuf fallback because their glycin loaders fail fast.

**`loader_peek_dimensions()` order.** A JPEG's size comes from the
decoder-free SOF scan in `detect.c` (no gdk-pixbuf, no libjpeg decode, every
build), and that scan *decides* for two of its outcomes: a declared size
over the cap and a SOF lying past the 64 KB peek prefix both fail closed
(`FALSE`, gdk-pixbuf never asked -- the same verdict
`loader_load_pixbuf_scaled()` gives the same file; a header parse that
decodes no pixels is still a glycin sandbox spawn). A SOF declaring a zero
side (a DNL-deferred height, legal JPEG) is never reported as a size; it
and a malformed marker stream defer to gdk-pixbuf's header parse, whose own
`> 0` check then answers. A format a specific backend claims
(JXL/AVIF/HEIF) is sized by that backend's decode and *never* by
`gdk_pixbuf_get_file_info()`, which on a glycin desktop hangs on garbage JXL
and spawns a sandbox even for a valid one; only the rest use the gdk-pixbuf
header parse. The previous order asked gdk-pixbuf first, which hung the
info worker on garbage JXL even in a build with libjxl.
`tests/test_loader_pixbuf.c` proves the "never asked" part by counting the
opens of a regular temp file with an inotify watch (`IN_OPEN` together with
`IN_CLOSE_NOWRITE`, because inotify coalesces an event identical to the
tail of its unread queue and every open the loader makes is closed before
the next, so the queue alternates and nothing merges). The kernel queues
`IN_OPEN` inside `openat()` itself, so once `loader_peek_dimensions()` has
returned the count is exact -- no helper thread, no deadline, no ordering
assumption: two (sniff + SOF peek) for an oversized header, three when the
peek legitimately defers to gdk-pixbuf's header parse (zero-height and
SOF-less vectors), and flipping either verdict fails the count. An earlier
version counted FIFO writer sessions instead, which is only sound for a
file shorter than every read made of it (a file that exactly fills a
`read_all()` is satisfied without EOF, the reader closes first, and the
next open is served by the still-open session): it flaked ~1.5 % of runs
and could pass the oversized case with the wrong count. The FIFO harness
is kept for what it is sound for -- proving the two `read_all()` header
reads (sniff and SOF peek) cope with a file delivered in two chunks.

**The thumbnail cache read is gated too, on the bytes it decodes, and
bounded.** `~/.cache/thumbnails` is shared with every TMS-compliant app,
so an entry under ggaze's name is as untrusted as a source file:
`thumbnail.c` reads the entry into memory once -- chunk by chunk, abandoned
the moment it would exceed `GGAZE_THUMB_ENTRY_MAX_BYTES` (16 MiB, sixteen
times the raw RGBA of a 512 px square; `thumbnail.h`), so a planted
multi-GB entry costs one chunk past the cap and never a whole-file load
into the pool worker (an entry padded to exactly the cap is still served,
one byte more is regenerated: `test_oversize_entry_regenerated`) -- runs
`loader_sniff_bytes()` on that buffer, and decodes it through a
`GdkPixbufLoader` (`pixbuf_util_decode_bytes()`, the same routine the
pixbuf backend uses) only when the sniff says PNG -- a TMS entry is a PNG
by spec, so anything else is junk to regenerate, not to decode. gdk-pixbuf
never gets the entry's path, so there is no second open for a foreign
writer to race and no gdk-pixbuf sniff to disagree with ours; the
`Thumb::MTime`/`Thumb::URI` tEXt options come back through the loader
exactly as they did from `gdk_pixbuf_new_from_file()` (the persistence
tests assert on them). The length gate alone would not do here: a JXL
longer than its minimum but garbage still hangs `glycin-jxl`, in a build
with libjxl included.

**Minimal build.** `GGAZE_HAVE_ANY_BACKEND` (derived in `ggaze-config.h`
from the four `GGAZE_HAVE_*` loader flags) compiles the backend table, the
progressive dispatch and the backend-only thumbnail/info paths out of a
build with no loader feature, so the minimal CI lane carries no code that
no input could reach and its coverage figure for `loader.c` is honest.

**Error-domain change on the thumbnail path.** Because the sniff opens the
file before gdk-pixbuf does, a missing or unreadable file now surfaces from
`loader_load_pixbuf_scaled()` (and so from `thumbnail_get_finish()`) as a
`G_IO_ERROR` (`NOT_FOUND`, `PERMISSION_DENIED`, ...) from GIO, no longer as
the `G_FILE_ERROR` gdk-pixbuf's path-taking call used to raise. Nothing in
ggaze matched on `G_FILE_ERROR`; callers that only check the domain
`G_IO_ERROR` for "the thumbnail failed" are unaffected.

## Progressive preview (low-res first)

For large/slow images, ggaze shows a quick low-res or progressive scan before
the full frame is decoded. Concretely: the libjpeg-turbo backend yields a
downscaled scan after reading only the header + a few MCU rows; the loader API
streams a low-res `GdkTexture` first, then replaces it.

## Thumbnail cache

- freedesktop Thumbnail Managing Standard: `~/.cache/thumbnails/normal`
  (128×128) and `large` (256×256); shared with other compliant apps.
- Store PNG with `Thumb::URI`, `Thumb::MTime`, `Thumb::Size` keys.
- Verify before trust, and re-decode on any mismatch: `Thumb::MTime` must equal
  the source file's mtime (staleness), and `Thumb::URI`, when present, must
  equal the source URI — the entry is named only `md5(URI)`, so this is what
  stops a hash collision or another app's entry from displaying the wrong
  picture.
- **This is what makes thumbnails survive a restart**, so reopening a folder
  re-uses last run's PNGs instead of re-decoding every file. Deliberately *not*
  a private `.ggaze` directory next to the pictures: the shared cache is also
  populated by Nautilus/gthumb, and picture folders are often read-only or
  cloud-synced. See decision #41.
- Gotcha that cost us exactly that persistence until ix0: gdk-pixbuf takes tEXt
  keys as `tEXt::Thumb::MTime` in `gdk_pixbuf_save()` **and** hands them back
  under that same prefixed name from its PNG loader. Reading the unprefixed
  `Thumb::MTime` silently yields `NULL`, i.e. "stale", i.e. a cache that is
  written on every run and never read.

## Settings keys (GSettings schema `org.buetow.ggaze`)

- `sort`            — enum: name / capture-time / size
- `wrap`            — bool: wrap at folder ends
- `background`      — enum: black / dark / grey / checker
- `scroll-behavior` — enum: zoom / pan-when-zoomed / navigate
- `slideshow-delay` — double (seconds)
- `thumbnail-size` — int: grid thumbnail pixel size (resizable via `+`/`-`)
- `hide-trashed`    — bool
- `window-geometry` — `(iiib)` (width, height, fullscreen, maximized)
- `destinations`    — `a(ss)`: ordered array of `(name, path)` pairs for the
  `m` move popup. List order determines auto-assigned hotkeys (`1`, `2`, …).
  Edited via the Preferences dialog (`,`) or `gsettings`.
- `editors`          — `a(ss)`: ordered array of `(name, command)` pairs for
  the `e` open-in-external popup. Commands use freedesktop `Exec` placeholders
  (`%f` = current file path); e.g. `gimp %f`, `identify %f`. Launched detached
  via `GSubprocess` (GLib), so ggaze stays responsive.
- `scripts`          — `a(ss)`: ordered array of `(name, command)` pairs for
  the `!` run-script popup. Run **asynchronously** through `/bin/sh -c`
  (pipes/redirection/`~/` work). Placeholders: `%f` = current image path,
  `%d` = current folder (paths single-quoted against injection). On exit,
  ggaze rescans the directory.
- `enhance-presets` — `a(ss)`: ordered array of `(name, gegl-graph)` pairs
  for the `a` enhance popup (GEGL only). Order = auto-assigned hotkey order.
  Ships with built-in defaults.

## External deps (Fedora package names, approximate)

```
gtk4-devel   glib2-devel   libadwaita-devel (?)
gdk-pixbuf2-devel
libjpeg-turbo-devel   libjxl-devel   libavif-devel   libheif-devel
gegl-devel   babl-devel   # optional, feature-gated
meson   gcc
```

## Testing

- Unit tests for the plain-C modules — `navigator`, `detect`, `thumbnail`,
  `mover`, `opener`, `runner`, `enhancer`, `trash`, `settings` (no GTK/display
  needed); run with `meson test`.
- **Coverage target ≥80%** on those modules, measured with `gcov`/`lcov`
  (`meson setup -Db_coverage=true && meson test && ninja -C build coverage`).
  A coverage gate in CI rejects drops below 80%.
- GTK-side: `gtk-test`-style smoke tests where useful; mostly manual.
- A small `tests/fixtures/` image set covering each supported format.