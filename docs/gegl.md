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

- `a` → **the edit panel**: a narrow column *beside* the large view,
  inside the main window (no second window, no popover), and the one home
  of every edit (6i2), compact enough that the eight built-in presets fit
  a 1280x800 window without scrolling: a title row (*Edit*, close `a/Esc`),
  *Presets* (the row cards below), *Transform* (crop `c`, straighten `r`,
  the quarter turns `[` `]` as four icon buttons) and the save state with
  Save copy `s` (naming the file it writes) beside Revert `x`, each button
  showing its key from `shortcuts.c`'s table (docs/ui-and-interactions.md
  "Quick enhance"). The image keeps the whole viewer; the cards are the
  choices: a small, dim `Original` reference first (a picture, not a
  button), then one row card per preset — the eight built-ins, then the
  user's own `enhance-presets` in their order (ai2), 32 rows at most —
  the first eight with their hotkey (`1`–`8`), the rows after them by
  name alone (`j`/`k` + `Enter`; the list scrolls), highlighted and checked
  while it is on, showing its **strength** when it has a tunable number
  (8i2, below); the selected card (`j`/`k`) wears a ring and shows its
  strength slider. By default each card carries a small preview thumbnail
  of that preset applied *alone* at its default strength, all generated as one cancellable background
  batch; Preferences can turn the thumbnails off, which leaves label-only
  cards (no batch at all) for slower systems. The batch is cut from the
  live preview's source (below, "The live preview renders at display
  resolution": a 128 px copy made with it, no decode of its own) and
  runs **after** the preview: it waits while a preview render is
  pending, and a preview asked for while it runs pauses it (cancelled
  between cards, started again once the preview has landed). The
  thumbnails and the
  Original reference ignore the geometric transform (crop / straighten / rotate):
  they are per-preset colour references, rendered once per image from the
  untransformed original, and re-rendering every one of them on every nudge
  would cost more than it tells — the large view is where the composition
  is judged. Documented as a deliberate limit, not an oversight.
- Selecting a preset toggles a **GEGL graph** on/off and re-renders the
  viewer through the chain of every currently-enabled preset —
  non-destructively, and **layered**: multiple presets compose (e.g.
  Auto-fix + Sharpen at once). The large view is the one place that shows
  the *combination*; the thumbnails stay per-preset references. A hotkey or
  card click does not close the panel, so combinations can be compared before
  dismissing it (`Esc` or re-press `a`; the preview stays). The digits
  are the panel's keys: with it closed they do nothing. `x` (or *Revert
  all*) discards the whole preview outright while the panel is open; `Esc`
  and `0` never discard (6i2 -- `0` is zoom only, and `Esc` once the panel
  is closed goes on to the grid, the edit still on screen). Applying the
  chain runs
  off the GTK main thread (a GTask worker; last-write-wins if superseded
  before it finishes), on a scaled-down copy of the image (next section);
  a render still pending after 300 ms says so — a *Rendering…* pill over
  the view and *· rendering…* on the panel's state line — until it lands,
  fails or is superseded.
- **Hold `Space`** to see the original; release to see the current edit. This
  works with or without the panel (the window binds it), and never touches
  the mask. A preview rendered from a scaled source compares against that
  source itself (the same resolution and the same decode), so only the
  presets differ between the two.

### The live preview renders at display resolution (8l2, decision #53)

The preview used to run the chain on the **full-resolution** image, decode
included, on every key: on a 64 MP camera JPEG (9248x6936) a toggle left
the view unchanged for ~10 s with no sign of work — and the card
thumbnails, which did the same (their `gegl:scale-size` of the full decode
alone took ~5 s), queued ahead of it. Now:

- **A source per image.** The first render (or the panel's first card
  batch) builds a SOURCE (`enhancer-preview.c`): the image decoded through
  `enhancer_load`'s own paths — the managed decode when the file has a
  profile to manage, else ggaze's loader, **except that the loader path
  takes the decode the viewer already shows** instead of decoding the file
  again — and scaled down with GEGL's box-filtered scaled read to the
  view: fit-to-window x device scale x **1.5** (`preview-scale.h`:
  headroom for a zoom step or a larger window), never upscaled, 512 to
  4096 px on the long side. The controller keeps it while the image and
  its decode stay; a view that would show it magnified at fit (a window
  grown by more than the 1.5x head room, a finer device scale) builds a
  new one — the panel closing or opening does not. A 64 MP decode
  is scaled to ~1500x1100 in ~0.26 s; a Brightness render on that takes
  ~15 ms (the process's first chain pays ~0.25 s of babl setup once).
- **The render on it** scales the geometric transform onto the source
  (`transform_scale`: the crop from base to base; the turn, the angle and
  the auto-crop are resolution-free) and every preset's **pixel lengths**
  with it: the properties `preview-scale.c` lists — unsharp-mask's and
  high-pass's `std-dev`, gaussian-blur's `std-dev-x`/`-y`, the blur /
  median / snn / bilateral / lens / motion radii, pixelize's cell, the
  drop shadow's offsets — are multiplied by the source's scale (clamped to
  the property's range; an int rounds, so a 1 px box blur on a fifth-size
  source is none, which is what it looks like at that size). Sharpen then
  looks on the preview as it will on the export instead of five times too
  wide. **Documented deviations:** counts are not scaled —
  noise-reduction's and mean-curvature-blur's `iterations` (a 3x3 kernel
  iterated has no scaled equivalent: Denoise looks a little stronger on
  the preview than on the export); stretch-contrast's (Auto-fix's) min /
  max come from the averaged source, so it may stretch a hair more;
  the straighten's one-pixel auto-crop inset is one *source* pixel.
- **The texture stands for the image.** The render comes back small but
  tagged with the size the export will have (`logical-size.h`), and the
  viewer lays a texture out at that size: fit, 100 %, the pan clamp and
  the tool overlay's geometry stay in image pixels, so the crop rectangle,
  the straighten horizon, the base size and zoom `0` are what they were at
  full resolution (tests: `/enhance_flow/crop_on_a_scaled_preview`,
  `/enhance_flow/straighten_on_a_scaled_preview`).
- **Zooming past the preview's resolution** shows the preview magnified
  (1.5x fit is sharp; 100 % of a 64 MP photo is not) — no second render at
  the zoom: every landed render resets the view to fit anyway, and a
  full-resolution render at 100 % costs the seconds this change removed.
  The export is the full-resolution result; a lazy detail render at deep
  zoom is left open.
