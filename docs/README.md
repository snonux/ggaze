# ggaze — GNOME Gaze

A small, fast, native image viewer for Fedora Linux, written in C with GTK4.

The aim is concrete: **quickly preview a folder of pictures downloaded from a
camera.** You pull a shoot off the camera into a directory, fire up ggaze on
any file, and flip through the lot fast — glance at EXIF to tell near-identical
frames apart, trash the obvious rejects, move on. It is a culling viewer, not
a library manager.

The emphasis is on **quick**: fast startup, instant first-frame display,
keyboard-driven navigation through a directory of images, and a UI that gets
out of the way. Think of it as a modern, GNOME-native take on the classic
`feh` / `nsxiv` / `qiv` lineage — minimal chrome, fast decode, no library
management bloat.

Layout-wise it nods to **gthumb** (header bar, thumbnail grid, full-window
viewer) but stays KISS — no sidebar, no catalogs, no toolbars.

This folder contains **planning only**. No implementation yet.

## Documents

- [goals-and-scope.md](goals-and-scope.md) — what ggaze is and is not
- [architecture.md](architecture.md) — module layout and data flow
- [ui-and-interactions.md](ui-and-interactions.md) — window layout, keybindings, gestures
- [tech-stack.md](tech-stack.md) — libraries, build system, dependencies
- [coding-conventions.md](coding-conventions.md) — C style (follows the c-best-practices skill)
- [gegl.md](gegl.md) — GEGL quick-enhance & image-processing plan
- [roadmap.md](roadmap.md) — milestones from skeleton to polish
- [open-questions.md](open-questions.md) — decisions still to be made

## One-line summary

`ggaze ~/Downloads/Camera/IMG_0001.jpg` — opens instantly in a thumbnail
grid of the folder, `Enter` into the large view, walk the shoot with `h`/`l` (or `←`/`→`),
check EXIF with `i`, `d` to bin a reject into `./Trash`, `D` to delete it
outright, `q` to leave.

## Command-line

- `ggaze [FILE|FOLDER]` — open a file (large view) or a folder (grid).
- `--version`, `--help`.
- `--sort=name|time|size`, `--view=grid|large` (convenience/scripting; later).

## Working assumptions (correct me)

- Desktop app, single main window with **two views**: a thumbnail grid
  (overview of the folder) and a large single-picture view.
- Rejected pictures go into a `./Trash` folder beside the images (local,
  recoverable), not the system trash. `D` deletes permanently.
- Opens a file **or** a folder (CLI arg or drag-and-drop); `ggaze dir/` → grid.
- Primary platform: Fedora Linux / GNOME. Other GTK4 platforms are a bonus.
- App ID / GApplication: `org.buetow.ggaze` (placeholder).
- Build system: Meson (GNOME/Fedora convention).