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

- `a` → **enhance side panel**: a narrow column of cards *beside* the large
  view, inside the main window (no second window, no popover). The image keeps
  the whole viewer; the cards are the choices: `0 Original` first, then one
  card per preset with its auto-assigned hotkey (`1`, `2`, …, capped at the
  mask's 8 slots). By default each card carries a bounded preview thumbnail
  of that preset applied *alone*, all generated as one cancellable background
  batch; Preferences can turn the thumbnails off, which leaves label-only
  cards (no batch at all) for slower systems. The thumbnails and the
  Original card ignore the geometric transform (crop / straighten / rotate):
  they are per-preset colour references, rendered once per image from the
  untransformed original, and re-rendering nine of them on every nudge
  would cost more than it tells — the large view is where the composition
  is judged. Documented as a deliberate limit, not an oversight.
- Selecting a preset toggles a **GEGL graph** on/off and re-renders the
  viewer through the chain of every currently-enabled preset —
  non-destructively, and **layered**: multiple presets compose (e.g.
  Auto-fix + Sharpen at once). The large view is the one place that shows
  the *combination*; the thumbnails stay per-preset references. A hotkey or
  card click does not close the panel, so combinations can be compared before
  dismissing it (`Esc` or re-press `a`; the preview stays). `0` (or the
  Original card) discards the whole preview outright while the panel is
  open, and so does `Esc` once the panel is closed. Applying the chain runs
  off the GTK main thread (a GTask worker; last-write-wins if superseded
  before it finishes).
- **Hold `Space`** to see the original; release to see the current edit. This
  works with or without the panel (the window binds it), and never touches
  the mask.
- The panel **stays open across navigation**: moving to the next image
  re-titles it and re-previews the new file, so a whole folder can be worked
  through with `a` pressed once. It is hidden (not closed) with the grid and
  comes back with the large view. Its cards are not keyboard-focusable on
  purpose: a focused button activates on Space, the compare key.
- **How to save is spelled out** on the panel: a state line reads
  *"No preset on"*, *"Unsaved preview — press s to save a copy"* or
  *"Saved as IMG_0001-enhanced.jpg"*, above a **Save copy** button (bound to
  the same action as `s`) and a key hint. When a preset is applied with the
  panel closed, a status line says it once per image: *hold Space to
  compare, s saves a copy, a shows the presets*.
- `s` / `Ctrl+S` (or the panel's *Save copy* button, or menu *Save enhanced
  copy…*) writes the enhanced result to a new file, e.g.
  `IMG_0001-enhanced.jpg`, or `-enhanced-1.jpg`, `-2`, … if that name is
  taken (same collision suffixing as the move popup), via a GEGL saver.
  Original untouched. ggaze **never auto-saves** — the preview is a live
  overlay only. A successful save marks the preview **saved**: it stays on
  screen (pressing `s` again exports another numbered copy), but it is no
  longer *dirty*, so moving on does not prompt for it. The saved thing is
  the exact (mask, transform) pair the export wrote: any change makes the
  preview unsaved, and landing back on that pair (a preset toggled off and
  on again, a crop/straighten tool cancelled back to it) makes it saved
  again — the file on disk is that state whichever way it was reached.
  Moving to another image (large-view keys/scroll **or** any
  grid/thumbnail selection), trashing/deleting/moving the current file,
  opening a different file/folder, or quitting (`q` **or** the window
  manager's close button / Alt+F4) with an **unsaved** preview prompts
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
copy), but interactive (crop/straighten) or one-shot (rotate). The state is
one plain-C value, `Transform` (`transform.h`: quarter turns, a clockwise
straighten angle in 0.5° steps within ±45°, the auto-crop flag, and a
`CropRect` in the coordinates of the image *after* the turn and straighten),
which the enhancer appends to the chain after the colour presets in decision
#35's order — the same chain for the async preview and the `s` export:

- **`[` / `]` — rotate 90°**: `gegl:rotate` with origin (0, 0) and the
  *nearest* sampler, which makes a quarter turn an exact pixel permutation
  (`gegl:rotate-on-center` about a non-integer centre would resample). One-
  shot, no overlay; repeat for 180°/270°; an applied crop turns with the
  image (`croprect_rotate_quarter`).
- **`R` — straighten**: `gegl:rotate` about the image centre, then a
  `gegl:crop` to the analytic size `transform_straighten_size` — the largest
  inscribed rectangle inset by `TRANSFORM_AUTOCROP_INSET` (1 px) on every
  side (auto-crop on) or the rotated bounding box (`transform_rotated_size`,
  auto-crop off) — centred on the rotation centre, not on the rotated
  node's bounding box. GEGL pads a rotation's extent asymmetrically for the
  sampler, and the inscribed rectangle touches the rotated edges at its
  corners where the default linear sampler blends with the transparent
  abyss: without the inset and the centring the auto-crop kept corner
  pixels with alpha 145–240, and a JPEG export (no alpha) differed from the
  preview there. Cropping to the analytic size is also what makes the
  preview exactly `transform_base_size` of the original, so the crop tool
  can lay its rectangle out on that size before the preview has rendered;
  `tests/test_enhancer.c` pins every auto-cropped pixel opaque at several
  angles and the PNG export byte-identical to the preview. With auto-crop
  off the corners are transparent in the preview and **black** in a JPEG
  export. Drag a horizon line or nudge (`h`/`l`, `-`/`+`) with a grid
  overlay; every change renders live, coalesced (at most one render in
  flight plus one queued for the latest state, `enhance-ctrl.c`), and a
  committed crop is re-anchored on the centre for the new base
  (`transform_rebase_crop`): the stored rectangle is only shifted, never
  cut — the cut is `transform_effective_crop`'s at render and export time —
  so a border-touching crop is not eroded by a straighten and back (0° → 5°
  → 0° gives back the exact rectangle; `tests/test_transform.c` pins it),
  and one pushed entirely outside the new base is kept too: its effective
  crop is empty, so the chain crops nothing (`_append_user_crop` skips it,
  in the preview and the export), the title says `crop (outside view)`
  (`transform_describe` with the original's size) and the status line says
  so, and the angle coming back applies it again — dropping it (an earlier
  round did) lost the rectangle one nudge too far. A horizon drag is
  accepted only on the render of the current state
  (`enhance_ctrl_is_current_render`; refused while pending or under a held
  `Space`), since its slope adds to the current angle. The inset's opacity
  guarantee needs an inscribed rectangle
  of at least 2 × inset + 1 = 3 px per side; below that
  `transform_straighten_size` floors at 1×1 inside the blend margin
  (documented, not refused: a 3×2 image has nothing to straighten).
- **`c` — crop**: `gegl:crop` of the rectangle, intersected with the base
  image and snapped to whole pixels (`transform_effective_crop`). Interactive
  overlay (`tool-ctrl.c` draws it on the viewer's overlay hook); mouse drag or
  keyboard; `Enter` applies. The overlay is drawn, drags are measured and
  `Enter` is accepted only while the texture on screen IS the render of
  the tool's state (`enhance_ctrl_is_current_render`: the last landed
  apply, or the cached original when nothing needs GEGL — an identity, not
  a size comparison, which could not tell 0° from 180° or a preset toggled
  under the tool from the base it replaced). While the tool is open the base is shown
  through a *preview override* (`enhance_ctrl_set_preview_transform`), not
  a commit: the committed crop keeps counting as work, so `s` in the tool
  exports it, navigation prompts for it, and a saved crop stays saved
  through `c` / `Esc`. Not a bug: with Denoise on, the pixels under the
  crop rectangle differ slightly from the same pixels of the un-cropped
  preview (measured ≤ 32/255 on 14 of 1200 px), because
  `gegl:noise-reduction`'s output is region-dependent and the crop changes
  the region GEGL computes; the preview and the export of one transform
  still match exactly.
- GEGL's positive `degrees` turn the image counter-clockwise on screen
  (y down; measured with a 3×2 probe on gegl 0.4.72), so the enhancer negates
  the Transform's clockwise angles.
- Large view only; GEGL required (without it the keys report "GEGL not
  built in"). Keys and behaviour: docs/ui-and-interactions.md "Crop,
  straighten & rotate tools".

## Module

`enhancer.{c,h}` → `Enhancer` (+ `EnhancerPreset`). Plain-C, no GtkWidget,
unit-testable. The synchronous API (`enhancer_load`/`enhancer_apply_chain`/
`enhancer_buffer_to_texture`/`enhancer_export_chain`) is what the async
wrapper below composes; `enhance-ctrl.c` calls the async form so GEGL's
CPU-heavy processing runs in a `GTask` worker, off the GTK main thread
(tu0). Every chain call takes the geometric `Transform` (nullable = identity)
beside the preset mask; `croprect.{c,h}` and `transform.{c,h}` are the
GEGL-free geometry those tools and the chain share, `tool-ctrl.{c,h}` the
interactive crop/straighten session over the viewer.

`enhancer_load` does NOT use `gegl:load` (which ignores EXIF Orientation):
it loads through ggaze's own orientation-aware loader (`loader_load`, every
backend honors Orientation per decision #26) and copies the upright RGBA8
pixels into a `GeglBuffer`, so the live preview and the A-menu per-preset
preview thumbnails render upright for portrait phone JPEGs etc.

```c
const GPtrArray *enhancer_get_presets(Enhancer *p_e);
GeglBuffer      *enhancer_apply_chain(GeglBuffer *p_in,
                                      const GPtrArray *p_presets, guint8 u_mask,
                                      const Transform *p_xf, GError **p_err);
gboolean         enhancer_export_chain(GeglBuffer *p_in,
                                       const GPtrArray *p_presets, guint8 u_mask,
                                       const Transform *p_xf, GFile *p_out,
                                       GError **p_err);

/* Async: load + apply_chain + buffer_to_texture in a GTask worker; finish
 * also reports the original's upright size (the crop tool's base). */
void       enhancer_apply_chain_async(GFile *p_file, const GPtrArray *p_presets,
                                      guint8 u_mask, const Transform *p_xf,
                                      GCancellable *p_cancel,
                                      GAsyncReadyCallback p_cb, gpointer p_data);
GdkTexture *enhancer_apply_chain_finish(GAsyncResult *p_res, gint *p_orig_w,
                                        gint *p_orig_h, GError **p_err);
```

Viewer integration: when a preset is active, the decoded pixels are imported
into a `GeglBuffer` (via the orientation-aware loader + babl; see
`enhancer_load` above), the enhancer processes
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
- **Transforms** — **crop** (`gegl:crop`), **straighten** and **rotate 90°**
  (both `gegl:rotate`, see above) ship as the `c`/`R`/`[`/`]` tools; lens
  correction (`gegl:lens-distortion`), red-eye (`gegl:red-eye-removal`), and
  `gegl:scale-ratio` remain later/maybe.
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