- **The export is unchanged**: `s` and the save gate's Save decode the file
  and run the chain at full resolution, pixel lengths unscaled —
  byte-identical to the export before this change
  (`/enhance_flow/export_is_full_resolution_and_unchanged`,
  `/enhancer_preview/full_resolution_chain_is_the_plain_graph`).
- **Copy** (`Ctrl+c`) of an active preview copies the texture on screen,
  i.e. the preview at its display resolution; `s` writes the full one.
- A rewrite of the file the texture cache's stamp cannot tell (same size,
  inode and nanosecond mtime) is now invisible to the preview as it is to
  the view — the render used to decode the file; `s` still reads it.

Measured in Xvfb (1280x800 window, key `2` after `a`, the time until the
view changes): a 64 MP JPEG **8.6 s → 94 ms** (key 0.5 s after `a`, cards
still rendering) and **6.6 s → 94 ms** (cards done); a 24 MP JPEG
**1.9 s → 120 ms** and **1.5 s → 92 ms**. The colour-managed case still
decodes the whole file through GEGL for its source — once per image.
- The panel **stays open across navigation**: moving to the next image
  re-titles it and re-previews the new file, so a whole folder can be worked
  through with `a` pressed once. It is hidden (not closed) with the grid and
  comes back with the large view. Its cards are not keyboard-focusable on
  purpose: a focused button activates on Space, the compare key.
- **How to save is spelled out** on the panel: a state line reads
  *"No edits yet"*, *"Unsaved edits — s saves a copy"* or
  *"Saved as IMG_0001-enhanced.jpg"*, above a **Save copy** button (bound to
  the same action as `s`) that names the file the next save writes
  (*as IMG_0001-enhanced.jpg*); the key-hint bar under the image lists the
  panel's keys.
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
  down with the process. Toggling every preset back off, or `x`
  (panel open), discards directly
  (no prompt). Slideshow auto-advance discards a dirty preview silently
  instead of blocking on an unanswerable prompt.
- Export format: defaults to the original extension (JPEG quality 95).
- Presets are configurable: `enhance-presets` GSettings `a(ss)` — ordered
  `(name, gegl-graph)` pairs, added / edited / moved / removed in
  Preferences (`,`). Each one is a **row of the edit panel** after the
  eight built-ins, in this order (ai2): selected with `j`/`k`, toggled with
  `Enter` or a click, tuned with `h`/`l` or its slider, part of the title,
  the saved / dirty rule, the export and undo like a built-in. Only the
  built-ins' rows carry digits (`1`–`8`). The layered mask is a `guint32`
  (bit *i* = row *i*), so the panel holds **32 rows: 24 user presets**
  (`GGAZE_ENHANCE_MAX_USER_PRESETS`) — Preferences refuses an Add at that
  many, and entries stored past it are listed as *Ignored* and never
  loaded. 32 is far more than a side panel is pleasant with (it scrolls
  past ~10 rows) and keeps the mask one machine word.
- **Editing the list while presets are on (ai2).** Every Preferences write
  delivers a new list. The edit refers to presets by row, so a changed
  list is *carried*, not reset: `enhancer_presets_map` matches each old
  row to a new one — the same name and graph (untouched, wherever it
  moved), else the same name (graph edited), else the same graph
  (renamed) — and `enhancer_state_remap` moves every on/off bit and
  strength with its preset (a kept strength clamped into an edited
  placeholder's range; a removed preset's state dropped; a new row off at
  its default). The same carry is applied to the saved state and, through
  `edit_history_remap`, to every undo snapshot (a step that only toggled
  a removed preset is dropped). What the chain renders is compared as text
  (`enhancer_chain_key`: each enabled graph with its strength written in,
  in row order): the preview re-renders only when that changed, and the
  saved copy stays saved only while its key is unchanged. A list equal to
  the old one (any other Preferences key moved) changes nothing at all.
  An export still running when the rows move wrote a state named in the
  old rows: it is reported, but not recorded as the saved state (a prompt
  too many at worst, never a lost edit).
- **Adjustable strength (8i2).** A preset graph may mark **one** number as
  tunable with a placeholder that also declares its range:
  `{s:DEFAULT:MIN..MAX}` (step: a twentieth of the range) or
  `{s:DEFAULT:MIN..MAX:STEP}`, e.g. `gegl:exposure
  exposure={s:0.5:-2..2:0.1}`. In the panel `h` / `l` (`Shift` five
  steps; the arrows stay navigation) move the selected preset's strength
  by a step and
  turn it on, and its slider does the same for the mouse; the title names
  a strength that is not the default (*Brightness +0.6*). The strength is
  part of the edit state — rendered, exported, compared for saved / dirty,
  undone as one step per run of presses or per drag — and every preset is
  back at its default on another image and after `x`. A graph without a
  placeholder is on / off only, exactly as before 8i2.

  *Why the range lives in the graph string.* The setting is `a(ss)`, one
  `(name, graph)` pair per preset. A third field would change the schema
  type (every stored list would stop loading) and a separate key would
  have to follow every rename, reorder and removal of a preset by hand.
  Inline, a preset stays one string, old strings parse unchanged, and the
  Preferences editor edits it as it always did — it only checks the
  placeholder and names what is wrong.

  *Rules* (`src/preset-strength.{c,h}`, plain C, unit-tested in every
  lane): numbers are C-locale decimals, parsed with `g_ascii_strtod` and
  written with `g_ascii_formatd`, so a comma-decimal `LC_NUMERIC` changes
  nothing; a value is written with the fewest decimals that represent the
  default, the range and the step (no trailing zeros, never `-0`), so a
  built-in at its default renders from the exact graph text it had before;
  values are clamped into the range and **canonical** (equal to the
  parse of their own text), so equal strengths compare equal and a step up
  and back down lands on the start; `h` / `l` step along the same grid
  through the default that a slider snaps onto (from an end of the range
  that is off the grid, the first step lands on the nearest grid point),
  so keys and slider agree on every value. Refused, with a
  message: a second placeholder, a stray brace, a name other than `s`, a
  missing field or a non-number (`1,5`, `nan`, ` 1`), a number beyond
  ±1e15 or needing more than 6 decimals (the default, an end, or the
  step — given, or implied as a twentieth of the range — could not be
  written exactly: `{s:0:0..1:1e-300}`), a range of more than 1e6 steps
  (a double cannot step 1e12 by 0.000001), an empty or inverted
  range, a default outside it, a step that is not above 0 or wider than
  the range. A malformed preset is kept (Preferences lists it) but is not
  tunable, and rendering it fails with the message instead of GEGL
  reading `{s:…` as 0.
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
`enhance-presets` (`a(ss)`), so any number in it is editable in
Preferences or via `gsettings`, and one of them per preset can be marked
`{s:…}` for the panel's `h` / `l` and slider (above). The Curves preset
uses a fixed curve shape (also editable in the graph text); a full
interactive curve editor is out of scope — hand off to GIMP (`e`) for
that.

