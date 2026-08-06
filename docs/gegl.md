# GEGL Integration

GEGL (Generic Graphics Library) — the data-flow, floating-point,
non-destructive image-processing framework behind GIMP, GNOME Photos, imgflo,
and iconographer. You chain *operations* (ops) into a graph; GEGL renders it.
Full op list: <https://gegl.org/operations/>.

ggaze uses GEGL for **quick, non-destructive photo enhance** plus a few bonus
capabilities. It is an **optional** dependency (meson `feature`); a minimal
build skips it and ggaze stays a plain fast viewer.

## Scope stance

ggaze is a *culling viewer*, not an editor. GEGL enhance is **opt-in and
non-destructive**: a preset is applied as a live preview to help judge a
keeper, and can be exported as a **copy** (the original file is never
modified). No layers, masks, undo stacks, or sidecar `.gegl` state. Full
editing remains a non-goal.

## The quick-enhance feature

- `a` → **enhance chooser**: a resizable preview window by default, or the same
  compact `GtkPopover` pattern as `m`/`e`/`!` when thumbnails are disabled. It
  lists presets with auto-assigned hotkeys (`1`, `2`, … then `0`, `a`-`z`),
  plus a `0 Original` reset row.
- Selecting a preset toggles a **GEGL graph** on/off and re-renders the
  viewer through the chain of every currently-enabled preset —
  non-destructively, and **layered**: multiple presets compose (e.g.
  Auto-fix + Sharpen at once). A hotkey/row click does not close the
  chooser, so combinations can be compared before dismissing it (`Esc`,
  re-press `a`, or the gallery's close button; outside click also works in
  compact popover mode). `0` (or `Esc` when the chooser is closed) discards the
  whole preview outright. Applying the chain runs off the GTK main thread (a
  GTask worker; last-write-wins if superseded before it finishes).
