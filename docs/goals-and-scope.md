# Goals & Scope

## Goals

1. **Fast.** Visible image on screen within ~100 ms of launch for a typical
   camera JPEG. UI thread never blocks on decode. Flipping through a few
   hundred shots in a session must feel frictionless.
2. **Native.** Looks and behaves like a GNOME/Fedora app: libadwaita styling
   (decision pending), header bar, system themes, HiDPI, gestures.
3. **Keyboard and mouse, equally first-class.** Every GUI element has a
   corresponding hotkey (shown on the element) or is reachable through a
   keyboard-navigable menu; and conversely, every action that has a hotkey is
   also reachable through the GUI (a header-bar button or a menu item).
   Sensible vi-ish bindings so the `feh`/`nsxiv` audience feels at home.
4. **Two views, one window.** A thumbnail grid for overview and a large
   single-picture view for detail. Flip between them instantly; both are
   keyboard-driven. Minimal chrome in either.
5. **Directory-aware.** Given one file, navigate its siblings without a separate
   browse step. Prev/next, wrap, default sort by filename (EXIF capture-time as
   an option), filter by extension, **hide RAW sidecars by default** (toggle to
   reveal). The grid shows the whole folder at a glance.
6. **Culling-friendly deletion.** `d` bins the current picture into a local
   `./Trash` folder (recoverable, lives with the shoot); `D` deletes it
   permanently. The grid dim-marks trashed items so you can see progress.
7. **Triage by moving.** Mark any number of pictures and send them to a
   configured destination folder with `m` → a quick popup whose entries each
   carry an auto-assigned hotkey. Destinations are user-configurable (an
   ordered list of name → path pairs).
8. **Modern formats.** PNG, JPEG, GIF, WebP, plus JPEG XL / AVIF / HEIF
   (pluggable loaders).
9. **Good citizen.** `.desktop` file, MIME handler for `image/*`, AppStream
   metadata, Fedora RPM packaging, man page.
10. **Self-documenting UI.** The interface explains itself: hotkeys printed on
    their elements, tooltips on every control, a `?` shortcuts overlay,
    badges/counters/toasts that narrate state, and an info overlay for EXIF.
    A user should not need the manual to use it.
11. **Hand off to other tools.** `e` opens the current image in a configurable
    external program (GIMP, identify, another viewer, …) via a popup with
    auto-assigned hotkeys. ggaze stays open and responsive while the external
    tool runs.
12. **Run shell scripts.** `!` runs a configurable shell script (e.g.
    `~/scripts/usbimport`) asynchronously through `/bin/sh -c`, with `%f`/`%d`
    placeholders, and rescans the folder on completion.
13. **Quick non-destructive enhance.** `a` applies a configurable GEGL preset
    (auto-fix, brightness, contrast, saturation, sharpen, denoise, …) as a
    live preview; `s` exports an enhanced **copy**. ggaze **never auto-saves** — navigating away from an un-exported
    preview prompts Save/Discard/Cancel. The original is never modified. GEGL
    is optional; the core viewer stays fast without it.
14. **Copy to clipboard.** `Ctrl+c` copies the current image (pixels, PNG) —
    or, with marks, the marked files (URIs) — to the clipboard, so it can be
    pasted into other apps (Katogram, GIMP, file managers) like gthumb.
15. **Crop, straighten & rotate.** `c`, `R`, and `[`/`]` are non-destructive
    GEGL tools for cropping, leveling the horizon, and 90° rotation; `s`
    exports the result, the original is never modified. GEGL is optional.
16. **Compare before/after.** Hold `Space` to flash the original image;
    release to see the modified preview — to judge whether to `s` save. Large
    view; needs an active preview.
17. **Correct orientation.** Honor EXIF Orientation on load so portrait and
    tilted camera shots display upright automatically; manual rotate/straighten
    compose on top.
18. **Open anything.** Accept a file **or a folder** as the argument
    (`ggaze ~/Downloads/Camera/` opens the grid), and accept drag-and-drop of a
    file or folder onto the window to open it.
19. **Live folder monitoring.** Watch the current directory with `GFileMonitor`
    so external additions/deletions (e.g. `usbimport` writing files) show up
    without a manual reload.
://20. **Tested.** ≥80% unit-test coverage on the plain-C modules (navigator,
    detect, thumbnail, mover, opener, runner, enhancer, trash, settings),
    measured via gcov/lcov with a coverage gate so it doesn't regress.
21. **Audited quality.** Run the `auditing-code-quality` skill (adapted for C:
    `c-best-practices` + `find-code-bugs` + `solid-principles` +
    `beyond-solid-principles`, findings tracked via `agent-task-management`)
    at each milestone boundary and before release; fix all HIGH/MEDIUM findings.

## Non-goals (at least initially)

- **Full image editing** (layers, masks, crop-save, annotate). Quick
  non-destructive enhance + export-copy via GEGL is in scope (goal 13); deep
  editing is not.
- **RAW development.** May show embedded JPEG preview later, but no demosaic.
  GEGL does not demosaic either.
- **Library / catalog / albums / tags.** That is beets-for-photos, not this.
- **Cloud / network sources.** Local files only.
- **Batch processing in-app.** Bulk convert/resize is a job for the `!` runner
  (`gegl` CLI) or external tools, not ggaze's UI.
- **GEGL as a hard dependency.** It stays an optional feature; a minimal build
  is a plain fast viewer.

## Target user & workflow

Someone who just downloaded a shoot from their camera into a folder and wants
to cull it: flip through fast (bursts produce many near-identical frames),
glance at EXIF (shutter/aperture/ISO/timestamp) to pick the sharpest or best-timed
shot, trash the rejects, and move on to editing the keepers elsewhere.

Typical session: `ggaze ~/Downloads/Camera/IMG_0001.jpg` opens the folder as
a thumbnail grid, `Enter` jumps into the large view, hold `l` (or `→`) to scrub forward,
`d` to bin obvious misses into `./Trash` (or `D` to delete outright), `v` to
mark keepers, `m` then `1` to ship them to "irregular ninja", `i` when a
frame is borderline, `e` to open a keeper in GIMP, `Esc` back to the
grid, `q` to leave. No library, no
database, no sidecar state — just the folder, faster.