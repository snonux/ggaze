# ggaze — Project Plan (living document)

**GNOME Gaze** — a small, fast, native image viewer for Fedora
Linux, written in C with GTK4. Its job: quickly preview a folder of pictures
downloaded from a camera, cull the rejects, move on.

This file is the running tracker. The detailed design lives in the sibling
docs; this page keeps the overview, the decisions log, and the status of each
milestone in one place. Update it as we go.

---

## Elevator pitch

`ggaze ~/Downloads/Camera/IMG_0001.jpg` opens the folder as a thumbnail grid,
`Enter` drops into the large view, `h`/`l` (or `←`/`→`) scrubs through the shoot, `i` shows
EXIF, `d` bins a reject into `./Trash` (undoable), `D` deletes it outright, `v` marks
keepers, `m` then `1` ships them to "irregular ninja", `e` opens a keeper in GIMP,
`!` runs `usbimport` to pull new shots, `a` previews a quick GEGL auto-fix, `Esc` returns to the grid, `q` quits.
No library, no database, no sidecars.

## Two views

- **Grid** — thumbnail overview of the whole folder, keyboard-navigable;
  thumbnails resize with `+`/`-` and the grid reflows to fit.
- **Large** — single picture with zoom/pan, fullscreen, slideshow.

Switch with `Enter` (grid→large) and `Esc`/`Backspace` (large→grid), or `t` to
toggle. The cursor stays in sync across the switch.

## Opening files & folders

- `ggaze file.jpg` opens the file (large view, parent folder as navigator);
  `ggaze dir/` opens the folder in the grid.
- Drag-and-drop a file or folder onto the window to open it.
- `o` opens a file/folder dialog.

## Folder monitoring

- `GFileMonitor` watches the current dir; external adds/deletes/moves
  refresh the grid live (debounced). A removed current file falls back to the
  nearest; `r` still does a manual reload.

## Deletion model

- `d` → move to `<folder>/.Trash/` (local, recoverable, undoable via `u`).
- `D` → permanent delete (unlink, no undo).
- `./Trash` lives with the shoot; empty it via the menu or from a shell.
- Trashed/deleted items stay listed but dimmed (hide toggle in settings).

## Moving & marks

- `v` / `V` / `Ctrl+a` mark pictures; marks persist across views.
- `m` → popup of **configured** destinations, each with an auto-assigned
  hotkey (`1`, `2`, …). Press the hotkey to move the marked set there.
- Destinations = ordered `a(ss)` list of name → path pairs in GSettings.
- `u` undoes the last `d` or `m`.

## External programs

- `e` → popup of configured programs (auto-assigned hotkeys), launches the
  current image in the chosen one (detached `GSubprocess`, `%f` = path).
- `editors` = ordered `a(ss)` list of name → command pairs in GSettings.

## Shell scripts

- `!` → popup of configured scripts (auto-assigned hotkeys), runs the chosen
  one **asynchronously** via `/bin/sh -c` (`%f` = image, `%d` = folder).
- On exit, ggaze rescans the directory (e.g. `usbimport` adds files).
- `scripts` = ordered `a(ss)` list of name → command pairs in GSettings.

## Quick enhance (GEGL, optional)

- `a` → popup of enhance presets (auto-assigned hotkeys); applies a GEGL graph
  as a **non-destructive live preview**; press again / `Esc` to turn off.
- `s` / menu *Save enhanced copy…* writes `<name>-enhanced.<ext>`; original
  untouched. **No auto-save** — navigating away from an un-exported preview
  prompts Save/Discard/Cancel.
- `enhance-presets` = ordered `a(ss)` list of `(name, gegl-graph)` pairs.
- GEGL is an optional meson feature; without it, ggaze is a plain fast viewer.
  GEGL also brings ICC color management. See [gegl.md](gegl.md).

## Crop, straighten & rotate (GEGL, optional)

- `c` crop (adjustable rect, aspect presets), `R` straighten (horizon drag /
  angle nudge), and `[`/`]` rotate 90° CCW/CW — non-destructive, same preview
  graph + `s` to export a copy.
- Hold `Space` to compare original vs modified (before/after); release returns
  to the modified preview.
- Large view only; GEGL required.

## Copy to clipboard

- `Ctrl+c` / menu *Copy*: no marks → current image as PNG pixels; marks →
  marked files as `text/uri-list`. Paste into Katogram/GIMP/file managers
  (like gthumb).

## Reachability (keyboard + GUI)

Every GUI element has a hotkey, and it's shown on the element (menu items,
tooltips, popup entries). Conversely, every action is also reachable by
mouse — header-bar buttons for the common ones, the `F10` menu for the rest.
`?` lists all shortcuts. Neither keyboard nor mouse is a fallback.

