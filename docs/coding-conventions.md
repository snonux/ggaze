# Coding Conventions

ggaze follows the **c-best-practices** skill
(`~/.agents/skills/c-best-practices/SKILL.md`). That skill and its references
are authoritative; this page summarizes the conventions as they apply to
ggaze so contributors don't have to context-switch. When in doubt, the skill
wins.

## Source style

- **Indentation**: 3 spaces, no tabs.
- **Line length**: max 80 chars.
- **Braces**: K&R — opening brace on the same line as the statement/condition.
- **Pointer asterisk**: on the variable, not the type — `Token *p_token`, not
  `Token* p_token`.
- **Returns**: parenthesized — `return (p_token);`. Return type on its own
  line above the function name.
- **Comments**: `/* ... */` for blocks, `//` for single lines; put notes on
  their own line, not trailing.

## Naming

| Category          | Convention                  | ggaze examples                          |
|-------------------|-----------------------------|------------------------------------------|
| Types             | PascalCase                  | `GgazeWindow`, `GgazeViewer`, `GgazeGrid`, `Navigator`, `Loader`, `Thumbnail` |
| Functions         | `module_action` (snake_case)| `navigator_new`, `navigator_next`, `viewer_set_texture`, `grid_get_selected`, `trash_bin`, `mover_move`, `opener_launch`, `runner_run`, `enhancer_apply`, `clipboard_copy_image` |
| Variables         | `prefix_name` (type prefix) | `p_nav`, `p_texture`, `i_count`, `c_path`, `u_idx`, `b_wrapped` |
| Macros/constants  | UPPER_SNAKE_CASE            | `GGAZE_APP_ID`, `GGAZE_PREFETCH_N`, `NO_DEFAULT` |
| Enum values       | `MODULE_PREFIX_NAME`        | `GGAZE_SORT_NAME`, `GGAZE_SORT_TIME` |
| Callbacks         | `name_cb`                   | `loader_ready_cb`, `navigator_changed_cb` |
| Static/private fns| `_prefix_name`              | `_viewer_clamp_pan`, `_loader_pick_backend` |

Variable prefixes: `p_` pointer, `i_` int, `c_` char/string, `u_` unsigned,
`b_` bool. Use them consistently.

## Module layout (one module per file pair)

Every module is a `foo.h` + `foo.c` pair named after its main type. Related
types (element, iterator, state) live in the same pair, not split out.

```
navigator.h / navigator.c   → Navigator (+ NavigatorIterator if needed)
viewer.h    / viewer.c      → GgazeViewer
gridview.h  / gridview.c    → GgazeGrid
loader.h    / loader.c      → Loader (+ per-backend structs under loader/)
trash.h     / trash.c       → Trash
mover.h     / mover.c       → Mover (+ MoverDest)
opener.h     / opener.c       → Opener (+ OpenerProg)
runner.h     / runner.c       → Runner (+ RunnerScript)
enhancer.h   / enhancer.c     → Enhancer (+ EnhancerPreset)  [GEGL, optional]
clipboard.h / clipboard.c   → helpers (no type) — like settings
thumbnail.h / thumbnail.c   → Thumbnail
settings.h  / settings.c    → wraps GSettings (no custom type, just helpers)
```

### Lifecycle: `_new` / `_delete`

Every concrete type gets `Type *type_new(...)` and `void type_delete(Type *p)`.
Pair every `_new` with a `_delete`; never leave allocation unbalanced.

```c
Navigator*
navigator_new(GFile *p_dir, GgazeSort sort);

void
navigator_delete(Navigator *p_nav);
```

For types passed as `void*` to generic callbacks (e.g. `g_list_free_full`),
provide `void type_delete_cb(void *p_void)`.

### Iterators

When a module exposes traversal, follow
`TypeIterator *typeiterator_new(Container *p)`,
`void typeiterator_delete(TypeIterator *p)`,
`void *typeiterator_next(TypeIterator *p)`,
`_Bool typeiterator_has_next(TypeIterator *p)`.
The iterator type lives in the same file pair as its container.

### Accessors

`module_get_field(obj)`, `module_set_field(obj, val)` — macros for trivial
access, functions for non-trivial logic. Keep accessor macros side-effect-free.

## Headers

- **Header guards** UPPERCASE, derived from filename:
  `#ifndef NAVIGATOR_H` / `#define NAVIGATOR_H`.
- **Header order** inside a header: guard, includes, macros, enums, structs,
  `new`/`delete`, then the rest.
- **`.c` include order**: own header first, then system, then project headers.
  Optional forward declarations between own header and the rest.

## Errors

- **Fatal** errors via the project's `ERROR(...)` (abort with a message).
- **Recoverable** errors via a documented return: a `RETCODE`, a `gboolean`,
  or a `GError **` where GLib conventions apply (loader, file I/O).
- Decoders return `NULL` + set `GError **` on failure; the window shows a
  placeholder and advances — never crash on a bad file.

## Globals

Minimal. Any global is named UPPER or `_prefix` and lives in the module that
owns it. No module reaches into another's globals — go through accessors.

## GObject / GTK notes

GTK/GObject types (`GgazeWindow`, `GgazeViewer`, `GgazeGrid`) use `G_DEFINE_TYPE`
and GObject conventions (constructed/dispose) for the GObject side; the
`_new`/`_delete` skill pattern still applies to the plain-C modules
(`Navigator`, `Loader`, `Trash`, `Mover`, `Opener`, `Runner`, `Enhancer`, `Thumbnail`) that have no GObject parent.
Keep the two worlds clean: plain-C modules own no GtkWidget and are unit-testable
without a display.

## Testing

Each plain-C module pair ships a `tests/test_<module>.c` using GLib's `GTest`
framework (or a tiny harness), run via `meson test`. Aim for **≥80% line
coverage** (gcov/lcov: `meson setup -Db_coverage=true && meson test && ninja -C
build coverage`), gated in CI. GObject/GTK widgets get smoke tests only; the
logic lives in the plain-C modules so it's testable without a display.
Beyond tests, run the `auditing-code-quality` skill (C-adapted:
`c-best-practices` + `find-code-bugs` + `solid-principles` +
`beyond-solid-principles`) at milestone boundaries and track findings via
`agent-task-management`.