The table above is ideas. **The built-in presets** that ship are graph
strings too (`BUILTINS[]` in `enhancer.c`), run by the same parser as a
user's: whitespace-separated `gegl:op` names each followed by
`prop=value` pairs. Each tunable default is the value the preset was
built with before 8i2 (then programmatically, `gegl_node_new_child`), so
the panel at its defaults renders the same pixels
(`tests/test_enhancer.c` compares them); each range lies inside the op's
own UI range (gegl 0.4.72):

| # | Preset     | Graph                                                   | Default · range · step |
|---|------------|---------------------------------------------------------|------------------------|
| 1 | Auto-fix   | `gegl:stretch-contrast`                                 | — (nothing to tune) |
| 2 | Brightness | `gegl:exposure exposure={s:0.5:-2..2:0.1}`              | 0.5 · −2..2 · 0.1 (stops) |
| 3 | Contrast   | `gegl:brightness-contrast contrast={s:1.3:0..2:0.05}`   | 1.3 · 0..2 · 0.05 |
| 4 | Saturation | `gegl:saturation scale={s:1.4:0..2:0.1}`                | 1.4 · 0..2 · 0.1 |
| 5 | Warm       | `gegl:color-enhance`                                    | — (the op takes no property) |
| 6 | Cool       | `gegl:exposure exposure={s:-0.3:-2..0:0.1}`             | −0.3 · −2..0 · 0.1 |
| 7 | Sharpen    | `gegl:unsharp-mask scale={s:0.5:0..3:0.1}`              | 0.5 (the op's default) · 0..3 · 0.1 |
| 8 | Denoise    | `gegl:noise-reduction iterations={s:4:1..12:1}`         | 4 (the op's default) · 1..12 · 1 |

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
- **`r` — straighten**: `gegl:rotate` about the image centre, then a
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
  apply, or — when nothing needs GEGL — the original as the viewer last
  showed it, learned at the window's texture choke point so a reload that
  decodes the file again refreshes it; an identity, not a size comparison,
  which could not tell 0° from 180° or a preset toggled under the tool from
  the base it replaced; no cache lookup on the draw or drag path). While the tool is open the base is shown
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

`enhancer_load` does NOT use `gegl:load` (which ignores EXIF Orientation).
A local PNG or JPEG whose embedded ICC profile is not sRGB decodes through
`gegl:png-load` / `gegl:jpg-load` for their ICC awareness (see "Color
management" below) with the Orientation applied by the enhancer itself
(pixbuf-util's permutation, the one every backend uses); every other file
(an untagged or sRGB-profiled PNG/JPEG included) loads through ggaze's own
orientation-aware loader
(`loader_load`, every backend honors Orientation per decision #26) and its
upright RGBA8 pixels are copied into an sRGB-tagged `GeglBuffer` — read
through a `GdkTextureDownloader` in an explicit `R8G8B8A8` layout since
xb2 (before it, `gdk_texture_download()` handed back premultiplied
`B8G8R8A8`, so every enhance preview and export had red and blue swapped
and premultiplied pixels: a visible change with xb2, see decision #45). Either way
the live preview and the per-preset preview thumbnails render upright for
portrait phone JPEGs etc.

```c
const GPtrArray *enhancer_get_presets(Enhancer *p_e);
GeglBuffer      *enhancer_apply_chain(GeglBuffer *p_in,
                                      const GPtrArray *p_presets, guint32 u_mask,
                                      const Transform *p_xf, GError **p_err);
gboolean         enhancer_export_chain(GeglBuffer *p_in,
                                       const GPtrArray *p_presets, guint32 u_mask,
                                       const Transform *p_xf, GFile *p_out,
                                       GError **p_err);

/* Async: load + apply_chain + buffer_to_texture in a GTask worker; finish
 * also reports the original's upright size (the crop tool's base) and
 * whether the decode was colour-managed. */
void       enhancer_apply_chain_async(GFile *p_file, const GPtrArray *p_presets,
                                      guint32 u_mask, const Transform *p_xf,
                                      GCancellable *p_cancel,
                                      GAsyncReadyCallback p_cb, gpointer p_data);
GdkTexture *enhancer_apply_chain_finish(GAsyncResult *p_res, gint *p_orig_w,
                                        gint *p_orig_h, gboolean *pb_managed,
                                        GError **p_err);
/* Async: the managed original for hold-Space, fetched on the first press. */
void        enhancer_managed_original_async(GFile *p_file,
                                            GCancellable *p_cancel,
                                            GAsyncReadyCallback p_cb,
                                            gpointer p_data);
GdkTexture *enhancer_managed_original_finish(GAsyncResult *p_res,
                                             GError **p_err);
```

Viewer integration: when a preset is active, the image is imported into a
`GeglBuffer` once per image (GEGL's ICC-aware loader for a PNG/JPEG with a
non-sRGB profile, else the decode the viewer shows, or the
orientation-aware loader; see `enhancer_load` above), scaled down to the
view (the preview SOURCE, "The live preview renders at display
resolution" above), the enhancer processes it, and the output buffer is
rendered back to a `GdkTexture` for display that stands for the image's
size. This path is heavier, so it is strictly on-demand and off the main
thread; the window compares a generation counter on completion so a
superseded request (a newer toggle, navigation, or discard) is dropped
instead of overwriting whatever the user is now looking at
(last-write-wins).

`enhancer-preview.c` (declared in `enhancer-gegl.h`, sharing enhancer.c's
load / chain / snapshot through `enhancer-private.h`) holds the source and
its render:

```c
void            enhancer_source_new_async(GFile *p_file,
                                          GdkTexture *p_decoded,
                                          const PreviewView *p_view,
                                          GCancellable *p_cancel,
                                          GAsyncReadyCallback p_cb,
                                          gpointer p_data);
EnhancerSource *enhancer_source_new_finish(GAsyncResult *p_res,
                                           GError **p_err);
void            enhancer_source_render_async(const EnhancerSource *p_src,
                                             const GPtrArray *p_presets,
                                             guint32 u_mask,
                                             const Transform *p_xf, ...);
void            enhancer_preview_thumbnails_async(const EnhancerSource *p_src,
                                                  const GPtrArray *p_presets,
                                                  ...);
```

## What else GEGL gives ggaze

- **Color management** — shipped on the enhance/export path (xb2, decision
  #45); see the section below. Closes open question **G** via babl, with no
  separate lcms2 wiring.
- **Format load/save** — `gegl:jpg-load`/`-save`, `gegl:png-load`/`-save`,
  `gegl:tiff-load`/`-save`, `gegl:webp-load`/`-save`, `gegl:ppm-*`,
  `gegl:rgbe-*`, `gegl:gegl-buffer-load`/`-save`. Can augment GdkPixbuf on the
  enhance/export path (JXL/AVIF/HEIF still need their own libs).
- **Transforms** — **crop** (`gegl:crop`), **straighten** and **rotate 90°**
  (both `gegl:rotate`, see above) ship as the `c`/`r`/`[`/`]` tools; lens
  correction (`gegl:lens-distortion`), red-eye (`gegl:red-eye-removal`), and
  `gegl:scale-ratio` remain later/maybe.
- **Tone mapping** — `gegl:reinhard-2005`, `gegl:mantiuk-2006`,
  `gegl:fattal-2002` (handy for linear/HDR-ish scenes).
- **Artistic** (optional/fun) — `gegl:vignette`, `gegl:sepia`, `gegl:softglow`,
  `gegl:oilify`, `gegl:cartoon`, `gegl:photocopy`.
- **Batch** — drive the `gegl` CLI via the `!` runner for bulk enhance/export
  without building it into ggaze's UI.

## Color management (decision #45)

Scope: the enhance preview, its hold-`Space` compare and the `s` export,
with GEGL. The plain large view (GdkPixbuf + the direct backends) is not
managed by ggaze: it shows what the decoder delivers and assumes sRGB (a
glycin desktop happens to convert PNG/JPEG to sRGB itself; fedora:40's
native loaders do not).

Op names, measured on gegl 0.4.72 / babl 0.1.128. The names in earlier
drafts of this page — `gegl:icc-file-loader`, `gegl:cast-color-space`,
`gegl:convert-color-space` — do not exist. What exists: `gegl:icc-load` /
`gegl:icc-save`, `gegl:cast-space`, `gegl:convert-space`,
`gegl:lcms-from-profile`, and the format loaders/savers. Of those, ggaze
needs only the PNG/JPEG loaders and savers:

1. **Decode** (`enhancer_load`). `gegl:png-load` / `gegl:jpg-load` read the
   embedded profile (PNG iCCP, JPEG APP2) and **tag** the buffer's babl
   format with the space babl builds from it (`babl_space_from_icc`); no
   pixel is converted. Only a local PNG/JPEG whose profile the managed path
   vouches for (below: "The profile is vetted before babl sees it") and
   babl parses to a space **other than sRGB** takes this path (`icc.c` reads
   the bytes, the enhancer makes the same `babl_space_from_icc` call GEGL's
   loader will):
   an untagged file, an iCCP/APP2 holding no profile, a profile babl cannot
   use, and an **sRGB profile** (babl folds an equivalent profile onto its
   own sRGB space) all keep the loader path — faster, and byte for byte
   what the same file decodes to with no profile at all (a test compares an
   sRGB-profiled PNG and JPEG with the same files stripped of their
   profile). That is NOT byte for byte what they decoded to on main before
   xb2: main's enhancer copied the loader's texture with
   `gdk_texture_download()`, i.e. premultiplied `B8G8R8A8`, into an
   `R'G'B'A u8` buffer, so every enhance preview and export had red and
   blue swapped (and premultiplied alpha). xb2 reads the texture in an
   explicit `R8G8B8A8` layout — a visible fix for every file on the enhance
   path, untagged ones included. A matrix/TRC RGB, a grey TRC and —
   through the lcms2 babl links against — a CMYK profile are managed, as
   long as the profile is for the image's number of colour components (a
   grey profile on an RGB JPEG, or an RGB one on a CMYK file, cannot
   describe the pixels and is declined: `IntactSize.u_comps`). The loader path (tag sRGB) is not used for a managed file
   because gdk-pixbuf may or may not have converted the pixels already, and
   tagging converted pixels would manage them twice.
2. **Working space.** The pixels are copied to `R'G'B'A u8` *in the image's
   own space* (`babl_format_with_space`), so presets, crop and rotation run
   in the source gamut and the export loses nothing. A CMYK or grey profile
   has no meaning in an RGB format, so such a buffer is converted to sRGB at
   this step (babl, colorimetric) and edited as sRGB. The EXIF Orientation
   is applied here (pixbuf-util's permutation), since GEGL's loaders do not.
3. **Preview** (`enhancer_buffer_to_texture`). It asks for `R'G'B'A u8`
   without a space — sRGB — so babl performs the image-space → sRGB
   conversion; that is what makes a wide-gamut preview look right on an sRGB
   display. For an sRGB buffer it is the plain copy it always was.
4. **Export.** `gegl:png-save` and `gegl:jpg-save` embed the buffer space's
   ICC profile — the bytes babl keeps for that space. That is the source's
   profile when babl made the space from it, but **not necessarily byte for
   byte**: babl answers a profile *equivalent* to one it already has (the
   same curves, primaries within 0.001 — `babl_space_match_trc_matrix`)
   with the earlier space, which keeps the earlier profile's bytes, and on
   its grey path it re-copies the latest profile onto a known grey space.
   So the copy carries *an equivalent profile*: the source's own when it
   was the first of its kind this process met (what the tests compare byte
   for byte), possibly an earlier file's otherwise — same colours, maybe
   another description. `gegl:webp-save` embeds nothing and needs no
   conversion node either: it reads the buffer as space-less `R'G'B'A u8`,
   i.e. sRGB, so babl converts on the way out (a test with
   `gegl:convert-space` hidden proves it), and an untagged WebP is what every
   viewer reads as sRGB. An sRGB buffer exports exactly as before.

**Hold-`Space` compares managed against managed.** The plain view is not
managed, so on a host whose decoder leaves the pixels alone (fedora:40's
native loaders) a profiled image's plain view and its enhance preview differ
in colour before any preset has changed a pixel. So hold-`Space` over a
managed preview does not put the plain decode back: it shows the file's
**identity chain through the same managed decode** (converted to sRGB like
the preview) — the compare shows exactly what the presets did. "Managed"
is what the render reports (`enhancer_apply_chain_finish`'s `pb_managed`:
the profile was applied), not the buffer's space, so the CMYK and grey
files — whose chain runs in sRGB — compare managed too.

The managed original is built **lazily**: on the first `Space` press over a
managed render the controller asks `enhancer_managed_original_async` for it
(a second decode in a worker through the render's managed path alone: a
decline — the file rewritten into one with nothing to manage, gone, past
the profile cap — is "no managed original", never a plain decode the
viewer already has) and shows the
plain original meanwhile, swapping the managed one in when it lands if
`Space` is still held; later presses show it at once. It is not built with
every render because it is a second full-size texture — `w × h × 4` bytes,
about 100 MB at 24 MP and 200 MB at 50 MP — held by the controller outside
the texture cache's cap, which most enhance sessions never look at. It is
dropped on discard / when nothing is left to render (`x`, the last
preset off), on navigation, on a rewrite of the file, and whenever the
controller learns a new decode of the original in place of a known one (a
rescan that finds a newer cached decode, a reload the viewer shows): it may
come from contents that are gone, and the next press fetches it again.
Released, navigated away, discarded or rewritten while the fetch is in
flight, the landing puts nothing up (`tests/test_enhance_flow.c`,
`icc_*_mid_fetch`). While it is up the
`i` card plots it (it stands for the current file's original at the
window's texture choke point, `enhance_ctrl_override_texture`). For every
unmanaged file, hold-`Space` shows the plain decode as before. Turning the preview on or off (a preset, `x`)
still switches between the plain view and the managed preview, so on
such a host that switch can show a colour shift the preset did not cause —
the managed side is the correct one.

**The managed path never gives a verdict of its own.** It either yields a
buffer it can vouch for or *declines*, and a declined file takes the loader
path, which decodes it — or refuses it with its usual error — exactly as
before xb2. "Vouch or decline" covers the embedded profile as much as the
pixel data: babl and GEGL's loaders fail badly where the loader fails
cleanly, so a file reaches them only through:

- **the profile check, before babl sees a byte** (xb2 review 3). babl
  0.1.128's `babl_space_from_icc` trusts the tag data it reads: a `curv`
  count is a loop bound and an allocation size never held against the tag
  (a gTRC count of `0x01000000` in a 520-byte profile segfaults it; a count
  that goes negative as an `int` makes `babl_fatal` exit the process), and
  its tag lookup loops over a count taken from the file. Pressing `i` on
  such a file used to kill the viewer, since the card asks the same
  question. `icc_profile_is_sane` (`icc.c`, plain C) now vouches first: the
  header's size field is the byte count (babl insists too), at most 1024
  tags whose table fits, every tag after the table and inside the profile
  with room for its type header (tags may share data, as r/g/bTRC often
  do), and every tag babl reads of the type and size it reads it as —
  `XYZ ` tags (r/g/bXYZ, wtpt) of ≥ 20 bytes, `chrm` ≥ 36, `chad` ≥ 44, a
  `curv` TRC with `12 + 2·count ≤ size`, a `para` TRC with all its
  parameters; a TRC of any other type is refused (babl would read it as a
  `curv` count). Review 4 found well-formed curves that crash or abort babl
  all the same, each reproduced against 0.1.128, and the check holds them
  back too:
  - a `para` whose **reserved word is not zero**: babl tells `para` from
    `curv` with `strcmp(data, "para")`, so a nonzero byte 4 makes it a
    `curv` whose "count" is the function type and padding (up to 0x4FFFF
    points), and the space's profile copy below overruns (SIGSEGV). The
    reserved word must be zero (a `curv` needs no such rule: babl reads
    anything that is not `para` as one);
  - **long curves**: babl writes the profile of every new RGB space into a
    `char icc[65536]` on its stack (`babl_space_to_icc_rgb`) — ~560 bytes of
    header and tags, and each curve at `12 + 2·points` bytes, once when r,
    g and b are the same babl curve, three times otherwise — and copies out
    what it wrote past 64 KiB too (a shared 65536-point curve: SIGSEGV;
    32768 points or three distinct 11000-point curves overrun silently). A
    curve may have at most **4096** points (`ICC_MAX_CURVE_POINTS`), the
    most real profiles use: three of them fit with room to spare, whatever
    they share (a static assert in `icc.c` keeps the arithmetic);
  - **`para` break points**: babl approximates a type 3 / 4 curve from x0 =
    `d` and x0 = `c·d` and **asserts** `0 ≤ x0 < 254.5/255` for both
    (`babl_polynomial_approximate_gamma`): `d = 1.0`, `d < 0`, a negative
    `c` aborted the process. Both must lie in `[0, 0.998)` (computed in
    float, as babl does; s15Fixed16 values are always finite);
  - **`para` types 1 and 2** (the CIE 122 / IEC 61966-3 forms) are refused:
    `babl_trc_formula_cie` packs four parameters into `float[4]` and babl
    then reads a fifth as the curve's x0 — an out-of-bounds read whose
    garbage decides the assertion above;
  - **`para` parameters out of any real range** (review 5): babl names a
    formula curve after its seven parameters (`"%i.%06i …"`), a space after
    its primaries and three curves' names, and each of the space's formats
    `"<encoding>-<space>"` in a 256-byte buffer. A curve with g = −32767
    (the review's `[-32767, -1.31, -1.7, 0.768, 0.038, 0.604, 1.10]`) named
    its space in 238 characters, so format names were cut short, two
    formats shared a name, and babl's fish search between them **spun
    forever** inside an uncancellable GEGL decode. g and a must lie in
    (0, 10], every other parameter within ±10 (`ICC_PARA_MAX`) — real curves
    use a gamma of 1.8–2.6, a ≈ 1, offsets under 0.1 — and the enhancer
    also checks the name babl built (below);
  - **curves babl cannot invert** (review 5): babl builds the conversion to
    each format by searching candidate paths until one converts its test
    pixels closely enough; for a flat or falling curve none does, the search
    runs into its deepest candidates, and one of those
    (`babl_conversion_planar_process`) overflows a buffer — glibc's fortify
    check **aborted the JPEG export** (`s`) of files whose profile had a
    constant `curv` (`[0, 0]`, `[30000, 30000]`, `[65535, 65535]`), a table
    of 1139 points with one spike, or a `para` type 4 with c = d = 0 and
    e, f > 1 (never inside [0, 1]). So every tone curve must be shaped like
    one: sampled over [0, 1] (a table at its points, a formula at 1024) and
    clamped to [0, 1], it never falls, rises by at least half the output
    range from first to last sample, and is flat (steps under half a u16
    step) over less than half its domain. That also refuses a zero or huge
    gamma and a `para` that falls at its break point (babl would have taken
    both). Every corpus curve (4096- and 1024-point tables, sRGB `para`
    curves, a u8Fixed8 gamma of 2.2) and Rec. 709's pass with room to spare.
  Unit-tested with mutated profiles, profiles built for each case
  (`tests/helpers/icc_build.c`) and a seeded 20 000-profile fuzz
  (`tests/test_icc.c`, also under ASan); the 106 profiled files of the
  local corpus and the system's colord profiles all pass. The babl
  hazards the bytes alone do not show are handled in `enhancer.c`:
  - a **CMYK profile** goes to LCMS inside babl, which keeps whatever
    transform LCMS returns — NULL included — and crashes on the first
    conversion through it (a one-byte change to `cmyk-icc.jpg`'s lut8
    header did it). So the enhancer builds that transform (CMYKA double →
    babl's scRGB, relative colorimetric, black-point compensation) with
    lcms2 first and declines a profile it fails for. Only that direction is
    required: the reverse is used only to convert INTO the CMYK space,
    which never happens (the chain runs in sRGB), and an input-only
    profile cannot give it. lcms2 is babl's own dependency (`babl-devel`
    requires it), so a GEGL build has it already;
  - a profile babl **declines outright** — a class other than display /
    input, a PCS other than XYZ, both A2B0 and B2A0, no curves or no
    primaries, Argyll's swapped matrix beside a CLUT — is never handed to
    it (`icc_babl_kind`, mirroring `babl_space_from_icc`'s own checks): some
    of those checks come after babl has parsed and kept the curves;
  - babl's **space and tone-curve tables** are fixed arrays of 100 entries,
    never freed (babl fills ~20 spaces and ~5 curves itself), and a full
    space table makes the next `babl_space_from_icc` dereference NULL. babl
    parses rTRC, gTRC, bTRC **and** kTRC of every profile it gets past its
    early checks, whatever the colour space, so a profile may add a space
    and four curves. At most 16 profiles per process
    (`GGAZE_ENHANCER_MAX_PROFILES`) that may grow those tables are handed to
    babl: one costs its slot when babl is asked, whatever the answer (a
    declined one may have added curves), **unless** babl answered with a
    space it already had (sRGB, or an earlier slot's) and the profile
    carries no curve that space does not use — a kTRC on an RGB profile, an
    rTRC on a grey one, costs a slot even then. So what the enhancer hands
    babl leaves the tables under 36 spaces and 69 curves, while camera
    files whose sRGB profiles differ only in their bytes cost nothing.
    GEGL's own loaders are held to the same bound by seeing only files
    whose vetted profile they will actually use. `gegl:png-load` (0.4.58
    and 0.4.72 alike) takes the iCCP when libpng kept one — and then
    nothing else, even if babl declines it — else sRGB for an sRGB chunk,
    else a space it builds from gAMA / cHRM, and libpng drops iCCPs babl
    takes — a rendering intent ≥ 0xFFFF, a v4 profile whose length is no
    multiple of 4, one over libpng's length limit (review 5: 110 such
    PNGs, each with its own gAMA, filled babl's tables past the cap and
    the next profile crashed `babl_space_from_icc`). So a PNG is managed
    only when libpng 1.6's fatal iCCP rules hold — one iCCP, before PLTE,
    its CRC right, a header passing `png_icc_check_length` / `_header` /
    `_tag_table` (1.6.40 on fedora:40 and 1.6.58 agree on those), at most
    8 000 000 bytes (upstream's `PNG_USER_CHUNK_MALLOC_MAX`; Fedora 44
    builds a larger one) — **and** nothing beside it makes libpng 1.6.40
    drop it: no sRGB chunk (stricter than needed, review 7: in 1.6.40 an
    sRGB before the iCCP makes it refuse the iCCP and invalidate the
    colour space, while one after a non-sRGB iCCP is taken beside it and
    the iCCP kept; 1.6.58 keeps both — a file tagged sRGB is not worth
    managing anyway; likewise a second iCCP, which 1.6.40 lets replace
    the first and 1.6.58 drops as a duplicate, is refused), and any
    gAMA / cHRM single, before PLTE, of its exact length, its CRC right,
    and of values 1.6.40 takes — a gamma of 16 to 625 000 000, and
    chromaticities its fixed-point xy → XYZ → xy round trip passes, which
    `icc.c` ports (a duplicate or rejected one invalidates the colour
    space there too; 1.6.58 checks neither at read time). Review 6:
    until then any gAMA / cHRM was refused, but `gegl:png-save` writes both
    beside the iCCP (so do GIMP and ImageMagick), so ggaze's own PNG
    exports reloaded unmanaged — on gdk-pixbuf 2.42 as plain sRGB, and a
    re-export baked the shift in. A gAMA / cHRM that passes never reaches
    babl: the kept iCCP wins in `gegl_png_space`. Checked against the real
    libraries: a generator of random chunk layouts, gAMA values and
    chromaticities read with libpng exactly as `gegl:png-load` reads them
    (benign errors on, `PNG_SKIP_sRGB_CHECK_PROFILE`) found no file ggaze
    vouches for whose iCCP libpng 1.6.40 or 1.6.58 did not keep, byte for
    byte (`icc_png_applied_profile`), besides files libpng refuses outright
    (a damaged or second PLTE: the load fails, and the completeness walk
    and the extent check hand those to the loader), and no in-range cHRM or
    gAMA on which the port and 1.6.40 disagree. Any other PNG takes the loader
    path, which never gets near babl. The bound then has one way past it,
    the file-swap window below;
  - a space babl builds may be **unusable by name** (review 5), and babl
    and GEGL find a space's formats by its name. Too long a name gets the
    formats cut short (above: a 238-character one spun); a name another
    space already has makes `babl_format_with_space` hand back the FIRST
    space's formats, so the file silently converts with the wrong profile
    — and babl names spaces only partly by content: every table curve is
    `lut-trc`, so all grey table-curve spaces are `space-gray-lut-trc` and
    RGB table-curve spaces of the same primaries share a name too. The
    enhancer declines a space whose name is longer than babl's format
    names leave room for — 254 characters, less the dash and the longest
    encoding in babl's format table, read from babl
    (`enhancer_max_space_name`: 229 with babl's and GEGL's own formats,
    the longest being `CIE LCH(ab) alpha double`; review 7, was a fixed
    220 that declined smooth type-4 curves on babl 0.1.112). It is read
    once, by `enhancer_babl_ready()` right after `gegl_init()` on the main
    thread (app.c), not per verdict (review 8): babl walks that table
    without its own lock while a GEGL worker converting in a new space
    inserts formats into it, and formats registered later reuse existing
    encodings, so no verdict depends on when it was asked. A Rec. 709
    `para` curve on all three channels names its space in ~220 characters
    with the tests' primaries, a few more in babl 0.1.112 (two spaces
    after a curve's gamma) than in 0.1.128, and the exact length depends
    on which curves babl already holds (next item: in 0.1.112 a formula
    curve is named by the first profile that made it), so the tests put
    the limit at babl's own length for a name, through a seam, rather
    than rely on a fixed curve's name. It also declines a space for which
    `babl_space(name)` is another space. babl has kept that space by
    then, so it costs its slot. (Primaries
    that differ only past the four printed decimals do not collide: babl
    matches such a profile to the earlier space itself.)
    A space babl answers with may also convert through **another profile's
    curve** (review 7): babl < 0.1.114 — fedora:40 ships 0.1.112 — keeps
    one formula curve per type and gamma (`babl_trc_new` compares type,
    table size and gamma, not the other `para` parameters), so a Rec. 709
    type 3 profile asked after a type 4 one with the same gamma and
    e = f = 0.005 got the type 4 curve, and with the same primaries its
    very space (black converted to 0.005 linear). After the name checks,
    the enhancer converts 64 inputs evenly over [0, 1] plus 1/1024 either
    side of each channel's own knee (`d` of a type 3 / 4 `para`,
    `icc_formula_curve_knee`) through the space, `R'G'B' float` →
    `RGB float` (grey: `Y' float` → `Y float`), and compares each channel
    with the profile's own formula curve (`icc_formula_curve_at`, which
    mirrors babl's deliberate swaps: a gamma within 0.01 of 1 is linear, a
    `para` within 0.01 of sRGB's parameters is babl's sRGB curve); off by
    more than 4e-4 linear plus 0.1 % of the value at any of those inputs,
    the profile is declined at the cost of its slot. Review 7's check (five
    inputs, half an 8-bit step of linear light) missed curves differing
    only between two knees and offsets under 0.002 — up to ~16 and ~6
    steps of an 8-bit sRGB display near black (review 8). The tolerance is
    measured, identically on babl 0.1.112 and 0.1.128: babl's own
    approximation of a real formula curve is off by at most 3.1e-4 (a type
    3 `para` of `d` 0 and gamma 1.1–1.2, at black only) and by 7e-5 or less
    elsewhere; 4e-4 is about one 8-bit sRGB display step at black and finer
    above, so a swapped curve closer than that at the probes still passes:
    off by about that at them, and by up to ~3 steps between black and the
    first even probe (1/63), at input codes of about 1 to 5, where only the
    profile's own knee is probed (review 9). A curve babl itself approximates worse — a to-linear gamma
    under 1 with `d` 0, 0.0053 off at black for 0.45 — is declined on any
    babl. Table
    curves are not checked — babl tells tables apart byte for byte and
    swaps in a formula for a table within its own tolerance of it (up to
    0.015 for linear) by design — nor is babl's own sRGB answer, which is
    never managed. Verdicts are kept by SHA-256 — for
    good for the slots, up to 64 slot-free ones
    (`GGAZE_ENHANCER_MAX_FREE_VERDICTS`, oldest dropped: asking babl again
    about one adds nothing) — so a file seen again costs a checksum and
    the table is bounded. Past the cap a new profile is declined, and its
    file decodes on the loader path, sRGB. The `i` card asks through the
    same table as the render: whichever meets a profile first pays its
    slot, once — the cap counts distinct profiles, not files or paths. The
    corpus has 15 distinct profiles, most of them sRGB, and costs 2 slots.
  A fuzz of this whole gate together with babl runs in subprocesses (each
  round with fresh tables, until its slots are spent) in
  `tests/test_enhancer_icc.c`: the seeds include `para` 0 / 3 / 4 and long
  `curv` curves, half the edits land inside a tag (a parameter at one of
  babl's edges or bounds, the function type, a reserved byte, a count, a
  table made constant or spiked), and every profile that gets through has
  its space built, pixels converted through it both ways in u8 and in
  float (values past both ends of [0, 1] included; float into the image's
  space is the direction babl aborted in) and its profile copied out. The
  rounds set `BABL_PATH_LENGTH=1`, which makes babl convert through its
  reference fish instead of timing candidate paths (~1 s per new space) —
  and so also skips the deep path search where the review-5 abort lived;
  that hazard is pinned outside the fuzz with babl's full search (a JPEG
  export of each uninvertible curve, `uninvertible_curves_are_declined`).
  The review-5 cases that spend slots run in fresh subprocesses, so they
  do not eat into the cap the later cases and the corpus count on;

- the loader's own sniff (`loader_read_header` + `loader_sniff_bytes`) and
  the shared dimension caps;
- `loader/intact.c`, which walks the whole container and reads the stored
  size on the way (PNG IHDR; the JPEG's first SOF wherever it lies — a Pixel
  photo's SOF sits past 64 KiB of EXIF/XMP, a CMYK file's behind a 187 KB
  press profile, where a bounded header peek gave up and refused them):
  - **truncated** files: `gegl:png-load` / `gegl:jpg-load` restart the file
    on a premature EOF (libpng then reports a duplicate iCCP, libjpeg a second
    SOI / SOF) and **never return** — or, for a JPEG, **exit the process**
    — so a PNG must reach IEND and a JPEG must decode to its EOI without
    reading past the end of the file (the libjpeg pass below, whose EOF is
    fatal). A byte scan for FF D9 after SOS, which xb2 first used, is not
    enough: a progressive JPEG cut right after a COM / APPn / DQT / DHT
    segment between two scans that holds the bytes FF D9 passed it, and so
    did libjpeg's own stdio source, which inserts a fake EOI at EOF;
  - **oversized** PNGs: IHDR's size is held against the loader's caps as
    soon as IHDR is read, before any image data is inflated (a small file
    declaring a huge image is a zlib bomb for the row check below);
  - **corrupt PNG image data**: libpng's error inside `gegl:png-load` is
    logged as "failed to open file" and the op yields a header-sized black or
    partial buffer, no error. So the walk checks the critical chunks' CRCs,
    inflates the IDAT stream (into a scratch buffer) to the rows IHDR promises
    (Adam7 included) and checks every row's filter byte. The IDAT data is
    one run of consecutive IDAT chunks, as libpng reads it: any other chunk
    after an IDAT while rows are still missing ends it short ("Not enough
    image data"), even if a later IDAT would complete it;
  - **JPEGs libjpeg gives up on**: `gegl:jpg-load` installs `jpeg_std_error()`
    with no longjmp handler, so libjpeg's fatal errors (two SOF markers, a
    bogus Huffman table, ...) **exit the process**. The file is decoded once
    by the same libjpeg at 1/8 scale under a longjmp handler first
    (`intact_jpeg_decodes`), through a source manager whose EOF raises a
    fatal error (`JERR_INPUT_EOF`) the way GEGL's restart ends in one; a
    build without the `jpeg` feature has no libjpeg to do that with and
    keeps every JPEG on the loader path (CI's gegl lane installs
    `libjpeg-turbo-devel` so it tests the managed JPEG path, and builds
    `-Djpeg=disabled` a second time for its unit suite, so the tests hold
    in both builds). The whole-file passes (the PNG inflate, the libjpeg
    pass) take the worker's `GCancellable` and stop between blocks. A load
    whose checks the cancellation cut short returns `G_IO_ERROR_CANCELLED`
    **without** falling through to the loader (a whole decode for a result
    nobody takes), GEGL's own decode is not started once the cancellable
    has fired, and the loader path's decode takes the same cancellable;
  - padding between JPEG segments is skipped as libjpeg skips it
    (`streamread_jpeg_marker`), not refused;
- after the decode, the op's bounding box and the buffer must match the
  stored size (a header libjpeg rejects gives an empty extent, not an error).

**The file-swap window.** The managed path opens the file several times —
the sniff, the profile walk, the completeness walk, the libjpeg pass, GEGL's
own open, and libexif for the orientation — and only the walks before
GEGL's open vouch for the bytes GEGL reads. A file *replaced* between the last
walk and that open by a truncated one can still spin GEGL's loader in a
worker that cannot be cancelled (GEGL processing never can), and one
replaced by a JPEG libjpeg gives up on can still exit the process. GEGL's
loaders take a path, not a descriptor, so the window cannot be closed from
here; it is kept short (the stored size comes from the completeness walk, so
no separate header peek opens the file), and the same race exists for
gdk-pixbuf's path-taking calls on the loader path (tech-stack.md "The
decode gate"). The window is also the one way a profile reaches babl
**unvetted**: a file swapped in after the profile walk gets its iCCP / APP2
(or, for a PNG, its gAMA / cHRM) read by GEGL's loader and handed to
`babl_space_from_icc` / `babl_space_from_chromaticities` without
`icc_profile_is_sane`, the name checks or the slot cap — so a crafted
profile can still crash babl, and each swap can add a space and curves
outside the 36 / 69 bound. That residual risk needs someone racing a
viewer they already have write access to the folder of. Nothing cheap
narrows it further: re-reading the profile after GEGL's decode would only
report the damage after babl had taken it, and comparing mtime / size /
inode before and after the decode would not stop a swap-and-back within the
window; closing it needs a GEGL loader that takes a descriptor.

**Unsupported ops degrade, never fail.** Without `gegl:png-load` /
`gegl:jpg-load` installed that format keeps the loader path (sRGB); without
a saver the export is refused as unsupported (the tests reach both through
`enhancer_test_set_missing_op`). A non-local file (GVFS) keeps the loader
path, since GEGL's loaders need a path. Without GEGL (the minimal lane) none
of this is compiled, `intact.c` included; the info card still names the
colour space. The card adds "may be managed on enhance/export" only where
the managed path's own gates pass header-deep: `enhancer_would_manage`
(GEGL present, a local PNG — or JPEG with libjpeg — within the size caps,
the loader op installed, a vetted, parseable non-sRGB profile for the
image's components). "May": the completeness checks read the whole file —
measured on the corpus, ~0.02 ms a file for the header gates against
~150 ms on average and ~0.9 s at most for the PNG inflate / libjpeg pass,
on every `i` press — so the card does not run them, and a file whose data
is broken past its headers (a JPEG cut inside its scan) still takes the
loader path at enhance time.

**Leaks we cannot fix.** Two per-decode leaks on the managed path are
upstream's, and the LSan suppressions (`tests/lsan_suppressions.txt`:
`leak:babl`, `leak:gio_source_init`) hide them, so they are measured here.
GEGL's loaders read the file's profile and header **four times per
decode** — the op's bounding box is queried by ggaze's extent check, by
the graph's prepare and by its prepare-request, and the decode reads the
header once more (counted under gdb on gegl 0.4.72):
- **grey profiles:** each of those four `babl_space_from_icc` calls on a
  grey profile babl already has a space for re-copies the profile onto that
  space (`ret->space.icc_profile = malloc (icc_length)` over the previous
  copy): **4 × the profile's size per decode** of a grey-profiled file
  (1472 bytes for `grey-icc.png`'s 368-byte profile; a real grey profile
  is a few hundred bytes to a few KB). ggaze's own call is made once per distinct profile (the
  verdict table); GEGL's loaders take no space from outside, so theirs
  cannot be avoided short of not decoding through them;
- **JPEGs:** `gegl:jpg-load`'s source manager allocates its 1 KiB read
  buffer in `init_source` and frees it only in `term_source`, which
  libjpeg calls from `jpeg_finish_decompress` alone — never after the
  header-only reads of the three bounding-box queries, nor after the
  decode, which destroys without finishing: **4 KiB per managed JPEG
  decode**.
Dropping ggaze's own bounding-box query would save a quarter of both but
lets a header libjpeg rejects reach `gegl_node_process` (GEGL's "0px
rectangle" warning); at these sizes the check is kept.

**Left open:** RGB profiles babl cannot parse (LUT-only), profiles in
WebP/AVIF/HEIF/JXL, a managed plain view, and non-sRGB displays.

## Costs & trade-offs

- Heavier deps: `gegl`, `babl` (and transitively more). Gate behind a meson
  `feature` so minimal builds and the core culling flow don't pay for it.
- GEGL processing is slower than straight decode — keep it off the hot path,
  and off the full resolution until the export (decision #53: the live
  preview runs on a source scaled to the view).
- `gegl-gtk` (GeglGtkView) is separate and thinly maintained — **avoid**; keep
  the custom `GgazeViewer` widget and render GEGL output to `GdkTexture`.
- GEGL does **not** demosaic RAW — RAW stays out of scope.

## Dependencies (Fedora)

```
gegl04-devel   babl-devel
```

(`gegl04-devel` is the GEGL 0.4 package on Fedora; there is no
`gegl-devel` on fedora:40, which CI's gegl lane runs on.)
