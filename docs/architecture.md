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
├── trash.{c,h}          # ./Trash folder management + permanent delete; restore/undo
├── mover.{c,h}          # configurable move destinations; move marked set into a dir (undoable)
├── opener.{c,h}         # configurable external programs; launch current image (GSubprocess)
├── runner.{c,h}          # configurable shell scripts; async run via /bin/sh -c, rescan on done
├── enhancer.{c,h}        # (optional) GEGL quick-enhance presets; non-destructive apply + export copy
├── clipboard.{c,h}       # copy image (PNG) or file URIs to GdkClipboard (no state, helpers)
├── loader/
│   ├── loader.{c,h}      # async load API: load(path, cancellable, ready_cb)
│   ├── detect.{c,h}      # sniff format from contents (magic), not extension
│   └── backends/         # one file per format family, behind a backend struct
│       ├── pixbuf.c      # fallback via GdkPixbuf (PNG/JPEG/GIF/WebP)
│       ├── jxl.c         # libjxl
│       ├── avif.c        # libavif
│       └── heif.c        # libheif
├── navigator.{c,h}       # directory listing, sort, filter, prev/next, wrap, recurse(opt)
├── thumbnail.{c,h}      # freedesktop thumbnail cache (normal/large), shared/mutex
├── settings.{c,h}       # GSettings schema wrapper
└── shortcuts.{c,h}      # keybinding → GAction map (configurable later)
```

## Responsibilities

- **app** — owns the `GtkApplication`, registers actions, handles the `open`
  signal (files **or a directory** → window), single-instance behavior. A
  directory arg opens the folder in the grid; a file arg opens its parent
  folder with that file current.
- **window** — owns the two view modes (**grid** and **large**) in a
  `GtkStack`, the header bar, and the info overlay. Routes actions to
  navigator/loader/viewer/gridview; manages fullscreen state. Keeps the
  navigator cursor in sync so switching grid↔large preserves position. Tracks
  the enhance "dirty" flag and gates navigation on it (prompt
  Save/Discard/Cancel when an un-exported enhance preview is active). Hosts
  interactive tool overlays (crop, straighten) in large view. Has a
  `GtkDropTarget` accepting dropped files/folders (open them).
- **viewer** — the *large* view. Pure display widget. Takes a `GdkTexture`
  (or `GtkSnapshot` paintable). Owns zoom level, pan offset, fit mode. Draws
  via GTK4 render nodes. Holds both the raw and GEGL-processed textures;
  `Space` swaps to the raw (compare) while held. Emits "needs-next" when nearing
  the end of a preloaded set.
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
  settings). Expands `%f` (and later `%F`) in the command and launches it
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
  rotate tools add `gegl:crop`/`gegl:rotate`/`gegl:rotate-on-center` to the same
  graph via the enhancer. GEGL also backs color-managed decode/export (ICC).
  Owns no GTK state.
- **clipboard** — stateless helpers that put content on the `GdkClipboard`:
  `clipboard_copy_image(GdkClipboard *clip, GFile *file, GCancellable *,
  GError **)` decodes the image in a `GTask` thread and sets a
  `GdkContentProvider` for `image/png`; `clipboard_copy_uris(GdkClipboard
  *clip, GList *files)` sets `text/uri-list` (+ `text/plain`). Single image →
  pixels; marks → URIs. (Optionally union both providers so one file offers
  PNG + URI.)
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
       → navigator.remove(path) → emits 'changed'
       → gridview drops cell / dims it; large view advances to next
```

## Data flow (move)

```
key 'm' → window shows move popup (GtkPopover)
        → mover_get_dests() → [ {"irregular ninja", ~/…}, {"alt …", …}, … ]
        → popup assigns hotkeys 1..9,0,a.. by list order
key '2' → mover_move(marked_paths, dests[1], &err)
        → g_file_move each (rename/copy+delete), collision-suffix
        → navigator.remove(each) → emits 'changed'
        → grid drops cells; large advances; counter updates
        → mover records move for 'u' undo
no marks? → move acts on navigator.current instead
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
```

## Data flow (quick enhance, GEGL)

```
key 'a' → window shows enhance popup (GtkPopover)
        → enhancer_get_presets() → [ {"Auto-fix", "stretch-contrast|color-enhance"}, … ]
        → popup assigns hotkeys 1..9,0,a.. by list order
key '1' → import decoded image → GeglBuffer
        → enhancer_apply(buf, presets[0], &err)   [GTask thread]
        → GeglBuffer out → render to GdkTexture → viewer (non-destructive)
        → toggle off on second press / Esc
key 's' → enhancer_export(buf, presets[0], out_file, &err)
        → writes IMG_0001-enhanced.<ext> via GEGL saver; original untouched
        → clears the dirty flag for this image
navigate with dirty preview → prompt: Save (export) / Discard / Cancel
GEGL disabled? → 'a' shows "GEGL not built in" toast
```

## Data flow (copy to clipboard)

```
Ctrl+c → marks? clipboard_copy_uris(clip, marked_files)  [text/uri-list]
       → no marks? clipboard_copy_image(clip, current, cancellable, &err)
                  → decode in GTask → GdkPixbuf/Texture → PNG content provider
                  → gdk_clipboard_set_content (main thread)
       → toast: "Copied image" / "Copied N files"
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
  folders / huge images.

## Threading / cancellation invariant

At most one *active* load per window. Issuing a new load cancels the previous
cancellable and drops its result. The viewer only ever shows a texture whose
path matches `navigator.current`.