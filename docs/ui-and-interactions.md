# UI & Interactions

## Design language (gthumb, but KISS)

The look and layout are reminiscent of **gthumb**: a libadwaita header bar with
prev/next + zoom + view-toggle, a thumbnail grid like gthumb's browser grid,
and a full-window dark viewer like gthumb's viewer mode — familiar to GNOME
image-app users.

The **KISS** part — things gthumb has that ggaze deliberately drops:
- No folder/sidebar tree (single folder; you point ggaze at a path).
- No catalogs, albums, tags, or search bar.
- No status bar clutter — the header subtitle carries `n / total` + filename.
- No batch-tools or editing toolbars; quick enhance is a popup, not a pane.
- No location/path entry; the current folder is shown, not editable.

Optional, off by default: a slim one-line status footer (filename · zoom ·
size) for those who want it — toggled in settings, hidden otherwise.

## Window layout

- `GtkApplicationWindow` holding a `GtkStack` with two views:
  - **Grid view** — `GgazeGrid`: a flow/grid of thumbnails for the whole folder.
  - **Large view** — `GgazeViewer`: the single-image canvas, fills the window.
- An overlay on top (auto-hide) carries the header bar — title = filename,
  subtitle = `n / total` (remaining) · `N marked` when marks exist.
- Header bar actions (icons): prev, next, zoom-fit, zoom-100, toggle-grid,
  open-file, slideshow, menu (sort, background, empty-`./Trash`, about).
- No sidebar, no tree, no toolbar beyond the header bar.
- Transparent background behind the image (configurable: black / dark / grey
  / checkered for transparency).

## Opening files & folders

- **CLI arg**: `ggaze IMG_0001.jpg` opens that file (large view, its folder as
  the navigator); `ggaze ~/Downloads/Camera/` opens the folder directly in the
  grid.
- **Drag-and-drop**: drop a file or a folder onto the window to open it — a
  folder opens the grid; a file opens it (large view, parent folder as
  navigator); multiple files open the first file's folder in the grid with the
  first selected. A drop highlight shows the window is a drop target.
- **`o`** opens a file dialog that can pick a file or a folder.

## Views & modes

- **Grid view** — default on launch (when opening a file, jump straight to
  large view on that file; `Esc` returns to grid). Thumbnails from the cache,
  selection follows the navigator cursor.
- **Large view** — one image, header bar visible (auto-hide after inactivity).
- **Fullscreen** — `f` toggles (from large view); header hidden, image centered
  on a dark backdrop. `Esc` exits fullscreen back to large; `q` quits.
- **Slideshow** — auto-advance every N seconds (GSettings), fullscreen-only by
  default; any manual key pauses/resumes.

View switching never loses the cursor: grid→large opens on the selected cell;
large→grid scrolls that cell into view and focuses it. **Marks persist across
views** (a check badge in grid, an indicator in large), so you can mark in
detail view and move from either.

- **Move popup** — transient, opened by `m`. Lists configured destinations,
  each with an auto-assigned hotkey (`1`-`9`, `0`, then `a`-`z` in order). Type
  the hotkey to move the marked set (or the current image if none marked);
  `Esc` cancels. Order in settings = hotkey order.

## Keybindings (default, all reassignable later via GSettings)

**Every GUI element has a corresponding hotkey, and it is shown on the
element itself** — menu items print their key (e.g. `Move …   m`),
header-bar buttons show it in their tooltip, the move popup prints the digit
beside each destination. Conversely, **every action that has a hotkey is also
reachable through the GUI** (a header-bar button or an entry in the `F10`
menu). Keyboard and mouse are equally first-class; neither is a fallback.
Navigation is **vi-style plus cursor keys**: `h`/`l` and `←`/`→` move
prev/next through the shoot; `j`/`k` and `↑`/`↓` pan when zoomed.

