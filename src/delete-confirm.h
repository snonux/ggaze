#ifndef GGAZE_DELETE_CONFIRM_H
#define GGAZE_DELETE_CONFIRM_H

/*:*
 * ggaze — bulk-delete confirm flow
 *
 * The >1-mark `D` delete opens an async GtkAlertDialog; while it is pending a
 * single-instance open / drop can replace the folder, so the dialog callback
 * must delete the targets CAPTURED at prompt time (not a re-read of the
 * navigator's marks) and only if the window still navigates the folder they
 * came from. This module owns that flow -- the captured target context, the
 * outstanding-dialog cancellable/slot, the folder-identity re-check and the
 * confirmed-delete -- so window.c's `D` path just asks it. The actual
 * permanent deletion (trash + navigator mark/advance) stays in window.c and
 * is invoked through the host ops, so the helper touches no GtkWidget and no
 * engine state directly.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct DeleteConfirm DeleteConfirm;

/* Window-side operations the helper calls back through. p_host is the window
 * (borrowed for the duration of each call). */
typedef struct {
   /* Transient status line (the window's info overlay). */
   void (*show_status)(gpointer p_host, const char *c_msg);
   /* The GtkAlertDialog's own toplevel, ref'd so it is not freed under the
    * GTask that still points at it (transfer-full). May return NULL. */
   GtkWindow *(*alert_dialog_window)(gpointer p_host);
   /* Permanently delete the captured targets (transfer-none for p_files;
    * borrowed for the call) and advance the cursor if one was current. */
   void (*perform_delete)(gpointer p_host, GList *p_files);
   /* The folder the window currently navigates (borrowed; NULL if none), for
    * the folder-identity re-check. */
   GFile *(*current_dir)(gpointer p_host);
} DeleteConfirmHostOps;

DeleteConfirm *delete_confirm_new(const DeleteConfirmHostOps *p_ops,
                                  gpointer                    p_host);
void           delete_confirm_delete(DeleteConfirm *p_dc);

/* TRUE iff a confirm dialog is currently outstanding (the window refuses a
 * native close while one is up). */
gboolean delete_confirm_outstanding(DeleteConfirm *p_dc);

/* Open the >1-target confirm dialog for p_files (borrowed). One dialog at a
 * time; a no-op (with a status line) if a target has no parent folder. */
void delete_confirm_ask(DeleteConfirm *p_dc, GList *p_files);

/* Cancel + drop any outstanding dialog (window dispose / close). */
void delete_confirm_dispose(DeleteConfirm *p_dc);

/* TRUE iff p_host still navigates p_dir (the captured targets' folder), so a
 * pending confirm may safely delete them. FALSE if the folder was replaced. */
gboolean delete_confirm_targets_still_current(DeleteConfirm *p_dc,
                                              GFile         *p_dir);

/* Delete EXACTLY p_files (borrowed) iff p_host still navigates p_dir;
 * otherwise refuse (no files touched). Returns TRUE iff the delete
 * proceeded. */
gboolean delete_confirm_captured(DeleteConfirm *p_dc, GFile *p_dir,
                                 GList *p_files);

G_END_DECLS

#endif /* GGAZE_DELETE_CONFIRM_H */