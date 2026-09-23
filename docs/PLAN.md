# ggaze — Project Plan (living document)

**GTK Gaze** — a small, fast, native image viewer for Fedora
Linux, written in C with GTK4. Its job: quickly preview a folder of pictures
downloaded from a camera, cull the rejects, move on.

This file is the running tracker. The detailed design lives in the sibling
docs; this page keeps the overview, the decisions log, and the status of each
milestone in one place. Update it as we go.

---

## Elevator pitch

`ggaze ~/Downloads/Camera/IMG_0001.jpg` opens the folder as a thumbnail grid,
`Enter` drops into the large view, `h`/`l` (or `←`/`→`) scrubs through the shoot, `i` shows
EXIF, `d` bins a reject into `.Trash` (undoable), `D` deletes it outright, `v` marks
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
- `.Trash` lives with the shoot; empty it via the menu or from a shell.
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

- `a` → side panel of enhance presets beside the image (auto-assigned hotkeys, preview thumbnails); applies a GEGL graph
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
| Packaging    | Fedora RPM + AppStream                  |
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
| M4  | Fullscreen + slideshow + info | not started | `f`, `s`, `i` EXIF overlay. Added later by task 0c2 (after M9; this row's status is not what changed): the `i` card also plots an RGB/luminance histogram of the displayed texture (`histogram.{c,h}` plain-C binner + `histogram-view.{c,h}` snapshot widget, gathered in the info overlay's GTask; plotted only when the texture provably belongs to the current file, and following the picture while the card is up) |
| M5  | Modern formats             | not started | JXL/AVIF/HEIF backends, animated GIF/WebP |
| M6  | Progressive low-res preview | not started | libjpeg-turbo early low-res scan |
| M7  | Thumbnail cache + grid view | not started | TMS cache + `gridview`, dim trashed items |
| M8  | Selection, move, open-external & scripts | not started | marks, `m`/`e`/`!` popups, `mover`/`opener`/`runner`, `Ctrl+c` clipboard, destinations+editors+scripts `a(ss)`, prefs |
| M9  | GEGL quick-enhance, crop/straighten/rotate (optional) | enhance done (tu0); crop/straighten/rotate done (wb2); ICC not started | `enhancer`, `a` side panel (layered presets) + hold-`Space` compare + non-destructive async preview + `s` save copy (no auto-save, prompt on navigate/trash/delete/move/open/quit) done; `c` crop / `R` straighten / `[` `]` rotate 90° tools on the same preview graph (`croprect`, `transform`, `tool-ctrl`; decision #35 order) done; ICC via GEGL still open |
| M10 | Polish & packaging         | not started | AppStream, RPM, man page, settings, keyboard-completeness audit, ≥80% coverage gate |

---

## Decisions log

Decisions made during planning. Newest first.

| # | Date       | Decision                                                                 | Rationale                         |
|---|------------|--------------------------------------------------------------------------|-----------------------------------|
| 42 | 2026-09-22 | **Info-card histogram (0c2)**: RGB + Rec.709 luminance, 64 bins, subsampled binning (≤512×512 samples), read from the texture's native memory without a copy; the card plots only a texture that viewload/texturecache (or the enhance override) vouch for as the current file's, else no plot. | Exposure judgement while culling at constant cost; last-write-wins must hold for the plot as it does for the picture. |
| 43 | 2026-09-22 | **Loader decode gate before gdk-pixbuf** (tb2): every loader entry point sniffs 64 bytes (`read_all`) and refuses an empty file and a file shorter than its signature's smallest complete file (`G_IO_ERROR_INVALID_DATA`, per-signature minimum table in `detect.c`, proven against the smallest real 1x1 file per format); **a build without the `jxl` feature refuses every JXL** (`G_IO_ERROR_NOT_SUPPORTED`, "JXL support is not built in") instead of handing it to gdk-pixbuf; `loader_peek_dimensions` sizes JPEG from its SOF header (an oversized or prefix-exceeding SOF fails closed without asking gdk-pixbuf, a zero side is never reported) and backend-claimed formats through the backend, `gdk_pixbuf_get_file_info` only as the last resort; the thumbnail cache read loads its entry once (bounded at `GGAZE_THUMB_ENTRY_MAX_BYTES`, 16 MiB), runs the same gate on those bytes (`loader_sniff_bytes`) and decodes a PNG entry only, never by path; the pixbuf backend re-gates the bytes it decodes and refuses whatever a specific backend of the build claims (`loader_sniff_bytes_for_fallback`, so a JXL never reaches gdk-pixbuf in any build), so the guarantee is exact except for the two path-taking gdk-pixbuf calls (documented). Thumbnail I/O failures now surface as `G_IO_ERROR`, not `G_FILE_ERROR`. `GGAZE_HAVE_ANY_BACKEND` compiles the backend-only paths out of the minimal build. | On Fedora >= 41 gdk-pixbuf forwards decodes to sandboxed glycin loaders with no cancel or timeout, and `glycin-jxl` waits forever on any garbage JXL (8 B, 60 B, container alike); the large view, thumbnail pool and info worker are uncancellable once it has the file -- and `~/.cache/thumbnails` is written by every TMS app, so a foreign entry is as untrusted as a source file. A hung worker that can never be cancelled is worse than a missing decode; AVIF/HEIF stay on the fallback because their loaders fail fast. See tech-stack.md "The decode gate". |
| 44 | 2026-09-22 | **Crop / straighten / rotate tools shipped as one `Transform` on the preset chain** (wb2): rotate 90° is `gegl:rotate` at origin (0,0) with the nearest sampler (an exact pixel permutation), not `gegl:rotate-on-center`; the straighten crops GEGL's padded extent to the analytic auto-crop / bounding-box size so the preview is exactly `transform_base_size`; the tool keys are **modal** (owned by the tool's capture-phase controller while active, listed as help-only rows under *Tools* in the `?` overlay) rather than global bindings; a tool switch or a quarter turn is refused while a tool is active; navigation or leaving the large view abandons a tool without applying it. Review round: the straighten auto-crop is inset 1 px per side and centred on the rotation centre (every kept pixel opaque, export == preview); the crop tool shows the base through a preview *override* so the committed crop keeps counting as work; "saved" is the exact (mask, transform) pair, so a state that returns to it is saved again; renders are coalesced (one in flight + one queued); a committed crop follows a straighten about the centre. Third round: the tools act on the *identity* of the rendered texture (`enhance_ctrl_is_current_render`), not on its size — the crop rectangle is drawn over and dragged on only the render of the current state, and a horizon drag is refused while a render is pending or `Space` holds the original; a crop pushed entirely outside its base is kept (title `crop (outside view)`, nothing cropped) rather than dropped, so nudge-away-and-back restores it unconditionally; every discard path (gate Discard, slideshow, failed render, `0` / Original card) ends a running tool first. Rounds 4–5: the original's identity is **learned at the window's single texture choke point** (`_show_texture` → `enhance_ctrl_texture_shown`, for every decoded texture the viewer is handed — never a progressive loader's low-res partial, which viewload now shows through its own `show_partial` op), so the remembered object is exactly the one on screen also after a same-file reload that decoded it again (the file touched, then a preset discarded); the draw / drag / `Enter` / horizon paths are pure identity compares with **no cache lookup and no stat** (a `touch` on the file cannot hide the overlay, and an identity learned once from the cache — round 4 — went stale after such a reload and refused every `Enter` for good); hold-`Space` shows that held reference rather than a cache entry; the one cache lookup left runs on a same-file rescan only, to tell a rewrite (entry evicted: forget, re-render an active preview from the new contents, and the reload's fresh decode re-teaches the identity and relays the crop tool out on the new base through `original_changed`) from the rescan a save's `-enhanced` copy causes (entry fresh: nothing); abandoning a tool (leaving the large view) == `Esc`; sixth round: an open (`o`, a drop, a single-instance activation) runs the same tool/controller choke point a navigation does (`window.c _open_rebuild`, before the view switch) because `navigator_set_current_file` is silent for a first-sorted file and a folder open places no cursor — a saved turn and an open crop tool used to survive into the new folder; a render's original size is told to the tool like a new original (`original_changed`), and the overlay hides a rectangle whose base is not the one the controller names. | The size the crop tool lays its rectangle out on must be the size the chain produces, and GEGL's rotation extent is not that size; modal keys are the only way `h`/`l` can both move a rectangle and navigate; refusing turns under a laid-out rectangle beats silently moving it; a crop must never be lost by opening the tool that edits it; a prompt for work already on disk is noise. |
| 47 | 2026-09-23 | **Texture-cache stamp to the nanosecond, plus inode** (fd2): an entry is stamped with `G_FILE_ATTRIBUTE_TIME_MODIFIED` + `_NSEC` (GLib 2.74, below the 2.76 GTK 4.10 needs; the usec attribute is the same value truncated), the byte count and `G_FILE_ATTRIBUTE_UNIX_INODE`, all from the one stat a `get` already made; a stamp that differs in any part evicts. A filesystem that keeps whole seconds reads 0 below the second at put and get alike, so the seconds + size check stands on its own there, as before. The enhance-flow subtest that pinned "a render lands before the decode" keeps forcing the cache hit deterministically -- it copies the whole stamp back (in-place write, mtime to the nanosecond) -- and a new subtest pins the fixed case: a same-second same-size rewrite under an open crop tool shows the new decode and re-bases the rectangle. | The old stamp (whole seconds + size) served a same-second same-size rewrite stale: the viewer kept the old decode while the enhance controller's base size, learned from the render, was the new one, and the crop overlay was laid out on the wrong picture. Sub-second mtime is what local filesystems record anyway; the inode costs nothing extra and tells an atomic replace (save to temp + rename over) whose mtime was copied back. |
| 28 | 2026-07-12 | Folder monitoring via `GFileMonitor` (GIO): external adds/deletes/moves refresh the grid live (debounced); removed current file falls back to nearest. | New shots from usbimport/etc. appear without manual reload. |
| 29 | 2026-07-12 | **UI toolkit: libadwaita** (was A). GNOME-native header bar/dark viewer/system theme; no theming overrides. | Native Fedora look per the gthumb-but-KISS direction. |
| 30 | 2026-07-12 | **App ID `org.buetow.ggaze`** (was B). | Matches buetow.org domain. |
| 31 | 2026-07-12 | **Custom viewer widget** (was L), not `GtkPicture`. | Cursor-centered zoom, pan clamp, hold-`Space` compare, tool overlays. |
| 32 | 2026-07-12 | **Single instance** (was E); new `open`/drop replaces current folder+image. | Standard GNOME behavior; no window sprawl. |
| 33 | 2026-07-12 | **Camera specifics** (was K): hide RAW sidecars by default (toggle to reveal); default sort = filename (EXIF capture-time as a menu option); import folder = just a path. | KISS first; culling-friendly grid; filename ≈ shot order. |
| 34 | 2026-07-12 | **GEGL integration** (was U): optional meson feature; built-in presets programmatic, user presets as `gegl-graph` text; apply only when settled (not during scrub); "enhanced" badge; export `<stem>-enhanced.<ext>` same dir, collision suffix `-1`; dirty-prompt fires on `d`/`D`/`m`; no gegl-gtk. | Keeps core fast; KISS preset UI; explicit save. |
| 35 | 2026-07-12 | **GEGL compose order** (was W): load → enhance(color) → rotate → straighten → crop → export; straighten auto-crop default on. | Crop the final framed image; remove rotated corners. |
| 36 | 2026-07-12 | **Enhance presets** (was X+#4): superseded by implementation (tu0) — presets are **layered** (any number toggle on/off independently and compose in the preview graph), not a single active/replace preset; `0`/"Original" reset clears all. Curves via `gegl:contrast-curve`; no fine-adjust nudging. Crop/straighten/rotate stack on top since wb2 (decision #44). | Layering turned out more useful than single-replace for comparing combinations; recorded here so the decision log matches what actually shipped. |
| 37 | 2026-07-12 | **Milestone Leans locked**: C GdkPixbuf-first; G color via GEGL/sRGB-else; H scroll=zoom + `pan-when-zoomed` mode; M GSettings `a(ss)` destinations; N move+suffix; O 1-9,0,a-z (cap 36); P one-level undo; Q marks path-based/survive re-sort/clear on trash; R raw cmd+%f+GSubprocess; S /bin/sh -c single-quote+rescan; T 64-512px ±32px custom bucket; V PNG+uri-list union provider; Y EXIF normalize-to-identity; Z folder→grid/file→parent/many→first; AA 250ms debounce nearest; F flat default; J RPM+AppStream first. | Working defaults confirmed at each milestone. |
| 40 | 2026-07-12 | Run the `auditing-code-quality` skill at each milestone + before release (C-adapted: `c-best-practices` + `find-code-bugs` + `solid-principles` + `beyond-solid-principles`, tracked via `agent-task-management`); fix all HIGH/MEDIUM findings. | Structured, well-factored project; catch defects + design smells early. |
| 39 | 2026-07-12 | **≥80% unit-test coverage** on plain-C modules (navigator/detect/thumbnail/mover/opener/runner/enhancer/trash/settings) via gcov/lcov with a CI coverage gate; GTK widgets get smoke tests. | Quality floor; refactor safely. |
| 38 | 2026-07-12 | **Gaps folded in**: mark count in header; CLI `--version`/`--help`; bulk `D` confirm >1; `scroll-behavior` = zoom/pan-when-zoomed/navigate; `Ctrl+c` copies the *displayed* image; export same ext + JPEG q95; `e` opens the *original* file; go-to-# not planned. | Plan now complete before implementation. |
| 41 | 2026-08-06 | **Thumbnail persistence stays freedesktop-TMS** (`~/.cache/thumbnails/`), not a hidden `.ggaze` folder beside the pictures, as ix0's report suggested. ix0 found the cache had *always* been written but never read back — `_load_cached()` asked gdk-pixbuf for `Thumb::MTime` while its PNG loader exposes tEXt keys as `tEXt::Thumb::MTime` — so the fix is a one-key read fix plus a `Thumb::URI` cross-check, not a new storage location. | The entries were already on disk and correct; a private folder would have thrown away the cache shared with Nautilus/gthumb, written into (possibly read-only or synced) picture folders, and left the same read bug in place. |
| 27 | 2026-07-12 | Open a folder arg (`ggaze dir/` → grid) and accept drag-and-drop of a file/folder onto the window; `o` dialog allows folders too. | Match gthumb flexibility; open anything from CLI, file manager, or drag. |
| 26 | 2026-07-12 | Honor EXIF Orientation on load (upright display); manual rotate/straighten compose on top. | Portrait/tilted camera shots display correctly without manual fix. |
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
| 5 | 2026-07-12 | `d` moves to a local `.Trash` folder, not the system trash; undoable.   | Trash travels with the shoot; easy to inspect/empty. |
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
- [open-questions.md](open-questions.md) — undecided items
- `PLAN.md` — this file (tracker)

## How to use this file

- Flip milestone **Status** as work starts/finishes.
- Add a row to the **Decisions log** whenever something is settled (and move
  the matching item out of `open-questions.md`).
- Keep the elevator pitch and tables in sync with the detail docs; if they
  disagree, the detail docs are authoritative and this file gets updated.