| Key            | Action |
|----------------|--------|
| `h` / `Left`   | previous image |
| `l` / `Right`  | next image |
| `j` / `Down`   | pan down (when zoomed) |
| `k` / `Up`     | pan up (when zoomed) |
| `H` / `L`      | pan left / right (when zoomed) |
| `v`            | toggle mark on current image |
| `V`            | range-mark from last mark to current |
| `Ctrl+a`       | mark all images |
| `Ctrl+c`       | copy image (or marked files) to clipboard |
| `g`            | first image |
| `G`            | last image |
| `Enter`        | grid → large (open selected) |
| `Esc` / `Back` | context back: fullscreen → large → grid → quit |
| `t`            | toggle grid ↔ large |
| `+` / `=`      | zoom in (large) / grow thumbnails (grid) |
| `-` / `_`      | zoom out (large) / shrink thumbnails (grid) |
| `0`            | zoom 100% / fit toggle (large) / reset thumbnail size (grid) |
| `Space`        | hold to compare original vs modified (large) |
| `f`            | toggle fullscreen |
| `s`            | save enhanced copy (GEGL); no auto-save    |
| `S`            | toggle slideshow                          |
| `i`            | toggle info overlay |
| `r`            | reload (re-read file from disk) |
| `d`            | move to `./Trash` (confirm via toast), then next; undoable |
| `D`            | delete permanently (no trash), then next; no undo |
| `m`            | move marks (or current) → destination popup |
| `e`            | open current image in an external program → popup |
| `!`            | run a shell script → popup (e.g. `usbimport`) |
| `a`            | quick enhance → preset popup (GEGL; non-destructive) |
| `c`            | crop tool (GEGL; non-destructive, large view) |
| `R`            | straighten tool (GEGL; non-destructive, large view) |
| `[` / `]`      | rotate 90° CCW / CW (GEGL; non-destructive, one-shot) |
| `u`            | undo last `d` / `m` (restore from `./Trash` or move back) |
| `o`            | open file dialog |
| `,`            | preferences (destinations, sort, background, …) |
| `F10`           | open app menu (sort, background, empty `./Trash`, about) |
| `?`            | shortcuts overlay |
| `q`            | quit (exits fullscreen first) |

`Esc` is *contextual back*: if there are marks, it clears them first; then in
fullscreen it returns to large view, in large view it returns to the grid, in
the grid it quits. `q` always quits outright (exiting fullscreen first).

## Mouse / touch

- **Scroll** — `zoom` (default), `pan-when-zoomed`, or `navigate` next/prev
  — `scroll-behavior` setting.
- **Click-drag** — pan when zoomed in.
- **Double-click** — toggle fit ↔ 100%.
- **Middle-click** — toggle mark on a grid cell (grid view) / toggle
  fullscreen (large view).
- **Touch pinch** — zoom; **swipe** — next/prev; **two-finger tap** — info.

## Zoom behavior

- Fit-to-window is the default on load.
- `0` toggles fit ↔ 100% (double-click also toggles).
- Zoom centers on cursor (mouse) / pinch midpoint (touch) / window center (keys).
- Panning clamps so the image can't drift off-screen.

## Info overlay (`i`)

Small card, top-left or bottom-right:
- filename, dimensions, format, file size
- EXIF: camera, lens, focal length, aperture, shutter, ISO, date taken,
  orientation (auto-applied on load)
- shot number within the current burst group (once burst grouping lands)
- color space (once color management lands)
Loaded lazily; never blocks display of the pixels.

## Grid view behavior

- Thumbnails load from the `thumbnail` cache (M7), decoding lazily as cells
  scroll into view; never block the grid on a full decode.
- Trashed (`./Trash`) and permanently-deleted items stay listed but **dimmed**
  with a small badge, so you can see culling progress at a glance. (Toggle to
  hide them entirely via a menu option / setting.)
- `h`/`l`/`j`/`k`, arrow keys, `g`/`G`, click, and type-to-search (jump by
  filename prefix) all move the cursor: `h`/`l` prev/next cell, `j`/`k` row
  down/up. `Enter` opens large; `d`/`D` work here too.
- Re-sorting (name / capture time / size) reflows the grid and keeps the
  current image visible.
