/* delete-confirm.c — bulk-delete confirm flow (see delete-confirm.h). */
#include "delete-confirm.h"

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

/* Button indices of the confirm dialog, in the order delete_confirm_ask hands
 * them to gtk_alert_dialog_set_buttons(). */
enum {
   _DELETE_BTN_CANCEL = 0,
   _DELETE_BTN_DELETE = 1
};

struct DeleteConfirm {
   const DeleteConfirmHostOps *p_ops;
   gpointer                    p_host;
   GCancellable *p_cancel; /* NULL when no dialog is outstanding */
};

/* Captured at prompt time: the targets' parent folder + the deep-copied
 * target list + the dialog's cancellable + the dialog's own toplevel (kept
 * alive for the GTask). Owns every ref so it outlives a folder swap or a
 * window dispose. */
typedef struct {
   DeleteConfirm *p_dc;   /* to clear its p_cancel slot on completion */
   gpointer       p_host; /* the window, OWNED ref: outlives the async dialog */
   GFile         *p_dir;  /* captured targets' own parent folder (owned) */
   GList         *p_files;  /* captured target GFile* list (owned) */
   GCancellable  *p_cancel; /* the ref handed to gtk_alert_dialog_choose() */
   GtkWindow     *p_dlg_window; /* ref on the dialog's toplevel (owned) */
} _DeleteCtx;

static GList *
_files_copy(GList *p_files) {
   GList *p_out = NULL;
   for (GList *p_it = p_files; p_it != NULL; p_it = p_it->next) {
      p_out = g_list_prepend(p_out, g_object_ref(G_FILE(p_it->data)));
   }
   return (g_list_reverse(p_out));
}

static void
_delete_ctx_free(_DeleteCtx *p_ctx) {
   if (p_ctx == NULL) {
      return;
   }
   g_clear_object(&p_ctx->p_dir);
   g_list_free_full(p_ctx->p_files, (GDestroyNotify)g_object_unref);
   g_clear_object(&p_ctx->p_cancel);
   g_clear_object(&p_ctx->p_dlg_window);
   if (p_ctx->p_host != NULL) {
      g_object_unref(p_ctx->p_host);
      p_ctx->p_host = NULL;
   }
   g_free(p_ctx);
}

/* Did the user really press "Delete"? i_btn / p_err are what
 * gtk_alert_dialog_choose_finish() returned: every non-answer (-1, a
 * G_IO_ERROR_CANCELLED or GTK_DIALOG_ERROR_DISMISSED) must NOT be read as a
 * confirmed permanent delete (task aw0), so the error is checked first and
 * only the "Delete" button's own index counts. */
static gboolean
_answered_yes(int i_btn, const GError *p_err) {
   return (p_err == NULL && i_btn == _DELETE_BTN_DELETE);
}

static void
_choose_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   GtkAlertDialog *p_dlg = GTK_ALERT_DIALOG(p_src);
   _DeleteCtx     *p_ctx = (_DeleteCtx *)p_data;
   GError         *p_err = NULL;
   int             i_btn = gtk_alert_dialog_choose_finish(p_dlg, p_res, &p_err);
   /* This dialog is gone, and so is its cancellable's job: a later `D` may
    * open a fresh one, which brings its own. Clearing the slot here is safe
    * even after delete_confirm_dispose already cleared it -- the ctx keeps
    * the cancellable alive until _delete_ctx_free. */
   g_clear_object(&p_ctx->p_dc->p_cancel);
   if (_answered_yes(i_btn, p_err)) {
      /* Delete the captured targets, NOT a re-read of marks: if the folder
       * was replaced while this dialog was pending, the safety check inside
       * delete_confirm_captured refuses and no files are touched. */
      delete_confirm_captured(p_ctx->p_dc, p_ctx->p_dir, p_ctx->p_files);
   }
   g_clear_error(&p_err);
   _delete_ctx_free(p_ctx);
}

DeleteConfirm *
delete_confirm_new(const DeleteConfirmHostOps *p_ops, gpointer p_host) {
   g_return_val_if_fail(p_ops != NULL, NULL);
   DeleteConfirm *p_dc = g_new0(DeleteConfirm, 1);
   p_dc->p_ops         = p_ops;
   p_dc->p_host        = p_host;
   return (p_dc);
}

