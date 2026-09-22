/*:*
 * ggaze — destructive file-operation policy
 *
 * See fileops.h.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "fileops.h"

#include <gio/gio.h>
#include <glib.h>

struct FileOps {
   Mover          *p_mover; /* borrowed */
   Undo           *p_undo;  /* borrowed */
   Navigator      *p_nav;   /* borrowed, NULL when no folder is open */
   Trash          *p_trash; /* borrowed, NULL when no folder is open */
   FileOpsReportFn fn_report;
   gpointer        p_host;
};

/* Failures are reported on screen through the host; the log line is
 * g_message, not g_warning, because the status line IS the report and a
 * warning would abort the unit tests that exercise these failures. */
static void
_report(FileOps *p_fo, const char *c_msg) {
   if (p_fo->fn_report != NULL) {
      p_fo->fn_report(p_fo->p_host, c_msg);
   }
}

static void _reportf(FileOps *p_fo, const char *c_fmt, ...) G_GNUC_PRINTF(2, 3);

static void
_reportf(FileOps *p_fo, const char *c_fmt, ...) {
   va_list t_args;
   va_start(t_args, c_fmt);
   char *c_msg = g_strdup_vprintf(c_fmt, t_args);
   va_end(t_args);
   _report(p_fo, c_msg);
   g_free(c_msg);
}

FileOps *
fileops_new(Mover *p_mover, Undo *p_undo, FileOpsReportFn fn_report,
            gpointer p_host) {
   FileOps *p_fo   = g_new0(FileOps, 1);
   p_fo->p_mover   = p_mover;
   p_fo->p_undo    = p_undo;
   p_fo->fn_report = fn_report;
   p_fo->p_host    = p_host;
   return (p_fo);
}

void
fileops_delete(FileOps *p_fo) {
   g_free(p_fo);
}

void
fileops_set_folder(FileOps *p_fo, Navigator *p_nav, Trash *p_trash) {
   if (p_fo == NULL) {
      return; /* GObject dispose may run twice; the second pass has freed us */
   }
   p_fo->p_nav   = p_nav;
   p_fo->p_trash = p_trash;
}

/* --- targets ---------------------------------------------------------------
 */

GList *
fileops_capture_targets(FileOps *p_fo) {
   g_return_val_if_fail(p_fo != NULL, NULL);
   if (p_fo->p_nav == NULL) {
      return (NULL);
   }
   if (navigator_get_mark_count(p_fo->p_nav) > 0) {
      return (navigator_get_marks(p_fo->p_nav)); /* transfer full */
   }
   GFile *p_cur = navigator_get_current(p_fo->p_nav);
   if (p_cur == NULL) {
      return (NULL);
   }
   return (g_list_prepend(NULL, g_object_ref(p_cur)));
}

/* Two independent things can invalidate a target captured at key-press
 * time: the folder was replaced (single-instance open / drop) behind a
 * prompt, or the file was removed externally while the prompt was up (the
 * folder monitor keeps firing behind the input-only modal grab). */
gboolean
fileops_target_still_in_folder(FileOps *p_fo, GFile *p_file) {
   g_return_val_if_fail(p_fo != NULL, FALSE);
   if (p_fo->p_nav == NULL || p_file == NULL) {
      return (FALSE);
   }
   GFile   *p_dir    = navigator_get_dir(p_fo->p_nav);
   GFile   *p_parent = g_file_get_parent(p_file);
   gboolean b_ok =
      (p_dir != NULL && p_parent != NULL && g_file_equal(p_dir, p_parent));
   g_clear_object(&p_parent);
   return (b_ok && g_file_query_exists(p_file, NULL));
}

/* Computed BEFORE anything is removed: navigator_mark_removed emits
 * "changed" and can move current as a side effect. */
gboolean
fileops_files_include_current(FileOps *p_fo, GList *p_files) {
   g_return_val_if_fail(p_fo != NULL, FALSE);
   if (p_fo->p_nav == NULL) {
      return (FALSE);
   }
   GFile *p_cur = navigator_get_current(p_fo->p_nav);
   for (GList *p_it = p_files; p_cur != NULL && p_it != NULL;
        p_it        = p_it->next) {
      if (g_file_equal(p_cur, G_FILE(p_it->data))) {
         return (TRUE);
      }
   }
   return (FALSE);
}

