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
   /* Background trash / delete (see "background jobs" below). */
   GThreadPool *p_pool;     /* one worker, created on first use */
   GAsyncQueue *p_done;     /* JobDone*: worker -> main thread */
   GHashTable  *p_busy;     /* GFile* (owned refs): queued or in flight */
   gint         i_running;  /* jobs the worker has not finished (atomic) */
   guint        u_drain_id; /* main-thread poll of p_done, 0 = idle */
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
   if (p_fo == NULL) {
      return;
   }
   p_fo->p_nav = NULL; /* whatever is still in flight has nobody to tell */
   fileops_flush(p_fo);
   if (p_fo->p_pool != NULL) {
      g_thread_pool_free(p_fo->p_pool, FALSE, TRUE);
   }
   g_clear_pointer(&p_fo->p_done, g_async_queue_unref);
   g_clear_pointer(&p_fo->p_busy, g_hash_table_unref);
   g_free(p_fo);
}

/* Flushes first: a job still running was started on the folder being left
 * and borrows its Trash, which the caller frees right after this. */
void
fileops_set_folder(FileOps *p_fo, Navigator *p_nav, Trash *p_trash) {
   if (p_fo == NULL) {
      return; /* GObject dispose may run twice; the second pass has freed us */
   }
   fileops_flush(p_fo);
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

/* --- background jobs --------------------------------------------------------
 *
 * `d` and `D` from the window: the disk work (a rename into .Trash, an
 * unlink per file -- slow on a network mount or for a few hundred marked
 * files) runs on ONE worker thread, in key-press order, so the UI never
 * waits for it. Everything else stays on the main thread: the worker hands
 * each finished file back through p_done, and a short poll that only runs
 * while jobs are pending applies it -- dim the cell, advance past it if it
 * was current, record the undo, say what happened -- exactly as the
 * synchronous functions above do, file by file. A file is dimmed within
 * milliseconds of leaving the disk, well inside the folder monitor's
 * debounce, so its cell is already "removed" (and keeps its place) when
 * the rescan for it comes.
 *
 * The Trash engine is not thread-safe and the worker borrows it, so the
 * main thread touches it only after fileops_flush(): undo, a folder
 * change, the window's Empty Trash. The flush waits for the worker (a few
 * file operations) and applies what it handed back. */

typedef struct {
   Navigator  *p_nav;    /* owned ref: the folder the job was started in */
   Trash      *p_trash;  /* borrowed; NULL for a permanent delete */
   GList      *p_files;  /* GFile* (owned refs), in order */
   GHashTable *p_set;    /* the same files as a set (borrowed from p_files) */
   gboolean    b_binned; /* worker: a file of this job is in the Trash's
                          * undo batch already (the next one joins it) */
   guint u_failed;       /* main thread: files that could not be removed */
   char *c_error;        /* main thread: the first failure's text (owned) */
} Job;

typedef struct {
   Job     *p_job;
   GFile   *p_file; /* borrowed from the job */
   gboolean b_ok;
   char    *c_error; /* owned, NULL on success */
   gboolean b_last;  /* the job's last file: report, then free the job */
} JobDone;

static void
_job_free(Job *p_job) {
   g_object_unref(p_job->p_nav);
   g_hash_table_unref(p_job->p_set);
   g_list_free_full(p_job->p_files, (GDestroyNotify)g_object_unref);
   g_free(p_job->c_error);
   g_free(p_job);
}

/* Worker thread: remove the job's files one by one, handing each result to
 * the main thread as it happens. Touches nothing of the FileOps but the
 * queue and the running count. */
static void
_job_run(gpointer p_data, gpointer p_user) {
   Job     *p_job = (Job *)p_data;
   FileOps *p_fo  = (FileOps *)p_user;
   for (GList *p_it = p_job->p_files; p_it != NULL; p_it = p_it->next) {
      GFile   *p_f   = G_FILE(p_it->data);
      GError  *p_err = NULL;
      JobDone *p_d   = g_new0(JobDone, 1);
      p_d->p_job     = p_job;
      p_d->p_file    = p_f;
      if (p_job->p_trash == NULL) {
         p_d->b_ok = trash_permanently_delete(p_f, &p_err);
      } else if (p_job->b_binned) {
         p_d->b_ok = trash_bin_more(p_job->p_trash, p_f, &p_err);
      } else {
         /* The job's first file to reach the bin starts the undo batch. */
         p_d->b_ok       = trash_bin(p_job->p_trash, p_f, &p_err);
         p_job->b_binned = p_d->b_ok;
      }
      p_d->c_error = (p_err != NULL) ? g_strdup(p_err->message) : NULL;
      p_d->b_last  = (p_it->next == NULL);
      g_clear_error(&p_err);
      g_async_queue_push(p_fo->p_done, p_d);
   }
   g_atomic_int_dec_and_test(&p_fo->i_running);
}

/* Main thread: the job's last file is done -- say how it went. */
static void
_job_report(FileOps *p_fo, Job *p_job) {
   if (p_job->p_trash == NULL) {
      _report_delete(p_fo, p_job->p_files, p_job->u_failed);
      return;
   }
   guint u_n = g_list_length(p_job->p_files);
   if (p_job->u_failed > 0 && u_n > 1) {
      _reportf(p_fo, "Trash failed for %u of %u files: %s", p_job->u_failed,
               u_n, p_job->c_error != NULL ? p_job->c_error : "?");
   } else if (u_n > 1) {
      _reportf(p_fo, "Trashed %u files — u to undo", u_n);
   } else {
      char *c_name = g_file_get_basename(G_FILE(p_job->p_files->data));
      if (p_job->u_failed == 0) {
         _reportf(p_fo, "Trashed %s — u to undo", c_name);
      } else {
         _reportf(p_fo, "Trash failed for %s: %s", c_name,
                  p_job->c_error != NULL ? p_job->c_error : "?");
      }
      g_free(c_name);
   }
}

/* TRUE iff the entry at i_idx can take the cursor for good: listed, not
 * removed, and not one of p_job's files (those are on their way out). */
static gboolean
_can_take_cursor(FileOps *p_fo, Job *p_job, gint i_idx) {
   if (i_idx < 0 || (guint)i_idx >= navigator_get_count(p_fo->p_nav)) {
      return (FALSE);
   }
   GFile *p_f = navigator_get_file(p_fo->p_nav, (guint)i_idx);
   return (p_f != NULL && !navigator_is_removed(p_fo->p_nav, p_f) &&
           !g_hash_table_contains(p_job->p_set, p_f));
}

/* The current file was just removed by p_job: put the cursor on the
 * NEAREST entry that stays -- the next one when both neighbours are equally
 * near, which is the plain "trash, then next" of a single `d`. For a
 * multi-selection that keeps the cursor beside where it was: stepping to
 * the next live entry instead walked it through the files still being
 * removed and left it behind the whole block, rows away from a selection
 * that had been made upwards, and the view scrolled after it. With nothing
 * left to go to, navigator_next parks the cursor. */
static void
_leave_removed_current(FileOps *p_fo, Job *p_job) {
   gint i_cur = navigator_get_current_index(p_fo->p_nav);
   gint i_n   = (gint)navigator_get_count(p_fo->p_nav);
   for (gint i_d = 1; i_cur >= 0 && i_d < i_n; i_d++) {
      gint i_to = _can_take_cursor(p_fo, p_job, i_cur + i_d)   ? i_cur + i_d
                  : _can_take_cursor(p_fo, p_job, i_cur - i_d) ? i_cur - i_d
                                                               : -1;
      if (i_to >= 0) {
         navigator_set_current(p_fo->p_nav, (guint)i_to);
         return;
      }
   }
   navigator_next(p_fo->p_nav); /* skip removed -> next live, or park */
}

/* Main thread: one file of a job is done. Applied only while the folder
 * the job was started in is still the open one; after a folder change the
 * file is removed on disk all the same and there is nothing to update. */
static void
_job_apply(FileOps *p_fo, JobDone *p_d) {
   Job     *p_job  = p_d->p_job;
   gboolean b_here = (p_fo->p_nav != NULL && p_fo->p_nav == p_job->p_nav);
   g_hash_table_remove(p_fo->p_busy, p_d->p_file);
   if (!p_d->b_ok) {
      g_message("ggaze: remove failed: %s",
                p_d->c_error != NULL ? p_d->c_error : "?");
      p_job->u_failed++;
      if (p_job->c_error == NULL) {
         p_job->c_error = g_steal_pointer(&p_d->c_error);
      }
   } else if (b_here) {
      /* Before the removal is shown: navigator_mark_removed emits
       * "changed" and can move current as a side effect. */
      GFile   *p_cur = navigator_get_current(p_fo->p_nav);
      gboolean b_was_current =
         (p_cur != NULL && g_file_equal(p_cur, p_d->p_file));
      navigator_mark_removed(p_fo->p_nav, p_d->p_file); /* dim */
      if (b_was_current) {
         _leave_removed_current(p_fo, p_job);
      }
      if (p_job->p_trash != NULL) {
         undo_record_trash(p_fo->p_undo);
      }
   }
   if (p_d->b_last) {
      if (b_here) {
         _job_report(p_fo, p_job);
      }
      _job_free(p_job);
   }
   g_free(p_d->c_error);
   g_free(p_d);
}

/* Main thread: apply everything the worker has handed back so far. */
static void
_drain(FileOps *p_fo) {
   JobDone *p_d;
   while (p_fo->p_done != NULL &&
          (p_d = (JobDone *)g_async_queue_try_pop(p_fo->p_done)) != NULL) {
      _job_apply(p_fo, p_d);
   }
}

static gboolean
_drain_tick(gpointer p_data) {
   FileOps *p_fo = (FileOps *)p_data;
   /* Read before the drain: a job that finishes in between is caught by
    * the next tick, never lost. */
   gboolean b_running = g_atomic_int_get(&p_fo->i_running) > 0;
   _drain(p_fo);
   if (b_running) {
      return (G_SOURCE_CONTINUE);
   }
   p_fo->u_drain_id = 0;
   return (G_SOURCE_REMOVE);
}

/* How often the main thread looks for finished files while a job runs. */
#define GGAZE_FILEOPS_POLL_MS 10

/* Queue p_files (transfer full) for removal on the worker: into p_trash,
 * or permanently for NULL. */
static void
_job_start(FileOps *p_fo, Trash *p_trash, GList *p_files) {
   if (p_fo->p_pool == NULL) {
      p_fo->p_pool = g_thread_pool_new(_job_run, p_fo, 1, TRUE, NULL);
      p_fo->p_done = g_async_queue_new();
   }
   Job *p_job     = g_new0(Job, 1);
   p_job->p_nav   = (Navigator *)g_object_ref(p_fo->p_nav);
   p_job->p_trash = p_trash;
   p_job->p_files = p_files;
   p_job->p_set =
      g_hash_table_new((GHashFunc)g_file_hash, (GEqualFunc)g_file_equal);
   for (GList *p_it = p_files; p_it != NULL; p_it = p_it->next) {
      g_hash_table_add(p_job->p_set, p_it->data);
   }
   g_atomic_int_inc(&p_fo->i_running);
   g_thread_pool_push(p_fo->p_pool, p_job, NULL);
   if (p_fo->u_drain_id == 0) {
      p_fo->u_drain_id =
         g_timeout_add(GGAZE_FILEOPS_POLL_MS, _drain_tick, p_fo);
   }
}

/* The files of p_files that are not already queued or in flight, each
 * noted as busy now (transfer full). A second `d` on a file whose first one
 * has not landed yet (key repeat) must not queue it again: the second job
 * could only fail on a file that is gone. */
static GList *
_claim_files(FileOps *p_fo, GList *p_files) {
   if (p_fo->p_busy == NULL) {
      p_fo->p_busy =
         g_hash_table_new_full((GHashFunc)g_file_hash, (GEqualFunc)g_file_equal,
                               (GDestroyNotify)g_object_unref, NULL);
   }
   GList *p_out = NULL;
   for (GList *p_it = p_files; p_it != NULL; p_it = p_it->next) {
      if (g_hash_table_add(p_fo->p_busy, g_object_ref(p_it->data))) {
         p_out = g_list_prepend(p_out, g_object_ref(p_it->data));
      }
   }
   return (g_list_reverse(p_out));
}

void
fileops_flush(FileOps *p_fo) {
   g_return_if_fail(p_fo != NULL);
   while (g_atomic_int_get(&p_fo->i_running) > 0) {
      g_usleep(200); /* the worker is on a handful of file operations */
   }
   _drain(p_fo);
   if (p_fo->u_drain_id != 0) {
      g_source_remove(p_fo->u_drain_id);
      p_fo->u_drain_id = 0;
   }
}

/* The files of p_files (transfer full, consumed) that are still in the open
 * folder; the others are un-claimed again and dropped. */
static GList *
_keep_present(FileOps *p_fo, GList *p_files) {
   GList *p_out = NULL;
   for (GList *p_it = p_files; p_it != NULL; p_it = p_it->next) {
      GFile *p_f = G_FILE(p_it->data);
      if (fileops_target_still_in_folder(p_fo, p_f)) {
         p_out = g_list_prepend(p_out, g_object_ref(p_f));
      } else {
         g_hash_table_remove(p_fo->p_busy, p_f);
      }
   }
   g_list_free_full(p_files, (GDestroyNotify)g_object_unref);
   return (g_list_reverse(p_out));
}

void
fileops_trash_async(FileOps *p_fo, GList *p_files) {
   g_return_if_fail(p_fo != NULL);
   if (p_fo->p_nav == NULL || p_fo->p_trash == NULL || p_files == NULL) {
      return;
   }
   GList *p_claimed = _claim_files(p_fo, p_files);
   if (p_claimed == NULL) {
      return; /* already on their way to the bin */
   }
   p_claimed = _keep_present(p_fo, p_claimed);
   if (p_claimed == NULL) {
      _report(p_fo, "Nothing trashed — the file is gone");
      return;
   }
   _job_start(p_fo, p_fo->p_trash, p_claimed);
}

void
fileops_delete_files_async(FileOps *p_fo, GList *p_files) {
   g_return_if_fail(p_fo != NULL);
   if (p_fo->p_nav == NULL || p_files == NULL) {
      return;
   }
   GList *p_claimed = _claim_files(p_fo, p_files);
   if (p_claimed != NULL) {
      _job_start(p_fo, NULL, p_claimed);
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
   fileops_flush(p_fo); /* a `d` still in flight is what `u` is about */
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
