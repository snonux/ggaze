# Open Questions

Decisions to make before/while coding. None block M0.

Most items are now **decided** — see PLAN.md decisions #29–#38. Each item's
**Decision:** line records the chosen value; the milestone-level Leans are
locked in decision #37.

## A. libadwaita or plain GTK4?
- libadwaita → GNOME-native styling, follows release style, but opinionated
  (header bar patterns, no theming overrides).
- plain GTK4 → full control, works under any GTK theme, less "GNOME-native".
- **Decision:** libadwaita (decision #29).

## B. App ID / GApplication name
- Placeholder `org.buetow.ggaze`. Confirm domain + naming.
- **Decision:** `org.buetow.ggaze` (decision #30).

## C. Image decode strategy
- Rely on GdkPixbuf loaders for the common case, or link libjpeg-turbo /
  libpng directly from the start for the progressive low-res preview feature?
- **Lean:** GdkPixbuf first; add direct libs in M5/M6.

## D. Thumbnail grid view — in scope (decided)
- Two views per window: a thumbnail grid (overview) and a large single-image
  view. `Enter`/`Esc` switch between them; cursor stays in sync.
- Trashed/deleted items stay listed but dimmed (with a hide toggle).
- **Lean:** grid view is a core feature, M7. Not "a different app" after all.

## E. Single instance?
- `GApplication` single-instance means a second `ggaze img.jpg` reuses the
  window and loads the new image. Convenient; small risk of state confusion.
- **Decision:** single instance, replace on new `open` (decision #32).

## F. Recursive directory walking
- `ggaze folder/` → recurse into subdirs, or just flat siblings?
- **Lean:** flat by default; add `--recursive` later.

## G. Color management
- Ignore sRGB-only for now; plan a later milestone for lcms2 + ICC.
- Confirm target displays are sRGB-ish (most are).
- **Update:** if GEGL is enabled (M9), color-managed decode/export comes via
  babl + LCMS (`gegl:icc-file-loader`, `gegl:lcms-from-profile`,
  `gegl:convert-color-space`) — no separate lcms2 wiring. Without GEGL, stay
  sRGB-only.

## H. Scroll behavior default
- Scroll = zoom (feh-style `--scale-zoom`) vs scroll = next/prev.
- **Lean:** zoom; expose in settings since it divides users.

## I. Trash vs delete (decided)
- `d` moves the file to a local `<folder>/.Trash/` directory (created lazily,
  collision-suffixed), undoable with `u`. This is **not** the system trash.
- `D` permanently deletes (unlink), no undo.
- **Lean:** confirmed; `./Trash` lives with the shoot for easy inspection/emptying.

## J. Packaging targets
- RPM first (Fedora native). Flatpak too? Copr repo?
- **Lean:** RPM + AppStream; Flatpak later.

## K. Camera-dump specifics
- **RAW + JPEG pairs.** Many cameras shoot both. Show only the JPEG and hide
  the matching `.RAF/.CR3/.NEF`? Or show RAW via embedded preview? Hide-pairs
  is the culling-friendly choice.
- **Burst grouping.** Auto-detect burst sequences (EXIF burst ID or
  sub-second capture time clustering) and collapse them into a group, letting
  `j`/`k` step group-by-group with an expand for within-burst? Big UX win for
  the stated use case, but scope creep — decide whether it's M-something or
  "later".
- **Default sort.** Capture time (EXIF `DateTimeOriginal`) vs filename. Camera
  dumps are usually already named in shot order, so filename sort ≈ capture
  order; but capture-time sort is more robust after a burst of edits.
- **Import folder.** Any integration with the camera-download location, or just
  "whatever path you point it at"? Lean: just a path.
- **Decision:** burst grouping deferred to "later"; hide RAW sidecars by
  default (toggle to reveal); default sort = filename (capture-time as a menu
  option); import folder = just a path (decision #33).

## L. Custom viewer widget vs GtkPicture
- `GtkPicture` is simplest but limited zoom/pan. A custom `GtkWidget` gives
  cursor-centered zoom, pan clamping, animated transitions.
- **Decision:** custom viewer widget (decision #31).

## M. Destination configuration storage
- GSettings `a(ss)` (ordered name→path) is simplest and integrates with
  `gsettings`/dconf. Alternative: a small TOML/ini under
  `~/.config/ggaze/destinations.conf` for hand editing.
- **Lean:** GSettings `a(ss)` via the Preferences dialog; revisit if users want
  to hand-edit.

## N. Move vs copy, and collision policy
- `g_file_move` (rename / copy+delete) by default; a copy mode ("send a copy
  without removing") could be a modifier (`M` for copy?).
- Collision: suffix `-1`, `-2`, … (never overwrite).
- **Lean:** move + suffix; copy as a later option.

## O. Auto-hotkey scheme past 10 destinations
- `1`-`9`, `0`, then `a`-`z` gives 36 slots — plenty. Beyond that, a scrollable
  list navigable by arrows + `Enter` (no single-key shortcut).
- **Lean:** digits then letters; cap at a sane number.

## P. Undo depth
- One level (`u` undoes the last `d` or `m`) is enough to start; a small stack
  (10) is a cheap upgrade later.
- **Lean:** one level now, stack later.

## Q. Marks vs re-sort / view switch / trash
- Marks are path-based and held by the navigator: a re-sort reflows but keeps
  marks; a view switch preserves them; trashing a marked item clears its mark.
- **Lean:** confirm during implementation.

## R. External programs — command format & launch
- Store `name → command` pairs; command uses freedesktop `Exec` placeholders
  (`%f` single file, `%F` multiple). Start with `%f` only; `%F` (marked set)
  later.
- Launch via `GSubprocess` detached (non-blocking), or `g_app_info`?
  GSubprocess is simpler for raw commands; `g_app_info` is better if reusing
  `.desktop` entries.
- **Lean:** raw command + `%f` + GSubprocess; revisit `.desktop` reuse later.
- Should `e` apply to marks (`%F`)? **Lean:** current image first; marks later.

## S. Shell scripts — execution model
- Run via `/bin/sh -c "<cmd>"` (POSIX sh) or `bash -c`? Use the user's
  `$SHELL`? **Lean:** `/bin/sh -c` for portability; revisit for bash-isms.
- Async, non-blocking; toast while running and on completion. On exit,
  rescan the directory (scripts may add/remove/move files). A destructive
  script could rename the current file away — rescan handles it (cursor falls
  back to nearest).
- Placeholders: `%f` (current image), `%d` (current folder); `%F` (marked
  set) later. **Shell injection:** single-quote substituted paths (filenames
  can contain spaces, `$`, backticks) — or pass paths via env/argv instead of
  `sh -c`.
- **Lean:** `/bin/sh -c`, single-quote paths, rescan on exit, `%f`/`%d` now.

## T. Thumbnail size — range, step, and cache
- freedesktop TMS defines only 128 (normal) and 256 (large) cached sizes.
  For larger grid thumbnails, decode a custom size on demand (downscale the
  full image or the `large` cache entry), cached in a separate bucket. Range
  ~64–512px, step ~32px. Confirm.
- Persist `thumbnail-size` (int) in GSettings; restore on launch.
- **Lean:** 64–512px, ±32px step, custom cache bucket for non-TMS sizes.

## U. GEGL integration
- **Hard or optional dep?** Lean: optional meson `feature`; core viewer works
  without it. Confirm RPM splits a `ggaze-gegl` subpackage or keeps it in.
- **Preset format:** store `gegl-graph` text (parsed via `gegl:gegl` /
  `gegl_node_new_from_xml`) or build programmatically per known preset? Lean:
  built-in presets built programmatically; user presets as graph text.
- **Apply timing:** only when paused on an image (not during `h`/`l` scrub);
  disable the preset during rapid navigation, reapply on settle. Confirm.
- **Viewer path:** import decoded pixels → GeglBuffer (GdkPixbuf-source op or
  babl), process, render output buffer → GdkTexture. Measure overhead; maybe
  show a subtle "enhanced" badge in the header.
- **Export naming:** `<stem>-enhanced.<ext>`; collision → suffix `-1`. Same dir
  as original by default; configurable later.
- **gegl-gtk?** No — keep the custom viewer, render to GdkTexture.
- **Save flow:** `s` = save enhanced copy; no auto-save; navigate-away (or
  quit, or `d`/`D`/`m`) with a dirty preview prompts Save/Discard/Cancel.
  Slideshow moved to `S` to free `s` for save. Confirm the prompt also fires
  on `d`/`D`/`m` of a dirty image (Lean: yes).
- **Decision:** optional meson feature; built-in presets programmatic, user
  presets as `gegl-graph` text; apply only when settled (not during scrub);
  "enhanced" badge; export `<stem>-enhanced.<ext>` same dir, collision suffix
  `-1`; dirty-prompt fires on `d`/`D`/`m`; no gegl-gtk (decision #34).

## V. Clipboard — pixels vs URIs
- Single image → `image/png` pixels (paste as image into chat apps); marks →
  `text/uri-list` (+ `text/plain` paths). Verify Katogram paste accepts image
  pixels (some apps want URIs even for one file).
- Offer both at once via `gdk_content_provider_new_union` (PNG + uri-list) when
  one file, so the target picks. Lean: yes.
- Decode-for-PNG-copy runs in a `GTask` thread; cap size (downscale very
  large images?) — Lean: copy full-res, note the memory cost.
- `Ctrl+Shift+c` to force copy-as-path (text) — later.

## W. Crop, straighten & rotate tools
- **Crop rect UI:** overlay rectangle with handles; keyboard move
  (`h`/`l`/`j`/`k`) + resize (`H`/`L`/`J`/`K`); aspect presets `1`-`4` + `0`
  free. Tool mode grabs keys until `Enter`/`Esc` so they don't clash with
  pan/zoom.
- **Straighten:** drag-a-horizon-line vs angle nudge; ±0.5° step; grid overlay.
  Auto-crop rotated corners? Lean: offer as a toggle, default on.
- **Rotate 90°:** one-shot `[`/`]`; repeat to 180°/270°; non-destructive. No
  overlay.
- All are GEGL ops in the same preview graph as enhance; `s` exports. If GEGL
  off, toast. Confirm compose order (crop after rotate, or user order).
- **Compare:** hold `Space` (momentary key-release event) vs a toggle; GUI uses
  a menu *Show original* toggle. Confirm momentary doesn't fight the
  navigate-away dirty prompt.
- **Decision:** compose order load→enhance(color)→rotate→straighten→crop→
  export; straighten auto-crop default on (decision #35).

## X. Enhance — fixed presets vs tunable parameters
- Ship one-shot presets with sensible fixed strengths (brightness/contrast/
  saturation/etc.). Strength is editable in the `enhance-presets` gegl-graph
  text (Preferences / `gsettings`) — no slider UI, keeps KISS.
- Later: a "fine adjust" mode that nudges the active preset's main parameter
  with keys (which keys? `+`/`-` clash with zoom; maybe `H`/`L` or a sub-mode).
  Lean: presets first; nudging only if asked.
- **Curves:** GEGL has `gegl:contrast-curve` and `gegl:curve`; confirm the
  exact op name and how to embed a curve shape in the graph text. Interactive
  curve editing is out of scope (GIMP hand-off via `e`).
- **Decision:** one active color preset (replace); crop/straighten/rotate
  stack; reset preview clears all; combine color presets via one `gegl-graph`
  entry; curves via `gegl:contrast-curve`; no fine-adjust nudging in v1
  (decision #36).

## Y. EXIF orientation
- Source: GdkPixbuf's `gdk_pixbuf_apply_embedded_orientation` covers JPEG/TIFF
  via GdkPixbuf; for direct-lib backends (libjpeg-turbo, libjxl, libavif,
  libheif) read the orientation tag (libexif or the format's own metadata) and
  rotate/flip. Confirm a single code path (normalize to identity orientation
  before producing the `GdkTexture`).
- Manual `[`/`]` and straighten compose **on top** of the EXIF-corrected image.
- On `s` export, the pixels are already upright → write the EXIF Orientation
  tag as 1 (normal) to avoid double-rotation in other apps. Confirm.

## Z. Opening files, folders & drag-drop
- **Folder arg vs file arg:** `ggaze dir/` → grid of dir; `ggaze file.jpg` →
  parent dir as navigator, file current (large view). Confirm.
- **Drop semantics:** drop folder → grid; drop one file → open it; drop many
  files → open the first file's folder in grid with the first selected (or
  treat the dropped set as the navigator's universe?). Lean: first-file's
  folder, first selected.
- **`o` dialog:** use `GtkFileDialog` (GTK4) with folder selection allowed as
  well as files.
- **Single-instance:** a new `open` (arg or drop) replaces the current
  folder/image (per open-question E).

## AA. Folder monitoring
- `GFileMonitor` on the current directory; debounce bursts (usbimport writes
  many files quickly) — e.g. 250 ms coalesce before emitting `changed`.
- Removed current file → fall back to nearest neighbor (or stay on a
  placeholder)? Lean: nearest.
- Recurse into subdirs? No (flat; matches open-question F). Watch only the
  current dir.
- Pause monitoring during rapid `h`/`l` scrubbing? Probably unnecessary;
  debounce handles it.