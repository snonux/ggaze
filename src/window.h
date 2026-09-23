#ifndef GGAZE_WINDOW_H
#define GGAZE_WINDOW_H

/*:*
 * ggaze — main window
 *
 * GgazeWindow : GtkApplicationWindow owns the layout -- an AdwHeaderBar with
 * navigation buttons and the main menu, and a GtkStack with the thumbnail
 * grid, the large GgazeViewer and an empty-state page -- plus the action
 * routing: every win.* action, the Navigator over the current folder, the
 * drop target, and the helpers that own the real logic (viewload, info
 * overlay, save gate, delete confirm, enhance controller).
 * See docs/architecture.md.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "app.h"

#include <gtk/gtk.h>

#include "croprect.h"    /* CropRect, for the crop-rectangle seam */
#include "ggaze-enums.h" /* GgazeTool */
#include "viewer.h"      /* GgazeViewerDragPhase */

G_BEGIN_DECLS

#define GGAZE_TYPE_WINDOW (ggaze_window_get_type())
G_DECLARE_FINAL_TYPE(GgazeWindow, ggaze_window, GGAZE, WINDOW,
                     GtkApplicationWindow)

/* Construct a new window attached to p_app. */
GgazeWindow *ggaze_window_new(GgazeApp *p_app);

/* Open p_arg (a file or a folder): for a file, list its parent folder with
 * p_arg current and show it in the large view; for a folder, list it and show
 * the grid. A path that does not exist, or a file that is not an image of
 * its folder, opens the folder and reports the problem on the status line. */
void ggaze_window_open(GgazeWindow *p_win, GFile *p_arg);

/* Open several files at once (CLI arguments, a multi-file drop): one file
 * behaves like ggaze_window_open; several open the FIRST file's folder in
 * the grid with that file current (decision #27). */
void ggaze_window_open_files(GgazeWindow *p_win, GFile **pp_files,
                             gint i_n_files);

/* The GtkStack (grid/large) — the tests use this instead of
 * gtk_window_get_child (which now returns the wrapping GtkOverlay). */
GtkStack *ggaze_window_get_stack(GgazeWindow *p_win);

/* --- INTERNAL: test hook --------------------------------------------------
 *
 * Drop every decoded texture from this window's bounded LRU, so the next load
 * of the current file cannot be served synchronously from the cache and has to
 * take the async (GTask) path instead. Nothing in the UI calls this; it exists
 * because the cache-HIT and cache-MISS legs of the enhance-preview override
 * reach the viewer through different callbacks, and the MISS leg (where the
 * async result used to win a race against the preview) is otherwise
 * unreachable from a test — the LRU holds 4 entries and evicting on demand is
 * not something the window exposes any other way. */
void ggaze_window_clear_texture_cache(GgazeWindow *p_win);

/* The info-overlay label (`i`; also reused for transient status messages
 * like "Copied image"). Exposed so tests can assert its visibility/text
 * directly instead of reaching into private window state -- e.g. the gu0
 * regression coverage that navigating away from a file while its info
 * overlay is showing hides it rather than leaving stale EXIF/dimensions on
 * screen for the newly-current file. */
GtkWidget *ggaze_window_get_info_label(GgazeWindow *p_win);

/* The info card's histogram plot (a GgazeHistogramView; see
 * histogram-view.h). Visible only while the card shows a file whose texture
 * was on screen when `i` was pressed. Exposed for the same reason as the
 * label: tests assert that `i` produces a plot for the displayed image and
 * that navigating to another image yields a different one. */
GtkWidget *ggaze_window_get_info_histogram(GgazeWindow *p_win);

/* The GdkContentProvider win.copy (Ctrl+c) would set on the clipboard, built
 * from the current window state WITHOUT touching the (display-backend-
 * dependent) system clipboard, so the copy decision is testable without a
 * clipboard round-trip. (Still needs a display -- there is no offscreen GTK4
 * backend and test_copy.c skips with exit 77 when gtk_init_check() fails.)
 *   - Marks present: text/uri-list + text/plain over the marked files.
 *   - No marks: the DISPLAYED GdkTexture as image/png (the enhanced preview
 *     when an enhance preset is active, else the original).
 * Returns a new ref the caller must unref, or NULL when nothing is open / no
 * marks and no texture is displayed. */