- **Resizable thumbnails**: `+`/`-` grow/shrink the thumbnails, and the grid
  auto-reflows (GtkFlowBox/GtkGridView) to fit the window — more columns when
  small, fewer when large. Size persists in GSettings (`thumbnail-size`) and
  is restored on next launch; `0` resets to default.
- Marks: `v` toggles a check badge on the current cell; `V` range-marks;
  `Ctrl+a` marks all; middle-click a cell toggles its mark. `d`/`D`/`m` act on the
  marked set (or current if none).

## Selection & moving

- **Marks** are a lightweight multi-select, shared by grid and large views.
  Toggle with `v`, range with `V`, all with `Ctrl+a`, clear with `Esc`.
- **`m` → move popup**: a small popover listing the configured destinations.
  Each entry shows an auto-assigned hotkey in order — `1`, `2`, `3` … (then `0`,
  then `a`-`z`). Example:
  ```
  Move 3 images to:
   1  irregular ninja
   2  alt irregular ninja
   3  something else
  ```
  Press `2` to move to "alt irregular ninja"; `Esc` cancels. Destinations
  are configured in Preferences (`,`) as an ordered list of name → path pairs.
- **Move semantics**: `g_file_move` (rename on same filesystem, else copy +
  delete). On name collision in the destination, suffix `-1`, `-2`, …. After
  a move, files leave the current folder: the navigator drops them, the grid
  removes their cells, and the counter updates.
- **Undo**: `u` undoes the last `d` (restore from `./Trash`) **or** the last
  `m` (move the set back to their original paths). One level of undo to start.

## Copy to clipboard

- **`Ctrl+c`** (or menu *Copy*) puts the current picture on the clipboard so
  you can paste it into other apps (Katogram, GIMP, chat clients) — like gthumb.
- **No marks** → copies the **displayed** image **pixels** as PNG (modified
  if a preview is active, else original) via `GdkClipboard` +
  `GdkContentProvider` for `image/png`; pastes as an image. Decoding runs in a
  `GTask` thread so the UI doesn't block.
- **Marks present** → copies the marked **files** as `text/uri-list` (plus a
  `text/plain` path list), so file-aware apps and file managers can paste them.
- A toast confirms ("Copied image" / "Copied 3 files"). `Ctrl+Shift+c` (later)
  copies the **original** (un-modified) image or the path.

## Opening in an external program

- **`e` → external-program popup**: same popover pattern as `m`. Lists the
  configured programs, each with an auto-assigned hotkey (`1`, `2`, … then `0`,
  `a`-`z`). Example:
  ```
  Open IMG_0001.jpg in:
   1  GIMP
   2  ImageMagick identify
   3  Nomacs
  ```
  Press `2` to launch that program with the current image's path; `Esc`
  cancels.
- Acts on the **current image** (the **original file on disk**, not the
  preview); a later option may pass the marked set (`%F`). To open the modified
  version, export it first (`s`).
- Programs are configured in Preferences (`,`) as an ordered list of
  name → command pairs. The command uses freedesktop `Exec` placeholders:
  `%f` = the single current file path (e.g. `gimp %f`, `identify %f`).
- Launch is **detached and non-blocking** (GSubprocess); ggaze stays
  responsive and the image stays open. Failures show a toast.

## Running shell scripts

- **`!` → scripts popup**: same popover pattern as `m`/`e`. Lists the
  configured shell scripts, each with an auto-assigned hotkey (`1`, `2`, …
  then `0`, `a`-`z`). Example:
  ```
  Run script:
   1  usbimport (import from camera)
   2  build contact sheet
  ```
  Press `1` to run that script; `Esc` cancels.
- Scripts run **asynchronously** via a shell (`/bin/sh -c`), so pipes,
  redirection, and `~/` expansion all work. ggaze never blocks on them; a
  toast shows "running usbimport…" while it runs.
- Placeholders: `%f` = current image path, `%d` = current folder (e.g.
  `~/scripts/usbimport %d`, or just `~/scripts/usbimport` with no args).
  Substituted paths are single-quoted to avoid shell injection from filenames.
