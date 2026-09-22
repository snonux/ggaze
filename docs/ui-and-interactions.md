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
  open-file, slideshow, menu (sort, background, empty-`.Trash`, about).
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

Keyboard first: every action has a vi-style key (listed first) AND a
traditional one, and `shortcuts.c`'s single table is what the `?` help, the
header-bar tooltips and the main menu are built from, so none of them can
drift from the live bindings.

| Key            | Action |
|----------------|--------|
| `h` / `Left` / `PageUp`   | previous image |
| `l` / `Right` / `PageDown` | next image |
| `j` / `Down`   | cursor down one row (grid) / pan down (large) |
| `k` / `Up`     | cursor up one row (grid) / pan up (large) |
| `H` / `L`, `Shift+Left` / `Shift+Right` | pan left / right (large) |
| `v`            | toggle mark on the highlighted / current image |
| `V`            | range-mark from last mark to current |
| `Ctrl+a`       | mark all images |
| `Ctrl+c`       | copy image (or marked files) to clipboard |
| `g` / `Home`   | first image |
| `G` / `End`    | last image |
| `Enter`        | grid → large (open highlighted) |
| `Esc`          | one step back: stop slideshow → cancel a crop/straighten tool → close the enhance panel → discard the preview → leave fullscreen → clear marks → large → grid; in the grid a second `Esc` within 2 s quits (the order `_action_back` in window.c implements) |
| `t`            | toggle grid ↔ large |
| `+` / `=`, `Ctrl++` | zoom in (large) / grow thumbnails (grid) |
| `-` / `_`, `Ctrl+-` | zoom out (large) / shrink thumbnails (grid) |
| `0`, `Ctrl+0`  | zoom fit ↔ 100% (large) / reset thumbnail size (grid) |
| `Space`        | hold to compare original vs modified (large, enhance) |
| `f` / `F11`    | toggle fullscreen |
| `s`            | save enhanced copy (GEGL); no auto-save |
| `S` / `F5`     | start / stop slideshow (large view; any navigation key stops it) |
| `i`            | toggle info overlay |
| `d` / `Delete` | move to `.Trash` (status line offers `u`), then next; undoable |
| `D` / `Shift+Delete` | delete permanently (no trash), then next; no undo |
| `E`            | empty this folder's `.Trash` (confirm dialog; no undo) |
| `m`            | move marks (or current) → destination popup |
| `e`            | open current image in an external program → popup |
| `!`            | run a shell script → popup (e.g. `usbimport`) |
| `a`            | quick enhance → side panel beside the image (GEGL) |
| `1`–`8` / `0`  | toggle enhance preset N (layered, large view) / back to original (panel open) |
| `c`            | crop tool (GEGL): rectangle overlay; `Enter` applies, `Esc` cancels — see "Crop, straighten & rotate tools" |
| `R`            | straighten tool (GEGL): horizon drag / `h` `l` nudge ±0.5°, `A` auto-crop; `Enter` / `Esc` |
| `]` / `[`      | rotate 90° clockwise / counter-clockwise (GEGL, one-shot; repeat for 180°/270°) |
| `u` / `Ctrl+z` | undo last `d` / `m` (restore from `.Trash` or move back) |
| `o` / `Ctrl+o` | open image dialog (image filter) |
| `O` / `Ctrl+Shift+o` | open folder dialog |
| `,` / `Ctrl+,` | preferences (destinations, editors, scripts, presets, sort, …) |
| `F10`          | main menu (every action, with its keys) |
| `?` / `F1`     | shortcuts overlay |
| `q` / `Ctrl+q` | quit |

Planned, not yet bound: `r` reload (the folder monitor reloads edited files
automatically).

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
  A scroll event does not always carry a pointer position — plain X11 wheel
  events carry none — and zoom then falls back to the **window center** rather
  than to a bogus point. Do not "simplify" that fallback away by ignoring
  `gdk_event_get_position()`'s return value: it writes NaN to its
  out-parameters on failure, and a NaN reaching the pan state makes the image
  vanish for good (hx0). `viewer.c` guards both the scroll centre and the pan.
