#ifndef GGAZE_NAVIGATOR_H
#define GGAZE_NAVIGATOR_H

/*:*
 * ggaze — directory navigator
 *
 * Navigator is a GObject (no GtkWidget) that owns the current directory
 * listing: it filters to image MIME types, sorts (name/time/size), keeps a
 * cursor and a path-based mark set, watches the directory with GFileMonitor
 * (debounced) and emits "changed" on structural changes so the views stay in
 * sync. If the current file disappears (external delete, or trash/move) it
 * falls back to the nearest. Owns no GTK state; unit-testable without a
 * display. See docs/architecture.md "Responsibilities / navigator".
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

G_BEGIN_DECLS

#define GGAZE_TYPE_NAVIGATOR (navigator_get_type())
G_DECLARE_FINAL_TYPE(Navigator, navigator, GGAZE, NAVIGATOR, GObject)

typedef enum {
   GGAZE_SORT_NAME = 0,
   GGAZE_SORT_TIME,
   GGAZE_SORT_SIZE
} GgazeSort;

/* Construct a navigator over p_dir (file or folder; a file's parent is used by
 * the caller). Refs p_dir. Lists immediately. e_sort is the initial sort;
 * b_wrap controls prev/next wrap; b_hide_raw hides RAW sidecars when a JPEG
 * with the same stem exists (decision #33). The current cursor is the first
 * file. */
Navigator *navigator_new(GFile *p_dir, GgazeSort e_sort, gboolean b_wrap,
                         gboolean b_hide_raw);

/* g_object_unref wrapper for the _new/_delete convention. */
void navigator_delete(Navigator *p_nav);

/* --- listing ------------------------------------------------------------- */
GFile *navigator_get_dir(Navigator *p_nav); /* (transfer none) */
guint  navigator_get_count(Navigator *p_nav);
GFile *navigator_get_file(Navigator *p_nav,
                          guint      u_index);        /* (transfer none) */
gint   navigator_get_current_index(Navigator *p_nav); /* -1 if empty */
GFile *navigator_get_current(Navigator *p_nav);       /* (transfer none) */
guint
navigator_get_remaining(Navigator *p_nav); /* remaining = count - removed */
gboolean navigator_set_current(Navigator *p_nav, guint u_index);
gboolean navigator_set_current_file(Navigator *p_nav,
                                    GFile     *p_file); /* by path */

/* --- navigation (honour wrap) ------------------------------------------- */
gboolean navigator_prev(Navigator *p_nav);
gboolean navigator_next(Navigator *p_nav);
gboolean navigator_first(Navigator *p_nav);
gboolean navigator_last(Navigator *p_nav);

/* --- sort/filter --------------------------------------------------------- */
GgazeSort navigator_get_sort(Navigator *p_nav);
void      navigator_set_sort(Navigator *p_nav, GgazeSort e_sort);
gboolean  navigator_get_wrap(Navigator *p_nav);
void      navigator_set_wrap(Navigator *p_nav, gboolean b_wrap);
gboolean  navigator_get_hide_raw(Navigator *p_nav);
void      navigator_set_hide_raw(Navigator *p_nav, gboolean b_hide_raw);

/* --- marks (path-based; survive re-sort; cleared on remove) ------------- */
gboolean navigator_is_marked(Navigator *p_nav, GFile *p_file);
void     navigator_toggle_mark(Navigator *p_nav, GFile *p_file);
void     navigator_mark_range(Navigator *p_nav, GFile *p_from, GFile *p_to);
void     navigator_mark_all(Navigator *p_nav);
void     navigator_clear_marks(Navigator *p_nav);
guint    navigator_get_mark_count(Navigator *p_nav);
GList *navigator_get_marks(Navigator *p_nav); /* (transfer full) GFile* refs */

/* --- mutations ---------------------------------------------------------- */
/* Re-read the directory; if the current file is gone, fall back to nearest;
 * emit "changed". */
void navigator_rescan(Navigator *p_nav);
/* Remove p_file from the listing (used by trash/move); clear its mark; if it
 * was current, fall back to nearest; emit "changed". Returns TRUE if removed.
 */
gboolean navigator_remove(Navigator *p_nav, GFile *p_file);

/* GFileMonitor debounce in ms (default 250; decision #28). Tests may lower it.
 */
void navigator_set_debounce_ms(Navigator *p_nav, guint u_ms);

/* --- removed/dimmed set (M7: trashed/deleted items stay listed but dimmed) --
 */
gboolean navigator_is_removed(Navigator *p_nav, GFile *p_file);
void     navigator_mark_removed(Navigator *p_nav, GFile *p_file);
gboolean navigator_unmark_removed(Navigator *p_nav, GFile *p_file);
guint    navigator_get_removed_count(Navigator *p_nav);

/* "changed" signal: emitted on sort/filter/rescan/remove/monitor event. */
void navigator_emit_changed(Navigator *p_nav);

G_END_DECLS

#endif /* GGAZE_NAVIGATOR_H */