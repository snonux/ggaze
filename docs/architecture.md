# Architecture

Layered: a thin GTK shell over a small set of focused C modules. Each module
has one job and a narrow interface; the UI never calls decoders directly.

## Module sketch

```
ggaze
├── main.c                # entry point, CLI arg parsing, GtkApplication setup
├── app/                  # GApplication, GActions (open, quit, prefs), single-instance
├── window.{c,h}          # GgazeWindow : GtkApplicationWindow — owns the layout, switches grid/large
├── viewer.{c,h}          # GgazeViewer : GtkWidget — large single-image canvas, zoom/pan, displays a GdkTexture
├── gridview.{c,h}        # GgazeGrid : GtkGridView/FlowLayout — thumbnail overview of the folder
├── trash.{c,h}          # .Trash folder management + permanent delete; restore/undo
├── mover.{c,h}          # configurable move destinations; move marked set into a dir (undoable)
├── opener.{c,h}         # configurable external programs; launch current image (GSubprocess)
├── runner.{c,h}          # configurable shell scripts; async run via /bin/sh -c, rescan on done
├── enhancer.{c,h}        # (optional) GEGL quick-enhance presets; non-destructive apply + export copy
├── clipboard.{c,h}       # image/png (displayed texture) or file-URI content providers (no state)
├── viewload.{c,h}        # large-view load pipeline: texture LRU, one active load, prefetch, last-write-wins
├── info-overlay.{c,h}    # EXIF card + histogram + status line over the stack (async gather, auto-hide)
├── histogram.{c,h}       # RGB/luminance binner over the displayed texture (plain C, subsampled)
├── histogram-view.{c,h}  # GgazeHistogramView : GtkWidget — snapshot-drawn plot inside the card
├── save-gate.{c,h}       # Save/Discard/Cancel prompt gate every discarding continuation funnels through
├── delete-confirm.{c,h}  # >1-target permanent-delete confirm (captured targets, folder re-check)
├── dialog-util.{c,h}     # alert-dialog toplevel lookup shared by the two dialog modules
├── enhance-ctrl.{c,h}    # (optional) enhance feature controller: mask, previews, side panel, saved flag, async save
├── enhance-ui.{c,h}      # (optional) pure enhance side-panel widget construction
├── popup_list.{c,h}      # shared hotkey list popover (e / ! / m)
├── undo.{c,h}            # which of trash/move `u` undoes
├── pathutil.{c,h}        # stem/ext split, safe mkdir -p, non-colliding child names
├── settings-pair.{c,h}   # the (name, value) pair of the a(ss) settings lists
├── ggaze-enums.h         # the shared preference enums (sort, background, scroll)
├── loader/
│   ├── loader.{c,h}      # sync + async load API; sniff, dispatch, explicit pixbuf fallback
│   ├── detect.{c,h}      # sniff format from contents (magic), not extension; dimension caps
│   ├── animation.{c,h}   # animated GIF/WebP: decoder-free frame probe, playback budget, delay clamp, frame store, texture <-> frames channel
│   ├── pixbuf-util.{c,h} # GdkPixbuf -> upright GdkTexture (shared by three decoders); bytes -> pixbuf / animation decode; animation -> one owned texture per frame
│   └── backends/         # one file per format family, behind a backend struct
│       ├── pixbuf.c      # fallback via GdkPixbuf (PNG/GIF/WebP/TIFF/ICO, JPEG without libjpeg)
│       ├── jpeg.c        # libjpeg-turbo: progressive low-res preview + full decode
│       ├── jxl.c         # libjxl
│       ├── avif.c        # libavif
│       └── heif.c        # libheif
├── croprect.{c,h}        # crop rectangle rules (move/resize/aspect/hit/drag/turn), plain C
├── transform.{c,h}       # rotate 90 / straighten / crop state + the sizes everyone agrees on, plain C
├── tool-ctrl.{c,h}       # the modal c / R tool session over the viewer (GEGL only)
├── navigator.{c,h}       # directory listing, sort, filter, prev/next, wrap, marks, monitor; "changed" carries flags
├── thumbnail.{c,h}      # freedesktop thumbnail cache (normal/large/x-large), bounded pool
├── texturecache.{c,h}   # bounded LRU of decoded textures, stamp-validated
├── settings.{c,h}       # GSettings schema wrapper
├── prefs.{c,h}          # Preferences dialog
└── shortcuts.{c,h}      # the ONE key table: bindings, ? help, header tooltips, menu labels
```

