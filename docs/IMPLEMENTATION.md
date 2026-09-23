# ggaze — Implementation Plan

The engineering execution layer between the design docs and the first line of
code. The design is locked in via the decisions log in [PLAN.md](PLAN.md);
this page says *how* we build it, in what order, and what "done" means.

## Guiding principles

1. **Every milestone ends runnable** — and every commit within a milestone
   compiles + `meson test` passes.
2. **Plain-C modules first, GTK shell thin.** Build and unit-test the logic
   modules before (or alongside) the UI that consumes them, so coverage is
   natural rather than retrofit.
3. **Conventions are build-time enforced**, not just documented: a `clang-format`
   CI gate + an ASan/UBSan build lane are worth more than a doc page.
4. **Optional features are off by default in CI** (`gegl`, `jxl`, `avif`,
   `heif`), with one CI lane that builds `gegl=enabled` so it never silently
   bit-rots.

## Two test tracks (mandatory)

ggaze has **two complementary test tracks**. Both run under `meson test`.

### Unit tests — `tests/test_<module>.c`

- Target the **plain-C modules** (`detect`, `navigator`, `thumbnail`,
  `trash`, `mover`, `opener`, `runner`, `enhancer`, `info`, `histogram`,
  `texturecache`, `clipboard` helpers). No GTK display needed.
- GLib `GTest` framework; per-module `a(ss)`/path/EXIF fixtures.
- **Coverage gate ≥80%** on these modules (gcov/lcov), flipped to *fail* at
  M10, *warn* before.
- Each module lands its test file in the same milestone as the module.

### Integration tests — `tests/test_<flow>.c`

- Exercise **multiple modules together** through the real public API, with a
  real temp directory and real files (no mocks of `GFile`/`GFileMonitor`). They
  verify the *contracts* between modules the unit tests can't reach.
- They live **directly in `tests/`**, next to the unit tests, and are told
  apart by `suite : 'integration'` in `tests/meson.build` — not by directory.
  `tests/integration/` holds only a `meson.build`; no suite has ever lived
  there and none should be added there (5w0).