GdkContentProvider *ggaze_window_get_copy_provider(GgazeWindow *p_win);

/* Launch editor u_idx (0-based, in the configured editors list order) on the
 * ORIGINAL current file (not an enhanced preview). The opener is detached
 * (GSubprocess), so the UI stays responsive. Returns TRUE iff the program
 * was started; FALSE (with a g_warning) on a parse/launch error, an out-of-
 * range index, or when nothing is open / no editors are configured. This is
 * the testable launch path the `e` open-external popup (and its hotkeys)
 * invoke. */
gboolean ggaze_window_open_external_index(GgazeWindow *p_win, guint u_idx);

/* Run script u_idx (0-based, in the configured scripts list order) on the
 * ORIGINAL current file (%f) and the current folder (%d) via /bin/sh -c,
 * asynchronously (GSubprocess), so the UI stays responsive. On completion the
 * navigator is rescanned (scripts may add/remove files) and a status line
 * reports success or the exit status / error. The rescan is folder-identity
 * safe: the folder the script ran against is captured at launch time, and the
 * completion callback only rescans it if the window still navigates that same
 * folder (a single-instance open / drop that replaced the folder while the
 * script ran does NOT trigger a rescan of the new folder). Returns TRUE iff the
 * script was started; FALSE (with a g_warning + status) on a launch error, an
 * out-of-range index, or when nothing is open / no scripts are configured. This
 * is the testable run path the `!` run-script popup (and its hotkeys) invoke.
 */
gboolean ggaze_window_run_script_index(GgazeWindow *p_win, guint u_idx);

/* Move to destination u_idx (0-based, in the configured destinations list
 * order). Acts on every marked file if any are marked, else just the current
 * file (docs/ui-and-interactions.md "Selection & moving"). Uses mover_move
 * (rename or copy+delete, collision-suffixed) and, for every target that
 * actually left its original path, dims it in the navigator/grid via
 * navigator_mark_removed (mirroring trash) and advances past the moved set.
 * Records the move so `u` (win.undo) can move the set back — win.undo
 * prefers whichever of trash/move happened most recently. Reports a status
 * line on success or failure. Returns TRUE iff mover_move reported overall
 * success; FALSE on an out-of-range index, no destinations configured, no
 * targets to move, or a mover_move failure (still leaves anything that DID
 * move flagged as removed — see window.c's collision-with-partial-failure
 * handling). This is the testable move path the `m` popup (and its hotkeys)
 * invoke. Runs immediately, synchronously, with no Save/Discard/Cancel gate
 * of its own (tu0): that gate lives one layer up, at the popup's row-click/
 * hotkey handlers (window.c's `_move_go`), so this function's contract
 * — and every existing test that calls it directly — stays unchanged. */
gboolean ggaze_window_move_index(GgazeWindow *p_win, guint u_idx);

/* Navigation over the current folder (bound to h/l/Left/Right/g/G via
 * shortcuts.c). No-ops if nothing is open. */
void ggaze_window_prev(GgazeWindow *p_win);
void ggaze_window_next(GgazeWindow *p_win);
void ggaze_window_first(GgazeWindow *p_win);
void ggaze_window_last(GgazeWindow *p_win);

/* TRUE iff a GEGL enhance preview is active and unsaved (an "-enhanced" copy
 * has not been exported for it yet). Always FALSE when GEGL is not built in.
 * Used by tests and by the Save/Discard/Cancel gate (_maybe_save_then) that
 * every navigation/trash/delete/move/quit/open path funnels through before
 * discarding or overwriting the current preview. */
gboolean ggaze_window_enhance_is_dirty(GgazeWindow *p_win);

