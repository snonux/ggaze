# AGENTS.md — guide for coding agents working on ggaze

ggaze is a small, fast, native GTK4 image viewer in C for Fedora/GNOME. This
file is the entry point for a fresh-context agent: read it first, then the
linked `docs/`. Humans should read `README.md`.

## Load these skills as instructions (not as shell commands)

- `agent-task-management` — task workflow (use `~/go/bin/ask` subcommands only)
- `c-best-practices` — C style, authoritative for all C here
- `solid-principles` — OO/design review
- `beyond-solid-principles` — architecture review
- `find-code-bugs` — defect hunting (at audit milestones)

## Build / test / coverage

```sh
meson setup build
ninja -C build
meson test -C build
meson test -C build --suite unit
meson test -C build --suite integration

# coverage (>=80% on plain-C modules; gate warns until M10, then fails)
meson setup -Db_coverage=true build-cov && ninja -C build-cov
meson test -C build-cov && ninja -C build-cov coverage

# leak check (run after every major feature — see "Memory" below)
meson setup build-asan -Db_sanitize=address,undefined
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 G_DEBUG=gc-friendly \
  meson test -C build-asan
```

**Run integration lanes one at a time, or give each its own display.** The
`is_parallel : false` that keeps the popover suites from racing each other
serialises tests *within one* `meson test` invocation and cannot do more —
meson has no cross-process lock on the display. So `meson test -C build` and
`meson test -C build-asan` running concurrently against one X display
reintroduce the contention in full, and what you see is a fatal signal in an
unrelated suite that reads like a product crash. CI is safe (one invocation
per lane, each under its own `xvfb-run -a`); a developer running two of the
commands above at once is not. See `tests/meson.build`, "SCOPE OF THE
GUARANTEE", for the measured instances.

## Two test tracks (both mandatory)

| Track | Where | What | Gate |
|-------|------|------|------|
| unit | `tests/test_<module>.c` | plain-C modules, no display | >=80% line coverage (gcov) |
| integration | `tests/test_<flow>.c` | cross-module flows, real temp dirs, real GTK on a real display | must be green, no coverage gate |

Integration tests are **not** offscreen and there is no headless GTK backend
here: each `main()` returns 77 (meson SKIP) when `gtk_init_check()` fails, and
on X11 they really do map popover surfaces. CI (`.woodpecker/ci.yml`) runs the
track as `xvfb-run -a meson test -C build --suite integration` on `fedora:40`,
so **the X11 backend is the backend CI tests**. On a Wayland desktop
`xvfb-run` alone does not reproduce that — GDK still finds the Wayland socket,
and unsetting `WAYLAND_DISPLAY` is not enough because libwayland falls back to
`$XDG_RUNTIME_DIR/wayland-0`. Reproduce the CI lane with:

```sh
env -u WAYLAND_DISPLAY XDG_RUNTIME_DIR=$(mktemp -d) \
  xvfb-run -a meson test -C build --suite integration
```

**Use that form for day-to-day local runs, not just for reproducing CI.**
Several suites present real toplevels — `test_grid_select_gate`,
`test_settings_ui`, `test_delete_safety`, `test_enhance_flow` (5 sites) and
`/open_external/popup_really_maps` (`grep -rn gtk_window_present tests/` for
the current list) — so a run against your live session steals focus and pops
windows over whatever you are doing.
The command above renders into Xvfb instead: nothing reaches your screen. It
is also the *stronger* lane for popover coverage, since a popover on one of the
never-presented toplevels the subtests build maps only on X11 — so preferring
it locally costs nothing except GDK-Wayland-backend coverage. Run the
live-display lane before claiming a lane green on Wayland; otherwise stay in
Xvfb and keep your desktop.

