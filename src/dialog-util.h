#ifndef GGAZE_DIALOG_UTIL_H
#define GGAZE_DIALOG_UTIL_H

/*:*
 * ggaze — modal alert-dialog plumbing shared by every async dialog
 *
 * gtk_alert_dialog_choose() puts up a private toplevel and keeps it only as
 * its GTask's task data. When the parent window disposes and cancels the
 * dialog, the task completes in an idle AFTER the parent's ::destroy already
 * destroyed that toplevel, and gtk_alert_dialog_choose_finish() then calls
 * gtk_window_destroy() on freed memory unless someone else still holds a
 * reference. Every module that raises such a dialog (save-gate,
 * delete-confirm) therefore grabs one right after choose(); this is the one
 * place that lookup lives.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The newest toplevel transient-for p_parent, reffed (transfer full), or
 * NULL. Called immediately after gtk_alert_dialog_choose(), when the dialog
 * just presented is that newest one by construction (anything else
 * transient-for p_parent can only be older). Measured on gtk 4.22.4. */
GtkWindow *dialog_util_newest_transient_for(GtkWindow *p_parent);

G_END_DECLS

#endif /* GGAZE_DIALOG_UTIL_H */
