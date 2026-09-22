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
| Packaging     | RPM (Fedora), optional Flatpak            | AppStream metainfo                       |
| Conventions   | c-best-practices skill                   | see [coding-conventions.md](coding-conventions.md) |
| Image proc.  | GEGL + babl *(optional, feature-gated)* | quick enhance, color mgmt, export copy; see [gegl.md](gegl.md) |
| Clipboard    | GdkClipboard + GdkContentProvider        | `image/png` (pixels) and `text/uri-list` (files) for `Ctrl+c` |
://| Folder watch | GFileMonitor (GIO)                       | live auto-refresh of the current dir (external adds/deletes) |
| Testing      | `meson test` + gcov/lcov                  | ≥80% on plain-C modules; see Testing below |
| Quality audit | `auditing-code-quality` skill            | at milestones (C: c-best-practices + find-code-bugs + SOLID + beyond-SOLID) |

## Image decode

Tiered: a sniffed-format dispatcher selects a backend; GdkPixbuf is the
fallback for anything common it already supports.

- **GdkPixbuf** (fallback) — PNG, JPEG, GIF (animated via `GdkPixbufAnimation`),
  WebP, TIFF, ICO.
- **libjpeg-turbo** (optional, direct) — faster JPEG + progressive first-scan
  low-res preview.
- **libjxl** — JPEG XL.
- **libavif** — AVIF.
- **libheif** — HEIF / HEIC (and AVIF via libheif if libavif absent).
- **libpng** — only if GdkPixbuf path is insufficient (unlikely first cut).
- **GEGL loaders** (optional, if GEGL enabled) — `gegl:jpg/png/tiff/webp/…-load`
  can augment the enhance/export path and bring ICC-aware decode; JXL/AVIF/HEIF
  still need their own libs.
- **EXIF orientation** — every backend honors the EXIF Orientation tag so the
  decoded texture is upright (GdkPixbuf: `gdk_pixbuf_apply_embedded_orientation`;
  others read the tag via libexif and rotate/flip). Manual rotate/straighten
  compose on top; `s` export resets the tag to "normal" (1) to avoid
  double-rotation in other apps.

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
build); a format a specific backend claims (JXL/AVIF/HEIF) is sized by that
backend's decode and *never* by `gdk_pixbuf_get_file_info()`, which on a
glycin desktop hangs on garbage JXL and spawns a sandbox even for a valid
one; only the rest (and a JPEG whose SOF lies past the 64 KB peek prefix)
use the gdk-pixbuf header parse. The previous order asked gdk-pixbuf first,
which hung the info worker on garbage JXL even in a build with libjxl.

**Error-domain change on the thumbnail path.** Because the sniff opens the
file before gdk-pixbuf does, a missing or unreadable file now surfaces from
`loader_load_pixbuf_scaled()` (and so from `thumbnail_get_finish()`) as a
`G_IO_ERROR` (`NOT_FOUND`, `PERMISSION_DENIED`, ...) from GIO, no longer as
the `G_FILE_ERROR` gdk-pixbuf's path-taking call used to raise. Nothing in
ggaze matched on `G_FILE_ERROR`; callers that only check the domain
`G_IO_ERROR` for "the thumbnail failed" are unaffected.

## Progressive preview (low-res first)

For large/slow images, show a quick low-res or progressive scan before the full
frame is decoded. Concretely: libjpeg-turbo can yield a downscaled scan after
reading only the header + a few MCU rows; libjxl supports partial decode. The
loader API should allow streaming a low-res `GdkTexture` first, then replacing
it. Treat as a later milestone, but design the loader signature for it now.

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
- `window-geometry` — `(iiib)` (width, height, fullscreen, maximized); persisted, restored on launch
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