## Responsibilities

- **app** — owns the `GtkApplication`, registers actions, handles the `open`
  signal (files **or a directory** → window), single-instance behavior. A
  directory arg opens the folder in the grid; a file arg opens its parent
  folder with that file current; several files open the first one's folder
  in the grid with it current. Every open is **one pass** through
  `window.c _open_now`: the start file and the grid/large intent travel
  with the folder, so the cursor is placed before the navigator's `changed`
  handler is connected and the choke point, the load and the enhance panel's
  preview batch run once (the multi-file open used to open the folder and
  then place the cursor, which re-ran all of it for any start file not
  sorted first — gd2). Because the first entry now goes through the same
  `_open_resolve_target` / `_report_open_target` as a single-file open, it
  decides the folder the same way: a folder first opens that folder itself
  (the old path opened its parent), and a missing, non-image or hidden-RAW
  first entry opens its folder on the first-sorted image with the status
  line saying why (the old path reported the folder, never the entry, so
  it stayed silent). The remaining entries only ask for the grid.
- **window** — owns the two view modes (**grid** and **large**) in a
  `GtkStack`, the header bar, and the info overlay. Routes actions to
  navigator/loader/viewer/gridview; manages fullscreen state. Keeps the
  navigator cursor in sync so switching grid↔large preserves position. Tracks
  the enhance "dirty" flag and gates navigation on it (prompt
  Save/Discard/Cancel when an un-exported enhance preview is active). Routes
  the `c`/`R`/`[`/`]` tool actions to `tool-ctrl` / `enhance-ctrl` and claims
  a tool's modal keys ahead of the global shortcut table. Has a
  `GtkDropTarget` accepting dropped files/folders (open them).
