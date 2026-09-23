/*:*
 * ggaze — shared GTK test helpers (see gtk_helpers.h)
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "gtk_helpers.h"

#include <glib.h>
#include <gtk/gtk.h>

#include "wait_until.h"

/* --- grid cells ---------------------------------------------------------- */

GtkFlowBox *
ggtest_find_flow_box(GtkWidget *p_w) {
   if (GTK_IS_FLOW_BOX(p_w)) {
      return (GTK_FLOW_BOX(p_w));
   }
   for (GtkWidget *p_c = gtk_widget_get_first_child(p_w); p_c != NULL;
        p_c            = gtk_widget_get_next_sibling(p_c)) {
      GtkFlowBox *p_f = ggtest_find_flow_box(p_c);
      if (p_f != NULL) {
         return (p_f);
      }
   }
   return (NULL);
}

void
ggtest_activate_cell(GgazeGrid *p_grid, gint i_idx) {
   GtkFlowBox *p_flow = ggtest_find_flow_box(GTK_WIDGET(p_grid));
   g_assert_nonnull(p_flow);
   GtkFlowBoxChild *p_child = gtk_flow_box_get_child_at_index(p_flow, i_idx);
   g_assert_nonnull(p_child);
   g_signal_emit_by_name(p_flow, "child-activated", p_child);
}

/* --- window focus --------------------------------------------------------- */

/* The viewer on p_win's stack. Asserted rather than returned NULL, so a
 * window without a "large" page fails here by name instead of as a NULL
 * dereference inside whichever wait or grab asked for it. */
static GgazeViewer *
_viewer_of(GgazeWindow *p_win) {
   GtkStack *p_stack = ggaze_window_get_stack(p_win);
   g_assert_nonnull(p_stack);
   GtkWidget *p_large = gtk_stack_get_child_by_name(p_stack, "large");
   g_assert_nonnull(p_large);
   g_assert_true(GGAZE_IS_VIEWER(p_large));
   return (GGAZE_VIEWER(p_large));
}

void
ggtest_focus_viewer(GgazeWindow *p_win) {
   GtkWidget *p_large = GTK_WIDGET(_viewer_of(p_win));
   /* GgazeViewer calls gtk_widget_set_focusable(TRUE) in its init, so the
    * grab succeeds on an unmapped, never-presented window too. Asserted
    * rather than ignored: if the viewer ever stops being focusable, the
    * failure should surface here and not as a backend-specific abort inside
    * an unrelated popover subtest. See gtk_helpers.h "window focus". */
   g_assert_true(gtk_widget_grab_focus(p_large));
}

/* --- alert dialogs -------------------------------------------------------- */