- Panning clamps so the image can't drift off-screen.
- Zoom is limited to 2 %–6400 % (`GGAZE_ZOOM_MIN`/`MAX`), except that the upper
  limit rises to the fit-to-window ratio when that is already larger — a small
  enough image in a large window fits above 6400 %, and clamping to the bare
  ceiling made zoom-in *shrink* it (jx0). At the top end zoom-in is a no-op,
  never a reversal.

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
- Trashed (`.Trash`) and permanently-deleted items stay listed but **dimmed**
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
  `Ctrl+a` marks all; middle-click a cell toggles its mark. `D`/`m` act on the
  marked set (or current if none); `d` always trashes just the current image.

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
  a move, moved files stay listed but **dimmed** (`navigator_mark_removed`),
  mirroring trash — the navigator does not drop them and the grid does not
  remove their cells; the large view just advances past them and the counter
  updates.
- **Undo**: `u` undoes the last `d` (restore from `.Trash`) **or** the last
  `m` (move the set back to their original paths), whichever happened more
  recently. One level of undo per engine to start. Reopening a folder gives
  both trash and move a fresh undo state for that folder, so `u` never reaches
  back into a folder you've since navigated away from.

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

- **`a` → enhance side panel**: a narrow column of cards *beside* the large
  view, inside the main window (switching to the large view first if needed)
  — no second window, no popover. The image keeps the whole viewer. Cards:
  `0  Original`, then one per configurable preset with an auto-assigned
  hotkey (`1`, `2`, … capped at the mask's 8 slots), by default each with a
  small preview thumbnail of that preset applied alone (Preferences can turn
  the thumbnails off for label-only cards; the cards and the Original card
  always show the **untransformed** image — they are references for the
  colour presets alone and ignore a crop, straighten or turn). Under the
  cards: a state line
  that says whether the preview is unsaved, a **Save copy** button, and the
  key hint (`1-8` toggle · `0` original · `Space` hold to see the original ·
  `s / Ctrl+S` save · `Esc` close). Example:
  ```
  Enhance IMG_0001.jpg
   [thumb] 0  Original
   [thumb] 1  Auto-fix
   [thumb] 2  Brightness
   …
  Unsaved preview — press s to save a copy.
  [ Save copy (s) ]
  ```
  Presets are **layered**: pressing `1` toggles "Auto-fix" on as a
  **non-destructive live preview**, and `2` composes "Brightness" on top of
  it — press either again to toggle it back off. A hotkey/card click does
  **not** close the panel (toggling combinations while comparing is the
  point); `Esc` or re-pressing `a` closes it and leaves the preview in place.
  `0` (or the Original card) discards the whole preview while the panel is
  open. The panel stays open across navigation (re-titled and re-previewed
  for the new image) and is hidden with the grid, back with the large view.
- **Hold `Space`** shows the original; release shows the current edit — with
  or without the panel. If a preset is applied with the panel closed, a
  status line says once per image how to compare, save and open the panel.
- Presets are GEGL op graphs (e.g. Auto-fix = `gegl:stretch-contrast` →
  `gegl:color-enhance`; Brightness = `gegl:exposure`; Contrast =
  `gegl:brightness-contrast`; Saturation = `gegl:saturation`; Sharpen =
  `gegl:sharpen`). Configurable in Preferences (`,`) as `enhance-presets`
  (`a(ss)` name → gegl-graph); the strength is just a number in the graph
  text — tune it there, no slider UI needed.
- Applying the enabled preset chain runs off the GTK main thread (a GTask
  worker); the UI stays responsive while GEGL processes, and a newer
  toggle/navigation/discard supersedes a still-in-flight one (last-write-wins
  — its result is dropped when it lands).
- **`s` / `Ctrl+S`** (or the panel's *Save copy* button, or menu *Save
  enhanced copy…*) writes the enhanced result to `<name>-enhanced.<ext>`, or
  `<name>-enhanced-1.<ext>`, `-2`, … if that name is already taken (same
  collision convention as the move popup) — the **original is never
  touched**. ggaze **never auto-saves**: an enhance preview is a live overlay
  only. A successful save marks the preview **saved**: it stays on screen
  (pressing `s` again exports another, separately-numbered copy), but it is
  no longer dirty, so moving on does not prompt. What was saved is the exact
  (presets, transform) combination: any change makes the preview unsaved,
  and coming back to exactly that combination — a preset toggled off and
  on, a tool cancelled back to it — makes it saved again, because that is
  what the file on disk holds.
- **Dirty state + prompt on navigate:** an active enhance preview that has
  not been exported since its last change is "dirty". Navigating to another image (`h`/`l`/`g`/`G`/scroll),
  picking a different image in the grid (double-click/Enter, middle-click
  mark, `j`/`k` cursor move, or toggling back to large on another cell),
  trashing/deleting/moving the current file (`d`/`D`/`m`), opening a
  different file/folder (`o`, drag-and-drop, File→Open), or quitting —
  either via `q` **or** the window manager's close button / Alt+F4 —
  with a dirty preview prompts **Save** (export the copy, then proceed),
  **Discard** (drop the preview, proceed), or **Cancel** (stay). If the
  export **fails** (read-only folder, full disk, ...), Save behaves like
  Cancel plus an error message: the preview is kept and nothing proceeds, so
  an unwritable destination can never cost the enhancement. `s` clears
  dirty by putting the work on disk (see above); toggling every preset back
  off, `0` (panel open), or `Esc` (panel closed) discards it directly
  (explicit, no prompt). Slideshow auto-advance is the one exception: it discards a dirty
  preview silently rather than blocking on a prompt no one is there to
  answer.
- **One prompt at a time, and it decides for the image it named.** Only one
  Save/Discard/Cancel dialog is ever up per window. Most triggers are input
  and the modal grab swallows them anyway, but three are not: Alt+F4 / the WM
  close button, a single-instance `ggaze <file>` arriving over D-Bus, and a
  drag-and-drop. Those are **queued**, not stacked and not dropped — one slot,
  newest wins, with a status line saying so. Answering **Save** or **Discard**
  performs the original action and then retries the queued one through the same
  gate; **Cancel** means "stay here, keep the preview", so the queued request is
  discarded too (again with a status line). Repeated Alt+F4 therefore still
  produces exactly one dialog and one quit. The one answer that proceeds and
  still drops the queue is a **quit**: by the time the queued request would
  run, the window is already closing, and a closing window can honour no
  request — so it is discarded (and logged) rather than half-run against a
  dead window.
- The action is bound to the image the prompt was **raised for**, not to
  whatever is current when it is answered. That distinction is real: GTK4
  modality is input-only, so the slideshow timer and the folder's file monitor
  keep running behind the dialog. They can move to the next image, and — via
  the rescan the monitor triggers — **prune marks** whose file has left the
  folder. `d`/`D`/`m` therefore capture their targets at key-press time, and
  for `D`/`m` — the two that consult the marks at all — the whole target set
  including the marks-vs-current decision: `D` with one file marked
  deletes that file or nothing, never "whatever is current now because the
  mark disappeared", and three marks still ask the >1-mark confirmation even
  if two of them vanish while the prompt is up. Answering Discard can never
  trash, delete or move a file the user did not pick, and a captured target
  that no longer exists is refused with a status line instead of failing
  silently. Trashing, deleting or moving a target set that does not contain
  the current image does not advance the cursor, so nothing is skipped
  unseen. If the preview itself is
  gone by the time Save is pressed, ggaze reports "nothing to save" and still
  performs the action rather than silently doing neither.
- GEGL runs only when a preset is active or on export; the fast decode path
  is unchanged, and enhance is **not** applied during `h`/`l` scrubbing (only
  when settled on an image). If the build has no GEGL, `a` (and `s`) show a
  "GEGL not built in" status message instead of opening anything. See
  [gegl.md](gegl.md).

## Crop, straighten & rotate tools (GEGL)

Non-destructive, like enhance — they add ops to the same live preview graph
(compose order, decision #35: colour presets → rotate 90° → straighten →
crop); `s` exports the composed result and navigating away prompts
Save/Discard/Cancel exactly as for a preset. The title names what is on
screen (`… · Auto-fix · 90° CW, crop`). Large view only; in the grid `c`,
`R`, `[` and `]` first open the highlighted image large. If GEGL is not
built in, all four report "GEGL not built in".

- **`c` → crop tool:** a rectangle overlay on the image (outside dimmed,
  rule-of-thirds lines, corner handles), starting as the whole image — or as
  the crop already applied, so it can be adjusted rather than redrawn.
  - Mouse: drag inside to move, drag an edge or corner to resize.
  - Keyboard: `h`/`l`/`j`/`k` move the rectangle; `H`/`L` move its right
    edge, `J`/`K` its bottom edge (1 % of the image per press); `1`-`4` lock
    the aspect ratio (1:1, 3:2, 4:3, 16:9), `0` frees it.
  - `Enter` applies (`gegl:crop`; a rectangle still covering the whole image
    removes the crop), `Esc` or `c` again cancels and restores. `Enter` is
    refused while the preview under the rectangle is still rendering.
  - While the tool is open the image is shown **without** its crop so the
    rectangle can be adjusted — but the crop already applied still counts:
    `s` inside the tool exports the cropped copy and navigating away still
    prompts for it. A saved crop stays saved through `c` / `Esc`.
- **`R` → straighten tool:** level the horizon; a grid overlay helps.
  - Mouse: drag a line along the horizon; the image rotates to level it
    (the angle adds to the current one).
  - Keyboard: `h` / `-` nudge counter-clockwise, `l` / `+` clockwise, by
    0.5°, within ±45°; `A` toggles the auto-crop of the rotated corners
    (default on, decision #35; off keeps the whole rotated bounding box —
    the preview then shows **transparent** corners while a JPEG export gets
    **black** ones, since JPEG has no alpha; a PNG export keeps them
    transparent). The auto-crop trims one extra pixel per side so every
    kept pixel is opaque; that needs an image whose inscribed rectangle is
    at least 3 px per side — straightening anything smaller (a 3×2 at 45°)
    is not meaningful and may keep translucent edge pixels.
  - Every change renders live (`gegl:rotate` about the centre); holding a
    nudge key queues one re-render, not one per repeat. `Enter` keeps it,
    `Esc` or `R` again restores the angle the tool started with. A crop
    already applied follows the changing image: its rectangle is kept whole
    and re-centred on the centre the straighten turns about, and only the
    part of it inside the straightened image is cropped (in the preview and
    the export). So a rectangle touching the border is never eroded — nudge
    away and back and the crop is exactly what it was. When nothing of it
    lies inside the image at the new angle it is removed and the status
    line says so (`Esc` brings it back with the old angle).
- **`[` / `]` → rotate 90°:** one-shot, no overlay — `]` clockwise, `[`
  counterclockwise; repeat to reach 180°/270°, four presses are the original
  again. Non-destructive (`gegl:rotate`, an exact pixel permutation); a crop
  already applied turns with the image.
- The tool keys are **modal**: while a tool is active they belong to it
  (`h` moves the rectangle instead of going to the previous image), the
  other tool's key and `[`/`]` are refused until `Enter`/`Esc`, and any key
  not listed keeps its usual meaning. Navigating away, or leaving the large
  view, ends a tool without applying it. `?` lists the tool keys under
  *Tools*.
- All compose with enhance presets in the same preview graph; hold `Space`
  compares against the original as usual.

## Compare original vs modified (hold)

- **Hold `Space`** to momentarily show the **original** image; **release** to
  return to the **modified** (preview-graph) image — a quick before/after to
  decide whether to `s` save. Only meaningful when a preview (enhance / crop /
  straighten / rotate) is active; otherwise original == modified, no-op.
- Large view only. Works with or without the enhance panel open; the panel's
  hint names it. GUI: menu *Show original* (toggle) for mouse users.

## Hotkey visibility

Hotkeys are not hidden — each is printed on the element it triggers:

- **Menu items** show their key right-aligned, e.g. `Copy   Ctrl+c`,
  `Move …   m`, `Open in …   e`, `Scripts …   !`, `Enhance …   a`,
  `Crop   c`, `Straighten   R`, `Rotate 90° clockwise   ]`, `Rotate 90°
  counter-clockwise   [`, `Save enhanced copy …
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
| zoom fit / 100%    | `0`           |
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
`.Trash`, About, Preferences….
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
  image asks for a confirm dialog first. Only pressing **Delete** on that
  dialog deletes: Cancel, `Esc`, closing the dialog and a window teardown
  underneath it all mean *no*, and nothing is touched. While the dialog is up
  the window itself refuses to close (Alt+F4 / the WM button are not input
  events, so the modal grab does not stop them) — answer the dialog and the
  close goes through.
- Counter in the header (`n / total`) reflects *remaining* images so you can
  see the folder shrinking as you cull.
- `.Trash` lives with the shoot: easy to inspect, empty via the menu, or
  `rsync`/`rm -rf` from a shell. Never the system trash.