- **viewer** — the *large* view. Pure display widget. Takes a `GdkTexture`
  (or `GtkSnapshot` paintable). Owns zoom level, pan offset, fit mode. Draws
  via GTK4 render nodes. Holds both the raw and GEGL-processed textures;
  `Space` swaps to the raw (compare) while held. Emits "needs-next" when nearing
  the end of a preloaded set. **Plays an animated GIF/WebP** (yb2): the
  texture it is given is that file's first frame with the other frames
  attached as textures of their own (`loader/animation.h`, decoded by the
  loader's worker), and the viewer alone steps through them — a tick
  callback on the frame clock picks the frame due at the clamped delay
  (`animation_playback_advance`, plain C), drawn at the first frame's
  geometry, only while mapped and while the frame clock runs. The tick is
  on the clock only near a frame change (a timeout re-adds it 40 ms
  before the next one; a tick callback makes GDK draw every vblank). It
  plays as many times as the file says (GIF NETSCAPE2.0 count N → N + 1
  plays, no block → once; WebP ANIM count as is; 0 → for ever) and then
  holds the last frame, restarts from the first frame on every
  `set_texture`, remap and hold release, and holds the first frame while
  a crop / straighten tool is up. Every other accessor, and every other
  module, keeps seeing the first frame.
- **gridview** — the *thumbnail* view. A `GtkGridView` (or `GtkFlowBox`)
  backed by a `GListModel` of the navigator's files, each cell rendered from
  the `thumbnail` cache. Thumbnail size is adjustable (`+`/`-`); cells reflow
  to fit the window. Size comes from GSettings `thumbnail-size`. Selection
  follows the navigator cursor. Double-click / `Enter` switches to large view
  on the selected item.
- **loader** — runs decode in a `GTask` thread, returns a `GdkTexture` on the
  main thread. Format detection by content sniffing. **Applies EXIF
  Orientation** so the texture is upright (GdkPixbuf path:
  `gdk_pixbuf_apply_embedded_orientation`; other backends read the EXIF tag and
  rotate/flip). Backend selected at build time via meson `feature` options.
  **Decode gate** (tb2): before any decoder — including the path-taking
  gdk-pixbuf calls the thumbnail (`loader_load_pixbuf_scaled`) and info
  (`loader_peek_dimensions`) paths make — every entry point sniffs the first
  64 bytes (`g_input_stream_read_all`, so FIFOs and GVFS streams sniff
  correctly) and refuses an empty file, a file shorter than its signature's
  smallest complete file (`detect_reject_truncated`, per-signature minimum
  table in `detect.c`) and, when the `jxl` feature is off, any JXL at all
  (`G_IO_ERROR_NOT_SUPPORTED`). Reason: on a glycin desktop gdk-pixbuf hands
  the file to a sandboxed loader with no cancel or timeout, and `glycin-jxl`
  waits forever on any garbage JXL — an uncancellable hung worker is worse
  than a missing decode. `loader_peek_dimensions` sizes a JPEG from its SOF
  header (decoder-free; an oversized or prefix-exceeding header fails
  closed without asking gdk-pixbuf, a zero side is never reported), a
  backend-claimed format through that backend, and only the rest through
  `gdk_pixbuf_get_file_info`. `loader_sniff_bytes` is the gate on bytes
  already in memory: the pixbuf backend re-runs it on the buffer it
  decodes as `loader_sniff_bytes_for_fallback` (the gate plus the dispatch
  rule -- whatever a specific backend of the build claims is refused, which
  is what keeps a JXL out of gdk-pixbuf in a libjxl build too), and the
  thumbnail cache read reads its entry once, bounded at
  `GGAZE_THUMB_ENTRY_MAX_BYTES`, gates those bytes and decodes a PNG entry
  only (through a `GdkPixbufLoader`, never by path) -- so wherever the
  gated bytes are the decoded bytes the guarantee is exact, and only the
  two path-taking gdk-pixbuf calls (the at-scale
  thumbnail decode, the header-only size peek) remain best-effort against
  a file swapped between the sniff's open and theirs. Side effect: a
  missing or unreadable file fails the thumbnail path as a `G_IO_ERROR`
  from the sniff, no longer as gdk-pixbuf's `G_FILE_ERROR`.
  Details in [tech-stack.md](tech-stack.md) "The decode gate".
- **navigator** — given a starting file, lists the parent directory, filters
  to image MIME types, sorts (name/time/size), exposes `current/prev/next`.
  Also owns the **mark set** (multi-select): `navigator_toggle_mark`,
  `navigator_mark_range`, `navigator_mark_all`, `navigator_clear_marks`,
  `navigator_get_marks` (returns a `GList` of `GFile*`). Emits a `changed`
  signal on sort/filter/trash/move so grid + large stay in sync. Watches the
  directory with `GFileMonitor` and emits `changed` on external
  adds/deletes/moves (debounced); if the current file is removed, falls back
  to the nearest. Owns no GTK state; testable standalone.
- **trash** — moves a file to `<dir>/.Trash/` (creating it lazily), preserving
  relative path uniqueness (suffix `-1`, `-2`… on collision). `D` calls
  `g_file_delete` instead. Tracks the last trashed item for `u` undo/restore.
  Never touches the system trash.
- **mover** — owns the configured destination list (loaded from settings) and
  performs `g_file_move` for a set of `GFile*` into a chosen destination, with
  collision suffixing. Records the last move (paths + dest) so `u` can move
  them back. Exposes `mover_get_dests` (ordered, for the popup + hotkey
  assignment) and `mover_move(GList *paths, MoverDest *dest, GError **)`.
- **opener** — owns the configured external-program list (loaded from
  settings). Expands `%f` in the command and launches it
  detached via `GSubprocess` (`g_subprocess_new`). Exposes
  `opener_get_progs` (ordered, for the popup + hotkey assignment) and
  `opener_launch(GFile *file, OpenerProg *prog, GError **)`. Owns no GTK
  state; the window owns the popup.
- **runner** — owns the configured shell-script list (loaded from settings).
  Expands `%f` (current image) and `%d` (current folder) in the command and
  runs it **asynchronously** via `/bin/sh -c` (`GSubprocess` with
  `g_subprocess_wait_async`); substituted paths are single-quoted to prevent
  shell injection. Exposes `runner_get_scripts` (ordered, for the popup +
  hotkey assignment) and `runner_run(GFile *file, GFile *dir,
  RunnerScript *script, GAsyncReadyCallback on_done, GError **)`. On
  completion the window calls `navigator_rescan()` (scripts may mutate the
  folder) and shows a toast with the exit status. Owns no GTK state.
- **enhancer** *(optional, if GEGL is enabled)* — owns the enhance-preset list
  (loaded from settings). Builds a GEGL op graph for a preset and applies it
  to a `GeglBuffer` in a `GTask` thread: `enhancer_get_presets`,
  `enhancer_apply(GeglBuffer *in, EnhancerPreset *, GError **) → GeglBuffer*`,
  `enhancer_export(GeglBuffer *in, EnhancerPreset *, GFile *out, GError **)`.
  The window imports the decoded image into a `GeglBuffer` when a preset is
  active and renders the result back to a `GdkTexture`. The crop/straighten/
  rotate tools add `gegl:rotate`/`gegl:crop` to the same graph via the
  enhancer, from one plain-C `Transform` (decision #35 order). GEGL also
  backs color-managed decode/export (ICC, not yet wired). Owns no GTK state.
- **clipboard** — stateless provider builders for the `GdkClipboard`:
  `clipboard_build_texture_provider(GdkTexture *)` offers the DISPLAYED
  texture as `image/png` (already decoded, so only the PNG encode runs, on
  the caller's thread); `clipboard_build_uri_provider(GList *files)` offers
  the marked files as `text/uri-list` + `text/plain`. The window picks one in
  `ggaze_window_get_copy_provider` (marks → URIs, else pixels) and sets it;
  the builders never touch the clipboard, so the decision is testable
  without a clipboard round trip.
- **thumbnail** — reads/writes `~/.cache/thumbnails/` per the freedesktop
  Thumbnail Managing Standard; shared so multiple windows don't re-decode.
  Also feeds the gridview cells.
- **settings** — wraps a `GSettings` schema: sort order, wrap, background
  colour, scroll behavior (zoom vs navigate), slideshow delay,
  `thumbnail-size` (grid thumbnail pixel size), hide-trashed toggle,
  `destinations` — an ordered `a(ss)` array of `(name, path)` pairs
  used by the move popup, and `editors` — an ordered `a(ss)` array of
  `(name, command)` pairs used by the `e` open-in popup (`%f` = current path),
  and `scripts` — an ordered `a(ss)` array of `(name, command)` pairs used by
  the `!` run-script popup (`%f` = current path, `%d` = current folder; run
  via `/bin/sh -c`), and `enhance-presets` — an ordered `a(ss)` array of
  `(name, gegl-graph)` pairs for the `a` enhance popup (GEGL only). List
  order = hotkey order (`1`, `2`, …).

## Data flow (next image, large view)

```
key 'l' → window action "next"
        → navigator.next() → path2
        → loader.load(path2, cancellable)        [thread]
        → GdkTexture ready                       [main thread]
        → viewer.set_texture(texture)
        → thumbnail.ensure(path2)                [background]
```

## Data flow (grid ↔ large)

```
grid Enter / double-click → window.set_view(LARGE)
                          → navigator.set_current(selected_path)
                          → viewer shows that image
large Esc / Backspace     → window.set_view(GRID)
                          → gridview scrolls cursor into view, focused
```

## Data flow (trash / delete)

```
key 'd' → trash.bin(path)   → mv path → <dir>/.Trash/<name>  (undoable)
key 'D' → trash.delete(path) → unlink(path)                  (not undoable)
       → navigator_mark_removed(path) → dims the entry (stays listed) and
         clears its mark → emits 'changed'
       → gridview dims the cell; large view advances to next
```

## Data flow (move)

```
key 'm' → window shows move popup (GtkPopover)
        → mover_get_dests() → [ {"irregular ninja", ~/…}, {"alt …", …}, … ]
        → popup assigns hotkeys 1..9,0,a.. by list order
key '2' → mover_move(marked_paths, dests[1], &err)
        → g_file_move each (rename/copy+delete), collision-suffix
        → navigator_mark_removed(each) → dims the entries (mirrors trash;
          stays listed) and clears their marks → emits 'changed'
        → grid dims the cells; large advances; counter updates
        → mover records the move for 'u' undo
no marks? → move acts on navigator.current instead
'u' undo → whichever of the last trash or the last move happened more
           recently; falls back to whichever engine can still undo within
           the SAME folder session (e.g. move, then trash, then undo twice)
           → reopening a folder resets both engines' undo state together,
           so a stale record from a folder no longer open is never reachable
```

## Data flow (open in external program)

```
key 'e' → window shows open-in popup (GtkPopover)
        → opener_get_progs() → [ {"GIMP", "gimp %f"}, {"identify", …}, … ]
        → popup assigns hotkeys 1..9,0,a.. by list order
key '2' → opener_launch(current_path, progs[1], &err)
        → expand %f → argv; g_subprocess_new (detached)
        → toast on failure; ggaze stays responsive, image stays open
```

## Data flow (run shell script)

```
key '!' → window shows scripts popup (GtkPopover)
        → runner_get_scripts() → [ {"usbimport", "~/scripts/usbimport %d"}, … ]
        → popup assigns hotkeys 1..9,0,a.. by list order
key '1' → runner_run(current_path, dir, scripts[0], on_done, &err)
        → expand %f/%d (single-quoted) → /bin/sh -c "<cmd>"
        → g_subprocess_wait_async; ggaze stays responsive; toast: "running…"
on done → navigator_rescan() (scripts may add/remove files)
        → toast: "usbimport finished (exit 0)" or error

   Folder-identity safety (mirrors eu0): the completion callback captures the
   folder the script ran against at launch time and rescans ONLY if the window
   still navigates that same folder. A single-instance open / drop that
   replaced the folder while the script ran does NOT trigger a rescan of the
   new folder (the script never touched it); only the completion status is
   shown. The callback holds an owned window ref and checks a disposed flag so
   it is safe if the window was closed while the script ran.
```

## Data flow (quick enhance, GEGL)

```
key 'a' → enhance side panel appended beside the large view (in-window)
        → enhancer_get_presets() → [ {"Auto-fix", "stretch-contrast|color-enhance"}, … ]
        → one card per preset, hotkeys 1..8 by list order; thumbnail batch
        → panel stays across navigation (re-previewed), hidden with the grid
key '1' → import decoded image → GeglBuffer
        → enhancer_apply(buf, presets[0], &err)   [GTask thread]
        → GeglBuffer out → render to GdkTexture → viewer (non-destructive)
        → toggle off on second press; Esc closes the panel, then discards
key 's' → enhancer_export(buf, presets[0], out_file, &err)  [GTask thread]
        → on success the preview is SAVED: still shown, no longer dirty
        → writes IMG_0001-enhanced.<ext> via GEGL saver; original untouched
        → does NOT clear the dirty flag (press again → another numbered copy)
navigate with dirty preview → prompt: Save (export) / Discard / Cancel
        (all nav paths incl. grid/thumbnail selection, plus quit via 'q' or
         the WM close button; slideshow auto-advance discards silently)
GEGL disabled? → 'a' shows "GEGL not built in" toast
```

## Data flow (copy to clipboard)

```
Ctrl+c → ggaze_window_get_copy_provider(win)
         marks?    clipboard_build_uri_provider(marked_files) [text/uri-list + text/plain]
         no marks? clipboard_build_texture_provider(viewer texture) [image/png]
       → gdk_clipboard_set_content (main thread)
       → status: "Copied image" / "Copied N files"
```

Prefetch: when `navigator.current` changes, schedule `loader.load` for the
*next* and *previous* paths into a small (2–3 slot) texture cache so navigation
feels instant.

## Concurrency model

- Only the main thread touches GTK widgets.
- Decode happens in `GTask` worker threads (one at a time per load, with a
  `GCancellable` so a rapid `jjjj` cancels stale work).
- Thumbnail I/O on a low-priority thread or `GThreadPool`.
- A bounded LRU of decoded `GdkTexture`s (e.g. 4) to bound memory on large
  folders / huge images. Every entry carries the file's stamp (mtime to
  the nanosecond where the filesystem records it, byte count, inode) as
  the cache miss read it BEFORE the decode started, and a `get` re-checks
  it with one query: a file rewritten in place -- even within the same
  second to the same byte count -- or atomically replaced is evicted and
  decoded afresh, never shown stale, and a rewrite landing mid-decode
  costs a redundant decode rather than a stale hit. Limits: many
  kernels/filesystems stamp mtimes from the coarse clock (1-4 ms ticks),
  so a same-size rewrite within one tick is still a hit; a whole-second
  filesystem falls back to seconds + size; grid thumbnails validate on
  the spec's whole-second `Thumb::MTime` + size (decision #47).
  An animated GIF/WebP is one entry like any still —
  its first frame, with the other frames riding on that texture and
  evicted with it; what such an entry may hold is bounded by the playback
  budget in `loader/animation.h` (frames × canvas ≤ 32 Mi pixels, i.e.
  128 MiB of RGBA; a canvas ≤ 4 Mi pixels; ≤ 1000 frames; beyond it the
  file is shown as its first frame). Every frame is decoded and copied
  into a texture of its own on the `GTask` thread; playback on the main
  thread only picks which texture to draw (plus the renderer's one-time
  upload of each frame). Memory, measured on Fedora 44 (glycin): a
  109-frame 640×480 GIF, just under the cap, holds 135 MB after its load
  and peaks at 270 MB during it (the decoder's frames and the copies live
  side by side until the decoder is dropped); one frame more takes the
  still path at 8 MB. Worst case if every file in sight is a maximal
  animation: 4 cache entries × 128 MiB held plus three decodes in flight
  (the visible load and two neighbour prefetches, which take every frame
  too — a first-frame-only prefetch would leave an entry a hit could not
  play; `viewload.c` `_prefetch`) at ~256 MiB peak each, ~1.3 GiB,
  plus the renderer's upload of the visible animation's frames as they
  play (up to another 128 MiB, system RAM on an integrated GPU with
  shared memory), ~1.4 GiB.
  The enhance controller may hold two more outside that cap — the current
  file's original as the viewer last showed it (the identity the tools
  and hold-`Space` compare against, learned at the window's texture choke
  point) and the rendered preview — bounded to those two, and released
  on navigation (an open or a drop of another file included: the open
  path runs the same identity reset, since a file that sorts first in its
  folder never emits "changed"), on a rewrite's rescan, and in dispose.

## Threading / cancellation invariant

At most one *active* load per window. Issuing a new load cancels the previous
cancellable and drops its result. The viewer only ever shows a texture whose
path matches `navigator.current`.