Backends are not equivalent, so a green Wayland run is not evidence about CI:
a popover on a never-presented toplevel maps on X11 but not on Wayland, and
X11 seat grabs are display-global (see `tests/meson.build`, "suites that need
exclusive use of the display's seat grab").

Shared helpers go in `tests/helpers/`; fixtures in `tests/fixtures/` (grow per
milestone). Integration suites land with the milestone that first makes a flow
possible — see `docs/IMPLEMENTATION.md` "Planned integration suites".

**Optional realistic corpus:** `./sample-images/` (a local, NOT git-tracked
613-image camera dump: 601 JPEG + 12 PNG, sizes up to 46MB, varied EXIF) is
the realistic test corpus for integration tests and leak-check sessions. Any
test or scripted session that uses it MUST skip cleanly when the directory is
absent (so CI, which only has `tests/fixtures/`, stays green) and MUST NOT
mutate the corpus — work on temp copies. The committed `tests/fixtures/` are
the CI-portable baseline; `./sample-images/` is a local supplement.

## Conventions (enforced)

- `docs/coding-conventions.md` summarizes; the `c-best-practices` skill wins.
- 3-space indent, 80 cols, K&R braces, `*` on the variable (`Token *p_token`),
  parenthesized returns (`return (x);`), return type on its own line.
- One module per `foo.h` + `foo.c`; every concrete type gets `type_new` /
  `type_delete` (pair them); plain-C modules own no GtkWidget and are
  unit-testable without a display.
- `clang-format --dry-run --Werror` must be clean on every `*.c`/`*.h`
  (`.clang-format` matches the conventions). CI fails on a dirty tree. The
  config targets LLVM clang-format >=16 (Fedora 40 ships 18); it uses the
  cross-version key spellings (`UseTab`, `AlwaysBreakAfterReturnType`) so
  both the Fedora-40 CI toolchain and newer local builds accept it.
- Header guards uppercase from filename (`NAVIGATOR_H`). `.c` includes: own
  header first, blank line, system `<...>`, then project `"..."`.

## Optional features are OFF in the minimal CI lane

`gegl`, `jxl`, `avif`, `heif` are meson `feature`s (default `auto`). The
**minimal** CI lane forces all disabled and must stay green; the **gegl** lane
forces `gegl=enabled`. Never break the minimal build when adding an optional
backend — gate code with `GGAZE_HAVE_*` from `ggaze-config.h` and toast
"GEGL not built in" gracefully.

## Architecture invariants (do not violate)

- Only the main thread touches GTK widgets. Decode runs in `GTask` threads.
- **One active load per window.** Issuing a new load cancels the previous
  `GCancellable` and drops its result. The viewer only ever shows a texture
  whose path == `navigator.current` (**last-write-wins**).
- Bounded `GdkTexture` LRU (cap 4) to bound memory.
- Plain-C modules (`navigator`, `loader`, `detect`, `thumbnail`, `trash`,
  `mover`, `opener`, `runner`, `enhancer`, `info`, `texturecache`,
  `clipboard`) own no GtkWidget and are unit-tested standalone.

## Memory (C has no GC)

After **every major feature milestone**, run a `+leakcheck` pass before
starting the next feature: ASan build + `G_DEBUG=gc-friendly`, full test suite
(unit + integration), and a scripted elevator-pitch session; any leak blocks
the next milestone. See `docs/IMPLEMENTATION.md` "Memory-leak profiling".

## Module map

```
src/main.c            entry, CLI, GtkApplication
src/app.{c,h}         GApplication, actions, single-instance
src/window.{c,h}      GgazeWindow : GtkApplicationWindow (grid/large stack)
src/viewer.{c,h}      GgazeViewer : GtkWidget (large canvas, zoom/pan)
src/gridview.{c,h}    GgazeGrid (thumbnail overview)
src/shortcuts.{c,h}    keybinding -> GAction map
src/navigator.{c,h}   dir listing, sort/filter, marks, GFileMonitor
src/trash.{c,h}       ./Trash bin + permanent delete + undo
src/mover.{c,h}       configurable move destinations
src/opener.{c,h}      configurable external programs
src/runner.{c,h}      configurable shell scripts (async /bin/sh -c)
src/enhancer.{c,h}    optional GEGL quick-enhance + export copy
src/clipboard.{c,h}   copy image/URIs to GdkClipboard
src/thumbnail.{c,h}   freedesktop TMS cache
src/settings.{c,h}    GSettings wrapper (org.buetow.ggaze)
src/info.{c,h}        EXIF/dimensions gather (libexif)
src/texturecache.{c,h} bounded LRU of decoded GdkTextures
src/loader/loader.{c,h}   async load API
src/loader/detect.{c,h}   content-sniff format detection
src/loader/backends/       pixbuf.c jxl.c avif.c heif.c (jpeg.c M6)
```

## Design docs (read before touching a milestone)

`docs/PLAN.md` (tracker + decisions log), `docs/IMPLEMENTATION.md` (execution
plan + test-track tables + leak-check rules), `docs/architecture.md`,
`docs/ui-and-interactions.md`, `docs/tech-stack.md`, `docs/gegl.md`,
`docs/roadmap.md`, `docs/open-questions.md`. When docs disagree, the detail
docs are authoritative and `PLAN.md` gets updated.

## Task workflow

Tasks are scoped to this git project via `~/go/bin/ask`. Use only its
subcommands (`list`, `ready`, `add`, `info`, `start`, `done`, `annotate`,
`modify`, `dep`, ...). When a task passes tests + sub-agent review, commit and
push to `origin main`, then mark `ask done <id>` and progress to the next ready
task.