- There is **no offscreen or headless harness** and GTK4 has none to offer:
  each suite's `main()` calls `gtk_init_check()` and returns 77 (meson SKIP)
  when there is no display, and everything else runs against a **real GTK
  display**. Earlier revisions of this page prescribed `gtk_test_init` +
  `gtk_widget_realize` under `GDK_DEBUG=no-grabs`; none of that is true here.
  **`GDK_DEBUG=no-grabs` does not exist in GTK4** and never did under that
  spelling. Checked against the gtk-4.22.4 sources: neither the `GDK_DEBUG`
  key table (`gdk/gdk.c`, `gdk_debug_keys[]`) nor the `GTK_DEBUG` one
  (`gtk/gtkmain.c`) has any grab-related key; the unhyphenated `nograbs` that
  once existed was dropped in 4.13.4 ("Tweak GDK_DEBUG values … The gl-legacy
  and nograbs values have been dropped", GTK `NEWS`). An unknown key is a
  behavioural no-op — `gdk_parse_debug_var()` just prints `Unrecognized value
  "no-grabs". Try GDK_DEBUG=help` to stderr, confirmed on this toolchain. So
  do **not** reach for it to work around the seat-grab problem below: it will
  appear to do something and will do nothing.
- No coverage gate (they cross module boundaries), but they must be green in CI.
- Land at the milestone that first makes the flow possible, and grow with it.

#### The X11 backend is the backend CI tests (5w0)

`.woodpecker/ci.yml`'s last step is `xvfb-run -a meson test -C build --suite
integration` on `fedora:40`, where no Wayland socket exists. Two consequences
that cost a full investigation to learn, so they are written down here:

- **X11 seat grabs are display-global.** An autohide `GtkPopover` (all four of
  ggaze's `e`/`!`/`m`/`a` popups) is a `GdkPopup`, and on X11
  `gdk_x11_surface_present_popup()` takes a `gdk_seat_grab()` for it. Only one
  client can hold that grab; when it fails, `gdk_seat_default_grab()` hides
  the popup surface it just mapped, which GtkPopover turns into `closed`, and
  ggaze's own popover-destroy handlers then unparent it — so the test finds
  NULL milliseconds after firing the action. CI runs every integration binary
  in parallel against **one** Xvfb, so the popover suites were racing each
  other for that single grab. The five suites that pop an autohide popover
  (`window`, `open_external`, `move_undo`, `runner_rescan`, `enhance_flow`)
  are therefore `is_parallel : false`; the full derivation and the measurements
  are in `tests/meson.build`, "suites that need exclusive use of the display's
  seat grab".
- **`xvfb-run` alone does not give you an X11 run.** On a Wayland desktop,
  `xvfb-run -a meson test …` silently runs the whole lane on **Wayland** and
  reports green — proving nothing about CI. Unsetting `WAYLAND_DISPLAY` is not
  enough either, because libwayland falls back to `$XDG_RUNTIME_DIR/wayland-0`.
  Both are required:

  ```sh
  env -u WAYLAND_DISPLAY XDG_RUNTIME_DIR=$(mktemp -d) \
    xvfb-run -a meson test -C build --suite integration
  ```

  Backends are not equivalent, so a green Wayland run is not evidence about
  CI: a popover on a never-presented toplevel maps on X11 but not on Wayland.
  See `tests/helpers/gtk_helpers.h`, "window focus".

#### Planned integration suites (mapped to milestones)

| Suite | Lands at | Verifies |
|-------|----------|----------|
| `test_open_and_show.c` | M1 | CLI file arg → window → viewer has a non-null `GdkTexture` of the right size + upright orientation. gd2: a multi-file open whose first file is not first-sorted lands in the grid with that file loaded exactly once (`ggaze_window_load_count`), the cursor placed before the `changed` handler exists. |
| `test_walk_folder.c` | M2 | Folder arg → navigator listing; `h`/`l` action changes current; `GFileMonitor` add/delete propagates to navigator; wrap at ends. |
| `test_responsive_nav.c` | M3 | Rapid `next` ×10: only the last `GdkTexture` is shown (last-write-wins invariant), UI thread not blocked (measured via a main-loop timer). |
| `test_progressive_jpeg.c` | M6 | A progressive JPEG fires the partial-texture callback at increasing resolution before the final. |
| `test_grid_cull.c` | M7 | Grid view shows N cells; `d` bins one into `.Trash`, cell dims; `u` restores; counter reflects remaining; `Enter`→large on the right cell. |
| `test_move_undo.c` | M8 | Mark 3 → `m`→dest2 → files gone from folder, present in dest; `u` moves back; collision suffixing. |
| `test_runner_rescan.c` | M8 | `!` runs a script that writes a file into the dir; on exit the navigator rescans and the new file appears; injection-guard filename is single-quoted. |
| `test_enhance_flow.c` | M9 (gated on `gegl`) | `a`→preset applies a preview off-thread (texture differs from raw); toggle-off restores the original; hold-`Space` compares and restores (incl. the flag not sticking when the mask is cleared mid-hold); `s` writes a collision-safe `-enhanced[-n].<ext>` with the original byte-identical; a dirty preview blocks grid selection and native window close behind the Save/Discard/Cancel prompt — and the prompt itself is **answered** (`tests/helpers/gtk_helpers.h`): Cancel keeps the preview and releases the continuation, Discard/Save apply the deferred grid select / open / move, a **failed** Save (read-only folder) keeps the preview and aborts the continuation, repeated close-requests do not stack dialogs, and `t` `t` keeps a dirty preview on screen. wb2: the `c`/`R`/`[`/`]` tools on a generated 400×300 PNG — quarter turns wrap to the original and export the turned copy, a turn composes with a preset and is gated by the prompt, crop keys / aspect presets / drags commit on `Enter` and `Esc` restores, straighten nudges and the horizon drag level live with auto-crop on and off, a tool refuses the other tool and the turns, `Enter` is refused while the base preview renders, a crop turns with the image, navigation / leaving the large view abandon a tool; review round: re-opening the crop tool keeps the applied crop as work (`s` in the tool exports it, navigation prompts), a tool cancelled back to the saved state stays saved, a drag END without BEGIN levels nothing, twenty rapid nudges cost two renders (`ggaze_window_enhance_render_count`), a crop follows a straighten (kept whole and re-centred, never dropped — see the third round), and the tools' capture-phase key controller stops `h` only while a tool is active (emitted on the controller; the shortcut's action navigates otherwise); second round: a discard (the Original card, `0` with the panel open) ends a straighten tool and a crop tool with its turn before the reset, the crop tool passes foreign keys (`q`/`s`/`?`/`t`/Space) while its base is unknown, a crop drag is ignored while the shown texture is not the base; third round: a horizon drag END is refused while a render is pending (twenty fast nudges) and while `Space` holds the original, then accepted on the landed render, the crop tool waits for the *exact* render (`]` `]` then `c`, a same-size preset toggled under the tool) before drawing/dragging/`Enter`, the gate's Discard and the slideshow tick end a straighten tool in a one-file folder (no navigation could have), a failed render (file made unreadable) ends the tool, and a crop pushed outside the view is kept and comes back by nudging or `Esc`; fourth round: a refused crop-drag BEGIN grabs nothing (the next accepted UPDATE cannot continue a stale gesture), the crop overlay survives a `touch` of the file with its cache entry evicted (the controller remembers the original's identity — no texture-cache lookup per frame or pointer motion), leaving the large view restores a nudged straighten like `Esc` without pulling the large view back, `c` over a crop pushed outside the view starts from the whole base, a held `Space` refuses the crop tool's `Enter`/drag with "Release Space first", and a same-file rewrite with another size re-bases the crop tool after the rescan; fifth round: a same-file reload without a rescan (the file touched, a preset discarded: a new texture object) refreshes the remembered original — `c` + `Enter` and a horizon drag at 0° are accepted on it (identity learned at the window's texture choke point, not once from the cache), the touch subtest reloads under the open tool to tell "refreshed" from "remembered", a rewrite under an active preset re-renders it at the new size (the `-enhanced` copy's rescan does not: render count) and hold-`Space` compares against the new original, and a crop tool open through a rewrite is laid out and drawn on the new base with no key pressed (`ggaze_window_tool_crop_rect`); sixth round: opening a first-sorted file in another folder (no "changed" is ever emitted for it) ends the crop tool and drops A's *saved* turn — B's own decode on screen, no rectangle, `s` has nothing to save — the gate's Save before such an open exports A and then shows B clean, a render that decodes the file at another size before any reload (the rewrite keeps the cache stamp) re-bases the crop tool through `original_changed`, and a folder's first rescan after an open is a same-file event (tool, rectangle and base stay, no render); gd2: a multi-file open with the panel up and a start file that is not first-sorted re-points the panel at it with one preview batch and one load (`ggaze_window_enhance_preview_count`, `ggaze_window_load_count`), no render. `test_viewload.c` counts a JPEG's low-res partial through `show_partial` (never `show_texture`; `>= 1` only with the direct JPEG backend) and none for a PNG, and what is left shown is the cached full decode. `test_window.c` (both lanes) covers the GEGL-disabled keys; `test_viewer.c` pins the pan re-base when an overlay leaves mid-drag. |
| `test_grid_select_gate.c` | M9 | `gridview.c` routes every selection through the installed `GgazeGridSelectFunc` instead of `navigator_set_current_file` — a refusing gate blocks the change, an allowing one lets it through, and with **no** gate (or after uninstalling one) it falls back to `navigator_set_current_file` itself. No GEGL/window/dialog involved, so it runs in the minimal lane too. |
| `test_clipboard_copy.c` | M8 | `Ctrl+c` with no marks → `image/png` on `GdkClipboard`; with marks → `text/uri-list`; paste back into a fake target. |
| `test_full_lifecycle.c` | M10 | The elevator-pitch session scripted: open → walk → `i` → `d` ×k → mark → `m`→dest → `e`→program (use `true`) → `!`→script → quit. End-to-end smoke. |

### Test infrastructure (built in Phase 0)

- `tests/meson.build` declares every suite and tags each `test()` with
  `suite : 'unit'` or `suite : 'integration'`, so a track is selected by
  `meson test -C build --suite unit` / `--suite integration`. The split is by
  tag, not by directory — both tracks' sources sit in `tests/`.
- `tests/helpers/` — shared helpers, built as the `ggaze_test_helpers` static
  library and linked by the suites that need it. `gtk_helpers.{c,h}`: grid-cell
  activation via the flowbox's `child-activated` (M9; needs no laid-out
  geometry, so no toplevel has to be presented); driving
  `gtk_alert_dialog_choose()`'s dialog — it is an ordinary `GtkWindow` in
  `gtk_window_list_toplevels()`, so its buttons can be found by label and
  clicked, which is how the dirty-preview prompt is answered in tests; the
  window-teardown rule (1w0); and `ggtest_focus_viewer()` (5w0), which every
  suite that pops one of the window's popovers must call.
- `tests/fixtures/` — curated images per format + a rotated-EXIF JPEG +
  progressive JPEG + RAW+JPEG pair + an injection-hostile filename (`;rm -rf /`).
- CI runs **both tracks on all three lanes** (`.woodpecker/ci.yml`): each of
  `minimal`, `gegl` and `asan` runs `meson test -C build --suite unit` and
  then `xvfb-run -a meson test -C build --suite integration`; the `gegl` lane
  additionally builds the GEGL-gated suites, and `minimal` also runs the
  coverage build.

---

## Phase 0 — Bootstrap (prerequisite to M0)

One commit. Lays the build/test/convention groundwork before any feature.

**Files**
- `meson.build` (root) — project `org.buetow.ggaze`, C11,
  `default_options: warning_level=2`, extra `-Wextra`, ninja. Declare
  `dependency('gtk4')`, `glib`, `gio`. Feature options `gegl`, `jxl`, `avif`,
  `heif` (each `auto`).
- `src/`, `tests/` (+ `tests/integration/`, `tests/helpers/`, `tests/fixtures/`),
  `data/`, `po/`, `build-aux/` dirs.
- `.clang-format` matching [coding-conventions.md](coding-conventions.md);
  `.editorconfig` mirroring it. CI `clang-format --dry-run` gate.
- `meson.build` test infra: GLib `g_test_init`; `-Db_coverage=true` support; a
  `coverage` target; separate `unit`/`integration` suites.
- CI matrix: `{minimal, gegl}` × `{x86_64}`, ASan/UBSan lane on minimal;
  coverage upload; gate set to *warn* until M10.
- `LICENSE`.
- **Top-level `README.md` for humans** — project pitch (the elevator
  pitch from PLAN.md), a screenshot placeholder, install/run quick-start
  (`ggaze ~/Downloads/Camera/IMG_0001.jpg`), keybindings cheat-sheet table
  (the one from ui-and-interactions.md), build/test commands, a "status:
  planning → skeleton" line, and links into `docs/`. The audience is a
  Fedora user who finds the repo and wants to know what it is and how to
  run it — not a contributor-only wall of links.
- **Top-level `AGENTS.md` for agents** — the machine-facing entry point: how
  to build/test (`meson setup`, `meson test --suite unit` / `--suite
  integration`, coverage target), the mandatory two test tracks and their
  locations, how to reproduce CI's X11/xvfb lane, convention
  enforcement (`clang-format`, header guards, `type_new`/
  `type_delete`), where the design lives (`docs/` index), the module map, the
  "optional features are off in CI" rule, the single-`GCancellable`/last-
  write-wins invariant, and a pointer to load the `agent-task-management` +
  `c-best-practices` + `solid-principles` + `beyond-solid-principles` skills.
  This is the file an agent in a fresh context reads first.

**Acceptance:** `meson setup build && ninja -C build && meson test -C build`
green (empty suites); CI builds the matrix; `clang-format --dry-run` clean.

---

## M0 — Skeleton (app + empty window)

**Deliverables**
- `src/main.c` — GApplication with `G_APPLICATION_HANDLES_OPEN`; local-options
  for `--version`/`--help`.
- `src/app.c/.h` — owns `GtkApplication`, single-instance (decision #32);
  `open` handler: file → parent dir + that file current; folder → grid.
- `src/window.c/.h` — `GgazeWindow : GtkApplicationWindow` (`G_DEFINE_TYPE`);
  empty `GtkStack` (`grid`, `large` children); `AdwHeaderBar` (decision #29).
- `data/org.buetow.ggaze.desktop` stub (`image/*` handler).
- `data/org.buetow.ggaze.gschema.xml` — **all keys now** with defaults so
  modules read them as they land.

**Tests**
- Unit: `test_app.c` — `--version`/`--help`/unknown arg.
- Smoke: `test_window.c` — an unpresented `GgazeWindow` on a real display
  (integration track, skipped when `gtk_init_check()` fails), stack has two
  children.

**Acceptance:** `ggaze IMG_0001.jpg` opens an empty window; `--version` works.

---

## M1 — Show one image (zoom/pan)

**Deliverables**
- `src/loader/loader.c/.h` — `loader_load(GFile*, GCancellable*, GError**) →
  GdkTexture*`; static backend dispatch; only `pixbuf` registered here.
- `src/loader/detect.c/.h` — `detect_format(head, len) → GgazeFormat`;
  magic-byte sniff. Unit-testable.
- `src/loader/backends/pixbuf.c` — GdkPixbuf →
  `gdk_pixbuf_apply_embedded_orientation` (decision #26) → `GdkTexture`.
- `src/viewer.c/.h` — `GgazeViewer : GtkWidget` (custom, decision #31): zoom,
  pan, fit, cursor-centered zoom, pan clamp, `viewer_set_texture`; scroll
  follows `scroll-behavior` (`zoom` / `pan-when-zoomed` / `navigate`).
- `src/window.c` — wire `open` → load → `viewer_set_texture`; large view.

**Tests**
- Unit: `test_detect.c`, `test_loader_pixbuf.c` (incl. rotated-EXIF fixture).
- Integration: `test_open_and_show.c`.

**Acceptance:** one image shows upright; zoom/pan works; detect+loader ≥80%.

---

## M2 — Walk the directory

**Deliverables**
- `src/navigator.c/.h` — plain-C; dir listing, MIME filter, sort (name default;
  time/size stubs), current index, mark set, rescan; hide RAW sidecars by
  default (decision #33). `GFileMonitor` debounced 250 ms (decision AA);
  nearest-fallback on current removal.
- `src/window.c` — `h`/`l`/`←`/`→` → nav → load → viewer; header subtitle
  `n / total · filename`; single `GCancellable` (architecture invariant);
  `GtkDropTarget` for file/folder.
- `src/shortcuts.c/.h` — `GtkShortcut`+`GtkShortcutController`; actions on the
  window/app; one table all milestones add to.

**Tests**
- Unit: `test_navigator.c` (the big one — filter, sort, wrap, marks, rescan,
  nearest-fallback). Target ~90%.
- Integration: `test_walk_folder.c`.

**Acceptance:** `ggaze dir/` lists; `h`/`l` walks with wrap; drop reloads;
external `touch` appears in ~250 ms; counter updates.

---

## M3 — Responsive + prefetch

**Deliverables**
- `loader_load_async` + `_finish` via `GTask`; keep a sync worker for tests.
- `src/texturecache.c/.h` — bounded LRU (cap 4) of `GFile → GdkTexture`;
  prefetch next+prev on current change.
- Window enforces: one `GCancellable`, drop results whose path ≠ current.

**Tests**
- Unit: `test_texturecache.c` (LRU eviction, cap; `gdk_memory_texture_new`
  1×1 so no display).
- Integration: `test_responsive_nav.c` (last-write-wins + non-blocking UI).

**Acceptance:** rapid `jjjjjj` never blocks; visible texture matches current;
bounded memory.

---

## M4 — Fullscreen + slideshow + info

**Deliverables**
- Fullscreen (`f`, auto-hide header); `Esc` contextual back
  (marks → fs → large → grid → quit).
- Slideshow (`S`), configurable delay; pause on manual key.
- `src/info.c/.h` — plain-C EXIF gather via `libexif`; rendered as viewer
  overlay.
- `src/histogram.c/.h` (task 0c2, landed after M9) — plain-C RGB/luminance
  binner over the displayed `GdkTexture`, subsampled to a fixed pixel budget;
  drawn on the card by `src/histogram-view.c/.h` (snapshot render nodes),
  gathered in the info overlay's existing `GTask`.

**Tests**
- Unit: `test_info.c` (EXIF extraction + orientation tag),
  `test_histogram.c` (per-channel counts, layouts, stride, subsampling
  budget, rejected input).
- Integration: `/window/info_shows_histogram` (plot for the image on screen,
  none from the grid, a different plot after navigation),
  `/window/info_no_plot_while_loading` (`i` during a cache-miss decode: the
  new file's text without the old file's plot, and the plot filled in once
  the decode lands), `/window/status_clears_plot` (a status line over the
  card drops the plot), `/window/toggle_view_follows_card` (`t` to the grid
  takes the plot off a card that stays up, `t` back fills it in);
  `test_info_overlay.c` (the overlay on a bare GtkOverlay: a plot landing
  after a status line took the card is dropped, `texture_changed(NULL)`
  clears and cancels, the auto-hide timer takes card and plot down, a
  disposed overlay is inert, and the plot widget's measure/snapshot path in
  a presented window); GEGL lane: `/enhance_flow/info_plots_preview` (the
  plot equals the displayed texture's bins through preset -> hold-Space ->
  release -> a second preset).

**Acceptance:** `f`/`S`/`i` work; EXIF shows; `Esc` chain correct.

---

## M5 — Modern formats

**Deliverables**
- `src/loader/backends/jxl.c`, `avif.c`, `heif.c` behind meson features;
  register into the dispatcher.
- Animated GIF/WebP via `GdkPixbufAnimation` → `GdkPaintable`.

**Tests**
- Extend `test_detect` + `test_loader_*` per backend, feature-gated.

**Acceptance:** JXL/AVIF/HEIF open when built; minimal build still green.

---

## M6 — Progressive low-res preview

**Deliverables**
- `src/loader/backends/jpeg.c` (libjpeg-turbo, optional; supersedes pixbuf for
  JPEG when enabled) — two-phase load emitting partial `GdkTexture`.
- Generalize `Loader` with a `progress_cb(GdkTexture *partial)`.
- Viewer accepts progressive replacement.

**Tests**
- Unit: `test_loader_jpeg.c`.
- Integration: `test_progressive_jpeg.c`.

**Acceptance:** 40 MP JPEG shows a coarse frame <50 ms, refines to full.

---

## M7 — Thumbnail cache + grid view

**Deliverables**
- `src/thumbnail.c/.h` — freedesktop TMS `~/.cache/thumbnails/{normal,large}`
  + custom bucket for 64–512 (decision T); mtime verify; thread-safe worker;
  `thumbnail_get_async`.
- `src/gridview.c/.h` — `GgazeGrid` over navigator `GListModel`; lazy cell
  decode; `+`/`-` resize → `thumbnail-size`; reflow; mark badges; dim
  trashed/deleted; `Enter`/double-click → large; cursor sync both ways.
- `src/trash.c/.h` — `.Trash` bin (lazy, collision suffix), restore-last,
  permanent delete.
- Window: `d`/`D`/`u`; `d` advances; `D` on **>1 marked** asks a confirm dialog; counter = remaining; `t` toggle.
- That confirm dialog deletes on the **Delete** button alone. Every other
  outcome — Cancel, a dismissal (`Esc` / closing the dialog), and the
  dispose-time cancel — is a *no*. How each one is *reported* changed when
  `aw0` gave the dialog a cancel button, and this list said otherwise until
  `dw0`: because `_delete_confirm_ask` calls
  `gtk_alert_dialog_set_cancel_button(p_dlg, 0)`, a dismissal now comes back
  as that button's **index `0`**, not as `-1` plus a `GError` — GTK's
  `response_cb` returns `cancel_return` whenever one is set and only raises
  `GTK_DIALOG_ERROR_DISMISSED` when none is
  (`gtk/gtkalertdialog.c:616-623`). Only the dispose-time cancel still
  reports `-1` plus a `GError` (`G_IO_ERROR_CANCELLED`), and `-1` read as a
  `gboolean` is `TRUE` — the misread `aw0` fixed. `src/window.c`'s
  `_delete_confirm_answered_yes` is the authority and carries the full
  argument for why the error check stays anyway. A native window close is
  refused while the dialog is up, the same way it is behind the
  Save/Discard/Cancel prompt.

**Tests**
- Unit: `test_thumbnail.c`, `test_trash.c`.
- Integration: `test_grid_cull.c`, `test_delete_safety.c` (the captured-target
  decision, plus the real confirm dialog: close refused under it, dispose
  cancels it, a dismissal deletes nothing, Delete deletes).

**Acceptance:** `ggaze dir/` → grid; thumbnails load async; `Enter`→large;
`d` dims+advances; `u` restores; `+`/`-` resizes and persists.

---

## M8 — Selection, move, open-external & scripts, clipboard, prefs

**Deliverables**
- `src/mover.c/.h` — `destinations` `a(ss)`; `mover_move` (`g_file_move` +
  suffix); undo move-back; acts on marks-or-current.
- `src/opener.c/.h` — `editors` `a(ss)`; `%f` expand; detached `GSubprocess`;
  acts on original file (decision #38).
- `src/runner.c/.h` — `scripts` `a(ss)`; `/bin/sh -c`, single-quoted `%f`/`%d`
  (decision S); `wait_async`; rescan + toast on done.
- `src/clipboard.c/.h` — copies the **displayed** image (modified if a
  preview is active, else original) as `image/png` (decode in `GTask`) /
  marked files as `text/uri-list`; union provider for one file (decision V).
- Popover pattern for move/open/scripts: `(hotkey, label)`
  rows + a capture-phase key controller firing on digit/letter, Esc cancels.
  In practice this landed as one popover built per action in `window.c`
  (`_action_open_external`/`_action_run_script`/`_action_move`), sharing only
  the two small hotkey-mapping helpers (`_popup_hotkey_char`,
  `_popup_key_to_index`) rather than a separate reusable `ggaze_popup` widget
  — simpler for four call sites and consistent with how `e`/`!` were already
  built before `m` (move) landed.
- Unified one-level undo `u` (decision P).
- Mark UI: `v`/`V`/`Ctrl+a`/`Esc`; header subtitle shows `N marked`; grid
  check-badges + large-view indicator.
- Preferences dialog (`,`): `AdwPreferencesWindow` editing ordered `a(ss)`
  lists + sort/background/scroll/slideshow/hide-trashed.

**Tests**
- Unit: `test_mover.c`, `test_opener.c` (`true`/`false` commands, weird
  filenames), `test_runner.c` (injection guard, exit status), `test_clipboard.c`.
- Integration: `test_move_undo.c`, `test_runner_rescan.c`, `test_clipboard_copy.c`.

**Acceptance:** full culling workflow works keyboard-only.

---

## M9 — GEGL quick-enhance, crop/straighten/rotate, compare (optional)

**Status (tu0 + wb2):** the enhance side panel, async apply, hold-`Space`,
reset, `s` export-copy, and the dirty Save/Discard/Cancel gate are done and
wired into the window (tu0), and the crop (`c`), straighten (`R`) and
rotate 90 (`[`/`]`) tools ride the same preview graph (wb2, see below). ICC
color management is **not yet built** — tracked as follow-up work.

**Deliverables**
- `meson` `gegl` feature; `src/enhancer.c/.h` plain-C.
- `enhancer_get_presets` (built-in programmatic / user `gegl-graph` text,
  decision #34); `enhancer_apply_chain_async` runs `enhancer_load` +
  `enhancer_apply_chain` + `enhancer_buffer_to_texture` in a `GTask` worker
  (off the GTK main thread); `enhancer_export_chain` → `<stem>-enhanced.<ext>`
  same dir, collision-suffixed `-1`, `-2`, … (mirrors `mover.c`'s move
  collision suffixing); defaults to the original format (JPEG quality 95).
- Window: `a` opens a `GtkPopover` (same pattern as `m`/`e`/`!`, sharing their
  `_popup_hotkey_char`/`_popup_key_to_index` helpers) listing presets;
  presets are **layered** (multiple toggle on/off independently, composing
  in the preview graph) rather than single-select, and the popover does not
  close on a row click/hotkey (only `Esc`/outside-click/re-press `a`) so
  combinations can be compared. `0` (row or hotkey) resets to the original.
- Viewer: preset chain active → `enhancer_apply_chain_async` → swap in the
  resulting `GdkTexture`; last-write-wins via a generation counter (a newer
  apply/discard/navigation supersedes a still-in-flight one). Not applied
  during `h`/`l` scrubbing.
- Crop/straighten/rotate 90° (wb2): one plain-C `Transform`
  (`transform.{c,h}`, with the crop rectangle rules in `croprect.{c,h}`)
  that the enhancer appends after the presets in decision #35's order, for
  the preview and the export alike; `enhance-ctrl.c` owns it beside the
  mask (active/dirty/saved/override all read one "has work" predicate), so a
  turn or crop is gated, saved and discarded exactly like a preset. The
  controller also keeps a *preview override* for the crop tool (the base is
  rendered, the committed crop still counts), remembers the saved (mask,
  transform) pair so a state that returns to it is saved again, and
  coalesces renders (one in flight, one queued).
  `tool-ctrl.{c,h}` is the modal `c`/`R` session: it draws on the viewer's
  overlay hook (`ggaze_viewer_set_overlay`: a draw callback with the image's
  on-screen geometry and a drag callback that takes over from panning),
  claims its keys in a capture-phase controller ahead of the global
  shortcut table, and edits the Transform through
  `enhance_ctrl_set_transform`. `[`/`]` need no tool
  (`enhance_ctrl_rotate_quarter`). Behaviour: docs/ui-and-interactions.md.
- Dirty flag: navigate (`h`/`l`/`g`/`G`/scroll), any grid/thumbnail
  selection (double-click/`Enter`, middle-click mark, `j`/`k` cursor move,
  toggle-to-large sync — routed through `ggaze_grid_set_select_func`'s gate
  rather than `gridview.c` calling `navigator_set_current_file` directly),
  `d`/`D`/`m`/`o`(open), and quit — both the `q` action **and** the native
  `close-request` (WM close button / Alt+F4) — with dirty →
  Save/Discard/Cancel (decisions #34/#18); `s` exports but does
  **not** clear dirty (pressing it again exports another numbered copy of
  the same still-active preview); toggling every preset off, `0`, or `Esc`
  (popover closed) discards directly. Slideshow auto-advance discards
  silently instead of blocking on an unanswerable prompt.
- Hold-`Space` compare (decision #23/#24): swaps to the cached original
  texture while held, restores the cached modified one on release — no GEGL
  recompute either way.
- ICC color management via GEGL/babl (open question G) is **not yet wired**.
- "GEGL not built in" status message (via the info-overlay label; this
  project has no toast infra) for `a`/`s` when the build has no GEGL; safe no-op for the numeric preset hotkeys.

**Tests**
- Unit: `test_enhancer.c` (gated): each preset dims + non-zero; export file
  written + original untouched; format selection by extension; stale-dest
  and unsupported-extension rejection; the transform on the chain pixel by
  pixel (quarter turns as permutations, crops in base coordinates, the
  straighten's analytic sizes, a turned export). `test_croprect.c` and
  `test_transform.c` (every lane, no GEGL): the rectangle rules and the
  angle / size maths.
- Integration: `test_enhance_flow.c` (gated `if gegl_dep.found()`): async
  apply swaps the texture without touching the original (byte-identical),
  toggle-off resets to the original, hold-Space compares then restores,
  `s` twice produces collision-suffixed copies, non-dirty navigation is
  immediate — plus the Save/Discard/Cancel prompt driven to each outcome
  (see the suite table above). `test_grid_select_gate.c` covers `gridview.c`'s
  side of the gate with no GEGL involved.
  `test_window.c::test_enhance_a_is_safe_with_and_without_gegl`
  (always built, both lanes) covers the GEGL-disabled safety message.

**Acceptance:** `a` popover (layered, async apply); `s` copy
(collision-safe); hold-`Space`; dirty prompt across navigate/trash/delete/
move/open/quit; minimal build reports "GEGL not built in" cleanly. `c`/`R`/
`[`/`]` and ICC remain open for a follow-up task.

---

## M10 — Polish & packaging

**Deliverables**
- AppStream metainfo, app icons (symbolic + full).
- Fedora RPM spec.
- `ggaze(1)` man page (stub in M0, finalized here).
- **Coverage gate → fail** at <80% on plain-C modules.
- **Quality audit:** `auditing-code-quality` skill (C-adapted:
  c-best-practices + find-code-bugs + solid-principles + beyond-solid-principles),
  triage via `agent-task-management`, fix all HIGH/MEDIUM.
- **Keyboard-completeness audit:** visible hotkeys, mnemonics, `?` overlay,
  full dialog traversal.
- Empty-`.Trash` menu action.
- Integration: `test_full_lifecycle.c` (the elevator-pitch session scripted).

**Acceptance:** RPM builds/installs/registers; man page; coverage gate green;
audit findings resolved; lifecycle integration green.

---

## Cross-cutting

- **Audit cadence:** run the C-adapted `auditing-code-quality` skill at each
  M-boundary (decision #40); findings → `agent-task-management` tasks.
- **Conventions:** `clang-format` CI gate + header-guard/`type_new`-`type_delete`
  pairing checks where feasible.
- **Fixtures** grow per milestone; one known-good + one known-bad per format.
- **Dependency risk:** pin libadwaita to a Fedora target; validate each
  `gegl:op` exists at M9 via a `gegl_operations` introspection test.
- **Memory:** bounded texture LRU from M3; ASan/UBSan lane in CI.
- **Memory-leak profiling (mandatory, per feature):** C has no GC, so **after
  every major feature milestone completes** (M1, M2, M3, M7, M8, M9, M10)
  run a dedicated leak-profiling pass before starting the next feature:
  - Build with `-Db_sanitize=address` and `G_DEBUG=gc-friendly`;
  - Run the full `meson test` suite under ASan (unit **and** integration) and
    assert zero leak reports for the plain-C modules (`navigator`, `loader`,
    `detect`, `thumbnail`, `trash`, `mover`, `opener`, `runner`, `enhancer`,
    `info`, `histogram`, `texturecache`, `clipboard`) — every `type_new` must
    have a matching `type_delete` and every `GTask`/`GSubprocess`/
    `GFileMonitor`/`GdkTexture` must be unreffed;
  - Run a scripted session (the elevator-pitch workflow from PLAN.md) under
    ASan — open a folder, walk, `d`/`u`, mark, `m`, `e`, `!`, `a`, `s`, quit —
    and assert no leak at exit;
  - `valgrind --leak-check=full --error-exitcode=1` on a representative subset
    where ASan is unavailable (e.g. the gegl lane), as a cross-check;
  - Any leak found is a **blocker** for progressing to the next milestone — fix
    it, re-run, then move on. Each leak pass is its own tracked task
    (`+leakcheck`) that depends on the feature milestone it follows.
  Rationale: deferring leak hunting to M10 means chasing leaks across the
  whole codebase at once; catching them right after each feature keeps the
  cost local and keeps the `_new`/`_delete` discipline honest as the tree grows.

## Suggested execution order

Phase 0 → M0 → M1 → M2 → M3 → **M7** → M4 → M5 → M6 → M8 → M9 → M10.

Pulling **M7 before M4/M5**: the grid is the core differentiator and depends
only on navigator + thumbnail + trash — the highest-value plain-C modules.
Getting them + tests in early maximizes coverage return and de-risks the most
visible feature. Fullscreen/slideshow/modern-formats are polish relative to
"flip through a folder and cull."