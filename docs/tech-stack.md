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
- Verify mtime before trust; re-decode on mismatch.

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