void
ggtest_drain_main(guint u_ms) {
   for (guint u = 0; u < u_ms; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
}

GtkWidget *
ggtest_find_button(GtkWidget *p_root, const char *c_label) {
   if (GTK_IS_BUTTON(p_root)) {
      const char *c_have = gtk_button_get_label(GTK_BUTTON(p_root));
      if (c_have != NULL && g_strcmp0(c_have, c_label) == 0) {
         return (p_root);
      }
   }
   for (GtkWidget *p_c = gtk_widget_get_first_child(p_root); p_c != NULL;
        p_c            = gtk_widget_get_next_sibling(p_c)) {
      GtkWidget *p_hit = ggtest_find_button(p_c, c_label);
      if (p_hit != NULL) {
         return (p_hit);
      }
   }
   return (NULL);
}

/* Single walk over the toplevel list behind all three public queries: counts
 * the toplevels other than p_skip that own a button labelled c_label and, for
 * the first of them, hands back the window (*pp_win) and that button
 * (*pp_btn). Either out-parameter may be NULL. */
static guint
_scan_dialogs(GtkWindow *p_skip, const char *c_label, GtkWindow **pp_win,
              GtkWidget **pp_btn) {
   GList *p_tops = gtk_window_list_toplevels();
   guint  u_n    = 0;
   for (GList *p_l = p_tops; p_l != NULL; p_l = p_l->next) {
      GtkWidget *p_top = GTK_WIDGET(p_l->data);
      if (p_skip != NULL && p_top == GTK_WIDGET(p_skip)) {
         continue;
      }
      GtkWidget *p_btn = ggtest_find_button(p_top, c_label);
      if (p_btn == NULL) {
         continue;
      }
      if (u_n == 0) {
         if (pp_win != NULL) {
            *pp_win = GTK_WINDOW(p_top);
         }
         if (pp_btn != NULL) {
            *pp_btn = p_btn;
         }
      }
      u_n++;
   }
   g_list_free(p_tops);
   return (u_n);
}

GtkWindow *
ggtest_find_dialog(GtkWindow *p_skip, const char *c_label) {
   GtkWindow *p_dlg = NULL;
   _scan_dialogs(p_skip, c_label, &p_dlg, NULL);
   return (p_dlg);
}

guint
ggtest_count_dialogs(GtkWindow *p_skip, const char *c_label) {
   return (_scan_dialogs(p_skip, c_label, NULL, NULL));
}

/* Patience ceiling for ggtest_wait_for_dialog(), in milliseconds, before the
 * scaling below. Not a "how long this normally takes" figure -- a prompt
 * normally shows within a few hundred ms -- but "how long we are willing to
 * wait before calling it a bug". */
#define GGTEST_DIALOG_WAIT_MS 10000

typedef struct {
   GtkWindow  *p_skip;
   const char *c_label;
   GtkWindow  *p_dlg; /* the dialog once found */
} DialogWait;

/* GgtestCondFn for ggtest_wait_for_dialog(): the dialog is up. */
static gboolean
_dialog_is_up(gpointer p_data) {
   DialogWait *p_w = (DialogWait *)p_data;
   p_w->p_dlg      = ggtest_find_dialog(p_w->p_skip, p_w->c_label);
   return (p_w->p_dlg != NULL);
}

/* Poll until the dialog is up, or until the ceiling, scaled by
 * ggtest_wait_scale() (wait_until.c: sanitizer lanes and
 * GGAZE_TEST_TIMEOUT_SCALE), expires.
 *
 * The budget is a real monotonic deadline -- ggtest_wait_until()'s, which
 * applies the scaling. It used to be a count of poll iterations, each
 * followed by a 1 ms sleep, which made it load-dependent in
 * the worst possible direction: a *sleeping* poll loop is barely slowed by an
 * oversubscribed box, while the worker thread it is waiting on is starved of
 * CPU by exactly that oversubscription. So the loop kept roughly its nominal
 * 2 s while the work it waited for took far longer, and a wait that always
 * passed with the suite run alone failed on a full parallel lane (1w0).
 *
 * Widening a CONDITION wait cannot hide a defect: this returns the instant the
 * dialog exists, so a dialog that is genuinely never shown still fails the
 * caller's g_assert_nonnull, just later. The ceiling only sets how long we
 * wait before saying so, and only one wait per run can ever burn it -- the
 * assertion aborts the suite. */
GtkWindow *
ggtest_wait_for_dialog(GtkWindow *p_skip, const char *c_label) {
   DialogWait s_w = {.p_skip = p_skip, .c_label = c_label, .p_dlg = NULL};
   ggtest_wait_until(_dialog_is_up, &s_w,
                     GGTEST_DIALOG_WAIT_MS * G_GINT64_CONSTANT(1000));
   return (s_w.p_dlg);
}

void
ggtest_click_button(GtkWidget *p_btn) {
   g_assert_true(GTK_IS_BUTTON(p_btn));
   g_signal_emit_by_name(p_btn, "clicked");
}

gboolean
ggtest_is_open_toplevel(GtkWindow *p_win) {
   GList   *p_tops = gtk_window_list_toplevels();
   gboolean b_open = (g_list_find(p_tops, p_win) != NULL);
   g_list_free(p_tops);
   return (b_open);
}

gboolean
ggtest_click_dialog_button(GtkWindow *p_skip, const char *c_label) {
   /* Wait for the button instead of sampling the toplevel list once. Dialogs
    * are raised asynchronously, so a bare check races the main loop exactly
    * as the old iteration-counted wait did -- observed as a load-dependent
    * "should be TRUE" failure on a full parallel lane (1w0). A button that is
    * genuinely never shown still returns FALSE, just after the ceiling. */
   if (ggtest_wait_for_dialog(p_skip, c_label) == NULL) {
      return (FALSE);
   }
   GtkWidget *p_btn = NULL;
   if (_scan_dialogs(p_skip, c_label, NULL, &p_btn) == 0) {
      return (FALSE);
   }
   ggtest_click_button(p_btn);
   return (TRUE);
}

/* See gtk_helpers.h for why this reports the way it does. The GString is
 * never freed: g_error() does not return. */
GtkWindow *
ggtest_assert_dialog_up_at(const char *c_loc, GtkWindow *p_own,
                           const char *c_button) {
   GtkWindow *p_dlg = ggtest_wait_for_dialog(p_own, c_button);
   if (p_dlg != NULL) {
      return (p_dlg);
   }
   GString *p_msg  = g_string_new(NULL);
   GList   *p_tops = gtk_window_list_toplevels();
   for (GList *p_l = p_tops; p_l != NULL; p_l = p_l->next) {
      g_string_append_printf(p_msg, " %s%s", G_OBJECT_TYPE_NAME(p_l->data),
                             (p_l->data == p_own) ? "(own)" : "");
   }
   g_list_free(p_tops);
   g_error("%s: no toplevel carries a \"%s\" button; toplevels present:%s",
           c_loc, c_button, p_msg->str);
}

/* --- large-view readiness ------------------------------------------------- */

/* Patience ceiling for both large-view waits, in milliseconds before
 * ggtest_wait_until()'s scaling (x3 under ASan, so up to 30 s there, which is
 * why tests/meson.build gives the suites that call it a longer timeout than
 * meson's 30 s default). A fixture decodes and a toplevel gets its first
 * configure within a few hundred milliseconds; the ceiling is how long we wait
 * before calling a missing one a bug. */
#define GGTEST_VIEW_WAIT_MS 10000

/* Consecutive polls the viewer's allocation must hold still for before it
 * counts as settled; each poll is separated from the last by one main-context
 * iteration and a 1 ms sleep. That is a COUNT, not a duration, so under load
 * it bounds nothing in time: it guards against a relayout that is already
 * queued when the texture lands (a prompt one), not against one that arrives
 * later. See gtk_helpers.h "large-view readiness". */
#define GGTEST_VIEW_SETTLE_POLLS 30

/* What the viewer shows at one instant: its allocation and the size of the
 * texture it holds (0x0 while it holds none). */
typedef struct {
   int i_w;
   int i_h;
   int i_tex_w;
   int i_tex_h;
} ViewState;

/* One view wait in progress: the GgtestCondFn state of both large-view waits.
 * b_settle selects whether the allocation must also be non-empty and still. */
typedef struct {
   GgazeViewer *p_v;
   int          i_want_w; /* the decoded texture size waited for */
   int          i_want_h;
   gboolean     b_settle;
   ViewState    last;    /* what the previous poll saw */
   guint        u_still; /* consecutive polls with an unchanged allocation */
} ViewWait;

static ViewState
_view_state(GgazeViewer *p_v) {
   ViewState   s     = {0};
   GdkTexture *p_tex = ggaze_viewer_get_texture(p_v);
   s.i_w             = gtk_widget_get_width(GTK_WIDGET(p_v));
   s.i_h             = gtk_widget_get_height(GTK_WIDGET(p_v));
   if (p_tex != NULL) {
      s.i_tex_w = gdk_texture_get_width(p_tex);
      s.i_tex_h = gdk_texture_get_height(p_tex);
   }
   return (s);
}

/* GgtestCondFn for both waits. With b_settle, TRUE once the wanted texture has
 * sat in the same non-empty allocation for GGTEST_VIEW_SETTLE_POLLS polls; any
 * change (texture or allocation) restarts the count, so only an unbroken run
 * of fully decoded, unchanged polls counts. Without it, TRUE as soon as the
 * texture has the wanted size. */
static gboolean
_view_ready(gpointer p_data) {
   ViewWait *p_w = (ViewWait *)p_data;
   ViewState now = _view_state(p_w->p_v);
   gboolean  b_tex =
      (now.i_tex_w == p_w->i_want_w && now.i_tex_h == p_w->i_want_h);
   gboolean b_held = b_tex && now.i_w > 0 && now.i_h > 0 &&
                     now.i_w == p_w->last.i_w && now.i_h == p_w->last.i_h;
   /* Counted for both waits for simplicity; only the settling wait reads
    * it -- the texture-only wait returns on b_tex below. */
   p_w->u_still = b_held ? p_w->u_still + 1 : 0;
   p_w->last    = now;
   if (!p_w->b_settle) {
      return (b_tex);
   }
   return (p_w->u_still >= GGTEST_VIEW_SETTLE_POLLS);
}

/* Shared body of the two public waits: poll _view_ready, g_error() naming the
 * call site and what was last seen if the ceiling expires. On success the
 * settling wait also asserts the stack really shows the viewer -- a scale
 * read off a hidden page is as meaningless as one off an unallocated viewer.
 * The texture-only wait does not: it only promises a texture, and a caller
 * may legitimately wait for a decode while another page is on screen. */
static GgazeViewer *
_wait_view(const char *c_loc, GgazeWindow *p_win, int i_tex_w, int i_tex_h,
           gboolean b_settle) {
   ViewWait  s_w     = {.p_v      = _viewer_of(p_win),
                        .i_want_w = i_tex_w,
                        .i_want_h = i_tex_h,
                        .b_settle = b_settle};
   GtkStack *p_stack = ggaze_window_get_stack(p_win);
   gint64    i_start = g_get_monotonic_time();
   if (!ggtest_wait_until(_view_ready, &s_w,
                          GGTEST_VIEW_WAIT_MS * G_GINT64_CONSTANT(1000))) {
      g_error("%s: large view not ready after %" G_GINT64_FORMAT
              " ms: stack shows \"%s\", viewer allocated %dx%d, texture "
              "%dx%d (wanted %dx%d%s)",
              c_loc, (g_get_monotonic_time() - i_start) / 1000,
              gtk_stack_get_visible_child_name(p_stack), s_w.last.i_w,
              s_w.last.i_h, s_w.last.i_tex_w, s_w.last.i_tex_h, i_tex_w,
              i_tex_h, b_settle ? ", settled" : "");
   }
   if (b_settle) {
      g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "large");
   }
   return (s_w.p_v);
}

GgazeViewer *
ggtest_wait_for_texture_at(const char *c_loc, GgazeWindow *p_win, int i_tex_w,
                           int i_tex_h) {
   return (_wait_view(c_loc, p_win, i_tex_w, i_tex_h, FALSE));
}

GgazeViewer *
ggtest_wait_for_view_at(const char *c_loc, GgazeWindow *p_win, int i_tex_w,
                        int i_tex_h) {
   return (_wait_view(c_loc, p_win, i_tex_w, i_tex_h, TRUE));
}
