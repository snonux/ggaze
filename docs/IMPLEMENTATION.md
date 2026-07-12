# ggaze — Implementation Plan

The engineering execution layer between the design docs and the first line of
code. The design is locked in via the decisions log in [PLAN.md](PLAN.md);
this page says *how* we build it, in what order, and what "done" means.

## Guiding principles

1. **Every milestone ends runnable** (per [roadmap.md](roadmap.md)) — and every
   commit within a milestone compiles + `meson test` passes.
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
  `trash`, `mover`, `opener`, `runner`, `enhancer`, `info`, `texturecache`,
  `clipboard` helpers). No GTK display needed.
- GLib `GTest` framework; per-module `a(ss)`/path/EXIF fixtures.
- **Coverage gate ≥80%** on these modules (gcov/lcov), flipped to *fail* at
  M10, *warn* before.
- Each module lands its test file in the same milestone as the module.

### Integration tests — `tests/integration/test_<flow>.c`

- Exercise **multiple modules together** through the real public API, with a
  real temp directory and real files (no mocks of `GFile`/`GFileMonitor`). They
  verify the *contracts* between modules the unit tests can't reach.
- Use a lightweight **offscreen GTK harness** (`gtk_test_init` +
  `GtkWindow` offscreen via `GdkSurface`/`gtk_widget_realize` under
  `GDK_DEBUG=no-grabs`) where a widget is needed; prefer pure-GLib harnesses
  where one isn't.
- No coverage gate (they cross module boundaries), but they must be green in CI.
- Land at the milestone that first makes the flow possible, and grow with it.

#### Planned integration suites (mapped to milestones)

| Suite | Lands at | Verifies |
|-------|----------|----------|
| `test_open_and_show.c` | M1 | CLI file arg → window → viewer has a non-null `GdkTexture` of the right size + upright orientation. |
| `test_walk_folder.c` | M2 | Folder arg → navigator listing; `h`/`l` action changes current; `GFileMonitor` add/delete propagates to navigator; wrap at ends. |
| `test_responsive_nav.c` | M3 | Rapid `next` ×10: only the last `GdkTexture` is shown (last-write-wins invariant), UI thread not blocked (measured via a main-loop timer). |
| `test_progressive_jpeg.c` | M6 | A progressive JPEG fires the partial-texture callback at increasing resolution before the final. |
| `test_grid_cull.c` | M7 | Grid view shows N cells; `d` bins one into `./Trash`, cell dims; `u` restores; counter reflects remaining; `Enter`→large on the right cell. |
| `test_move_undo.c` | M8 | Mark 3 → `m`→dest2 → files gone from folder, present in dest; `u` moves back; collision suffixing. |
| `test_runner_rescan.c` | M8 | `!` runs a script that writes a file into the dir; on exit the navigator rescans and the new file appears; injection-guard filename is single-quoted. |
| `test_enhance_flow.c` | M9 (gated) | `a`→preset applies a preview (texture differs from raw); `s` writes `-enhanced.<ext>` with EXIF Orientation=1 and original byte-identical; navigate-away dirty prompt Save/Discard/Cancel. |
| `test_clipboard_copy.c` | M8 | `Ctrl+c` with no marks → `image/png` on `GdkClipboard`; with marks → `text/uri-list`; paste back into a fake target. |
| `test_full_lifecycle.c` | M10 | The elevator-pitch session scripted: open → walk → `i` → `d` ×k → mark → `m`→dest → `e`→program (use `true`) → `!`→script → quit. End-to-end smoke. |

### Test infrastructure (built in Phase 0)

- `tests/meson.build` wires `unit` and `integration` subdirs as separate
  `meson test` suites (`-t suite:unit`, `-t suite:integration`) so they can be
  run selectively in CI.
- `tests/helpers/` — shared helpers: temp-dir factory, fixture locator,
  offscreen-window builder, fake `GdkClipboard` target, main-loop drain with
  timeout (prevents a hung test from blocking CI).
- `tests/fixtures/` — curated images per format + a rotated-EXIF JPEG +
  progressive JPEG + RAW+JPEG pair + an injection-hostile filename (`;rm -rf /`).
