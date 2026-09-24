# ggaze — GTK Gaze

A small, fast, native image viewer for Fedora Linux, written in C with GTK4.
Its job: **quickly preview a folder of pictures downloaded from a camera,
cull the rejects, move on.** Think `feh` / `nsxiv` / `qiv`, but GNOME-native
and KISS — no library, no database, no sidecars. The layout nods to gthumb
(header bar, thumbnail grid, full-window viewer) without the weight.

> **Status:** usable for its core job (browse, cull, move, open externally,
> run scripts, quick GEGL enhance, crop / straighten / rotate 90°). Animated
> GIF/WebP play in the large view; thumbnails and everything else use the
> first frame.

## Quick start

```
ggaze ~/Downloads/Camera/IMG_0001.jpg
```

`ggaze FOLDER` opens the folder as a grid; `ggaze FILE...` (several files,
or a multi-file drop onto the window) opens the first file's folder in the
grid with that file selected — the first entry decides the folder, the rest
only ask for the grid.

### Install on Fedora (local user + GNOME launcher)

```sh
./packaging/install-fedora.sh
```

Installs build deps (via `dnf`), builds, and installs into `~/.local` by
default — including `org.buetow.ggaze.desktop` and the `ggaze` app icon so
the app shows up in the GNOME Activities overview. Override with
`PREFIX=/usr/local` (needs write access) or skip packages with `SKIP_DEPS=1`.

Opens the folder as a thumbnail grid, `Enter` drops into the large view, and
you flip through the shoot:

Keyboard first: every action has a vi-style key and a traditional one
(`?` shows the full, always-current table). The most used:

| Key | Action |
|-----|--------|
| `h` / `l`, `←`/`→`, `PgUp`/`PgDn` | previous / next image |
| `g` / `G`, `Home` / `End` | first / last image |
| `Enter` / `Esc`    | grid → large / large → grid (`t` toggles; `Esc` twice in the grid quits) |
| `j` / `k`, `↓`/`↑` | cursor row down / up (grid) · pan (large) |
| `H` / `L`, `Shift+←`/`→` | pan left / right (large) |
| `+` / `-`, `Ctrl+±` | zoom in / out (large) · grow / shrink thumbnails (grid) |
| `0` / `Ctrl+0`     | zoom fit ↔ 100% (large) · reset thumbnail size (grid) |
| `i`               | toggle info overlay (EXIF + RGB/luminance histogram) |
| `v` / `V` / `Ctrl+a` | mark / range-mark / mark all |
| `d` / `Delete`     | trash to `.Trash` (undoable), then next |
| `D` / `Shift+Delete` | delete permanently |
| `u` / `Ctrl+z`     | undo last `d` or `m` |
| `E`               | empty this folder's `.Trash` (asks first) |
| `m`               | move marks → destination popup (`1`, `2`, …) |
| `e`               | open in external program popup |
| `!`               | run a shell script popup |
| `Ctrl+c`          | copy image (or marked files) to the clipboard |
| `a`, `1`–`8`, `0`  | quick GEGL enhance side panel · toggle preset · original (optional) |
| `c`               | crop tool (GEGL): drag or `h`/`l`/`j`/`k` move, `H`/`L`/`J`/`K` resize, `1`–`4` aspect, `0` free; `Enter` applies, `Esc` cancels |
| `R`               | straighten tool (GEGL): drag along the horizon or `h`/`l` nudge ±0.5°, `A` auto-crop; `Enter` / `Esc` |
| `]` / `[`         | rotate 90° clockwise / counter-clockwise (GEGL, non-destructive; repeat for 180°/270°) |
| `s` / `Ctrl+S`     | save an enhanced copy — presets, crop, straighten and rotation composed (original is never modified; a saved preview no longer prompts) |
| `Space` (hold)     | compare original vs modified |
| `f` / `F11`        | fullscreen |
| `S` / `F5`         | slideshow |
| `o` / `O`          | open image / open folder dialog (`Ctrl+o`, `Ctrl+Shift+o`) |
| `,`               | preferences |
| `F10`             | main menu |
| `?` / `F1`         | shortcuts overlay |
| `q` / `Ctrl+q`     | quit |

On a touchscreen: pinch to zoom, swipe left / right for the next / previous
image, tap with two fingers for the info card.

Full keybindings and mouse/touch gestures: `docs/ui-and-interactions.md`.

## Build

```sh
meson setup build
ninja -C build
meson test -C build          # all tests
meson test -C build --suite unit
meson test -C build --suite integration
```

A minimal build (GdkPixbuf-only, no GEGL, no libjpeg) is valid and fast,
and is what the minimal CI lane builds:

```sh
meson setup build -Dgegl=disabled -Djxl=disabled -Davif=disabled -Dheif=disabled -Djpeg=disabled
```

Enable optional backends with the matching feature option (`auto` by default):

```sh
meson setup build -Dgegl=enabled
```

Coverage (plain-C modules, ≥80% target):

```sh
meson setup -Db_coverage=true build-cov
ninja -C build-cov
meson test -C build-cov
ninja -C build-cov coverage     # needs lcov + genhtml
```

## Dependencies (Fedora)

```
meson ninja-build gcc pkgconf-pkg-config desktop-file-utils
gtk4-devel glib2-devel libadwaita-devel libexif-devel
# optional:
gegl04-devel babl-devel   libjpeg-turbo-devel
libjxl-devel   libavif-devel   libheif-devel
# checks (CI): clang-tools-extra (clang-format) xorg-x11-server-Xvfb
```

## Documentation

The full design lives in `docs/`:

- [`docs/PLAN.md`](docs/PLAN.md) — living tracker + decisions log
- [`docs/IMPLEMENTATION.md`](docs/IMPLEMENTATION.md) — execution plan (this repo's roadmap in practice)
- [`docs/goals-and-scope.md`](docs/goals-and-scope.md) — what ggaze is and is not
- [`docs/architecture.md`](docs/architecture.md) — modules + data flow
- [`docs/ui-and-interactions.md`](docs/ui-and-interactions.md) — views, keybindings, gestures
- [`docs/tech-stack.md`](docs/tech-stack.md) — libraries, decode backends, settings keys
- [`docs/coding-conventions.md`](docs/coding-conventions.md) — C style
- [`docs/gegl.md`](docs/gegl.md) — optional GEGL enhance plan
- [`docs/open-questions.md`](docs/open-questions.md) — undecided items

Contributing and agent workflow: see `AGENTS.md`.

## License

GPL-3.0-or-later. See [`LICENSE`](LICENSE).