## Quality & testing

- ≥80% unit-test coverage on plain-C modules (gcov/lcov), gated in CI.
- Run the `auditing-code-quality` skill at each milestone boundary and before
  release — for C: `c-best-practices` + `find-code-bugs` + `solid-principles`
  + `beyond-solid-principles`, findings tracked as tasks via
  `agent-task-management`. Fix all HIGH/MEDIUM before release.

---

## Tech at a glance

| Concern      | Choice                          |
|--------------|---------------------------------|
| Language     | C11                             |
| UI           | GTK4 + libadwaita (decided)    |
| Async/objects| GLib / GObject / GTask          |
| Config       | GSettings (`org.buetow.ggaze`) |
| Build        | Meson + Ninja                   |
| Decode       | GdkPixbuf fallback + libjxl/libavif/libheif (feature options) |
| Image proc.  | GEGL + babl (optional, feature-gated) — enhance, ICC, export copy |
| Thumbnails   | freedesktop TMS, `~/.cache/thumbnails/` |
| Packaging    | Fedora RPM + AppStream (Flatpak later) |
| Testing      | `meson test` + gcov/lcov; ≥80% on plain-C modules |
| Quality audit | `auditing-code-quality` skill at milestones (C: c-best-practices + find-code-bugs + SOLID + beyond-SOLID) |

See [tech-stack.md](tech-stack.md).

---

## Milestones

| M   | Title                      | Status   | Notes |
|-----|----------------------------|----------|-------|
| M0  | Skeleton (app + empty window) | not started | Meson, GtkApplication, file/folder arg, --version/--help |
| M1  | Show one image (zoom/pan)    | not started | custom viewer widget, GdkPixbuf backend, EXIF orientation on load |
| M2  | Walk the directory          | not started | navigator, `h`/`l` prev/next, folder arg + drag-drop open, `GFileMonitor` auto-refresh, wrap, header counter |
| M3  | Responsive + prefetch       | not started | GTask decode, 2–3 slot LRU, cancel-in-flight |
| M4  | Fullscreen + slideshow + info | not started | `f`, `s`, `i` EXIF overlay |
| M5  | Modern formats             | not started | JXL/AVIF/HEIF backends, animated GIF/WebP |
| M6  | Progressive low-res preview | not started | libjpeg-turbo early low-res scan |
| M7  | Thumbnail cache + grid view | not started | TMS cache + `gridview`, dim trashed items |
| M8  | Selection, move, open-external & scripts | not started | marks, `m`/`e`/`!` popups, `mover`/`opener`/`runner`, `Ctrl+c` clipboard, destinations+editors+scripts `a(ss)`, prefs |
| M9  | GEGL quick-enhance, crop/straighten/rotate (optional) | not started | `enhancer`, `a`/`c`/`R`/`[`/`]` tools, hold-`Space` compare, non-destructive preview, `s` save copy (no auto-save, prompt on navigate), ICC via GEGL |
| M10 | Polish & packaging         | not started | AppStream, RPM, man page, settings, keyboard-completeness audit, ≥80% coverage gate |

Later / maybe: configurable keybindings, recursive walking, RAW embedded
preview, burst grouping, RAW+JPEG pair hiding, GEGL transforms/artistic.

See [roadmap.md](roadmap.md).

---

## Decisions log

Decisions made during planning. Newest first.