- On completion, ggaze **rescans the directory** (scripts like `usbimport`
  add files); a toast reports success/failure and the exit status.
- Scripts are configured in Preferences (`,`) as an ordered list of
  name → command pairs (`a(ss)`), separate from `editors`.

## Quick enhance (GEGL, optional)

- **`a` → enhance popup**: same popover pattern as `m`/`e`/`!`. Lists
  configurable presets, each with an auto-assigned hotkey (`1`, `2`, … then
  `0`, `a`-`z`). Example:
  ```
  Enhance IMG_0001.jpg:
   1  Auto-fix
   2  Brightness
   3  Contrast
   4  Saturation
   5  Sharpen
   6  Denoise
  ```
  Press `1` to apply "Auto-fix" as a **non-destructive live preview**; press
  it again (or `Esc`) to turn the preview off.
- Presets are GEGL op graphs (e.g. Auto-fix = `gegl:stretch-contrast` →
  `gegl:color-enhance`; Brightness = `gegl:exposure`; Contrast =
  `gegl:brightness-contrast`; Saturation = `gegl:saturation`; Sharpen =
  `gegl:sharpen`). Configurable in Preferences (`,`) as `enhance-presets`
  (`a(ss)` name → gegl-graph); the strength is just a number in the graph
  text — tune it there, no slider UI needed.
- **`s`** (or menu *Save enhanced copy…*) writes the enhanced result to
  `<name>-enhanced.<ext>` via a GEGL saver — the **original is never
  touched**. ggaze **never auto-saves**: an enhance preview is a live overlay
  only.
- **Dirty state + prompt on navigate:** an active (un-exported) enhance
  preview is "dirty". Navigating to another image (`h`/`l`/`g`/`G`/click) or
  quitting with a dirty preview prompts **Save** (export the copy, then
  proceed), **Discard** (drop the preview, proceed), or **Cancel** (stay).
  `s` saves and clears dirty; toggling the preview off (`Esc`/re-press)
  discards it directly (explicit, no prompt).
- GEGL runs only when a preset is active or on export; the fast decode path
  is unchanged, and enhance is **not** applied during `h`/`l` scrubbing (only
  when settled on an image). If the build has no GEGL, `a` shows a "GEGL not
  built in" toast. See [gegl.md](gegl.md).

## Crop, straighten & rotate tools (GEGL)

Non-destructive, like enhance — they add ops to the live preview graph; `s`
exports the result and navigating away prompts Save/Discard/Cancel. Large view
only; in grid, `c`/`R` first switch to large on the selected cell. If GEGL is
not built in, all show the "GEGL not built in" toast.

- **`c` → crop tool:** overlay an adjustable crop rectangle on the image.
  - Mouse: drag inside to move, drag edges/corners to resize.
  - Keyboard: `h`/`l`/`j`/`k` move the rectangle; `H`/`L`/`J`/`K` resize the
    edges; `1`-`4` set aspect ratio (1:1, 3:2, 4:3, 16:9), `0` free.
  - `Enter` applies (`gegl:crop`), `Esc` cancels.
- **`R` → straighten tool:** level the horizon.
  - Mouse: drag a line along the horizon; the image rotates to align it.
  - Keyboard: `h`/`l` (or `+`/`-`) nudge the angle by ±0.5°; a grid overlay
    helps. Optional auto-crop to remove the rotated corners.
  - `Enter` applies (`gegl:rotate`), `Esc` cancels.
- **`[` / `]` → rotate 90°:** one-shot, no overlay — `]` clockwise, `[`
  counterclockwise; repeat to reach 180°/270°. Non-destructive
  (`gegl:rotate-on-center`); `s` exports the rotated copy.
- All compose with enhance presets in the same preview graph.

## Compare original vs modified (hold)

- **Hold `Space`** to momentarily show the **original** image; **release** to
  return to the **modified** (preview-graph) image — a quick before/after to
  decide whether to `s` save. Only meaningful when a preview (enhance / crop /
  straighten / rotate) is active; otherwise original == modified, no-op.
