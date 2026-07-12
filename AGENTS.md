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

## Two test tracks (both mandatory)

| Track | Where | What | Gate |
|-------|------|------|------|
| unit | `tests/test_<module>.c` | plain-C modules, no display | >=80% line coverage (gcov) |
| integration | `tests/integration/test_<flow>.c` | cross-module flows, real temp dirs, offscreen GTK | must be green, no coverage gate |

Shared helpers go in `tests/helpers/`; fixtures in `tests/fixtures/` (grow per
milestone). Integration suites land with the milestone that first makes a flow
possible — see `docs/IMPLEMENTATION.md` "Planned integration suites".

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