| # | Date       | Decision                                                                 | Rationale                         |
|---|------------|--------------------------------------------------------------------------|-----------------------------------|
| 28 | 2026-07-12 | Folder monitoring via `GFileMonitor` (GIO): external adds/deletes/moves refresh the grid live (debounced); removed current file falls back to nearest. | New shots from usbimport/etc. appear without manual reload. |
| 29 | 2026-07-12 | **UI toolkit: libadwaita** (was A). GNOME-native header bar/dark viewer/system theme; no theming overrides. | Native Fedora look per the gthumb-but-KISS direction. |
| 30 | 2026-07-12 | **App ID `org.buetow.ggaze`** (was B). | Matches buetow.org domain. |
| 31 | 2026-07-12 | **Custom viewer widget** (was L), not `GtkPicture`. | Cursor-centered zoom, pan clamp, hold-`Space` compare, tool overlays. |
| 32 | 2026-07-12 | **Single instance** (was E); new `open`/drop replaces current folder+image. | Standard GNOME behavior; no window sprawl. |
| 33 | 2026-07-12 | **Camera specifics** (was K): burst grouping deferred to "later"; hide RAW sidecars by default (toggle to reveal); default sort = filename (EXIF capture-time as a menu option); import folder = just a path. | KISS first; culling-friendly grid; filename ≈ shot order. |
| 34 | 2026-07-12 | **GEGL integration** (was U): optional meson feature; built-in presets programmatic, user presets as `gegl-graph` text; apply only when settled (not during scrub); "enhanced" badge; export `<stem>-enhanced.<ext>` same dir, collision suffix `-1`; dirty-prompt fires on `d`/`D`/`m`; no gegl-gtk. | Keeps core fast; KISS preset UI; explicit save. |
| 35 | 2026-07-12 | **GEGL compose order** (was W): load → enhance(color) → rotate → straighten → crop → export; straighten auto-crop default on. | Crop the final framed image; remove rotated corners. |
| 36 | 2026-07-12 | **Enhance presets** (was X+#4): one active color preset (replace); crop/straighten/rotate stack on top; "reset preview" clears all; combine color presets via one `gegl-graph` entry; curves via `gegl:contrast-curve`; no fine-adjust nudging in v1. | Predictable + KISS; combinable via graph text. |
| 37 | 2026-07-12 | **Milestone Leans locked**: C GdkPixbuf-first; G color via GEGL/sRGB-else; H scroll=zoom + `pan-when-zoomed` mode; M GSettings `a(ss)` destinations; N move+suffix; O 1-9,0,a-z (cap 36); P one-level undo; Q marks path-based/survive re-sort/clear on trash; R raw cmd+%f+GSubprocess; S /bin/sh -c single-quote+rescan; T 64-512px ±32px custom bucket; V PNG+uri-list union provider; Y EXIF normalize-to-identity, export tag=1; Z folder→grid/file→parent/many→first; AA 250ms debounce nearest; F flat default; J RPM+AppStream first. | Working defaults confirmed at each milestone. |
| 40 | 2026-07-12 | Run the `auditing-code-quality` skill at each milestone + before release (C-adapted: `c-best-practices` + `find-code-bugs` + `solid-principles` + `beyond-solid-principles`, tracked via `agent-task-management`); fix all HIGH/MEDIUM findings. | Structured, well-factored project; catch defects + design smells early. |
| 39 | 2026-07-12 | **≥80% unit-test coverage** on plain-C modules (navigator/detect/thumbnail/mover/opener/runner/enhancer/trash/settings) via gcov/lcov with a CI coverage gate; GTK widgets get smoke tests. | Quality floor; refactor safely. |
| 38 | 2026-07-12 | **Gaps folded in**: mark count in header; window-geometry persistence; CLI `--version`/`--help` (+`--sort`/`--view` later); bulk `D` confirm >1; `scroll-behavior` = zoom/pan-when-zoomed/navigate; `Ctrl+c` copies the *displayed* image; export same ext + JPEG q95 (lossless later); `e` opens the *original* file; go-to-# skipped for v1. | Plan now complete before implementation. |
| 27 | 2026-07-12 | Open a folder arg (`ggaze dir/` → grid) and accept drag-and-drop of a file/folder onto the window; `o` dialog allows folders too. | Match gthumb flexibility; open anything from CLI, file manager, or drag. |
| 26 | 2026-07-12 | Honor EXIF Orientation on load (upright display); manual rotate/straighten compose on top; export resets the tag to normal. | Portrait/tilted camera shots display correctly without manual fix. |
| 25 | 2026-07-12 | Expand GEGL enhance presets: brightness, contrast, saturation, warm/cool, white balance, shadows/highlights, levels, clarity + artistic (B&W/sepia/vignette/softglow); strength tunable via `enhance-presets` gegl-graph text (no slider UI). | Cover the common quick fixes as one-shot presets; keep KISS. |
| 24 | 2026-07-12 | Compare moved to `Space` (hold); zoom-fit folded into `0` (toggle fit/100%); `\` freed. | `Space` is the comfortable hold-to-compare key; one zoom toggle key. |
| 23 | 2026-07-12 | Hold `Space` to flash the original image; release to return to the modified preview (before/after compare) to decide whether to `s` save. | Judge edits before saving; no accidental keeps. |
| 22 | 2026-07-12 | Add 90° rotation (`[`/`]`, `gegl:rotate-on-center`) as a one-shot non-destructive GEGL transform; `s` exports a copy. | Quick orientation fix; original untouched. |
| 21 | 2026-07-12 | Add crop (`c`, `gegl:crop`) and straighten (`R`, `gegl:rotate`) as non-destructive interactive GEGL tools; `s` exports a copy. | Level horizons and frame shots without leaving ggaze; original untouched. |
| 20 | 2026-07-12 | `Ctrl+c` copies to clipboard: current image as PNG pixels, or marked files as `text/uri-list` (paste into Katogram/GIMP/etc. like gthumb). | Quick hand-off of an image/selection to other apps. |
| 19 | 2026-07-12 | Layout & design reminiscent of gthumb (header bar, thumbnail grid, full-window viewer) but KISS: no folder sidebar, no catalogs/tags, no status-bar clutter, no batch/edit toolbars. | Familiar GNOME image-app feel without the weight. |
| 18 | 2026-07-12 | No auto-save of image changes. `s` saves an enhanced copy manually; navigating away (or quitting) from an un-exported enhance preview prompts Save/Discard/Cancel. Slideshow moved to `S` to free `s` for save. | Originals never silently modified; explicit consent. |
| 17 | 2026-07-12 | Plan GEGL (optional, feature-gated) for quick non-destructive enhance (`a` popup of presets) + export copy (`s`); also brings ICC color mgmt and format save. | Judge/fix keepers in-app without a full editor; original never modified. |
| 16 | 2026-07-12 | Grid thumbnails are resizable (`+`/`-`); grid auto-reflows to fit; size persisted in `thumbnail-size` GSettings. | Overview at a glance vs. detail, ad hoc. |
| 15 | 2026-07-12 | `!` runs configurable shell scripts asynchronously via `/bin/sh -c` (`%f`/`%d`), rescan dir on exit; `scripts` `a(ss)` settings. | Run usbimport etc. from within ggaze; pick up new files. |
| 14 | 2026-07-12 | `e` opens the current image in a configurable external program via a popup (auto-assigned hotkeys); `editors` `a(ss)` settings; detached GSubprocess launch. | Hand off to GIMP/identify/etc. without leaving ggaze. |
| 13 | 2026-07-12 | UI is self-documenting: hotkeys shown on elements, tooltips, `?` overlay, badges/counters/toasts narrate state. | Discoverable without a manual. |
| 12 | 2026-07-12 | Every action is also reachable through the GUI (button/menu), not only by hotkey. | Keyboard and mouse are equally first-class. |
| 11 | 2026-07-12 | Hotkeys are shown on the elements themselves (menu items, tooltips, popup entries). | Discoverability; no hidden keys. |
| 10 | 2026-07-12 | Every GUI element has a hotkey / is keyboard-reachable; full mnemonics. | Fully keyboard-driven, no mouse needed. |
| 9 | 2026-07-12 | `m` moves marked pictures to a configured destination via a popup with auto-assigned hotkeys; destinations user-configurable; multi-select via marks. | Fast triage of camera dumps into named folders. |
| 8 | 2026-07-12 | Follow the c-best-practices skill; conventions pinned in coding-conventions.md. | Consistent C style across the project. |
| 7 | 2026-07-12 | vi-style nav (`h`/`l` prev/next) plus cursor keys (`←`/`→`); `j`/`k` pan when zoomed. | vi users + cursor fallback. |
| 6 | 2026-07-12 | `D` permanently deletes; no undo.                                       | Fast path for obvious garbage.    |
| 5 | 2026-07-12 | `d` moves to a local `./Trash` folder, not the system trash; undoable.   | Trash travels with the shoot; easy to inspect/empty. |
| 4 | 2026-07-12 | Two views: thumbnail grid + large single-picture, in one window.         | Overview + detail, both keyboard-driven. |
| 3 | 2026-07-12 | Aim = quickly preview camera downloads and cull them.                    | Narrows scope to a culling viewer. |
| 2 | 2026-07-12 | Stack: C + GTK4, native Fedora/GNOME look.                               | User requirement.                 |
| 1 | 2026-07-12 | Planning docs only first; no implementation yet.                         | Get the design straight first.    |

Most open questions are now **decided** (see decisions #29–#38); remaining
working Leans and per-milestone details live in
[open-questions.md](open-questions.md).

---

## Documents index

- [README.md](README.md) — overview + working assumptions
- [goals-and-scope.md](goals-and-scope.md) — goals, non-goals, target workflow
- [architecture.md](architecture.md) — modules, data flow, concurrency
- [ui-and-interactions.md](ui-and-interactions.md) — views, keybindings, gestures, grid
- [tech-stack.md](tech-stack.md) — libraries, decode backends, deps
- [coding-conventions.md](coding-conventions.md) — C style (c-best-practices skill)
- [gegl.md](gegl.md) — GEGL quick-enhance & image-processing plan
- [roadmap.md](roadmap.md) — milestone detail
- [open-questions.md](open-questions.md) — undecided items
- `PLAN.md` — this file (tracker)

## How to use this file

- Flip milestone **Status** as work starts/finishes.
- Add a row to the **Decisions log** whenever something is settled (and move
  the matching item out of `open-questions.md`).
- Keep the elevator pitch and tables in sync with the detail docs; if they
  disagree, the detail docs are authoritative and this file gets updated.