- Large view only. GUI: menu *Show original* (toggle) for mouse users.

## Hotkey visibility

Hotkeys are not hidden — each is printed on the element it triggers:

- **Menu items** show their key right-aligned, e.g. `Copy   Ctrl+c`,
  `Move …   m`, `Open in …   e`, `Scripts …   !`, `Enhance …   a`,
  `Crop …   c`, `Straighten …   R`, `Rotate 90°   ] / [`, `Save enhanced copy …
  s`, `Show original (hold)   Space`, `Slideshow   S`, `Trash   d`, `Delete   D`,
  `Preferences …   ,`, `Fullscreen   f`.
- **Header-bar buttons** show the key in the tooltip (plus an underline
  mnemonic where GTK draws one).
- **Popup** entries (move / open-in / scripts / enhance) lead with the
  hotkey: `1  irregular ninja`, `1  GIMP`, `1  usbimport`, `1  Auto-fix`.
- **Shortcuts overlay** (`?`) lists everything in one place.

If an element has no direct key, it lives in the `F10` menu (navigable with
arrows + `Enter` and mnemonic underlines).

## Reachability (keyboard + GUI)

Every action is reachable **two ways**:

- **By keyboard** — a direct hotkey, or via the `F10` menu (arrows + `Enter`).
- **By mouse/GUI** — a header-bar button for the common ones, or an entry in
  the `F10` app menu for the rest.

So the keyboard alone, or the mouse alone, can reach everything. Header-bar
buttons and their hotkeys:

| Element            | Hotkey        |
|--------------------|---------------|
| prev / next        | `h` / `l`     |
| zoom fit / 100%    | `Space` / `0` |
| zoom in / out      | `+` / `-`     |
| toggle grid/large  | `t`           |
| open file          | `o`           |
| slideshow          | `S`           |
| fullscreen         | `f`           |
| info overlay       | `i`           |
| app menu           | `F10`         |
| preferences        | `,`           |

App menu (via `F10`) items — each reachable by mnemonic, by arrows + `Enter`,
and by click: Copy, Move…, Open in…, Scripts…, Enhance…, Crop…, Straighten…,
Rotate 90° CW/CCW, Save enhanced copy…, Show original, Trash, Delete, Sort (by
name / capture time / size), background colour, hide-trashed toggle, empty
`./Trash`, About, Preferences….
The move popup and all dialogs (open, preferences, shortcuts overlay) are
likewise fully operable by keyboard and by mouse.

## Self-documenting

The UI explains itself; the manual is a bonus, not a requirement.

- Hotkeys are printed on their elements (see Hotkey visibility).
- Tooltips on every header-bar button and menu item: name + key + a one-line
  hint.
- `?` opens a shortcuts overlay listing every action and its key.
- The header shows `n / total` + filename (+ `N marked` when marks exist);
  the grid dim-marks trashed/deleted items and check-badges marks; the move
  popup shows each destination's hotkey and the count being moved ("Move 3
  images to:").
- The info overlay (`i`) shows EXIF, format, and size on demand.
- Empty states ("no images in this folder") and toasts ("Moved 3 → irregular
  ninja", "Undo") narrate every side-effect.

If a user can't tell what a key or button does by looking, that's a bug.

## Culling workflow notes
- `d` moves the file to `<folder>/.Trash/` (created lazily), advancing to the
  next image automatically, so `d d d` clears a run of rejects without
  re-aiming. A transient toast confirms and offers **Undo** (`u`).
- `D` permanently deletes (unlinks) — fast path for obvious garbage; **no
  undo**, so the toast warns and the grid badge marks it. Deleting **>1 marked**
  image asks for a confirm dialog first.
- Counter in the header (`n / total`) reflects *remaining* images so you can
  see the folder shrinking as you cull.
- `./Trash` lives with the shoot: easy to inspect, empty via the menu, or
  `rsync`/`rm -rf` from a shell. Never the system trash.