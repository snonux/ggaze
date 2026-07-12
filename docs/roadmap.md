# Roadmap

Rough milestones. Each ends in something runnable. Every plain-C module
ships with unit tests; overall coverage target ≥80% (gate in M10). Run the
`auditing-code-quality` audit at each milestone boundary and before release.

### M0 — Skeleton
- Meson build, `GtkApplication` with app ID, empty `GtkApplicationWindow`.
- Opens a file or folder from `argv`; `--version` / `--help`.
- `.desktop` stub, `meson install` lays out `/usr/bin` + `/usr/share/applications`.

### M1 — Show one image
- GdkPixbuf backend; load and display one image via the viewer widget.
- Honor EXIF Orientation on load (upright display).
- Zoom (fit / 100% / in / out) and pan when zoomed.
- Cursor-centered zoom.

### M2 — Walk the directory
- `navigator` lists siblings, filters by image MIME, sorts by name.
- `h`/`l` (and `←`/`→`) prev/next with wrap.
- Hide RAW sidecars by default (toggle to reveal); default sort = filename.
- Accept a **folder** arg → open the grid; file arg → open its parent folder
  with that file current.
- Drag-and-drop a file or folder onto the window to open it (`GtkDropTarget`).
- `GFileMonitor` on the current dir: external adds/deletes/moves update the
  listing live (debounced); removed current file falls back to nearest.
- Header bar shows `n / total` + filename.
- Cancel-in-flight load on rapid navigation.

### M3 — Responsive + prefetch
- Decode in `GTask` thread; UI never blocks.
- Prefetch next/previous into a 2–3 slot texture LRU.
- Bounded texture cache to cap memory.

### M4 — Fullscreen + slideshow + info
- Fullscreen toggle (`f`), auto-hiding header bar.
- Slideshow mode with configurable delay.
- Info overlay (`i`) with dimensions, format, EXIF (via libexif or GdkPixbuf).

### M5 — Modern formats
- Loader backend dispatcher + content sniffing.
- libjxl, libavif, libheif backends (meson feature options).
- Animated GIF/WebP playback.

### M6 — Progressive low-res preview
- libjpeg-turbo direct backend; low-res early scan shown, then refined.
- Loader streaming API finalized.

### M7 — Thumbnail cache + grid view
- freedesktop-compliant `~/.cache/thumbnails/`.
- Shared cache across windows; verify mtime.
- `gridview`: whole-folder thumbnail overview backed by the cache.
- Resizable thumbnails (`+`/`-`); grid reflows to fit the window; size
  persisted in GSettings `thumbnail-size`.
- `Enter` → large view, `Esc` → grid; cursor stays in sync.
- Dim-mark `./Trash`/deleted items; toggle to hide them.

### M8 — Selection, move, open-external & scripts
- Mark set in navigator: `v` toggle, `V` range, `Ctrl+a` all, `Esc` clear;
  shared by grid (check badge) and large (indicator).
- `mover` module + `destinations` GSettings key (`a(ss)`).
- `m` → GtkPopover listing destinations with auto-assigned hotkeys
  (`1`-`9`, `0`, then `a`-`z`); press to move the marked set (or current).
- `g_file_move` with collision suffixing; `u` undoes last move (or trash).
- `opener` module + `editors` GSettings key (`a(ss)`); `e` → popup of external
  programs; `GSubprocess` detached launch with `%f` expansion.
- `runner` module + `scripts` GSettings key (`a(ss)`); `!` → popup of shell
  scripts; async `/bin/sh -c` with `%f`/`%d` expansion; rescan dir on exit.
- `clipboard` helpers + `Ctrl+c`: copy current image as PNG, or marked files
  as `text/uri-list`, to `GdkClipboard` (paste into Katogram/GIMP/etc.).
- Preferences dialog (`,`) to edit destinations, editors, scripts, sort,
  background, etc.

### M9 — GEGL quick-enhance & export *(optional, feature-gated)*
- `enhancer` module + `enhance-presets` GSettings key (`a(ss)`); built-in
  presets: auto-fix, brightness, contrast, saturation, warm/cool, white
  balance, shadows, highlights, levels, curves, sharpen, denoise, clarity;
  optional grayscale, sepia, vignette, softglow. Strength tunable in the
  gegl-graph text.
- `a` → popup of presets (auto-assigned hotkeys); non-destructive live preview
  in the viewer (apply in a `GTask` thread; off the scrub hot path).
- `s` / menu *Save enhanced copy…* → GEGL saver writes `<name>-enhanced.<ext>`;
  original untouched. **No auto-save**: navigating away from an un-exported
  prompt prompts Save/Discard/Cancel; window tracks a dirty flag.
- Crop (`c`, `gegl:crop`), straighten (`R`, `gegl:rotate`) and rotate 90°
  (`[`/`]`, `gegl:rotate-on-center`) tools: interactive overlays or one-shot;
  aspect-ratio presets, angle nudge/horizon-drag; non-destructive; compose with
  presets.
- Compare: hold `Space` to view original, release for modified (before/after
  to decide on `s`).
- Color-managed decode/export via GEGL/babl + ICC (closes open question G).
- meson `gegl` feature option; graceful "GEGL not built in" toast.

### M10 — Polish & packaging
- AppStream metainfo, app icon (symbolic + full).
- `org.buetow.ggaze.desktop` registered as `image/*` handler.
- Fedora RPM spec; optional Flatpak manifest.
- Man page `ggaze(1)`.
- Settings: sort order, wrap, background, scroll behavior, slideshow delay,
  hide-trashed, destinations, editors, scripts, enhance-presets.
- Trash: `d` → `./Trash` folder (local, undoable via `u`); `D` → permanent
  `g_file_delete`. Menu action to empty `./Trash`.
- Persist & restore window geometry (GSettings width/height/maximized).
- **Coverage gate**: ≥80% unit-test coverage on plain-C modules (gcov/lcov);
  fails the build/CI below that.
- **Quality audit**: run `auditing-code-quality` (c-best-practices +
  find-code-bugs + SOLID + beyond-SOLID); resolve all HIGH/MEDIUM findings
  before release.
- Keyboard-completeness audit: every GUI element has a hotkey shown on it;
  mnemonics on all menu items; full keyboard traversal of dialogs.

### Later / maybe
- Configurable keybindings UI.
- Recursive directory walking.
- RAW embedded-preview display.
- Burst grouping (collapse near-identical frames in grid + large).
- GEGL transforms: lens correction, red-eye, scale (crop, straighten & rotate
  are in M9).
- GEGL artistic presets (vignette, sepia, B&W) as optional enhance entries.
- Go-to image # (count-prefix / jump).