- By default `a` opens a separate, resizable gallery of bounded previews for
  all eight presets. It can be maximized to use the whole screen. The nine
  cards (Original + eight presets) sit in a fixed square-ish grid — 3×3 — whose
  cells **expand to fill the window exactly** at any size, so the cards grow
  and shrink with the window and never leave an unused strip beside them.
  That fit comes from GTK's own layout: the cells expand, and each card fills
  its cell. Nothing measures the window and pushes pixel sizes back into it.
  Do not reintroduce such a pass (a tick callback or size-allocate handler that
  sets size requests from the window's dimensions): it made every thumbnail
  visibly resize whenever the window size changed by even a pixel, and it fed
  the gallery's own size back into the measurement it reacted to.
  Each preset is shown independently on the current
  original and all previews are generated as one cancellable background batch.
  Preferences can disable thumbnails and restore the compact popover text list
  on slower systems.
- `s` (or menu *Save enhanced copy…*) writes the enhanced result to a new
  file, e.g. `IMG_0001-enhanced.jpg`, or `-enhanced-1.jpg`, `-2`, … if that
  name is taken (same collision suffixing as the move popup), via a GEGL
  saver. Original untouched. ggaze **never auto-saves** — the preview is a
  live overlay only, and `s` does not clear the dirty flag (pressing it again
  just exports another numbered copy of the same preview). Moving to
  another image (large-view keys/scroll **or** any grid/thumbnail
  selection), trashing/deleting/moving the current file, opening a
  different file/folder, or quitting (`q` **or** the window manager's close
  button / Alt+F4) with an un-exported preview prompts
  Save/Discard/Cancel; a Save whose export fails keeps the preview and does
  not proceed (it is not silently downgraded to Discard). At most one prompt is
  outstanding per window: a second request that the modal grab cannot swallow
  (Alt+F4, a single-instance D-Bus open, a drop) is queued in a single slot and
  retried through the same gate once the prompt is answered in favour of
  proceeding, or discarded with a status line on Cancel — except when the
  prompt's own answer closes the window (a quit), where the queued request is
  discarded instead of being run against a window that is going away. The
  prompt's answer always applies to the image it was raised for: `d`/`D`/`m`
  capture their targets at key-press time — for `D`/`m`, the two that consult
  the marks at all, that includes the marks-vs-current decision itself —
  because the slideshow timer and the folder's GFileMonitor keep running
  behind an input-only modal grab and can both move navigator.current and
  prune marks out from under the dialog. A captured
  target that has since been removed is refused with a status line rather than
  acted on. A native close (Alt+F4 / the WM button) arriving while the prompt
  is up is **refused** — the check is on any modal dialog the window owns (the
  Save prompt, or the `D` >1-mark delete confirm the prompt's own answer can
  go on to raise), not just on the dirty mask, precisely because the timer and
  the monitor above can clear that mask behind the dialog — and a close
  refused by the *prompt* is queued behind it, so
  answering it in favour of proceeding then closes the window (a close refused
  by the delete confirm is not queued — the queue belongs to the Save prompt,
  which is not up then, so routing that close through it would either do
  nothing at all or stack a second dialog on top of the confirm; answer the
  confirm and press the close again).
  Without that,
  the close reached `gtk_window_destroy()` with the dialog still up, which
  cannot dispose the window (the prompt holds a window ref) and so orphaned
  the dialog and everything it carried. If the window is *disposed* while the
  prompt is still up, the prompt is **cancelled** (a `GCancellable` handed to
  `gtk_alert_dialog_choose`) and resolves as Cancel: by then the preview and
  the engine a Save would need are already gone, so the only thing left to do
  is release everything the prompt was holding — without that cancel nothing
  could ever finish the dialog's `GTask` and its contexts leaked. That cancel
  is a safety property of dispose, not a shutdown fix: ggaze itself never
  forces a dispose (`GtkApplication`'s shutdown does not destroy windows), so
  the case it actually covers is a forced `g_object_run_dispose()`. What stays
  uncovered is a process **exiting** under the dialog (SIGTERM, session
  logout, `^C`), where no dispose runs at all and the prompt's contexts go
  down with the process. Toggling every preset back off, `0`, or
  `Esc` discards directly
  (no prompt). Slideshow auto-advance discards a dirty preview silently
  instead of blocking on an unanswerable prompt.
- Export format: defaults to the original extension (JPEG quality 95); a
  format/quality chooser and a lossless `jpegtran`/`exiftool` path are later.
- Presets are configurable: `enhance-presets` GSettings `a(ss)` — ordered
  `(name, gegl-graph)` pairs. Order = hotkey order. Ships with sensible
  built-in defaults; user can add/edit in Preferences (`,`).
- GEGL runs **only** when a preset is active or on export. The default fast
  decode path (GdkPixbuf / direct libs) is unchanged — the "fast" goal holds.
- Enhance is **not** applied during `h`/`l` scrubbing — only when settled on an
  image — so flipping stays instant.

### Built-in preset ideas (real GEGL ops)

Defaults ship with the first ~12; the rest are optional/artistic. Each is a
one-shot GEGL graph applied as a non-destructive preview (`a` popup, `s` to
save a copy).

| Preset        | Graph (ops)                                                |
|---------------|-------------------------------------------------------------|
| Auto-fix      | `gegl:stretch-contrast` → `gegl:color-enhance`             |
| Brightness    | `gegl:exposure` (or `gegl:brightness-contrast` brightness) |
| Contrast      | `gegl:brightness-contrast` (contrast +)                     |
| Saturation    | `gegl:saturation` (or `gegl:color-enhance`)                  |
| Warm          | `gegl:color-temperature` (warmer)                          |
| Cool          | `gegl:color-temperature` (cooler)                          |
| White balance | `gegl:color-enhance` → `gegl:stretch-contrast`            |
| Shadows       | `gegl:shadows-highlights` (lift shadows)                    |
| Highlights    | `gegl:shadows-highlights` (recover highlights)             |
| Levels        | `gegl:levels`                                              |
| Curves        | `gegl:contrast-curve` (gentle S-curve) / `gegl:curve`       |
| Sharpen       | `gegl:sharpen` (unsharp mask)                               |
| Denoise       | `gegl:noise-reduction` (or `gegl:bilateral-filter`)        |
| Clarity       | `gegl:high-pass-filter` blend (local contrast)            |
| Grayscale     | `gegl:color-to-grayscale` (or `gegl:mono-mixer`)           |
| Sepia         | `gegl:sepia`                                              |
| Vignette      | `gegl:vignette`                                           |
| Softglow      | `gegl:softglow`                                           |

**Tunable parameters:** every preset is a `gegl-graph` string in
`enhance-presets` (`a(ss)`), so the exact strength (saturation amount,
contrast level, exposure stops) is editable in Preferences or via
`gsettings` — no slider UI needed. The Curves preset uses a fixed curve shape
(also editable in the graph text); a full interactive curve editor is out of
scope — hand off to GIMP (`e`) for that. A later "fine adjust" mode could
expose ± nudging of the active preset's main parameter.

Graph strings are illustrative. Built-in presets are built programmatically
with `gegl_node_new_child`; user-authored presets can be stored as `gegl:gegl`
graph text and parsed with `gegl_node_new_from_xml`.

## Crop, straighten & rotate tools

Same non-destructive model as enhance (live preview graph + `s` to export a
copy), but interactive (crop/straighten) or one-shot (rotate):

- **`c` — crop** (`gegl:crop`): adjustable rectangle; aspect-ratio presets;
  mouse drag or keyboard (`h`/`l`/`j`/`k` move, `H`/`L`/`J`/`K` resize); `Enter`
  apply.
- **`R` — straighten** (`gegl:rotate`): drag a horizon line or nudge the
  angle (`h`/`l`, ±0.5°) with a grid overlay; optional auto-crop of rotated
  corners; `Enter` apply.
- **`[` / `]` — rotate 90°** (`gegl:rotate-on-center`): one-shot CCW/CW; repeat
  for 180°/270°. No overlay.
- Compose with enhance presets in the same graph. Large view only; GEGL
  required.

## Module

`enhancer.{c,h}` → `Enhancer` (+ `EnhancerPreset`). Plain-C, no GtkWidget,
unit-testable. The synchronous API (`enhancer_load`/`enhancer_apply_chain`/
`enhancer_buffer_to_texture`/`enhancer_export_chain`) is what the async
wrapper below composes; `window.c` calls the async form so GEGL's CPU-heavy
processing runs in a `GTask` worker, off the GTK main thread (tu0).

```c
const GPtrArray *enhancer_get_presets(Enhancer *p_e);
GeglBuffer      *enhancer_apply_chain(Enhancer *p_e, GeglBuffer *p_in,
                                      const GPtrArray *p_presets, guint8 u_mask,
                                      GError **p_err);
gboolean         enhancer_export_chain(Enhancer *p_e, GeglBuffer *p_in,
                                       const GPtrArray *p_presets, guint8 u_mask,
                                       GFile *p_out, GError **p_err);

/* Async: load + apply_chain + buffer_to_texture in a GTask worker. */
void       enhancer_apply_chain_async(Enhancer *p_e, GFile *p_file,
                                      const GPtrArray *p_presets, guint8 u_mask,
                                      GCancellable *p_cancel,
                                      GAsyncReadyCallback p_cb, gpointer p_data);
GdkTexture *enhancer_apply_chain_finish(GAsyncResult *p_res, GError **p_err);
```

Viewer integration: when a preset is active, the decoded pixels are imported
into a `GeglBuffer` (GEGL GdkPixbuf-source op / babl), the enhancer processes
it, and the output buffer is rendered back to a `GdkTexture` for display.
This path is heavier, so it is strictly on-demand and off the main thread;
the window compares a generation counter on completion so a superseded
request (a newer toggle, navigation, or discard) is dropped instead of
overwriting whatever the user is now looking at (last-write-wins).

## What else GEGL gives ggaze

- **Color management** — `gegl:icc-file-loader`, `gegl:lcms-from-profile`,
  `gegl:cast-color-space`, `gegl:convert-color-space`, plus ICC-aware savers.
  Closes open question **G** (color management) via babl + LCMS, no separate
  wiring.
- **Format load/save** — `gegl:jpg-load`/`-save`, `gegl:png-load`/`-save`,
  `gegl:tiff-load`/`-save`, `gegl:webp-load`/`-save`, `gegl:ppm-*`,
  `gegl:rgbe-*`, `gegl:gegl-buffer-load`/`-save`. Can augment GdkPixbuf on the
  enhance/export path (JXL/AVIF/HEIF still need their own libs).
- **Thumbnail generation** — `gegl:load` → `gegl:scale-size` → save; an
  alternative backend for the thumbnail cache.
- **Transforms** — **crop** (`gegl:crop`), **straighten** (`gegl:rotate`) and
  **rotate 90°** (`gegl:rotate-on-center`) are in scope as tools
  (`c`/`R`/`[`/`]`); lens correction (`gegl:lens-distortion`), red-eye
  (`gegl:red-eye-removal`), and `gegl:scale-ratio` remain later/maybe.
- **Tone mapping** — `gegl:reinhard-2005`, `gegl:mantiuk-2006`,
  `gegl:fattal-2002` (handy for linear/HDR-ish scenes).
- **Artistic** (optional/fun) — `gegl:vignette`, `gegl:sepia`, `gegl:softglow`,
  `gegl:oilify`, `gegl:cartoon`, `gegl:photocopy`.
- **Batch** — drive the `gegl` CLI via the `!` runner for bulk enhance/export
  without building it into ggaze's UI.

## Costs & trade-offs

- Heavier deps: `gegl`, `babl` (and transitively more). Gate behind a meson
  `feature` so minimal builds and the core culling flow don't pay for it.
- GEGL processing is slower than straight decode — keep it off the hot path.
- `gegl-gtk` (GeglGtkView) is separate and thinly maintained — **avoid**; keep
  the custom `GgazeViewer` widget and render GEGL output to `GdkTexture`.
- GEGL does **not** demosaic RAW — RAW stays out of scope.

## Dependencies (Fedora)

```
gegl-devel   babl-devel
```