/* Hold-Space compare (docs/ui-and-interactions.md "Compare original vs
 * modified"): b_hold TRUE shows the unmodified original in place of the
 * active enhance preview; FALSE restores the preview. No-op if nothing is
 * dirty, GEGL is not built in, or the requested state is already in effect.
 * This is the testable entry point the window's own Space key controller
 * (press/release, which shortcuts.c's action-trigger table cannot express)
 * calls. Every place that clears the enhance mask (discard, a real
 * navigation away, etc.) also force-resets the internal hold flag to FALSE,
 * so a release that arrives after the mask was cleared out from under a
 * still-held Space key can never leave it stuck TRUE (tu0 review round 2,
 * issue 4). */
void ggaze_window_set_hold_original(GgazeWindow *p_win, gboolean b_hold);

/* --- the crop / straighten tools (c / R; docs/ui-and-interactions.md) -----
 *
 * Which interactive tool has the large view right now (always
 * GGAZE_TOOL_NONE without GEGL). */
GgazeTool ggaze_window_get_tool(GgazeWindow *p_win);

/* Offer a key press to the active tool: TRUE iff it consumed it. This is the
 * testable entry point the window's own capture-phase key controller calls
 * for every key while a tool is active (the tool keys -- h/l/j/k, H/L/J/K,
 * 1-4/0, +/-, A, Enter, Esc -- are modal and are NOT in shortcuts.c's action
 * table; that table documents them as help-only rows). Always FALSE without
 * GEGL. */
gboolean ggaze_window_tool_key(GgazeWindow *p_win, guint u_keyval,
                               GdkModifierType e_state);

/* A pointer drag over the viewer, in viewer-widget coordinates, delivered to
 * the active tool (crop: move / resize the rectangle; straighten: draw and,
 * on END, level a horizon line). The viewer's drag gesture feeds the same
 * path; this is the hook tests use instead of synthesising pointer events.
 * A no-op with no tool active or without GEGL. */
void ggaze_window_tool_drag(GgazeWindow *p_win, GgazeViewerDragPhase e_phase,
                            gdouble d_x, gdouble d_y);

/* How many enhance preview renders (a full decode + GEGL chain in a worker)
 * this window has launched so far; always 0 without GEGL. A test seam: the
 * controller coalesces renders (at most one in flight plus one queued), and
 * tests/test_enhance_flow.c pins that a burst of N changes costs at most two
 * launches. */
guint ggaze_window_enhance_render_count(GgazeWindow *p_win);

/* The crop rectangle as the overlay draws it right now (tool_ctrl_get_crop_
 * rect): TRUE with the rectangle and the size of the base it is laid out on
 * iff the crop tool is up and its overlay is visible over the texture on
 * screen; FALSE while it is hidden, with no tool, and always without GEGL.
 * A test seam for "the overlay follows a rewrite of the file with no key
 * pressed" (tests/test_enhance_flow.c). */
gboolean ggaze_window_tool_crop_rect(GgazeWindow *p_win, CropRect *p_rect,
                                     gint *p_base_w, gint *p_base_h);

/* --- INTERNAL: bulk-delete safety (used by the confirm-dialog flow and the
 * delete-safety regression test) -------------------------------------------
 *
 * The >1-mark delete opens an async GtkAlertDialog. While it is pending, a
 * single-instance open / drop can replace the folder (ggaze_window_open swaps
 * p_nav). Re-reading the navigator's marks on confirm would then delete files
 * from the NEW folder. To prevent that, the targets are captured at prompt
 * time and the dialog callback validates the folder is still the same one
 * before deleting the captured (not re-read) files.
 *
 * ggaze_window_delete_targets_still_current returns TRUE iff p_win's current
 * navigator is over p_dir (the folder the captured targets came from is still
 * open), so a pending confirm may safely delete them. FALSE means the folder
 * was replaced while the dialog was pending and the delete must be refused.
 *
 * ggaze_window_delete_captured deletes EXACTLY p_files (the captured targets;
 * the list is borrowed and not freed here) iff that folder is still open, and
 * returns TRUE iff the delete proceeded. p_files are GFile* (borrowed refs).
 */
gboolean ggaze_window_delete_targets_still_current(GgazeWindow *p_win,
                                                   GFile       *p_dir);
gboolean ggaze_window_delete_captured(GgazeWindow *p_win, GFile *p_dir,
                                      GList *p_files);

G_END_DECLS

#endif /* GGAZE_WINDOW_H */