void
delete_confirm_delete(DeleteConfirm *p_dc) {
   if (p_dc == NULL) {
      return;
   }
   g_clear_object(&p_dc->p_cancel);
   g_free(p_dc);
}

gboolean
delete_confirm_outstanding(DeleteConfirm *p_dc) {
   g_return_val_if_fail(p_dc != NULL, FALSE);
   return (p_dc->p_cancel != NULL);
}

void
delete_confirm_dispose(DeleteConfirm *p_dc) {
   g_return_if_fail(p_dc != NULL);
   if (p_dc->p_cancel != NULL) {
      g_cancellable_cancel(p_dc->p_cancel);
      g_clear_object(&p_dc->p_cancel);
   }
}

gboolean
delete_confirm_targets_still_current(DeleteConfirm *p_dc, GFile *p_dir) {
   g_return_val_if_fail(p_dc != NULL, FALSE);
   g_return_val_if_fail(G_IS_FILE(p_dir), FALSE);
   GFile *p_now = p_dc->p_ops->current_dir(p_dc->p_host);
   if (p_now == NULL) {
      return (FALSE);
   }
   return (g_file_equal(p_now, p_dir));
}

gboolean
delete_confirm_captured(DeleteConfirm *p_dc, GFile *p_dir, GList *p_files) {
   g_return_val_if_fail(p_dc != NULL, FALSE);
   g_return_val_if_fail(G_IS_FILE(p_dir), FALSE);
   if (!delete_confirm_targets_still_current(p_dc, p_dir)) {
      g_warning("ggaze: bulk delete refused \u2014 the folder was replaced "
                "while the confirm dialog was pending; no files deleted.");
      return (FALSE);
   }
   p_dc->p_ops->perform_delete(p_dc->p_host, p_files);
   return (TRUE);
}

void
delete_confirm_ask(DeleteConfirm *p_dc, GList *p_files) {
   g_return_if_fail(p_dc != NULL);
   if (p_files == NULL) {
      return;
   }
   if (delete_confirm_outstanding(p_dc)) {
      /* One confirm at a time, so the slot always names THE dialog on screen
       * -- unreachable through the UI (modal + input-driven `D`), but it is
       * what makes that invariant structural. */
      return;
   }
   /* The guard folder comes from the CAPTURED targets, not the live
    * navigator: the question the callback answers is "do these targets still
    * belong to the folder they came from". */
   GFile *p_dir = g_file_get_parent(G_FILE(p_files->data)); /* owned */
   if (p_dir == NULL) {
      p_dc->p_ops->show_status(p_dc->p_host,
                               "Nothing deleted \u2014 the folder is gone");
      return;
   }
   GtkAlertDialog *p_dlg = gtk_alert_dialog_new(
      "Permanently delete %u marked images?", g_list_length(p_files));
   gtk_alert_dialog_set_buttons(p_dlg,
                                (const char *[]){"Cancel", "Delete", NULL});
   /* Escape / WM close arrive as Cancel instead of DISMISSED. Belt-and-braces
    * with _answered_yes: on a destructive dialog, "the user got rid of the
    * question" must mean no. No default button: Enter must not confirm a
    * permanent delete. */
   gtk_alert_dialog_set_cancel_button(p_dlg, _DELETE_BTN_CANCEL);
   _DeleteCtx *p_ctx   = g_new(_DeleteCtx, 1);
   p_ctx->p_dc         = p_dc;
   p_ctx->p_host       = g_object_ref(p_dc->p_host); /* outlives the dialog */
   p_ctx->p_dir        = p_dir;                      /* transfer full */
   p_ctx->p_files      = _files_copy(p_files);       /* owned by the ctx */
   p_ctx->p_cancel     = g_cancellable_new();
   p_ctx->p_dlg_window = NULL;
   /* The slot is NULL here (the outstanding-guard above let us past), so this
    * assignment never drops a ref on the floor. Written BEFORE choose(), so
    * the gate is closed before anything can react to the dialog. */
   p_dc->p_cancel = (GCancellable *)g_object_ref(p_ctx->p_cancel);
   gtk_alert_dialog_choose(p_dlg, GTK_WINDOW(p_dc->p_host), p_ctx->p_cancel,
                           _choose_cb, p_ctx);
   p_ctx->p_dlg_window = p_dc->p_ops->alert_dialog_window(p_dc->p_host);
   g_object_unref(p_dlg);
}