/* --- trash / delete --------------------------------------------------------
 */

void
fileops_trash(FileOps *p_fo, GFile *p_target) {
   g_return_if_fail(p_fo != NULL);
   if (p_fo->p_nav == NULL || p_fo->p_trash == NULL || p_target == NULL) {
      return;
   }
   if (!fileops_target_still_in_folder(p_fo, p_target)) {
      _report(p_fo, "Nothing trashed — the file is gone");
      return;
   }
   GFile   *p_cur         = navigator_get_current(p_fo->p_nav);
   gboolean b_was_current = (p_cur != NULL && g_file_equal(p_cur, p_target));
   GError  *p_err         = NULL;
   char    *c_name        = g_file_get_basename(p_target);
   if (trash_bin(p_fo->p_trash, p_target, &p_err)) {
      navigator_mark_removed(p_fo->p_nav, p_target); /* dim; emits changed */
      if (b_was_current) {
         navigator_next(p_fo->p_nav); /* advance; emits changed */
      }
      undo_record_trash(p_fo->p_undo);
      _reportf(p_fo, "Trashed %s — u to undo", c_name);
   } else {
      g_message("ggaze: trash failed for %s: %s", c_name,
                p_err != NULL ? p_err->message : "?");
      _reportf(p_fo, "Trash failed for %s: %s", c_name,
               p_err != NULL ? p_err->message : "?");
      g_clear_error(&p_err);
   }
   g_free(c_name);
}

/* Report the outcome of a delete over u_n targets. */
static void
_report_delete(FileOps *p_fo, GList *p_files, guint u_failed) {
   guint u_done = g_list_length(p_files) - u_failed;
   if (u_failed > 0) {
      _reportf(p_fo, "Delete failed for %u file%s", u_failed,
               u_failed == 1 ? "" : "s");
   } else if (u_done == 1) {
      char *c_name = g_file_get_basename(G_FILE(p_files->data));
      _reportf(p_fo, "Deleted %s permanently (no undo)", c_name);
      g_free(c_name);
   } else {
      _reportf(p_fo, "Deleted %u files permanently (no undo)", u_done);
   }
}

void
fileops_delete_files(FileOps *p_fo, GList *p_files) {
   g_return_if_fail(p_fo != NULL);
   if (p_fo->p_nav == NULL || p_files == NULL) {
      return;
   }
   gboolean b_was_current = fileops_files_include_current(p_fo, p_files);
   guint    u_failed      = 0;
   for (GList *p_it = p_files; p_it != NULL; p_it = p_it->next) {
      GFile  *p_f   = G_FILE(p_it->data);
      GError *p_err = NULL;
      if (trash_permanently_delete(p_f, &p_err)) {
         navigator_mark_removed(p_fo->p_nav, p_f);
      } else {
         g_message("ggaze: delete failed: %s", p_err->message);
         g_clear_error(&p_err);
         u_failed++;
      }
   }
   _report_delete(p_fo, p_files, u_failed);
   if (b_was_current) {
      navigator_next(p_fo->p_nav); /* skip removed -> next live, or park */
   }
}

/* --- move -------------------------------------------------------------------
 */

/* After mover_move() (success or partial failure), dim every target that
 * actually left its original path, so the grid never shows a file that is
 * no longer where it thinks it is. Returns the count. */
static guint
_mark_moved(FileOps *p_fo, GList *p_files) {
   guint u_removed = 0;
   for (GList *p_it = p_files; p_it != NULL; p_it = p_it->next) {
      GFile *p_f = G_FILE(p_it->data);
      if (!g_file_query_exists(p_f, NULL)) {
         navigator_mark_removed(p_fo->p_nav, p_f);
         u_removed++;
      }
   }
   return (u_removed);
}

