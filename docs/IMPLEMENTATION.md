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
  `texturecache`, `clipboard` helpers, `gesture-math`, `icc` + `streamread`,
  and in the GEGL lane `loader/intact`). No GTK display needed.
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
| `test_open_and_show.c` | M1 | CLI file arg → window → viewer has a non-null `GdkTexture` of the right size + upright orientation. gd2: a multi-file open whose first file is not first-sorted lands in the grid with that file loaded exactly once (`ggaze_window_load_count`), the cursor placed before the `changed` handler exists; review round: the first entry decides the folder — a folder first opens that folder (its first image, no status line), an empty folder shows the empty state after one ask, and a missing / non-image / hidden-RAW-sidecar first entry opens its folder on the first-sorted image (title `plain.jpg · 1/3`) with the status line saying why (the RAW wording names the preference, `navigator_is_raw_name`). Temp folders come down through `tests/helpers/temp_dir.h`, which asserts the rmdir. |
| `test_walk_folder.c` | M2 | Folder arg → navigator listing; `h`/`l` action changes current; `GFileMonitor` add/delete propagates to navigator; wrap at ends. |
| `test_responsive_nav.c` | M3 | Rapid `next` ×10: only the last `GdkTexture` is shown (last-write-wins invariant), UI thread not blocked (measured via a main-loop timer). |
| `test_gestures.c` | M1 (zb2) | Touch on a presented window, driven through the viewer's public gesture bodies (GTK4 cannot synthesise touch) and the controllers' own signals: a pinch zooms about its midpoint and pans with it (the image pixel under the fingers stays under them; a constant-distance two-finger move pans) with the wheel's clamp and NaN / Inf / zero / negative-scale / non-finite-midpoint guards; the real GtkGestureZoom handlers ("begin" / "scale-changed" / "end" zoom about the widget centre, a quick "begin" + "end" toggles the card, a "cancel" in between does not) and GtkGestureSwipe "swipe" handler (touch-only; a laid-down leftward path navigates; the same path spoiled by a pinch does not; no point, no navigation); a pinch takes a one-finger tool drag away (CANCEL where the finger was) and a pan drag, swallowing the rest, and a tap after it sends the tool a REVERT (a real pinch does not); an unmap mid-drag CANCELs the tool drag and a new texture mid-pan ends the pan, the rest of the drag reaching nobody and a pinch after it starting afresh; a two-finger pan (scale ~1.01) over a fitted picture keeps it fitted, also after a pinch out and back, so a swipe still navigates; leaving that fit detent is continuous at both edges (the first frame past it is the fit zoom, 2× the edge scale is 2× fit), while a pinch that began zoomed has no detent; re-entering the detent after moving the picture along its letterbox keeps that position (no snap back to the begin pan) and then holds it still; a 32768 × 16 panorama that fits below 2 % is never enlarged by `-`, a wheel notch (the real scroll handler) or a pinch in, fitted or not; pinching out at the 6400 % ceiling still pans with the midpoint; a two-finger tap toggles the info card on and off and leaves a fitted view fitted, restores the view from before the first finger's drag (its jitter undone), a moving / pinching / > 250 ms one, one whose first finger wandered, a touchpad pinch and one cut short by a new texture or an unmap do not; a swipe and a navigate-mode wheel notch (the real scroll handler) stop a running slideshow; a leftward swipe shows the next file and a rightward one the previous, while vertical, tiny and zero-velocity swipes, a swipe with a tool overlay installed and one over a picture zoomed wider than the window navigate nowhere (zoomed but narrower: it does); a slideshow step while a finger is down spoils that swipe (no navigation when it lands), the next finger swiping as ever. `test_enhance_flow.c` adds the GEGL halves: a swipe away from a dirty preview prompts (Cancel stays, Discard moves) and the crop tool refuses a swipe until `Esc`. |
| `test_progressive_jpeg.c` | M6 | A progressive JPEG fires the partial-texture callback at increasing resolution before the final. |
| `test_grid_cull.c` | M7 | Grid view shows N cells; `d` bins one into `.Trash`, cell dims; `u` restores; counter reflects remaining; `Enter`→large on the right cell. |
| `test_move_undo.c` | M8 | Mark 3 → `m`→dest2 → files gone from folder, present in dest; `u` moves back; collision suffixing. |
| `test_runner_rescan.c` | M8 | `!` runs a script that writes a file into the dir; on exit the navigator rescans and the new file appears; injection-guard filename is single-quoted. |
| `test_enhance_flow.c` | M9 (gated on `gegl`) | `a`→preset applies a preview off-thread (texture differs from raw); toggle-off restores the original; hold-`Space` compares and restores (incl. the flag not sticking when the mask is cleared mid-hold); `s` writes a collision-safe `-enhanced[-n].<ext>` with the original byte-identical; a dirty preview blocks grid selection and native window close behind the Save/Discard/Cancel prompt — and the prompt itself is **answered** (`tests/helpers/gtk_helpers.h`): Cancel keeps the preview and releases the continuation, Discard/Save apply the deferred grid select / open / move, a **failed** Save (read-only folder) keeps the preview and aborts the continuation, repeated close-requests do not stack dialogs, and `t` `t` keeps a dirty preview on screen. wb2: the `c`/`R`/`[`/`]` tools on a generated 400×300 PNG — quarter turns wrap to the original and export the turned copy, a turn composes with a preset and is gated by the prompt, crop keys / aspect presets / drags commit on `Enter` and `Esc` restores, straighten nudges and the horizon drag level live with auto-crop on and off, a tool refuses the other tool and the turns, `Enter` is refused while the base preview renders, a crop turns with the image, navigation / leaving the large view abandon a tool; review round: re-opening the crop tool keeps the applied crop as work (`s` in the tool exports it, navigation prompts), a tool cancelled back to the saved state stays saved, a drag END without BEGIN levels nothing, twenty rapid nudges cost two renders (`ggaze_window_enhance_render_count`), a crop follows a straighten (kept whole and re-centred, never dropped — see the third round), and the tools' capture-phase key controller stops `h` only while a tool is active (emitted on the controller; the shortcut's action navigates otherwise); second round: a discard (the Original card, `0` with the panel open) ends a straighten tool and a crop tool with its turn before the reset, the crop tool passes foreign keys (`q`/`s`/`?`/`t`/Space) while its base is unknown, a crop drag is ignored while the shown texture is not the base; third round: a horizon drag END is refused while a render is pending (twenty fast nudges) and while `Space` holds the original, then accepted on the landed render, the crop tool waits for the *exact* render (`]` `]` then `c`, a same-size preset toggled under the tool) before drawing/dragging/`Enter`, the gate's Discard and the slideshow tick end a straighten tool in a one-file folder (no navigation could have), a failed render (file made unreadable) ends the tool, and a crop pushed outside the view is kept and comes back by nudging or `Esc`; fourth round: a refused crop-drag BEGIN grabs nothing (the next accepted UPDATE cannot continue a stale gesture), the crop overlay survives a `touch` of the file with its cache entry evicted (the controller remembers the original's identity — no texture-cache lookup per frame or pointer motion), leaving the large view restores a nudged straighten like `Esc` without pulling the large view back, `c` over a crop pushed outside the view starts from the whole base, a held `Space` refuses the crop tool's `Enter`/drag with "Release Space first", and a same-file rewrite with another size re-bases the crop tool after the rescan; fifth round: a same-file reload without a rescan (the file touched, a preset discarded: a new texture object) refreshes the remembered original — `c` + `Enter` and a horizon drag at 0° are accepted on it (identity learned at the window's texture choke point, not once from the cache), the touch subtest reloads under the open tool to tell "refreshed" from "remembered", a rewrite under an active preset re-renders it at the new size (the `-enhanced` copy's rescan does not: render count) and hold-`Space` compares against the new original, and a crop tool open through a rewrite is laid out and drawn on the new base with no key pressed (`ggaze_window_tool_crop_rect`); sixth round: opening a first-sorted file in another folder (no "changed" is ever emitted for it) ends the crop tool and drops A's *saved* turn — B's own decode on screen, no rectangle, `s` has nothing to save — the gate's Save before such an open exports A and then shows B clean, a render that decodes the file at another size before any reload (the rewrite keeps the cache stamp) re-bases the crop tool through `original_changed`, and a folder's first rescan after an open is a same-file event (tool, rectangle and base stay, no render); gd2: a multi-file open with the panel up and a start file that is not first-sorted re-points the panel at it with one preview batch and one load (`ggaze_window_enhance_preview_count`, `ggaze_window_load_count`), no render; zb2: a touch swipe away from a dirty preview raises the prompt (Cancel keeps file and preview, Discard moves on) and is refused while the crop tool is up, navigating again once `Esc` ended it; review: a pinch landing on a straighten horizon drag with (1, 1) px of jitter levels nothing (no render, no angle; a stray END sent to the tool afterwards neither, which proves the line was dropped), and one landing on a crop corner drag keeps the rectangle as dragged before it (`Enter` crops 300×200); a two-finger tap whose first finger jittered a crop corner leaves the rectangle as it was (`ggaze_window_tool_crop_rect`); third review: a `set_texture(NULL)` mid straighten drag drops the line (a stray END then levels nothing) and mid crop drag lets go of the corner (a CANCEL and REVERT with no texture put the rectangle back; a stray UPDATE after does not resize it); xb2: a red/blue-swapped-profile PNG previews blue under a preset (managed) and its `s` copy carries the source ICC profile byte for byte; 6i2 (decision #49): the digits are inert with the panel closed (the capture-phase router passes them on, no global binding) and toggle a preset with it open, `c`/`r`/`[`/`]` open the panel first (from the grid too), `Esc` closes the panel and keeps the edit through large → grid and back, `0` only zooms, `x` explains itself with the panel closed and reverts with it open (ending a straighten / crop tool first, via the panel's Revert button too), every crop side grows with `Shift` and shrinks with `Ctrl`, `a` cycles the crop aspect without shrinking the rectangle and toggles the straighten auto-crop, the panel's buttons show their keys and Save its target file, and the key-hint bar follows the mode (panel, crop, straighten, grid, closed); 6i2 review: Caps Lock keys through the router (`L`+Lock moves the crop rectangle, `Shift+h`+Lock grows it, `A`+Lock is the aspect key and leaves the panel open), the panel hidden beside the grid is no key mode (a digit is nobody's, `x` and a direct `win.enhance-N` explain themselves, `Esc` skips it to the grid chain), closing the panel under a tool (its close button, `win.enhance`) cancels the tool, the first `a` in a fresh crop tool is 1:1 (visible) and the status names the next lock, and at 1280x800 all eight preset rows fit without scrolling (7i2: the Undo / Redo row too; 8i2: with a tunable card selected, its slider too, mapped and non-focusable); 7i2 (decision #50): through the edit-key router with the panel open, `u` undoes and `U` redoes a preset toggle (status "Undid: Auto-fix on", the original back and clean, the title without it; "Nothing to undo" / "Nothing to redo" at the ends), `U`+Lock is undo and `Shift+u`+Lock redo, `Ctrl+z` / `Ctrl+Shift+Z` and the panel's Undo / Redo buttons do the same, a card click is a step like its digit, the buttons are insensitive exactly when there is nothing to undo / redo and a new step drops the redo; each quarter turn is a step (180 → 90 → 0 and back), a crop applied with `Enter` and a straighten applied with `Enter` are one step each (texture size and title follow the render), `x` is a step (`u` brings the preset and the turn back together); `u` under the straighten tool is refused with "Finish the current tool first" and the tool stays, and a preset toggled under it undoes back to the angle the tool started from (clean, no "0.5°"); stepping back onto the state `s` wrote is clean again ("Saved as"), off it dirty; a navigation, the gate's Discard and the slideshow's silent discard (one-file folder, so no navigation could have) clear the history; with the panel closed the router leaves `u` / `Ctrl+z` to the global table and `win.undo` restores a trashed file (the panel's `u` never does), and the menu's *Undo edit* then only says where it works; 8i2 (decision #51): `j`/`k` move the selection ring (stopping at the ends, `J`+Lock is `j`, no render, no edit), a digit selects its row, `Enter` / keypad Enter toggle the selected preset, the hint bar lists "j/k select · Enter toggle · h/l strength"; `l` on Brightness turns it on at +0.6 (card label, slider, title, status), `h`/`H`+Lock step down one, `Shift+l` five, back at the default the title drops the value, and the range end says so; Auto-fix and Warm refuse `h`/`l` with a status line (no render, no step); a run of five `l` costs at most two renders and undoes as one step to the clean original, redoes as one, and another card's run is a step of its own; a slider drag (three values, snapped) selects its row, turns the preset on and undoes as one step; `s` after a strength change exports a copy that differs from the default's, and a strength moved back onto the saved value is clean again; `x` resets the strengths (undoably) and navigation resets them while the selection stays; review: a Preferences change (the background) keeps a tuned, saved strength (card, slider, title, render count, clean), the card keys (`j`/`k`/`Shift+H`/`Shift+L`) do not reach the panel under the straighten tool while a digit still does, the arrows are not the panel's (`←` navigates with it open, strengths untouched), `Shift+Enter` / `Shift+Esc` / `Shift`+keypad Enter apply and cancel the crop and straighten tools, a tuned preset switched off is saved again, a save and a panel close end the strength run (undo stops at the saved / closed-on value), and the slider's own scroll controller is off (the wheel scrolls the cards). ai2: two user presets (one tunable, one plain) are rows 9 and 10 after the built-ins, named without a digit, reached by `j`, toggled by `Enter`, tuned by `l` and the slider, in the title, saved / dirty and undo; `9` is not the panel's and the hint bar still says `1–8 presets`; 25 user presets make 32 rows (the 25th ignored) that scroll at 1280x800 while Transform and the Actions stay on screen, `j` to the last row scrolls it into view and `k` back to the first; a Preferences add / reorder that keeps the enabled presets in order renders nothing and stays saved with the state and selection on their presets' new rows and undo still walking the old steps, a swap of two enabled presets renders (counted) and forgets the saved copy, a graph edited in place keeps its preset on, and a removed enabled preset goes with the undo step that only toggled it. `test_settings_ui.c` (GEGL builds): at 24 presets the editor's Add is insensitive with the reason, and a stored 25th is listed as *Ignored*. `test_keymodes.c` (unit, every lane) pins the table's modes: which key is whose in which mode, the Shift / Ctrl / Caps Lock matching (7i2: `u` / `Ctrl+z` / `U` / `Ctrl+Shift+Z` in the panel alone, `u` the global file undo elsewhere; 8i2: the card keys `j`/`k`/`Enter`/`h`/`l`, `Shift` five steps, Caps Lock not Shift, the arrows NOT the panel's, and the same keys global outside the panel; Shift on a key that prints nothing counts only against a Shift twin: global `Shift+Left` pans, `Shift+Enter` / `Shift+Esc` apply / cancel both tools), the three hint lines word for word, the preset digits trimmed to the preset count (`1–N`, `1/2`, `1 preset`, none) and the menu's short labels. `test_viewload.c` counts a JPEG's low-res partial through `show_partial` (never `show_texture`; `>= 1` only with the direct JPEG backend) and none for a PNG, and what is left shown is the cached full decode. `test_window.c` (both lanes) covers the GEGL-disabled keys; `test_viewer.c` pins the pan re-base when an overlay leaves mid-drag. |
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
  window-teardown rule (1w0); `ggtest_focus_viewer()` (5w0), which every
  suite that pops one of the window's popovers must call; and the large-view
  readiness waits (2d2), `GGTEST_WAIT_FOR_TEXTURE()` (the viewer holds a
  texture of the caller's known, EXIF-upright decoded size — not the JPEG
  backend's 1/8-scale preview, which for the 6×3 `plain.jpg` is 1×1) and
  `GGTEST_WAIT_FOR_VIEW()` (that, inside a non-empty allocation that held
  still for a run of polls — presented windows only), used by `test_viewer`
  and `test_enhance_flow` before any scale, pan or texture read.
  `wait_until.{c,h}` (plain GLib, hd2): `ggtest_wait_until()` polls a
  condition under a monotonic deadline scaled by `ggtest_wait_scale()`
  (sanitizer lanes, `GGAZE_TEST_TIMEOUT_SCALE`) — the replacement for any
  "N iterations of a 1 ms sleep, then assert" wait, which a loaded lane
  outlives (`test_thumbnail`'s queue drain read 23 == 24 once that way).
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

**Touch gestures (added later, zb2, PLAN decision #48)**
- `src/gesture-math.c/.h` — plain C: zoom about a point (the one rule the
  wheel, the keys and a pinch share, with the jx0 clamp and hx0 guard),
  the pinch's zoom, swipe and two-finger-tap classification.
- `src/viewer.c` — `GtkGestureZoom` (pinch about the midpoint, panning
  with it), touch-only `GtkGestureSwipe` (`navigate`), two-finger tap
  (`toggle-info`); a pinch hands a tool drag in progress a
  `GGAZE_VIEWER_DRAG_CANCEL`, followed by a `GGAZE_VIEWER_DRAG_REVERT`
  when the pinch ends as a tap (the crop tool puts the rectangle back); a
  pinch over a fitted picture keeps it fitted while its scale stays within
  a tap's 10 % wobble (the "fit detent": a two-finger pan reports ~1.01)
  and, past it, zooms by the scale measured from the band's edge
  (`gesture_math_detent_scale`), so leaving the detent is continuous;
  a new texture or an unmap CANCELs a drag in progress and ignores the
  rest of it; `src/tool-ctrl.c` handles CANCEL / REVERT before its
  no-texture geometry guard (the new texture may be NULL);
  `src/window.c` routes `toggle-info` to `win.info`, stops a slideshow on
  `navigate`, and spoils a swipe in progress on each slideshow step
  (`ggaze_viewer_spoil_swipe`). The GTK handlers are
  thin adapters (read the gesture's point / velocity / event type) over
  the public `ggaze_viewer_pinch_*` / `ggaze_viewer_swipe_track` /
  `ggaze_viewer_swipe`.
- Unit: `test_gesture_math.c` — zoom keeps the point's pixel, clamps
  (incl. the fit-raised ceiling and the fit-lowered floor, nonsense fits
  ignored), NaN/Inf anywhere refused; pinch refuses
  zero / negative / non-finite scale and overflow; the detent's rebased
  scale is 1 inside the band, continuous at both edges and proportional
  beyond; swipe refuses
  zero-duration (no velocity), vertical, diagonal, tiny, slow and
  reversing drags; tap refuses long, moving, scaling and nonsense input.
- Integration: `test_gestures.c` (see the table above) and seven
  `test_enhance_flow.c` subtests (dirty-gated swipe, crop tool refuses, a
  pinch in the straighten tool levels nothing -- not even on a stray END
  afterwards --, a pinch in the crop tool keeps the dragged rectangle, a
  two-finger tap in the crop tool leaves it as it was, and a
  `set_texture(NULL)` mid tool drag still drops the straighten line / lets
  go of and reverts the crop rectangle).
- **Known test gap** (needs real touch sequences, which GTK 4 cannot
  synthesise and Xvfb has no device for): what the adapters read from a
  live gesture — the midpoint from `gtk_gesture_get_bounding_box_center`,
  the finger's point from `gtk_gesture_get_point`, and that a real
  touchpad pinch's event type is `GDK_TOUCHPAD_PINCH` (the rule it feeds,
  "a touchpad pinch is never a tap", is tested through
  `ggaze_viewer_pinch_begin`'s `b_touchpad`) — and the drag gesture's
  `GTK_EVENT_SEQUENCE_DENIED` claim at pinch begin, which is a no-op on a
  gesture with no sequences (the viewer's own `b_drag_dead` flag, which
  is what the tests pin, keeps the drag inert either way). The emitted
  signals run these handlers: GtkGestureZoom "begin" / "scale-changed"
  / "cancel" / "end" (no points: the widget centre), GtkGestureSwipe
  "begin" / "swipe", the drag gesture and the scroll controller. Not
  every handler: GtkGestureSwipe's "update" (`_swipe_update_cb`) is only
  asserted to be connected, never run -- the tests lay the swipe's path
  down with `ggaze_viewer_swipe_track`, the call it would make.

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
- Animated GIF/WebP playback (task yb2, decision #46) — done. Not a
  `GdkPaintable` after all: the pixbuf backend decodes a multi-frame
  GIF/WebP as a `GdkPixbufAnimation`, walks its frames on the worker into
  one owned `GdkTexture` each (`pixbuf_util_animation_to_texture`: copied
  out of the decoder's buffer, which gdk-pixbuf 2.42 shares between all
  frames) and returns the **first frame** as the one `GdkTexture` the rest
  of the app expects, with the other frames and every delay attached to it
  (`src/loader/animation.{c,h}`: `GgazeAnimation`, `animation_attach` /
  `animation_lookup`, GObject qdata, one quark). `GgazeViewer` alone looks
  for them and plays them: a tick callback on the frame clock hands the
  frame time to `animation_playback_advance` (plain C: schedule, stall
  resync, the file's play count) and draws the frame that is due at the
  first frame's geometry, only while mapped and the frame clock runs; the
  tick is on the clock only near a frame change (a timeout re-adds it
  40 ms before), it ends on the last frame once the file's plays are done
  (a GIF without a NETSCAPE2.0 block plays once, a WebP its ANIM count),
  restarts from the first frame on every `set_texture`, remap and hold
  release, and holds it while a crop / straighten tool is up. So the
  texture LRU, prefetch, last-write-wins, the grid (first frame at scale),
  the histogram, the enhance graph, the tools, the clipboard and
  hold-`Space` are all unchanged and all operate on the first frame. Which
  files take the animated path is decided by a **decoder-free probe** of
  the bytes (`animation_probe`: GIF block walk / WebP RIFF chunk walk,
  frames + canvas + loop count) and a **playback budget**
  (`animation_within_budget`: frames × canvas ≤ 32 Mi pixels = 128 MiB,
  canvas ≤ 4 Mi pixels, ≤ 1000 frames; the memory arithmetic and the
  measured figures are at `GGAZE_ANIM_MAX_PIXELS`); anything else goes
  down the still path byte-for-byte as before. Frame delays are clamped to
  `GGAZE_ANIM_MIN_DELAY_MS` (20 ms) — which a 10 ms delay on glycin
  reaches; a 0 ms one never does (both decoders make it 100 ms).

**Tests**
- Extend `test_detect` + `test_loader_*` per backend, feature-gated.
- Unit (yb2): `test_animation.c` (TAP) — the probe over `anim.gif` /
  `anim.webp` / `zerodelay.gif`, the loop count (`/probe/fixture_plays`,
  `/probe/gif_loop_count`: NETSCAPE2.0 / ANIMEXTS1.0, N → N + 1 plays,
  first block wins, foreign and cut extensions; `/probe/webp_loop_count`),
  `manyframes.gif` one frame over the frame cap, EVERY prefix of the
  animated fixtures (truncation
  never over-reads, counts monotonically), over hand-built containers (a
  local colour table, an extension cut short, a VP8X flag with no ANMF
  frames, a chunk size past the end), the budget at each boundary (pixels,
  the 1000-frame cap incl. a 200 000-frame 1 × 1 probe, the 4 Mi-pixel
  canvas cap) and with a frame count that would wrap 32 bits, the delay
  clamp, the animation decode (still → static animation, garbage → error),
  the frame store and attach/lookup including the frames dying with their
  first frame, and the frame walk (`/animation/to_texture_*`: every frame
  its own texture with the file's colours and delays, the first frame
  intact after the rest were taken, a frame count, a pre-set cancel, a
  static decode, 0 ms delays arriving as 100 ms, `fastdelay.gif`'s 10 ms
  clamped to 20, the play count carried, a sub-2 ms first frame through
  a `GdkPixbufSimpleAnim`). `test_animation_playback.c` (TAP) — the
  playback schedule over a fake clock: anchor and due times, the average
  rate kept under late calls, the stall resync, play counts 1 and 2
  holding the last frame, delay -1, the clamp in the schedule.
  `test_loader_pixbuf.c` `/loader/pixbuf/animated_gif_attaches_animation`,
  `/short_delay_gifs_attach_animation` (0 ms → 100 ms, 10 ms → 20 ms),
  `/animated_webp_attaches_animation`
  (skipped where the webp module is absent or frame-less; each asserts the
  first-frame texture is byte-identical after every frame was read),
  `/single_frame_gif_has_no_animation`, `/truncated_animation_is_gated`
  (the decode gate refuses a truncated animation; a mid-frame cut answers
  within budget; the WebP half runs only with a webp module and tolerates
  exactly webp-pixbuf-loader 0.2.7's two messages, on the classic and the
  structured log road alike — `/webp_noise_filter_catches_both_roads`),
  `/scaled_animation_is_first_frame`, `/animation_play_count` (anim.gif
  0, once.gif 1, once.webp 1), `/over_budget_animation_is_still`
  (`manyframes.gif`: first frame, nothing attached).
  `test_viewload.c` `/viewload/animation_rides_with_the_texture` (miss,
  neighbour prefetch and hit all hand out the same first-frame object with
  the animation on it; the still next to it carries none).
- Integration (yb2): `test_viewer.c` `/viewer/animation_plays_and_keeps_
  first_frame_texture` (same object AND same pixels after six frame
  changes), `/animation_survives_zoom_pan_and_overlay` (the overlay hook
  sees the canvas geometry while frames play), `/still_image_does_not_
  animate`, `/still_after_animation_stops_playback`, `/hold_first_frame_
  pauses_and_resumes`, `/animation_pauses_while_unmapped` (the grid page
  over the viewer stops playback, coming back restarts it),
  `/fast_delay_plays_at_the_clamp` (`fastdelay.gif`: frame changes over
  one second ≤ W / 20 ms + 2, and at least one),
  `/play_once_gif_holds_last_frame`, `/play_once_webp_holds_last_frame`
  (ends on the last frame, nothing scheduled),
  `/slow_animation_ticks_only_near_frames` (`slow.gif`, 500 ms frames:
  ≤ 8 ticks per due frame, measured 8 in 1.4 s; a tick every vblank
  measured 86), `/unmapped_viewer_holds_first_frame`.
  `test_window.c` `/window/info_plots_animation_first_frame` (the `i` card
  plots the first frame, and a plot gathered afresh after the frames
  played still does). `test_enhance_flow.c` `/enhance_flow/crop_tool_
  holds_animation_first_frame` (GEGL lanes: the crop tool holds frame 1;
  Esc and an applied "no crop" both let it play on).
- Verified against gdk-pixbuf 2.42.12 + webp-pixbuf-loader 0.2.7 (Debian
  trixie container, minimal build): the animation, loader_pixbuf,
  viewload, viewer and window suites pass; with the frame copy removed,
  the first-frame assertions of all four fail (the texture reads back as
  the last frame, green 75 instead of 255).

**Acceptance:** JXL/AVIF/HEIF open when built; minimal build still green;
animated GIF (and WebP where gdk-pixbuf decodes its frames) plays in the
large view, grid thumbnails and every other consumer use the first frame,
static images unchanged, rapid `h`/`l` scrubbing stays instant (an
animation is one cache entry like any still).

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
wired into the window (tu0), and the crop (`c`), straighten (`R`, `r`
since 6i2) and rotate 90 (`[`/`]`) tools ride the same preview graph (wb2,
see below). 6i2 (decision #49) made the panel the one edit panel with
modal edit keys (`src/edit-mode.{c,h}`) and a key-hint bar; 7i2
(decision #50) added `u` / `U` undo / redo of edit steps over a bounded,
per-image history of whole edit snapshots (`src/edit-history.{c,h}`);
8i2 (decision #51) made one number per preset graph tunable
(`{s:DEFAULT:MIN..MAX[:STEP]}`, `src/preset-strength.{c,h}`), tuned from
the panel with `j`/`k` card selection, `h`/`l` and a slider. ICC
color management on the enhance/export path is done (xb2, decision #45; see
`docs/gegl.md` "Color management").

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
  `tool-ctrl.{c,h}` is the modal `c`/`r` session: it draws on the viewer's
  overlay hook (`ggaze_viewer_set_overlay`: a draw callback with the image's
  on-screen geometry and a drag callback that takes over from panning),
  answers its keys (rows scoped to its mode in `shortcuts.c`, as
  `GgazeKeyOp`s) through `edit-mode.c`'s capture-phase router ahead of the
  global shortcut table, and edits the Transform through
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
  recompute either way. For a colour-managed preview the original held up is
  the managed one (decision #45), not the plain decode: fetched in a worker
  on the first press (the plain original shows until it lands), dropped on
  discard / navigation / rewrite, and plotted by the `i` card.
- ICC color management via GEGL/babl (open question G, decision #45, xb2):
  a PNG/JPEG with a non-sRGB profile decodes through
  `gegl:png-load`/`gegl:jpg-load` (space-tagged; untagged and sRGB-profiled
  files keep the loader path, byte for byte the same file without a
  profile — main's copy swapped R/B and was premultiplied, fixed by xb2),
  only when the loader's gate and `loader/intact.c` vouch for it, the
  profile passes `icc_profile_is_sane` (tone-shaped curves, bounded `para`
  parameters), babl's space has a name of its own no longer than its format names
  leave room for (254 − 1 − the longest registered encoding: 229) and
  converts through the profile's own formula curves (babl < 0.1.114
  shares one `para` curve per type and gamma), a
  PNG's iCCP is the one libpng keeps (no sRGB beside it, any gAMA / cHRM
  one libpng 1.6.40 takes without discarding the iCCP:
  `icc_png_applied_profile`), and the profile fits the image's components
  (else the loader path decides, as before);
  preview converted to sRGB; hold-`Space` shows the managed original;
  PNG/JPEG exports keep the source profile, WebP exports come out sRGB. The
  `i` card names the colour space in every build (`icc.{c,h}`).
- "GEGL not built in" status message (via the info-overlay label; this
  project has no toast infra) for `a`/`s` when the build has no GEGL; safe no-op for the numeric preset hotkeys.

**Tests**
- Unit: `test_enhancer.c` (gated): each preset dims + non-zero; export file
  written + original untouched; format selection by extension; stale-dest
  and unsupported-extension rejection; the transform on the chain pixel by
  pixel (quarter turns as permutations, crops in base coordinates, the
  straighten's analytic sizes, a turned export). `test_croprect.c` and
  `test_transform.c` (every lane, no GEGL): the rectangle rules and the
  angle / size maths. `test_edit_history.c` (every lane, no GTK; 7i2):
  push / undo / redo over whole snapshots with their labels, a new step
  dropping the redo tail, a no-op step not recorded (and not costing the
  redo), the bound forgetting the oldest step (3, and the default 64),
  runs coalescing one kind + key into one step, what closes a run
  (another key or kind, a plain push, an undo, `end_run`), a run back at
  its start dropped, clear, and every entry point's NULL argument (8i2:
  the strengths in the snapshot, and a strength run undone as one step;
  ai2: all 32 mask bits are state, `edit_history_remap` rewrites every
  snapshot and drops a step it made a no-op, keeping the undo / redo
  split, and closes an open run).
  `test_preset_strength.c` (every lane, no GTK / GEGL; 8i2): the
  placeholder parsed in both forms (explicit and default step, the
  decimals), substituted into the graph (the default written exactly as
  the pre-8i2 text, clamped, never `-0`), stepped / clamped / snapped to
  canonical values (seven steps up and down land on the parsed default; review: keys step on the slider's grid, `{s:0.3:0..1:0.25}` max then down once is 0.8),
  the card / title label (signed when the range goes below zero), every
  malformed placeholder refused with its message (review: numbers needing more than 6 decimals or beyond ±1e15 too), and the same text under
  a `de_DE` `LC_NUMERIC` (skipped where no such locale is installed).
  `test_enhancer.c` (8i2): each built-in at its default renders the same
  pixels as the pre-8i2 programmatic node and substitutes to its old
  graph text, a resolved strength reaches the pixels (Brightness 1.2 ==
  a plain `exposure=1.2` graph), the title names only non-default
  strengths, and a user preset with a placeholder is tunable while one
  with a malformed placeholder is kept, not tunable, and fails to render
  with the parse message. ai2: the list stops at 32 rows (24 user presets;
  a longer `enhancer_set_presets` list too); a user preset at row 20 has
  its default strength, its title name and value, its chain key, and
  renders alone through bit 20; `enhancer_presets_map` over the same list,
  a reorder, a removal, an append, a graph edited, a rename, both at once
  (removed) and duplicates; `enhancer_state_remap` moving bits and
  strengths, clamping into an edited range, a gained placeholder at its
  default, a removed preset dropped and a new row off. xb2 (ICC): `test_enhancer_icc.c` (gated) — a
  profiled PNG/JPEG is tagged with its space and previews managed (the
  fixtures store pure red under a red/blue-swapped profile, so managed =
  blue), also with EXIF Orientation 6 (`swapped-rot6.jpg`), CMYK
  (`cmyk-icc.jpg`, a lut8 printer profile through babl's LCMS) and grey
  (`grey-icc.png`/`.jpg`, linear curve) running in sRGB, and with the SOF
  past 64 KiB or padding between segments; the space survives two presets +
  a quarter turn; sRGB-profiled PNG/JPEG decode byte-identical to the same
  files stripped of the profile; untagged / corrupt-iCCP / non-local
  (`mem_file`) / GEGL-op-missing (`enhancer_test_set_missing_op`) files and
  a profile for other colour components (a grey profile on RGB) take the
  loader path; every broken file (cut, lying headers, two SOFs, corrupt PNG
  data -- bad CRC, bad deflate, short IDAT -- and a progressive JPEG cut
  mid-scan or right after a COM holding FF D9 between scans, which made
  `gegl:jpg-load` exit the process) gets exactly the loader's verdict;
  PNG/JPEG exports carry the source's profile (byte for byte for the first
  profile of its kind in the process; babl answers a later equivalent
  profile with the earlier one's space and bytes), a WebP export
  comes out sRGB, a missing saver is NOT_SUPPORTED; the render reports
  "managed" for CMYK/grey too; the lazy managed original (swapped, grey,
  CMYK; none for an untagged file; CANCELLED when cancelled);
  `enhancer_would_manage` (the card's note) per fixture; every JPEG case
  also holds in a GEGL build without libjpeg (`GGAZE_HAVE_JPEG`); and every
  profiled file in `./sample-images` (skipped when absent) is vouched for
  with its size and, when its profile is not sRGB, decodes managed (and a
  corpus PNG's iCCP is the one libpng keeps). Review 5, the review-5 cases
  in fresh subprocesses: the curves babl could not invert (constant
  tables, 1137 of 1139 points at 0, a spike, a `para` above [0, 1]) are
  declined and their files export as JPEGs; the `para` at −32767 is
  declined without a slot, a `para` curve whose babl space name is past
  the limit is declined (a kept slot) and one at the limit managed -- the
  limit put at babl's own length for a name through a seam, since babl
  0.1.112 spells names longer than 0.1.128; review 7: the computed limit
  is 254 − 1 − 24 with babl's and GEGL's formats, a space named in 226
  (0.1.128) / 229 (0.1.112) characters is managed under it, and two type 3 / 4
  `para` profiles of one gamma and the same primaries are each managed
  with their own curve (babl 0.1.128) or the second declined (babl
  0.1.112, where it got the first one's curve and space); a second grey and a second
  same-primaries RGB table-curve profile (both `lut-trc`) are declined and
  their files take the loader path; PNGs whose iCCP libpng drops (intent
  0xFFFF, a v4 odd length) next to a gAMA, and sound ones next to an sRGB
  or a gAMA / cHRM libpng 1.6.40 rejects (gamma 0, two gAMA, no
  primaries), are declined with no slot, verdict or new babl format, and
  sound ones next to a good gAMA (and cHRM) are managed in the vetted
  profile's own space (review 6); a PNG export carries gAMA + cHRM beside
  its iCCP and `enhancer_would_manage` vouches for its reload; corrupt PNG
  data gets the loader's verdict, whatever it is (gdk-pixbuf 2.42 decodes
  past a bad IDAT CRC); the
  fuzz also flattens / spikes tables and converts float both ways.
  `test_icc.c`, `test_info.c` (every lane) and `test_intact.c` (GEGL lane,
  like `intact.c`): profile extraction (PNG iCCP, multi-segment JPEG APP2,
  padding skipped, every broken container), the iCCP libpng keeps (one,
  before PLTE, CRC right, libpng 1.6's fatal header checks one rule at a
  time, the colour space per PNG colour type, 8 000 000 bytes, no sRGB, gAMA /
  cHRM only single, well-formed and of values libpng 1.6.40 takes -- its
  gamma range, its chromaticity round trip: out of range, collinear
  primaries, a white point outside them), the curve-shape and `para`-bound rules, the `desc` parser, the card's
  colour-space line in each state (one line, capped at 64 characters, a
  padded JPEG not "unreadable"), the completeness walk and sizes (SOF past
  64 KiB) and component counts, PNG rows (Adam7 sizes cross-checked with
  real interlaced files), one IDAT run (IDAT, tEXt, IDAT is short), the
  IHDR caps before the inflate (a <1 MB zlib bomb declaring 32768²), CRC /
  inflate / filter corruption, cancellation, and the libjpeg pass (a
  two-SOF JPEG; EOF fatal: cut, no EOI, the progressive cuts).
- Integration: `test_enhance_flow.c` (gated `if gegl_dep.found()`): async
  apply swaps the texture without touching the original (byte-identical),
  toggle-off resets to the original, hold-Space compares then restores
  (for a managed file -- swapped, CMYK, grey -- against the lazily fetched
  managed original, which the `i` card plots and a discard drops),
  `s` twice produces collision-suffixed copies, non-dirty navigation is
  immediate — plus the Save/Discard/Cancel prompt driven to each outcome
  (see the suite table above). `test_grid_select_gate.c` covers `gridview.c`'s
  side of the gate with no GEGL involved.
  `test_window.c::test_enhance_a_is_safe_with_and_without_gegl`
  (always built, both lanes) covers the GEGL-disabled safety message.

**Acceptance:** `a` popover (layered, async apply); `s` copy
(collision-safe); hold-`Space`; dirty prompt across navigate/trash/delete/
move/open/quit; minimal build reports "GEGL not built in" cleanly. `c`/`R`/
`[`/`]` landed with wb2 and ICC with xb2.

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