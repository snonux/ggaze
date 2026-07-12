# ggaze — GNOME Gaze

A small, fast, native image viewer for Fedora Linux, written in C with GTK4.
Its job: **quickly preview a folder of pictures downloaded from a camera,
cull the rejects, move on.** Think `feh` / `nsxiv` / `qiv`, but GNOME-native
and KISS — no library, no database, no sidecars. The layout nods to gthumb
(header bar, thumbnail grid, full-window viewer) without the weight.

> **Status:** planning → skeleton. The design is complete in `docs/`; the
> implementation is just starting. Not usable yet.

## Quick start

```
ggaze ~/Downloads/Camera/IMG_0001.jpg
```

Opens the folder as a thumbnail grid, `Enter` drops into the large view, and
you flip through the shoot:

| Key | Action |
|-----|--------|
| `h` / `l`, `←`/`→` | previous / next image |
| `Enter` / `Esc`    | grid ↔ large view (`t` toggles) |
| `+` / `-`          | zoom in / out (large) · grow / shrink thumbnails (grid) |
| `0`               | zoom fit ↔ 100% (large) · reset thumbnail size (grid) |
| `i`               | info overlay (EXIF) |
| `v` / `V` / `Ctrl+a` | mark / range-mark / mark all |
| `d` / `D`          | trash to `./Trash` (undoable) / delete permanently |
| `u`               | undo last `d` or `m` |
| `m`               | move marks → destination popup (`1`, `2`, …) |
| `e`               | open in external program popup |
| `!`               | run a shell script popup |
| `a`               | quick GEGL enhance popup (optional) |
| `c` / `R` / `[` / `]` | crop / straighten / rotate 90° (GEGL, optional) |
| `s`               | save an enhanced/edited copy (original is never modified) |
| `Space` (hold)     | compare original vs modified |
| `f` / `S`          | fullscreen / slideshow |
| `o`               | open file/folder dialog |
| `,`               | preferences |
| `?`               | shortcuts overlay |
| `q`               | quit |

Full keybindings and mouse/touch gestures: `docs/ui-and-interactions.md`.

## Build

```sh
meson setup build
ninja -C build
meson test -C build          # all tests
meson test -C build --suite unit
meson test -C build --suite integration
```

A minimal build (GdkPixbuf-only, no GEGL) is valid and fast:

```sh
meson setup build -Dgegl=disabled -Djxl=disabled -Davif=disabled -Dheif=disabled
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
meson ninja-build gcc pkgconf-pkg-config
gtk4-devel glib2-devel libadwaita-devel
# optional:
gegl-devel babl-devel   libjxl-devel   libavif-devel   libheif-devel
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
- [`docs/roadmap.md`](docs/roadmap.md) — milestones
- [`docs/open-questions.md`](docs/open-questions.md) — undecided items

Contributing and agent workflow: see `AGENTS.md`.

## License

GPL-3.0-or-later. See [`LICENSE`](LICENSE).