static void
_report_move(FileOps *p_fo, const SettingsPair *p_dest, gboolean b_ok,
             guint u_moved, guint u_n, const GError *p_err) {
   const char *c_why = p_err != NULL ? p_err->message : "(no detail)";
   if (b_ok) {
      _reportf(p_fo, "Moved %u file%s to %s", u_n, u_n == 1 ? "" : "s",
               p_dest->c_name);
      return;
   }
   g_message("ggaze: move to '%s' failed: %s", p_dest->c_name, c_why);
   if (u_moved > 0) {
      _reportf(p_fo, "Moved %u of %u files to %s; then failed: %s", u_moved,
               u_n, p_dest->c_name, c_why);
   } else {
      _reportf(p_fo, "Move to %s failed: %s", p_dest->c_name, c_why);
   }
}

gboolean
fileops_move(FileOps *p_fo, guint u_dest_idx, GList *p_files) {
   g_return_val_if_fail(p_fo != NULL, FALSE);
   if (p_fo->p_nav == NULL || p_fo->p_mover == NULL || p_files == NULL) {
      return (FALSE);
   }
   const GPtrArray *p_dests = mover_get_dests(p_fo->p_mover);
   if (p_dests == NULL || u_dest_idx >= p_dests->len) {
      return (FALSE);
   }
   const SettingsPair *p_dest =
      g_ptr_array_index((GPtrArray *)p_dests, u_dest_idx);
   guint u_n = g_list_length(p_files);
   /* Before mover_move/_mark_moved: navigator_mark_removed emits "changed"
    * and can move current as a side effect. */
   gboolean b_was_current = fileops_files_include_current(p_fo, p_files);
   GError  *p_err         = NULL;
   gboolean b_ok          = mover_move(p_fo->p_mover, p_files, p_dest, &p_err);
   guint    u_moved       = _mark_moved(p_fo, p_files);
   if (u_moved > 0) {
      undo_record_move(p_fo->p_undo);
   }
   /* Advance only when one of the moved files really was the current one:
    * the targets were captured at key-press time, so by the time a prompt
    * is answered current may have moved on by itself. */
   if (u_moved > 0 && b_was_current) {
      navigator_next(p_fo->p_nav);
   }
   _report_move(p_fo, p_dest, b_ok, u_moved, u_n, p_err);
   g_clear_error(&p_err);
   return (b_ok);
}

/* --- undo -------------------------------------------------------------------
 */

static void
_undo_trash(FileOps *p_fo) {
   GError *p_err = NULL;
   if (trash_restore_last(p_fo->p_trash, &p_err)) {
      navigator_rescan(p_fo->p_nav); /* re-list; restored file un-removed */
      _report(p_fo, "Restored from Trash");
      undo_reset(p_fo->p_undo);
      return;
   }
   g_message("ggaze: trash undo failed: %s",
             p_err != NULL ? p_err->message : "?");
   _reportf(p_fo, "Undo failed: %s", p_err != NULL ? p_err->message : "?");
   g_clear_error(&p_err);
}

/* The files reappear at their original path in the folder they came from,
 * so a rescan of the CURRENT folder only visibly restores them if that is
 * where they were moved from; the move itself is undone on disk either way. */
static void
_undo_move(FileOps *p_fo) {
   GError *p_err = NULL;
   if (mover_undo_last(p_fo->p_mover, &p_err)) {
      navigator_rescan(p_fo->p_nav);
      _report(p_fo, "Move undone");
      undo_reset(p_fo->p_undo);
      return;
   }
   g_message("ggaze: move undo failed: %s",
             p_err != NULL ? p_err->message : "?");
   _reportf(p_fo, "Undo failed: %s", p_err != NULL ? p_err->message : "?");
   g_clear_error(&p_err);
}

void
fileops_undo(FileOps *p_fo) {
   g_return_if_fail(p_fo != NULL);
   if (p_fo->p_nav == NULL) {
      return;
   }
   gboolean b_trash_ok = p_fo->p_trash != NULL && trash_can_undo(p_fo->p_trash);
   gboolean b_move_ok  = p_fo->p_mover != NULL && mover_can_undo(p_fo->p_mover);
   switch (undo_choose(p_fo->p_undo, b_trash_ok, b_move_ok)) {
   case GGAZE_UNDO_MOVE:
      _undo_move(p_fo);
      break;
   case GGAZE_UNDO_TRASH:
      _undo_trash(p_fo);
      break;
   default:
      _report(p_fo, "Nothing to undo");
      break;
   }
}
