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

- `a` → **enhance popup** (same popover pattern as `m`/`e`/`!`): lists presets,
  each with an auto-assigned hotkey (`1`, `2`, … then `0`, `a`-`z`).
- Selecting a preset applies a **GEGL graph** to the current image and
  re-renders the viewer through it — non-destructively. Press it again (or
  `Esc`) to turn the preview off.
- `s` (or menu *Save enhanced copy…*) writes the enhanced result to a
  new file, e.g. `IMG_0001-enhanced.jpg`, via a GEGL saver. Original untouched.
  ggaze **never auto-saves** — the preview is a live overlay only. Moving to
  another image (or quitting) with an un-exported preview prompts
  Save/Discard/Cancel; `s` saves and clears dirty, `Esc`/re-press discards.
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
unit-testable. Runs GEGL in a `GTask` thread (off the UI thread).

```c
EnhancerPreset *enhancer_get_presets(void);            /* from GSettings */
GeglBuffer     *enhancer_apply(GeglBuffer *p_in, EnhancerPreset *p, GError **);
gboolean        enhancer_export(GeglBuffer *p_in, EnhancerPreset *p,
                                GFile *p_out, GError **);
```

Viewer integration: when a preset is active, the decoded pixels are imported
into a `GeglBuffer` (GEGL GdkPixbuf-source op / babl), the enhancer processes
it, and the output buffer is rendered back to a `GdkTexture` for display.
This path is heavier, so it is strictly on-demand.

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