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

#include "ggaze-enums.h"

G_BEGIN_DECLS

#define GGAZE_TYPE_NAVIGATOR (navigator_get_type())
G_DECLARE_FINAL_TYPE(Navigator, navigator, GGAZE, NAVIGATOR, GObject)

/* What a "changed" emission is about (bit flags, several may be set). The
 * signal used to carry nothing, so every consumer reconstructed the meaning
 * from counters -- and got it wrong for a same-count listing change. */
typedef enum {
   GGAZE_NAV_CURSOR  = 1 << 0, /* navigator.current moved */
   GGAZE_NAV_MARKS   = 1 << 1, /* the mark set / range anchor changed */
   GGAZE_NAV_LISTING = 1 << 2, /* the listing was rebuilt (rescan, sort,
                                * filter, monitor); cells may be new */
   GGAZE_NAV_REMOVED = 1 << 3, /* the removed/dimmed set changed */
   GGAZE_NAV_ALL     = 0xf,
} GgazeNavChange;

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
/* navigator_get_file: (transfer none) */
GFile *navigator_get_file(Navigator *p_nav, guint u_index);
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
/* Whether c_name (a basename) carries one of the RAW extensions the
 * hide-raw preference prunes when a JPEG twin exists. Pure: needs no
 * navigator, so window.c can tell "hidden RAW sidecar" from "not an image"
 * without owning a copy of the extension table. */
gboolean navigator_is_raw_name(const char *c_name);

/* --- marks (path-based; survive re-sort; cleared on remove) ------------- */
gboolean navigator_is_marked(Navigator *p_nav, GFile *p_file);
void     navigator_toggle_mark(Navigator *p_nav, GFile *p_file);
void     navigator_mark_range(Navigator *p_nav, GFile *p_from, GFile *p_to);
void     navigator_mark_all(Navigator *p_nav);
void     navigator_clear_marks(Navigator *p_nav);
guint    navigator_get_mark_count(Navigator *p_nav);
GList *navigator_get_marks(Navigator *p_nav); /* (transfer full) GFile* refs */

/* Borrowed pointer to the last file marked via a `v` toggle-on - the anchor
 * for `V` range-mark (NULL if none / cleared). Survives re-sort (path-based).
 */
GFile *navigator_get_last_mark(Navigator *p_nav); /* (transfer none) */

/* --- mutations ---------------------------------------------------------- */
/* Re-read the directory; if the current file is gone, fall back to nearest;
 * emit "changed". */
void navigator_rescan(Navigator *p_nav);

/* FALSE iff the last (re)listing could not enumerate the folder (vanished,
 * unreadable); navigator_get_error() then holds the reason. */
gboolean    navigator_is_readable(Navigator *p_nav);
const char *navigator_get_error(Navigator *p_nav); /* (transfer none) */

/* 1-based position of the current file among the LIVE (not removed) entries,
 * for the "n/total" title; 0 when nothing is current. */
guint navigator_get_live_position(Navigator *p_nav);
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
guint    navigator_get_removed_count(Navigator *p_nav);

/* "changed" signal: void changed(Navigator *, guint u_flags, gpointer), where
 * u_flags is a GgazeNavChange mask saying what changed. Emitted on every
 * cursor move, mark change, listing rebuild and removed-set change. Test
 * seam: emit it with GGAZE_NAV_ALL. */
void navigator_emit_changed(Navigator *p_nav);

G_END_DECLS

#endif /* GGAZE_NAVIGATOR_H */