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
- No batch-tools or editing toolbars; every edit lives in one narrow side
  panel (`a`) that is only there while you use it.
- No location/path entry; the current folder is shown, not editable.

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
  first selected — in one pass, like a single-file open that lands in the
  grid: the first file is loaded once, whatever its position in the sort
  order. The **first entry decides** and the rest only ask for the grid: a
  folder as the first entry opens that folder itself (not its parent); a
  first entry that is missing, not an image, a hidden dotfile (never
  listed), or a RAW sidecar hidden by the hide-raw preference opens its
  folder on the first-sorted image with the status line saying why ("… not
  found — opened its folder", "… is not an image in this folder …", "… is a
  hidden dotfile, never listed …", "… is a RAW sidecar hidden by
  Preferences …"),
  exactly as a single-file open of that entry would. A drop highlight shows
  the window is a drop target.
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

### Animated GIF / WebP

- An animated GIF (or WebP, where gdk-pixbuf decodes its frames) **plays by
  itself in the large view**, looping for ever (a GIF that says "play
  once" holds its last frame), from its first frame each time it is shown.
  There is no play/pause key: `h`/`l` away and back restarts it, and the
  grid page, a minimized window (or, on Wayland, one the compositor stops
  presenting) pause it — frames are only produced while the picture is on
  screen. The first frame appears once every frame is decoded (a large
  animation takes longer to open than a still of the same size).
- Zoom, pan, fit and `0` work as on a still; the whole animation is one
  picture of the canvas size.
- The **grid thumbnail is the first frame**, and so is everything that
  reads "the picture" rather than watches it: the `i` card's dimensions and
  histogram, `Ctrl+c`'s `image/png`, the enhance preview and `s` export, the
  crop / straighten / rotate tools and hold-`Space`. An enhance preview is
  therefore static; releasing `Space` shows the animation again from its
  first frame. While the **crop or straighten tool** is up the animation
  holds its first frame (the one the tool crops or straightens) and plays
  on from the first frame when the tool ends.
- Frame delays under 20 ms play at 20 ms (a 0 ms GIF is common). An
  animation over the playback budget is shown as its first frame only,
  like a still: frames × canvas over the still pixel cap (100 M pixels), a
  canvas over 4 Mi pixels (2048 × 2048), or more than 1000 frames.

- **Move popup** — transient, opened by `m`. Lists configured destinations,
  each with an auto-assigned hotkey (`1`-`9`, `0`, then `a`-`z` in order). Type
  the hotkey to move the marked set (or the current image if none marked);
  `Esc` cancels. Order in settings = hotkey order.

## Keybindings (default)

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
| `Esc`          | one step back: stop slideshow → cancel a crop/straighten tool → close the edit panel (the edit stays; large view only — beside the grid the panel is hidden and `Esc` skips it) → leave fullscreen → clear marks → large → grid; in the grid a second `Esc` within 2 s quits (the order `_action_back` in window.c implements). **`Esc` never discards an edit** — `x` does |
| `t`            | toggle grid ↔ large |
| `+` / `=`, `Ctrl++` | zoom in (large) / grow thumbnails (grid) |
| `-` / `_`, `Ctrl+-` | zoom out (large) / shrink thumbnails (grid) |
| `0`, `Ctrl+0`  | zoom fit ↔ 100% (large) / reset thumbnail size (grid) — nothing else, in every mode |
| `Space`        | hold to compare original vs modified (large, enhance; a colour-managed preview compares against the managed original) |
| `f` / `F11`    | toggle fullscreen |
| `s`            | save an edited copy (GEGL); no auto-save |
| `S` / `F5`     | start / stop slideshow (large view; any navigation key, a swipe and a navigate-mode wheel notch stop it) |
| `i`            | toggle info overlay |
| `d` / `Delete` | move to `.Trash` (status line offers `u`), then next; undoable |
| `D` / `Shift+Delete` | delete permanently (no trash), then next; no undo |
| `E`            | empty this folder's `.Trash` (confirm dialog; no undo) |
| `m`            | move marks (or current) → destination popup |
| `e`            | open current image in an external program → popup |
| `!`            | run a shell script → popup (e.g. `usbimport`) |
| `a`            | open / close the **edit panel** beside the image (GEGL) — presets, crop, straighten, rotate, save, revert; in the grid, where an open panel is hidden, `a` goes back to the large view and shows it (it never closes a panel you cannot see) |
| `1`–`8`        | toggle preset N (layered) — **only while the edit panel is open and on screen** (large view); with it closed, or hidden beside the grid, a digit does nothing. The digit also selects that preset's card. Only the first eight rows (the built-ins) have a digit: your own presets, listed after them, are reached with `j` / `k` and `Enter` |
| `j` / `k`      | **edit panel open:** select the next / previous preset card — every row, your own presets after the built-ins included; the list scrolls to keep it in view (a ring marks it; stops at the first and last) — instead of panning (`Down` / `Up` still pan) |
| `Enter`        | **edit panel open:** toggle the selected preset (any row), as a digit does |
| `h` / `l`      | **edit panel open:** lower / raise the selected preset's **strength** one step, turning it on (`Shift`: five steps) — instead of previous / next image; `Left` / `Right` and `Page Up` / `Page Down` still change the image, so flicking through a folder with the arrows never moves a strength. A preset with nothing to tune (Auto-fix, Warm) says so (and that `←`/`→` or `PgUp`/`PgDn` change the image) and changes nothing |
| `c`            | crop tool (GEGL; opens the edit panel first): rectangle overlay; `Enter` applies, `Esc` cancels — see "Crop, straighten & rotate tools" |
| `r`            | straighten tool (GEGL; opens the edit panel first): horizon drag / `h` `l` nudge ±0.5°, `a` auto-crop; `Enter` / `Esc` |
| `]` / `[`      | rotate 90° clockwise / counter-clockwise (GEGL, one-shot; opens the edit panel first; repeat for 180°/270°) |
| `x`            | revert every edit — presets and transform (edit panel open, large view; with it closed or in the grid a status line says where it works) |
| `u` / `Ctrl+z` | undo last `d` / `m` (restore from `.Trash` or move back) — **with the edit panel open and on screen, undo the last edit step instead** (see "Undo / redo of edit steps") |
| `U` / `Ctrl+Shift+Z` | redo the edit step undone last (edit panel open, large view only; elsewhere nothing) |
| `o` / `Ctrl+o` | open image dialog (image filter) |
| `O` / `Ctrl+Shift+o` | open folder dialog |
| `,` / `Ctrl+,` | preferences (destinations, editors, scripts, presets, sort, …) |
| `F10`          | main menu (every action, with its keys) |
| `?` / `F1`     | shortcuts overlay |
| `q` / `Ctrl+q` | quit |


`Esc` is *contextual back*: if there are marks, it clears them first; then in
fullscreen it returns to large view, in large view it returns to the grid, in
the grid it quits. `q` always quits outright (exiting fullscreen first).

Caps Lock never turns a letter into its Shift chord: with it on, `h` still
moves the crop rectangle (`Shift+h` grows it), `a` is still the tool's
aspect / auto-crop key, `c` still the crop tool — the key table matches a
letter Caps Lock upper-cased as the plain letter. So in the edit panel
`u` with Caps Lock on is still undo, and `Shift+u` (whatever case it
arrives in) redo.

### Edit modes and the key-hint bar

Edit keys are **modal**. Three contexts own keys beyond the table above,
and each owns them only while it is on screen:

| Mode | Live while | Its own keys |
|------|------------|--------------|
| **Edit** | the edit panel is open and on screen (large view), no tool up | `1`–`N` presets — one digit per preset row, at most 8 (the built-ins; rows past them have none) · `j`/`k` select a preset card (any row) · `Enter` toggles it · `h`/`l` its strength, `Shift` five steps (the vi keys only: the arrows stay global) · `u` / `Ctrl+z` undo and `U` / `Ctrl+Shift+Z` redo an edit step (plus the global `c` `r` `[` `]` `s` `x` `Space` `a`/`Esc` it lists) |
| **Crop** | the crop tool is up | `h`/`j`/`k`/`l` move · `Shift+h/j/k/l` grow that side · `Ctrl+h/j/k/l` shrink that side · `a` aspect cycle (free → 1:1 → 3:2 → 4:3 → 16:9 → original) · `Enter` apply · `Esc` cancel |
| **Straighten** | the straighten tool is up | `h`/`l` (and `-`/`+`) nudge ½° · `a` auto-crop · `Enter` apply · `Esc` cancel |

A tool's keys win over the panel's, which win over the global table; a key
a mode does not own keeps its global meaning (`h` is "previous image" again
the moment a tool ends). While a mode is active a **key-hint bar** runs
along the bottom of the large view listing exactly that mode's live keys,
e.g.

```
Edit   1–8 presets · j/k select · Enter toggle · h/l strength · c crop · r straighten · [/] rotate · s save copy · x revert · u/Shift+u undo/redo · Space hold: original · a/Esc close
Crop   h/j/k/l move · Shift+h/j/k/l grow · Ctrl+h/j/k/l shrink · a aspect · Enter apply · Esc cancel
```

The bar lists the preset digits that exist: `1–8 presets` with the eight
built-ins, `1/2 presets` or `1 preset` with fewer, no presets segment for
none. Your own presets add rows after the built-ins but no digits — the
bar never promises a `9`; `j/k select` and `Enter toggle` reach them. The bar, the edit panel's button keys, the `?` help, the header
tooltips and the `F10` menu are all generated from `shortcuts.c`'s one
table (rows scoped to a mode, each with a short hint label and, for the
menu, a short label such as *Crop* where the help says "Crop tool (Enter
applies, Esc cancels)"), so they cannot drift from the keys that actually
work. Inside the panel `j`/`k` select a preset card, `Enter` toggles it
and `h`/`l` tune its strength (8i2) — the vi keys only: while the panel
is the key mode the image still changes with `←` / `→` or `Page Up` /
`Page Down`, and `Up` / `Down` and `Shift+←` / `Shift+→` still pan a
zoomed picture; `u` / `Ctrl+z` are the panel's own undo. All of them are
the global keys again the moment the panel closes or hides with the grid.
Under a crop / straighten tool the tool's own `h`/`j`/`k`/`l` and `Enter`
come first; of the keys the tool leaves alone the panel still gets the
digits and `u` (refused with "Finish the current tool first", see "Undo /
redo of edit steps"), but never the selected card's `j` / `k` / `Enter` /
`h` / `l` — a selection or strength moved behind a modal tool would
re-render and record steps behind it; there they keep their global
meaning. `Shift` held on a key that prints nothing (`Enter`, `Esc`, the
arrows) only matters where a mode binds its Shift chord (`Shift+←`
pans); elsewhere it is ignored, so `Shift+Enter` applies a crop and
`Shift+Esc` cancels it.

## Mouse / touch

- **Scroll** — `zoom` (default), `pan-when-zoomed`, or `navigate` next/prev
  — `scroll-behavior` setting. In `navigate` a notch is exactly `l` / `h`:
  it stops a running slideshow and goes through the Save/Discard/Cancel
  prompt (the slideshow stop is new with zb2; before it a notch turned the
  page under a running slideshow and left it running).
- **Click-drag** — pan when zoomed in.
- **Double-click** — toggle fit ↔ 100%.
- **Middle-click** — toggle mark on a grid cell (grid view) / toggle
  fullscreen (large view).
- **Touch** (large view; zb2, decision #48). None of these touches the
  mouse, the wheel or the `scroll-behavior` setting, and all work the same
  in fullscreen:
  - **Pinch** — zoom around the pinch midpoint (a touchpad pinch too —
    on **Wayland only**: X11 does not deliver touchpad pinch events to
    GTK 4, so under X11 only a touchscreen pinches), through the same zoom
    rule, 2 %–6400 % clamp (widened to the fit ratio) and NaN guard as
    the wheel
    (`src/gesture-math.c`, see "Zoom behavior"). The zoom is absolute from
    where the pinch began, and the picture **moves with the midpoint**:
    the image pixel under the fingers stays under them, so two fingers
    moved together at a constant distance drag the picture, as common
    viewers do. Over a **fitted** picture a two-finger move whose finger
    distance stays within 10 % keeps it fitted (it has nothing to pan, and
    fit must survive for `0`, a window resize and the swipe); pinching out
    and back within that 10 % returns to fit — **where the picture is now**:
    fit mode does not re-centre (a fitted picture may sit anywhere in its
    letterbox, as a one-finger drag leaves it), so the position the
    fingers gave it along the letterbox is kept rather than snapping back
    to where the pinch began; zoom and position are both continuous
    across the band's edge. Past the 10 % the zoom picks
    up **from the edge of that band**, not from where the fingers began:
    the first step out is still the fit size and the picture grows (or
    shrinks) smoothly from there, rather than jumping straight to 110 %
    (90 %) of fit. A pinch that starts while
    one finger is already dragging takes that drag away where the finger
    is — a pan stops; a crop tool lets go and keeps the rectangle as far as it was dragged; a straighten
    tool drops its horizon line **without levelling** (the line was never
    finished — ending it would level by whatever the finger jittered) —
    and the second finger never drags it.
  - **Swipe** — one finger, flicked horizontally: leftward = next image,
    rightward = previous. It must travel ≥ 80 px, end at ≥ 300 px/s in the
    same direction, and stay mostly horizontal (|dy| ≤ ½|dx|); a slower,
    shorter, vertical or reversing drag is not a swipe. It is exactly `l` /
    `h`: a dirty enhance preview raises the Save/Discard/Cancel prompt
    first. It is **refused** while the picture is zoomed wider than the
    window (the same finger is panning it — zoom out or `0` first) and
    while a crop / straighten tool is up (the tool owns every drag, and a
    navigation would abandon it). A flick whose finger was already down
    when a running slideshow stepped to the next picture turns **no** page
    when it lands (it was aimed at the picture the slideshow replaced;
    turning again would skip one); the slideshow keeps running, and the
    next flick navigates and stops it as usual. Touch only: a mouse drag
    still only pans.
  - **Two-finger tap** — toggle the info card (`i`). Both fingers down and
    the first one up within 250 ms, the midpoint moving ≤ 20 px and the
    finger distance changing ≤ 10 %; whatever tiny zoom the fingers caused
    is undone, and so is any pan the first finger made before the second
    landed (the view goes back to what it was before the *first* finger
    went down, and the 250 ms and 20 px count from that finger), so a
    fitted view stays fitted. A tap edits nothing either: when its first
    finger had grabbed the crop rectangle, the rectangle goes back to what
    it was before that finger went down. A touchpad pinch is never a tap.
    A new picture on screen mid-pinch (the slideshow, a preview render) ends
    the pinch: the rest of it neither zooms nor taps; a new picture or
    leaving the large view mid-drag ends that drag the same way (a tool
    lets go as on a pinch) — also when the new "picture" is none at all
    (the view blanked between files): the tool still lets go of the line
    or the rectangle it held.

## Zoom behavior

- Fit-to-window is the default on load.
- `0` toggles fit ↔ 100% (double-click also toggles).
- Zoom centers on cursor (mouse) / pinch midpoint (touch) / window center (keys).
  A scroll event does not always carry a pointer position — plain X11 wheel
  events carry none — and zoom then falls back to the **window center** rather
  than to a bogus point. Do not "simplify" that fallback away by ignoring
  `gdk_event_get_position()`'s return value: it writes NaN to its
  out-parameters on failure, and a NaN reaching the pan state makes the image
  vanish for good (hx0). `viewer.c` guards both the scroll centre and the pan,
  and the zoom rule itself (`gesture_math_zoom_about`, shared by the wheel,
  the keys and a pinch) refuses any non-finite input.
- Panning clamps so the image can't drift off-screen.
- Zoom is limited to 2 %–6400 % (`GGAZE_ZOOM_MIN`/`MAX`), except that either
  limit gives way to the fit-to-window ratio when that lies outside it: the
  upper one rises to it — a small enough image in a large window fits above
  6400 %, and clamping to the bare ceiling made zoom-in *shrink* it (jx0) —
  and the lower one falls to it — a 32768 px panorama in a 600 px window fits
  at 1.83 %, and clamping to the bare floor made zoom-out (`-`, the wheel, a
  pinch in) *enlarge* it (zb2). At either end zooming further is a no-op,
  never a reversal — also when a resize moved the fit ratio since the zoom
  was set (a panorama zoomed out to 1.83 % stays there, rather than jumping
  to 2 %, after the window widens; zooming back in, or a two-finger pan's
  wobble, moves it only as far as asked).

## Info overlay (`i`)

Small card, top-left or bottom-right:
- filename, dimensions, format, file size
- EXIF: camera, lens, focal length, aperture, shutter, ISO, date taken,
  orientation (auto-applied on load)
- **histogram** of the image on screen: red, green and blue as translucent
  bars over a grey luminance (Rec.709) curve, 64 bins, one shared scale, for
  judging exposure while culling. It is built from the *displayed* texture
  (so an active enhance preview shows the preview's histogram), but only
  when that texture provably belongs to the current file: from the grid, or
  while a cache-miss decode is still in flight (the previous picture stays
  up until the new one lands), the card comes up without a plot rather than
  with another file's. The plot follows the picture for as long as the card
  is up: a decode landing under the card fills its plot in, hold-Space
  swaps the plot to the original's and back, and a preset landing replaces
  it with the new preview's (the old plot is cleared at once, the new one
  is binned in the background), and `t` to the grid under a card takes the
  plot down while the text stays (`t` back fills it in again). The text and
  the auto-hide timer are not touched by any of that.
- **color space** (decision #45): the embedded ICC profile's own name for a
  PNG or JPEG that carries one — `Color space: Display P3 (embedded ICC)` —
  plus `; may be managed on enhance/export` inside the parentheses only
  when this build's enhance path would apply it as far as the headers tell
  (GEGL built in, a local file within the size caps, a JPEG only with the
  `jpeg` feature's libjpeg, a profile the enhancer vets and babl parses that
  is not sRGB and fits the image's colour components). "May": the
  whole-file checks are too slow for the card (docs/gegl.md), so a file
  whose data turns out broken still falls back. A PNG/JPEG with no profile
  reads `sRGB (assumed, no embedded profile)`; a profile container that is
  there but broken (or holds no profile) reads `embedded ICC profile
  unreadable (shown as sRGB)` — never a silent sRGB. WebP/AVIF/HEIF/JXL are
  not searched and read `not read for this format (shown as sRGB)`. The
  profile's name is the file's text: it is shown on one line (control
  characters become spaces) and cut at 64 characters with `…`. Padding
  between JPEG segments (which libjpeg skips) does not make a profile
  "unreadable". The plain large view is not colour-managed by ggaze (it
  shows what the decoder delivers); the enhance preview and the `s` export
  are, with GEGL, for a PNG/JPEG whose profile is not sRGB. **Compare
  semantics:** holding `Space` over such a managed preview shows the
  *managed* original (the file through the same managed decode, no preset),
  so the compare shows only what the presets did; turning the preview off
  (the last preset off, `x`) goes back to the plain, unmanaged view,
  so on a host whose decoder does not apply profiles (fedora:40's native
  loaders; a glycin desktop does apply them) that switch can show a colour
  shift no preset caused — the managed side is the correct one. Until the
  first render of a file lands, hold-`Space` shows the plain view.
Loaded lazily; never blocks display of the pixels. The histogram is gathered
in the same background task as the EXIF text, only once `i` is pressed. The
binning itself is subsampled to at most 512×512 pixels, so that part costs
the same for a 100-megapixel photo as for a small one; the pixels are read
straight out of the decoded texture's own memory (no copy, whatever the
size). Only a texture in a layout the binner cannot read (16-bit, float —
nothing the loader produces today) is converted through a transient copy,
which is capped at 32 megapixels (128 MiB) and skipped, plot-free, above
that or when the copy cannot be allocated. Navigating away hides the card
(and its plot) with the previous file; `i` on the new image shows the new
image's histogram.

## Grid view behavior

- Thumbnails load from the `thumbnail` cache (M7), decoding lazily as cells
  scroll into view; never block the grid on a full decode.
- Trashed (`.Trash`) and permanently-deleted items stay listed but **dimmed**
  with a small badge, so you can see culling progress at a glance. (Toggle to
  hide them entirely via a menu option / setting.)
- `h`/`l`/`j`/`k`, arrow keys, `g`/`G`, and click all move the cursor: `h`/`l` prev/next cell, `j`/`k` row
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
- A toast confirms ("Copied image" / "Copied 3 files").

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
  preview). To open the modified version, export it first (`s`).
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

- **`a` → the edit panel**: a narrow column *beside* the large view,
  inside the main window (switching to the large view first if needed) —
  no second window, no popover. The image keeps the whole viewer. It is the
  **one home of every edit**, compact enough that all eight built-in
  presets show without scrolling in a 1280×800 window (your own presets
  add rows below them, and the preset list scrolls), and every button in
  it shows its key (read from `shortcuts.c`'s table, like the hint bar) —
  and every panel key has a button. Top to bottom:
  - **Title row** — just *Edit* (the window title already names the file)
    and a flat close button `a/Esc ✕` (tooltip "Close (a/Esc)").
  - **Original** — a small reference thumbnail beside "Original · hold
    Space to compare", dim and frameless so it never reads as a preset.
    Reverting is `x`, not a click on it.
  - **Presets** — one **row card** per preset: the eight built-ins, then
    **your own** from Preferences in their Preferences order (ai2), at
    most 32 rows in all (24 of yours; Preferences refuses more). The first
    eight rows carry their digit (`1`–`8`); a row past them shows its name
    alone. Each row: a
    small preview thumbnail of that preset applied alone, `1  Auto-fix`,
    its **strength** when it has one to tune (`+0.5`, `1.3`; 8i2), and a
    check mark. An enabled preset's row is **highlighted and checked**; the
    **selected** row (`j`/`k`, a digit, a click) wears a **ring**, a
    different mark from the on state, and under it — for a tunable preset
    — its **strength slider** (the preset's own range, a tick at the
    default; one slider at a time, so all eight rows still fit).
    Preferences can turn the thumbnails off for label-only rows;
    the rows and the Original always show the **untransformed** image —
    they are references for the colour presets alone and ignore a crop,
    straighten or turn. More presets than fit the window **scroll** — only
    the preset list: the title, Original, Transform and the actions below
    never move — and `j` / `k` scroll the selected row (with its slider)
    into view.
  - **Transform** — one row of four icon buttons with key badges and
    tooltips: crop `c`, straighten `r`, rotate left `[`, rotate right `]`
    (the same actions as the keys).
  - **Save state and actions** — one line saying *No edits yet*, *Unsaved
    edits · original kept* or *Saved as …*, then **Save copy** `s` (naming
    the file it will write, `as IMG_0001-enhanced.jpg`) beside **Revert**
    `x`, and under them **Undo** `u` and **Redo** `Shift+u`, each
    insensitive while there is nothing to undo / redo.

  Example:
  ```
  Edit                       a/Esc ✕
  [thumb] Original
          hold Space to compare
  PRESETS
  [thumb] 1  Auto-fix             ✓   <- on: highlighted + checked
  [thumb] 2  Brightness    +0.8   ✓
  [thumb] 3  Contrast       1.3
 ([thumb] 4  Saturation     1.6   ✓)  <- selected: a ring ...
   ━━━━━━━━━━━━━━━━━━━●━━━━━━━          ... and its slider
   …
  [thumb] 8  Denoise          4
  [thumb] Film look               ✓   <- your own: no digit, j/k + Enter
  [thumb] Punch           1.3         <- ... tunable like a built-in
  TRANSFORM
  [✂ c] [⟳ r] [↶ [] [↷ ]]
  Unsaved edits · original kept
  [⤓ Save copy   s] [⟲ Revert x]
     as IMG_0001-enhanced.jpg
  [↶ Undo      u] [↷ Redo Shift+u]
  ```
  While the panel is open the key-hint bar under the image lists its keys
  (see "Edit modes and the key-hint bar"). Presets are **layered**:
  pressing `1` toggles "Auto-fix" on as a **non-destructive live preview**,
  and `2` composes "Brightness" on top of it — press either again to toggle
  it back off. The digits are the **panel's keys**: with the panel closed
  they do nothing, so no edit can change without the panel showing it. A
  hotkey/card click does **not** close the panel (toggling combinations
  while comparing is the point); `Esc`, re-pressing `a` or the close
  button closes it and **keeps** the edit on screen — `Esc` never
  discards. Closing the panel while a crop or straighten tool is up (its
  close button, the menu) cancels the tool first, as `Esc` would, so no
  tool is ever left running without the panel. **`x`** (or *Revert*)
  drops every edit, presets and transform, with a status line; with the
  panel closed `x` only says where it works. `c`, `r`, `[` and `]` open
  the panel first when it is closed, then act. The panel stays open across
  navigation (re-previewed and its Save target renamed for the new image)
  and is **hidden** with the grid, back with the large view — while hidden
  it is no key mode: digits do nothing, `x` says it works in the large
  view, `Esc` goes straight on to the grid's marks / quit steps.
- **Preset strength (8i2).** A preset may have **one tunable number** —
  Brightness's exposure, Contrast's contrast, Saturation's scale, Cool's
  exposure, Sharpen's amount, Denoise's iterations (Auto-fix and Warm have
  none). With the panel open, `h` / `l` lower / raise the **selected**
  preset's strength one step (`Shift`: five) along the grid its slider
  snaps to,
  within its range, and **turn it on** if it was off; its card and slider
  show the value, the status line says it (*Brightness +0.6*, or
  *Brightness is at its maximum (+2)*), and the title names it when it is
  not the default (`… · Auto-fix, Brightness +0.6`). A mouse user drags
  the selected card's slider instead (clicking a card toggles and selects
  it, showing its slider); the value snaps to the preset's steps. On a
  preset with nothing to tune `h` / `l` say *Auto-fix has no strength to
  adjust (on / off only — Enter toggles it; ←/→ or PgUp/PgDn change
  image)* and change nothing. The mouse wheel over a slider scrolls the
  cards, never the strength. The
  strength is part of the edit: the render (coalesced while `l` is held —
  the one in flight and one more, the last value always lands), `s`'s
  export, the saved / dirty rule (a strength moved off the saved value is
  unsaved, moved back saved again; only presets that are on count — a
  tuned preset switched off is no unsaved change) and undo all include
  it; a run of `h` / `l` presses on one card undoes as **one step**
  (another card, a save or closing the panel ends the run, so undo can
  stop at the saved value), as does one drag of the slider. A
  Preferences change keeps every strength (see "Editing your presets
  while they are on" below). Every preset is back at its **default** on another image and
  after `x`. The cards' preview thumbnails show each preset at its
  default. The **selection** is not an edit: it survives navigation and
  undo never moves it.
- **Undo / redo of edit steps.** With the panel open and on screen, `u` /
  `Ctrl+z` (or *Undo*) undo the last **edit step** and `U` /
  `Ctrl+Shift+Z` (or *Redo*) redo it; the status line names it — *Undid:
  Auto-fix on*, *Redid: crop*, *Undid: rotate right*, *Undid: straighten
  1.0° CW*, *Undid: Brightness +0.8*, *Undid: revert all* — or says
  *Nothing to undo* / *Nothing to redo*. An edit step is a preset toggled
  (digit, card or `Enter`), a run of strength steps on one card (`h` / `l`
  pressed any number of times, or one slider drag; another card, another
  kind of step or a new drag starts a new one), one quarter
  turn (each `[` / `]`), a crop applied with `Enter`, a straighten applied
  with `Enter` (one step, however many nudges it took), and **`x`** — so a
  revert is undoable: `u` right after `x` puts every edit back at once.
  Opening or cancelling a tool is no step. A new step after an undo drops
  what could have been redone; the history keeps the last **64** steps,
  forgetting the oldest. It belongs to **the image**: another image (a
  navigation, an open, the folder running on), the Save/Discard gate's
  *Discard*, the slideshow's silent discard and a failed render all
  clear it — only `x` is itself a step. Undo and redo put the whole edit
  state back through the same live-preview path a toggle takes, so the
  saved / dirty rule holds exactly: stepping back to the combination `s`
  wrote is *saved* again (no prompt on moving on), stepping off it is
  unsaved. Under a crop or straighten tool `u` / `U` are **refused** with
  "Finish the current tool first (Enter applies, Esc cancels)", like `[` /
  `]`: the tool's own nudges are not steps, and cancelling the tool on a
  `u` meant as "take my last nudge back" would silently drop the whole
  rectangle or angle. A preset toggled *under* a tool is a step that
  records the transform the tool started from, never its unapplied
  working angle. Outside the panel — closed, or hidden with the grid —
  `u` / `Ctrl+z` undo the last trash or move exactly as before, and the
  menu's *Undo edit* / *Redo edit* only say where they work.
- **Hold `Space`** shows the original; release shows the current edit — with
  or without the panel.
- Presets are GEGL op graph strings, the built-ins included (Brightness =
  `gegl:exposure exposure={s:0.5:-2..2:0.1}`; the full table is in
  [gegl.md](gegl.md) "Built-in presets"). User presets are configured in
  Preferences (`,`) as `enhance-presets` (`a(ss)` name → gegl-graph); one
  number in a graph may be marked tunable with
  `{s:DEFAULT:MIN..MAX}` or `{s:DEFAULT:MIN..MAX:STEP}` (C-locale
  numbers; the step defaults to a twentieth of the range), which gives the
  preset its card value, slider and `h` / `l` — a graph without one is on /
  off only, exactly as before. The preset editor explains the syntax and
  refuses a malformed placeholder with the reason (a second one, a missing
  field, an empty range, a default outside it, a step that is not
  positive). Each of your presets is a **row of the edit panel** after
  the built-ins, in the editor's order: `j` / `k` select it, `Enter`
  toggles it, `h` / `l` and its slider tune it, and it is in the title,
  the save state and undo exactly like a built-in (ai2). The panel holds
  32 rows, so **24** presets of yours: at that many the editor's *Add* is
  insensitive and says why, and an entry stored past it (written with
  `gsettings`, say) is listed as *Ignored*.
- **Editing your presets while they are on (ai2).** Preferences applies
  every change at once, and the edit on screen follows **its presets**,
  not their row numbers: each preset keeps its on / off, strength and the
  selection in whatever row it moves to. A preset is the same one when it
  has the same name and graph; else the same name (its graph edited —
  a kept strength is clamped into the new range); else the same graph
  (renamed). So adding, moving, editing or renaming one never turns
  another on or off, and a **removed** preset simply drops out of the
  edit. The undo history is carried along the same way (a step that only
  toggled a removed preset goes with it) rather than cleared, so the
  steps taken before still undo. The picture renders again only when what
  it runs changed — an enabled preset edited or removed, two enabled ones
  swapped; a copy saved earlier stays *saved* only while it still renders
  the same (otherwise the state is unsaved, and moving on prompts). An
  edit of both the name and the graph at once reads as a removal plus a
  new preset. Hiding built-ins you never use is not offered: their rows
  and digits stay fixed, which keeps `1`–`8` meaning the same everywhere.
- Applying the enabled preset chain runs off the GTK main thread (a GTask
  worker); the UI stays responsive while GEGL processes, and a newer
  toggle/navigation/discard supersedes a still-in-flight one (last-write-wins
  — its result is dropped when it lands).
- **`s` / `Ctrl+S`** (or the panel's *Save copy* button, or menu *Save
  edited copy*) writes the enhanced result to `<name>-enhanced.<ext>`, or
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
  off, or `x` / *Revert* with the panel open, discards it directly
  (explicit, no prompt). `Esc` and `0` never do. Slideshow auto-advance is the one exception: it discards a dirty
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
  when settled on an image). If the build has no GEGL, `a`, `c`, `r`, `[`,
  `]`, `s` and `x` (and the menu's *Undo edit* / *Redo edit*) show a "GEGL
  not built in" status message instead of opening anything (the digits,
  which belong to the panel, do nothing; `u` / `Ctrl+z` are the file undo
  as ever). See
  [gegl.md](gegl.md).

## Crop, straighten & rotate tools (GEGL)

Non-destructive, like enhance — they add ops to the same live preview graph
(compose order, decision #35: colour presets → rotate 90° → straighten →
crop); `s` exports the composed result and navigating away prompts
Save/Discard/Cancel exactly as for a preset. The title names what is on
screen (`… · Auto-fix · 90° CW, crop`). Large view only; in the grid `c`,
`r`, `[` and `]` first open the highlighted image large, and they open the
edit panel first when it is closed (its *Transform* buttons are the same
actions). If GEGL is not built in, all four report "GEGL not built in".
While a tool is up the key-hint bar under the image lists its keys.

- **`c` → crop tool:** a rectangle overlay on the image (outside dimmed,
  rule-of-thirds lines, corner handles), starting as the whole image — or as
  the crop already applied, so it can be adjusted rather than redrawn. A crop
  that a straighten has pushed entirely outside the view crops nothing, so
  the tool starts from the whole image again then (never from a sliver
  clamped into the border).
  - Mouse: drag inside to move, drag an edge or corner to resize.
  - Keyboard: `h`/`j`/`k`/`l` move the rectangle; `Shift`+`h`/`j`/`k`/`l`
    **grow** the side the key points at (`Shift+h` moves the left edge out,
    `Shift+l` the right, `Shift+k` the top, `Shift+j` the bottom),
    `Ctrl`+`h`/`j`/`k`/`l` **shrink** that side (moves it in) — all four
    edges alike, 1 % of the image's shorter side per press. `a` cycles the
    aspect lock **free → 1:1 → 3:2 → 4:3 → 16:9 → original (the image's own
    shape) → free**, each the largest such rectangle centred inside the
    rectangle as it was before the run of `a` presses (so cycling never
    shrinks it, and coming round to *free* gives that rectangle back); the
    status line names the lock and the next one (`Crop aspect: 1:1 (a:
    next is 3:2)`). *Original* comes last because the tool starts on the
    whole image, where it changes nothing — as the first press it made `a`
    look dead. The digits are not crop keys (`0` is zoom, `1`–`8` the
    panel's presets).
  - `Enter` applies (`gegl:crop`; a rectangle still covering the whole image
    removes the crop), `Esc` or `c` again cancels and restores. `Enter` and
    a drag are refused while `Space` holds the original ("Release Space
    first") or the preview under the rectangle is still rendering ("Preview
    still rendering"); the rectangle is hidden meanwhile, and a refused drag
    grabs nothing, so the gesture cannot go on once the preview is back.
  - While the tool is open the image is shown **without** its crop so the
    rectangle can be adjusted — but the crop already applied still counts:
    `s` inside the tool exports the cropped copy and navigating away still
    prompts for it. A saved crop stays saved through `c` / `Esc`.
- **`r` → straighten tool:** level the horizon; a grid overlay helps.
  - Mouse: drag a line along the horizon; the image rotates to level it
    (the angle adds to the current one).
  - Keyboard: `h` / `-` nudge counter-clockwise, `l` / `+` clockwise, by
    0.5°, within ±45°; `a` toggles the auto-crop of the rotated corners
    (default on, decision #35; off keeps the whole rotated bounding box —
    the preview then shows **transparent** corners while a JPEG export gets
    **black** ones, since JPEG has no alpha; a PNG export keeps them
    transparent). The auto-crop trims one extra pixel per side so every
    kept pixel is opaque; that needs an image whose inscribed rectangle is
    at least 3 px per side — straightening anything smaller (a 3×2 at 45°)
    is not meaningful and may keep translucent edge pixels.
  - Every change renders live (`gegl:rotate` about the centre); holding a
    nudge key queues one re-render, not one per repeat. `Enter` keeps it,
    `Esc` or `r` again restores the angle the tool started with. A crop
    already applied follows the changing image: its rectangle is kept whole
    and re-centred on the centre the straighten turns about, and only the
    part of it inside the straightened image is cropped (in the preview and
    the export). So a rectangle touching the border is never eroded — nudge
    away and back and the crop is exactly what it was, unconditionally: when
    nothing of it lies inside the image at the new angle it is kept but
    crops nothing, the title says `crop (outside view)`, the status line
    says so, and `s` exports without a crop until a nudge back (or `Esc`)
    brings the image back over it.
  - A horizon drag is measured on the picture on screen, so it is refused
    (with a status line) while a render is still pending after fast nudges
    ("Preview still rendering") or while `Space` holds the original
    ("Release Space first"); repeat the drag once the preview is back.
- **`[` / `]` → rotate 90°:** one-shot, no overlay — `]` clockwise, `[`
  counterclockwise; repeat to reach 180°/270°, four presses are the original
  again. Non-destructive (`gegl:rotate`, an exact pixel permutation); a crop
  already applied turns with the image.
- The tool keys are **modal**: while a tool is active they belong to it
  (`h` moves the rectangle instead of going to the previous image), the
  other tool's key and `[`/`]` are refused until `Enter`/`Esc`, and any key
  not listed keeps its usual meaning. Navigating away (opening another file
  or folder with `o`/`O`/a drop included — the open runs the same reset a
  navigation does, whichever position the file sorts to), or leaving the
  large view, ends a tool without applying it — leaving the large view is
  an `Esc`:
  a nudged straighten goes back to the angle it started with, the crop
  tool's rectangle is dropped and the crop already applied stays. So does
  discarding the preview
  under it — `x` / *Revert*, the gate's Discard, the slideshow's
  auto-advance, or a render that failed — the tool is gone before the
  transform is reset, so nothing it was editing can come back on a later
  nudge or `Enter`. `?` lists the tool keys under *Crop tool (c)* and
  *Straighten tool (r)*; the key-hint bar lists them while the tool is up.
- The crop rectangle is drawn over, and its drags measured on, only the
  exact picture rendered for the current state (not merely one of the same
  size): after `]` `]` then `c`, or a preset toggled with the tool up, the
  rectangle appears — and `Enter` is accepted — once that render is on
  screen.
- All compose with enhance presets in the same preview graph; hold `Space`
  compares against the original as usual.

## Compare original vs modified (hold)

- **Hold `Space`** to momentarily show the **original** image; **release** to
  return to the **modified** (preview-graph) image — a quick before/after to
  decide whether to `s` save. Only meaningful when a preview (enhance / crop /
  straighten / rotate) is active; otherwise original == modified, no-op.
- Large view only. Works with or without the edit panel open; the panel's
  key-hint bar names it. GUI: menu *Show original* (toggle) for mouse users.
- On an animated GIF/WebP the modified image is a still of the first frame;
  holding `Space` shows the original animation (from its first frame).

## Hotkey visibility

Hotkeys are not hidden — each is printed on the element it triggers:

- **Menu items** show their key right-aligned, e.g. `Copy   Ctrl+c`,
  `Move …   m`, `Open in …   e`, `Scripts …   !`, `Edit panel   a`,
  `Crop   c`, `Straighten   r`, `Rotate left   [`, `Rotate right   ]`,
  `Save edited copy   s`, `Revert all edits   x` (short labels; `?` keeps
  the long descriptions),
  `Show original (hold)   Space`, `Slideshow   S`, `Trash   d`, `Delete   D`,
  `Preferences …   ,`, `Fullscreen   f`.
- **Header-bar buttons** show the key in the tooltip (plus an underline
  mnemonic where GTK draws one).
- **Popup** entries (move / open-in / scripts) and the edit panel's
  preset cards lead with the hotkey: `1  irregular ninja`, `1  GIMP`,
  `1  usbimport`, `1  Auto-fix`; the panel's other buttons carry a key
  badge (`✂ c`, `Save copy   s`, `Revert   x`, `a/Esc ✕`).
- **Key-hint bar**: while the edit panel or a tool is active, a bar under
  the image lists that mode's live keys (see "Edit modes and the key-hint
  bar").
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
and by click: Copy, Move…, Open in…, Scripts…, Edit panel, Crop, Straighten,
Rotate left/right, Save edited copy, Revert all edits, Show original, Trash, Delete, Sort (by
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