- CI runs: unit always; integration on the minimal lane; the `gegl` lane adds
  the gated suites; ASan lane runs both.

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
  to build/test (`meson setup`, `meson test -t suite:unit` / `suite:integration`,
  coverage target), the mandatory two test tracks and their locations,
  convention enforcement (`clang-format`, header guards, `type_new`/
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
- Smoke: `test_window.c` — offscreen window, stack has two children.

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
  pan, fit, cursor-centered zoom, pan clamp; `viewer_set_texture`.
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

**Tests**
- Unit: `test_info.c` (EXIF extraction + orientation tag).

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
- `src/trash.c/.h` — `./Trash` bin (lazy, collision suffix), restore-last,
  permanent delete.
- Window: `d`/`D`/`u`; `d` advances; counter = remaining; `t` toggle.

**Tests**
- Unit: `test_thumbnail.c`, `test_trash.c`.
- Integration: `test_grid_cull.c`.

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
- `src/clipboard.c/.h` — `image/png` (decode in `GTask`) / `text/uri-list`;
  union provider for one file (decision V).
- Reusable popover (`ggaze_popup`) for move/open/scripts/(enhance later):
  `(hotkey, label)` rows + key controller firing on digit/letter.
- Unified one-level undo `u` (decision P).
- Preferences dialog (`,`): `AdwPreferencesWindow` editing ordered `a(ss)`
  lists + sort/background/scroll/slideshow/hide-trashed.

**Tests**
- Unit: `test_mover.c`, `test_opener.c` (`true`/`false` commands, weird
  filenames), `test_runner.c` (injection guard, exit status), `test_clipboard.c`.
- Integration: `test_move_undo.c`, `test_runner_rescan.c`, `test_clipboard_copy.c`.

**Acceptance:** full culling workflow works keyboard-only.

---

## M9 — GEGL quick-enhance, crop/straighten/rotate, compare (optional)

**Deliverables**
- `meson` `gegl` feature; `src/enhancer.c/.h` plain-C.
- `enhancer_get_presets` (built-in programmatic / user `gegl-graph` text,
  decision #34); `enhancer_apply` in `GTask`; `enhancer_export` →
  `<stem>-enhanced.<ext>` same dir, collision `-1`; EXIF Orientation=1 on
  export (decision #26).
- Viewer: preset active → import → `GeglBuffer` → apply → `GdkTexture`; not
  during scrub (decision #34); "enhanced" badge.
- Compose order load→enhance→rotate→straighten→crop→export (decision #35).
  Crop (`c`), straighten (`R`), rotate 90 (`[`/`]`) stack on the preview graph.
- Dirty flag: navigate/`d`/`D`/`m`/quit with dirty → Save/Discard/Cancel
  (decisions #34/#18); `s` clears; re-press/Esc discards directly.
- Hold-`Space` compare (decision #23/#24).
- ICC decode/export via GEGL/babl (closes open question G).
- "GEGL not built in" toast when off.

**Tests**
- Unit: `test_enhancer.c` (gated): each preset dims + non-zero; export file
  written + orientation=1 + original untouched; rotate-then-crop compose.
- Integration: `test_enhance_flow.c`.

**Acceptance:** `a` preview; `s` copy; `c`/`R`/`[`/`]`; hold-`Space`; dirty
prompt; minimal build toasts cleanly.

---

## M10 — Polish & packaging

**Deliverables**
- AppStream metainfo, app icons (symbolic + full).
- Fedora RPM spec; optional Flatpak manifest.
- `ggaze(1)` man page (stub in M0, finalized here).
- Window geometry persistence (GSettings `window-geometry`).
- **Coverage gate → fail** at <80% on plain-C modules.
- **Quality audit:** `auditing-code-quality` skill (C-adapted:
  c-best-practices + find-code-bugs + solid-principles + beyond-solid-principles),
  triage via `agent-task-management`, fix all HIGH/MEDIUM.
- **Keyboard-completeness audit:** visible hotkeys, mnemonics, `?` overlay,
  full dialog traversal.
- Empty-`./Trash` menu action.
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
    `info`, `texturecache`, `clipboard`) — every `type_new` must have a matching
    `type_delete` and every `GTask`/`GSubprocess`/`GFileMonitor`/`GdkTexture`
    must be unreffed;
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