/*:*
 * ggaze — thumbnail grid view
 *
 * GgazeGrid wraps a GtkFlowBox (in a GtkScrolledWindow): one cell per
 * navigator file, thumbnails loaded lazily (async, while visible) from the
 * thumbnail cache. Removed (trashed/deleted) cells are dimmed; marked cells
 * get a check badge. Resize (+/-) updates the cell size and reflows. Enter or
 * double-click emits "activate". Cursor follows navigator.current.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "gridview.h"

#include <glib.h>

/* Per-cell data, attached to the GtkPicture via qdata. */
typedef struct {
   GFile   *p_file;     /* owned ref */
   GFile   *p_expected; /* the file this thumbnail request is for (owned) */
   gboolean b_done;     /* thumbnail painted for the current size bucket */
} CellData;

static const char *CELL_DATA_KEY = "ggaze-cell-data";

struct _GgazeGrid {
   GtkWidget           parent_instance;
   Navigator          *p_nav;
   Thumbnail          *p_thumb;
   GtkWidget          *p_flow;     /* GtkFlowBox */
   GtkWidget          *p_scrolled; /* GtkScrolledWindow */
   int                 i_size;
   gboolean            b_hide_trashed;
   guint               u_nav_handler;
   guint               u_visible_idle; /* queued viewport thumbnail scan */
   GCancellable       *p_cancel;  /* cancels pending thumbnails on dispose */
   GgazeGridSelectFunc fn_select; /* selection gate hook, or NULL */
   gpointer            p_select_data; /* fn_select's user data */
};

G_DEFINE_TYPE(GgazeGrid, ggaze_grid, GTK_TYPE_WIDGET)

/* Signals (see gridview.h). The grid never names a window action: it says
 * what the user asked for and the owner decides what that means. */
static guint u_activate_signal = 0;
static guint u_navigate_signal = 0;
static guint u_mark_signal     = 0;
static guint u_mark_all_signal = 0;

static void _queue_visible_thumbnails(GgazeGrid *p_grid);

/* --- cell walking --------------------------------------------------------
 *
 * Every cell is a GtkFlowBoxChild carrying its GFile as "file" qdata and a
 * vertical box (picture + caption) as its child. These helpers are the one
 * place that layout is known; the five walks that used to repeat it now go
 * through _foreach_cell / _find_cell. */

/* The cell's file (borrowed from the child's qdata), or NULL. */
static GFile *
_cell_file(GtkWidget *p_child) {
   return ((GFile *)g_object_get_data(G_OBJECT(p_child), "file"));
}

/* The cell's content box (picture + caption), or NULL. */
static GtkWidget *
_cell_box(GtkWidget *p_child) {
   return (gtk_flow_box_child_get_child(GTK_FLOW_BOX_CHILD(p_child)));
}

typedef void (*GgazeGridCellFn)(GgazeGrid *p_grid, GtkWidget *p_child,
                                gpointer p_data);

/* Call fn for every cell in flow-box order. Safe against fn removing the
 * cell it was handed (the next sibling is fetched first). */
static void
_foreach_cell(GgazeGrid *p_grid, GgazeGridCellFn fn, gpointer p_data) {
   if (p_grid->p_flow == NULL) {
      return;
   }
   GtkWidget *p_child = gtk_widget_get_first_child(p_grid->p_flow);
   while (p_child != NULL) {
      GtkWidget *p_next = gtk_widget_get_next_sibling(p_child);
      fn(p_grid, p_child, p_data);
      p_child = p_next;
   }
}

/* The cell showing p_file, or NULL when it has none (hidden or gone). */
static GtkWidget *
_find_cell(GgazeGrid *p_grid, GFile *p_file) {
   if (p_grid->p_flow == NULL || p_file == NULL) {
      return (NULL);
   }
   GtkWidget *p_child = gtk_widget_get_first_child(p_grid->p_flow);
   while (p_child != NULL) {
      GFile *p_f = _cell_file(p_child);
      if (p_f != NULL && g_file_equal(p_f, p_file)) {
         return (p_child);
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   return (NULL);
}

/* Apply the "ggaze-marked" badge state to one cell's box. */
static void
_set_marked(GtkWidget *p_box, gboolean b_marked) {
   if (b_marked) {
      gtk_widget_add_css_class(p_box, "ggaze-marked");
   } else {
      gtk_widget_remove_css_class(p_box, "ggaze-marked");
   }
}

/* Apply the dimmed/"ggaze-removed" appearance to one cell's box. */
static void
_set_removed(GtkWidget *p_box, gboolean b_removed) {
   gtk_widget_set_opacity(p_box, b_removed ? 0.35 : 1.0);
   if (b_removed) {
      gtk_widget_add_css_class(p_box, "ggaze-removed");
   } else {
      gtk_widget_remove_css_class(p_box, "ggaze-removed");
   }
}

static void
_cell_data_free(gpointer p_void) {
   CellData *p_d = (CellData *)p_void;
   g_clear_object(&p_d->p_file);
   g_clear_object(&p_d->p_expected);
   g_free(p_d);
}

static CellData *
_cell_data(GtkWidget *p_pic) {
   return ((CellData *)g_object_get_data(G_OBJECT(p_pic), CELL_DATA_KEY));
}

/* Route a navigator.current change request through the installed select gate
 * (see gridview.h GgazeGridSelectFunc), so a window-level dirty-preview
 * prompt can run before the navigator's "changed" signal actually fires.
 * Falls back to calling navigator_set_current_file() directly when no gate
 * is installed. Every call site that used to call
 * navigator_set_current_file() straight now goes through this instead. */
static gboolean
_grid_select(GgazeGrid *p_grid, GFile *p_file) {
   if (p_grid->fn_select != NULL) {
      return (p_grid->fn_select(p_grid, p_file, p_grid->p_select_data));
   }
   return (navigator_set_current_file(p_grid->p_nav, p_file));
}

/* --- thumbnail request --------------------------------------------------- */

static void
_thumb_finish_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   (void)p_src;
   GtkWidget *p_pic = (GtkWidget *)p_data;
   CellData  *p_d   = _cell_data(p_pic);
   if (p_d == NULL) {
      g_object_unref(p_pic);
      return;
   }
   /* If the grid was detached/disposed (or the cell rebuilt) after this
    * request was issued, drop the result without touching the (possibly
    * orphaned) picture. g_task_propagate_pointer also yields NULL for a
    * cancelled task, but checking the cancellable explicitly keeps the
    * widget-touch path out of cancelled grids entirely. */
   GCancellable *p_cancel = g_task_get_cancellable(G_TASK(p_res));
   if (p_cancel != NULL && g_cancellable_is_cancelled(p_cancel)) {
      g_clear_object(&p_d->p_expected);
      g_object_unref(p_pic);
      return;
   }
   GError     *p_err = NULL;
   GdkTexture *p_tex = thumbnail_get_finish(NULL, p_res, &p_err);
   if (p_tex != NULL) {
      /* Only apply if this request is still for this cell's file. */
      if (p_d->p_expected != NULL &&
          g_file_equal(p_d->p_expected, p_d->p_file)) {
         gtk_picture_set_paintable(GTK_PICTURE(p_pic), (GdkPaintable *)p_tex);
         p_d->b_done = TRUE; /* no re-request until the size changes */
      }
      g_object_unref(p_tex);
   } else {
      g_clear_error(&p_err);
   }
   g_clear_object(&p_d->p_expected);
   g_object_unref(p_pic);
}

static void
_request_thumbnail(GtkWidget *p_pic) {
   CellData *p_d = _cell_data(p_pic);
   if (p_d == NULL || p_d->p_file == NULL) {
      return;
   }
   if (p_d->p_expected != NULL || p_d->b_done) {
      return; /* already in flight, or already painted for this size */
   }
   p_d->p_expected = (GFile *)g_object_ref(p_d->p_file);
   GgazeGrid *p_grid =
      GGAZE_GRID(gtk_widget_get_ancestor(p_pic, GGAZE_TYPE_GRID));
   if (p_grid == NULL || p_grid->p_thumb == NULL) {
      g_clear_object(&p_d->p_expected);
      return;
   }
   thumbnail_get_async(p_grid->p_thumb, p_d->p_file, p_grid->i_size,
                       p_grid->p_cancel, _thumb_finish_cb, g_object_ref(p_pic));
}

static gboolean
_picture_is_visible(GgazeGrid *p_grid, GtkWidget *p_pic) {
   graphene_rect_t t_bounds;
   if (!gtk_widget_compute_bounds(p_pic, p_grid->p_flow, &t_bounds)) {
      return (FALSE);
   }
   GtkAdjustment *p_hadjustment = gtk_scrolled_window_get_hadjustment(
      GTK_SCROLLED_WINDOW(p_grid->p_scrolled));
   GtkAdjustment *p_vadjustment = gtk_scrolled_window_get_vadjustment(
      GTK_SCROLLED_WINDOW(p_grid->p_scrolled));
   double d_left   = gtk_adjustment_get_value(p_hadjustment);
   double d_top    = gtk_adjustment_get_value(p_vadjustment);
   double d_right  = d_left + gtk_adjustment_get_page_size(p_hadjustment);
   double d_bottom = d_top + gtk_adjustment_get_page_size(p_vadjustment);
   return (t_bounds.origin.x < d_right && t_bounds.origin.y < d_bottom &&
           t_bounds.origin.x + t_bounds.size.width > d_left &&
           t_bounds.origin.y + t_bounds.size.height > d_top);
}

static gboolean
_request_visible_idle_cb(gpointer p_data) {
   GgazeGrid *p_grid      = GGAZE_GRID(p_data);
   p_grid->u_visible_idle = 0;
   if (!gtk_widget_get_mapped(GTK_WIDGET(p_grid)) || p_grid->p_thumb == NULL ||
       p_grid->p_cancel == NULL) {
      return (G_SOURCE_REMOVE);
   }
   GtkWidget *p_child = gtk_widget_get_first_child(p_grid->p_flow);
   while (p_child != NULL) {
      GtkWidget *p_box =
         gtk_flow_box_child_get_child(GTK_FLOW_BOX_CHILD(p_child));
      GtkWidget *p_pic =
         (p_box != NULL) ? gtk_widget_get_first_child(p_box) : NULL;
      if (p_pic != NULL && _picture_is_visible(p_grid, p_pic)) {
         _request_thumbnail(p_pic);
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   return (G_SOURCE_REMOVE);
}

static void
_queue_visible_thumbnails(GgazeGrid *p_grid) {
   if (p_grid->p_cancel == NULL || p_grid->u_visible_idle != 0) {
      return;
   }
   p_grid->u_visible_idle = g_idle_add_full(
      G_PRIORITY_DEFAULT_IDLE, _request_visible_idle_cb, p_grid, NULL);
}

static void
_on_pic_map(GtkWidget *p_pic, gpointer p_data) {
   (void)p_data;
   GgazeGrid *p_grid =
      GGAZE_GRID(gtk_widget_get_ancestor(p_pic, GGAZE_TYPE_GRID));
   if (p_grid != NULL) {
      _queue_visible_thumbnails(p_grid);
   }
}

static void
_on_adjustment_changed(GtkAdjustment *p_adjustment, gpointer p_data) {
   (void)p_adjustment;
   _queue_visible_thumbnails(GGAZE_GRID(p_data));
}

/* --- cell construction --------------------------------------------------- */

static GtkWidget *
_make_picture(GgazeGrid *p_grid, GFile *p_file) {
   GtkWidget *p_pic = gtk_picture_new();
   gtk_widget_set_size_request(p_pic, p_grid->i_size, p_grid->i_size);
   gtk_picture_set_content_fit(GTK_PICTURE(p_pic), GTK_CONTENT_FIT_CONTAIN);
   CellData *p_d = g_new0(CellData, 1);
   p_d->p_file   = (GFile *)g_object_ref(p_file);
   g_object_set_data_full(G_OBJECT(p_pic), CELL_DATA_KEY, p_d, _cell_data_free);
   g_signal_connect(p_pic, "map", G_CALLBACK(_on_pic_map), NULL);
   return (p_pic);
}

static GtkWidget *
_make_cell(GgazeGrid *p_grid, GFile *p_file) {
   GtkWidget *p_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
   gtk_widget_set_size_request(p_box, p_grid->i_size + 4, p_grid->i_size + 18);
   GtkWidget *p_pic = _make_picture(p_grid, p_file);
   gtk_box_append(GTK_BOX(p_box), p_pic);
   char      *c_name  = g_file_get_basename(p_file);
   GtkWidget *p_label = gtk_label_new(c_name);
   g_free(c_name);
   gtk_label_set_max_width_chars(GTK_LABEL(p_label), 20);
   gtk_label_set_ellipsize(GTK_LABEL(p_label), PANGO_ELLIPSIZE_END);
   gtk_widget_add_css_class(p_label, "caption");
   gtk_box_append(GTK_BOX(p_box), p_label);

   _set_marked(p_box, navigator_is_marked(p_grid->p_nav, p_file));
   _set_removed(p_box, navigator_is_removed(p_grid->p_nav, p_file));

   GtkWidget *p_child = gtk_flow_box_child_new();
   gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(p_child), p_box);
   g_object_set_data_full(G_OBJECT(p_child), "file", g_object_ref(p_file),
                          (GDestroyNotify)g_object_unref);
   return (p_child);
}

static void
_on_child_activated(GtkFlowBox *p_flow, GtkFlowBoxChild *p_child,
                    gpointer p_data) {
   (void)p_flow;
   GgazeGrid *p_grid = GGAZE_GRID(p_data);
   GFile     *p_file = (GFile *)g_object_get_data(G_OBJECT(p_child), "file");
   if (p_file != NULL) {
      _grid_select(p_grid, p_file);
   }
   g_signal_emit(p_grid, u_activate_signal, 0);
}

/* Keys the grid answers itself. Arrow/vi rows move the cursor; Left/Right,
 * Ctrl+a and the middle-click mark are reported as signals for the owner to
 * act on (the grid does not know the window's action names). Ctrl+a must be
 * caught here: GtkFlowBox binds it to its own "select-all", which counts as
 * handled even in SINGLE selection mode, so the window's global shortcut
 * would never see it. */
static gboolean
_on_flow_key(GtkEventControllerKey *p_key, guint u_kv, guint u_kc,
             GdkModifierType e_st, gpointer p_data) {
   (void)p_key;
   (void)u_kc;
   GgazeGrid      *p_grid = GGAZE_GRID(p_data);
   GdkModifierType e_mods = e_st & gtk_accelerator_get_default_mod_mask();
   if (e_mods == GDK_CONTROL_MASK && u_kv == GDK_KEY_a) {
      g_signal_emit(p_grid, u_mark_all_signal, 0);
      return (TRUE);
   }
   if (e_mods != 0) {
      return (FALSE); /* other chords belong to the window */
   }
   if (u_kv == GDK_KEY_Return || u_kv == GDK_KEY_KP_Enter) {
      /* Sync navigator.current to the highlighted cell before activating, so
       * Enter opens the arrow-selected image, not a stale current. */
      ggaze_grid_sync_current(p_grid);
      g_signal_emit(p_grid, u_activate_signal, 0);
      return (TRUE);
   }
   if (u_kv == GDK_KEY_j || u_kv == GDK_KEY_Down) {
      /* Move the cursor down one row; h/l and Left/Right stay linear via the
       * global win.next/win.prev shortcuts. */
      ggaze_grid_move_cursor(p_grid, 1);
      return (TRUE);
   }
   if (u_kv == GDK_KEY_k || u_kv == GDK_KEY_Up) {
      ggaze_grid_move_cursor(p_grid, -1);
      return (TRUE);
   }
   if (u_kv == GDK_KEY_Left || u_kv == GDK_KEY_Right) {
      g_signal_emit(p_grid, u_navigate_signal, 0,
                    (u_kv == GDK_KEY_Left) ? -1 : 1);
      return (TRUE);
   }
   return (FALSE); /* everything else (d, v, ...) is a window action */
}

/* A click (or GtkFlowBox's own Home/End/PageUp/PageDown bindings) moved the
 * highlight: keep navigator.current in step through the select gate, so the
 * per-image actions (d/D/m/e/!/i) and the header act on the cell the user is
 * looking at, not on whatever was current before the click. Re-entrant
 * selections made by _select_current land on the file that is already
 * current and are no-ops in the navigator. */
static void
_on_selection_changed(GtkFlowBox *p_flow, gpointer p_data) {
   (void)p_flow;
   ggaze_grid_sync_current(GGAZE_GRID(p_data));
}

/* Toggle the mark on the cell at (i_x, i_y) — see gridview.h.
 *
 * The owner's mark handler targets the FLOWBOX SELECTION, not
 * navigator.current -- window.c's _action_mark prefers
 * ggaze_grid_get_selected_file() while the grid is the visible stack child --
 * so gtk_flow_box_select_child() below is what makes the mark land on this
 * cell. The _grid_select() call next to it
 * is a separate concern: it keeps navigator.current (header, large view,
 * prefetch) in step with the cell the user just pointed at, and since tu0 may
 * legitimately be refused or deferred by the installed select gate when an
 * unsaved enhance preview is active. The mark applies either way. */
gboolean
ggaze_grid_mark_at_pos(GgazeGrid *p_grid, gint i_x, gint i_y) {
   g_return_val_if_fail(GGAZE_IS_GRID(p_grid), FALSE);
   if (p_grid->p_nav == NULL || p_grid->p_flow == NULL) {
      return (FALSE);
   }
   GtkFlowBoxChild *p_child =
      gtk_flow_box_get_child_at_pos(GTK_FLOW_BOX(p_grid->p_flow), i_x, i_y);
   if (p_child == NULL) {
      return (FALSE);
   }
   /* Select the cell (this is what win.mark acts on), keep navigator.current
    * in step with it through the gate, then ask the owner to toggle the mark
    * ("mark-requested": badge + header update happen there). */
   gtk_flow_box_select_child(GTK_FLOW_BOX(p_grid->p_flow), p_child);
   GFile *p_file = _cell_file(GTK_WIDGET(p_child));
   if (p_file != NULL) {
      _grid_select(p_grid, p_file);
   }
   g_signal_emit(p_grid, u_mark_signal, 0);
   return (TRUE);
}

/* Middle-click on a grid cell toggles its mark (pointer-accessible marks).
 * The gesture callback is deliberately nothing but a button filter plus a
 * coordinate hand-off: driving a real middle-click needs a synthesized
 * pointer press, which GTK4 exposes no supported API for, so everything
 * worth testing lives in ggaze_grid_mark_at_pos() where a test can reach it
 * (tu0 review round 4). */
static void
_on_flow_middle_pressed(GtkGesture *p_g, gint i_n_press, gdouble d_x,
                        gdouble d_y, gpointer p_data) {
   (void)i_n_press;
   if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(p_g)) !=
       GDK_BUTTON_MIDDLE) {
      return;
   }
   ggaze_grid_mark_at_pos(GGAZE_GRID(p_data), (gint)d_x, (gint)d_y);
}

/* --- refresh / rebuild --------------------------------------------------- */

static void
_clear_flow(GgazeGrid *p_grid) {
   GtkWidget *p_child = gtk_widget_get_first_child(p_grid->p_flow);
   while (p_child != NULL) {
      GtkWidget *p_next = gtk_widget_get_next_sibling(p_child);
      gtk_flow_box_remove(GTK_FLOW_BOX(p_grid->p_flow), p_child);
      p_child = p_next;
   }
}

static void
_select_current(GgazeGrid *p_grid) {
   GtkWidget *p_child =
      _find_cell(p_grid, navigator_get_current(p_grid->p_nav));
   if (p_child != NULL) {
      gtk_flow_box_select_child(GTK_FLOW_BOX(p_grid->p_flow),
                                GTK_FLOW_BOX_CHILD(p_child));
      gtk_widget_grab_focus(p_child);
   }
}

/* Sync navigator.current to the flowbox's currently-selected child, so that
 * leaving the grid (Enter or toggle-to-large) opens the highlighted cell,
 * not a stale current left over from when the grid was entered. Mirrors the
 * sync _on_child_activated does for a double-click.
 *
 * Returns whatever the select gate returns, i.e. navigator_set_current_file's
 * own contract: TRUE ONLY when current actually MOVED. FALSE covers all three
 * of "no selection", "the highlighted cell already IS current" (navigator.c's
 * i_current == i early return) and "a dirty enhance preview deferred the
 * change behind the Save/Discard/Cancel prompt". window.c's _action_toggle_view
 * relies on exactly this: TRUE means nav_changed_cb has already reloaded, so
 * only the FALSE cases still need a _load_current of their own. */
gboolean
ggaze_grid_sync_current(GgazeGrid *p_grid) {
   g_return_val_if_fail(GGAZE_IS_GRID(p_grid), FALSE);
   if (p_grid->p_nav == NULL) {
      return (FALSE);
   }
   GList *p_sel =
      gtk_flow_box_get_selected_children(GTK_FLOW_BOX(p_grid->p_flow));
   if (p_sel == NULL) {
      return (FALSE);
   }
   GFile *p_file = _cell_file(GTK_WIDGET(p_sel->data));
   g_list_free(p_sel);
   if (p_file == NULL) {
      return (FALSE);
   }
   return (_grid_select(p_grid, p_file));
}

/* Running best-match for _nearest_cell_in_row_direction's walk. */
typedef struct {
   GtkWidget      *p_cur;
   graphene_rect_t r_cur;
   int             i_dy;
   GtkWidget      *p_best;
   float           f_best_dy;
   float           f_best_dx;
} NearestSearch;

/* One step of the nearest-cell search: a laid-out cell in the wanted vertical
 * direction wins if it is in a nearer row, or in the same row and nearer to
 * the current column. */
static void
_nearest_step(GgazeGrid *p_grid, GtkWidget *p_child, gpointer p_data) {
   NearestSearch  *p_s = (NearestSearch *)p_data;
   graphene_rect_t r;
   if (p_child == p_s->p_cur ||
       !gtk_widget_compute_bounds(p_child, p_grid->p_flow, &r) ||
       r.size.width == 0) {
      return;
   }
   float f_dy = r.origin.y - p_s->r_cur.origin.y;
   if (!((p_s->i_dy > 0 && f_dy > 0) || (p_s->i_dy < 0 && f_dy < 0))) {
      return;
   }
   float f_ady = ABS(f_dy);
   float f_adx = ABS(r.origin.x - p_s->r_cur.origin.x);
   if (p_s->p_best == NULL || f_ady < p_s->f_best_dy ||
       (f_ady == p_s->f_best_dy && f_adx < p_s->f_best_dx)) {
      p_s->p_best    = p_child;
      p_s->f_best_dy = f_ady;
      p_s->f_best_dx = f_adx;
   }
}

/* The laid-out cell in the row below (i_dy > 0) or above (i_dy < 0) p_cur
 * that is closest to p_cur's column, or NULL when there is none (or the grid
 * is not laid out yet). Pure geometry over the flow box. */
static GtkWidget *
_nearest_cell_in_row_direction(GgazeGrid *p_grid, GtkWidget *p_cur, int i_dy) {
   NearestSearch st = {p_cur, {{0, 0}, {0, 0}}, i_dy, NULL, 0.0f, 0.0f};
   if (!gtk_widget_compute_bounds(p_cur, p_grid->p_flow, &st.r_cur) ||
       st.r_cur.size.width == 0) {
      return (NULL); /* not laid out yet */
   }
   _foreach_cell(p_grid, _nearest_step, &st);
   return (st.p_best);
}

/* Move the grid cursor one row down (i_dy = +1) or up (i_dy = -1), selecting
 * the cell in the adjacent row closest to the current column. Updates
 * navigator.current so the header and large-view preview track the move (the
 * "changed" emission re-selects via _on_nav_changed). */
void
ggaze_grid_move_cursor(GgazeGrid *p_grid, int i_dy) {
   g_return_if_fail(GGAZE_IS_GRID(p_grid));
   if (p_grid->p_nav == NULL || p_grid->p_flow == NULL) {
      return;
   }
   GList *p_sel =
      gtk_flow_box_get_selected_children(GTK_FLOW_BOX(p_grid->p_flow));
   if (p_sel == NULL) {
      return;
   }
   GtkWidget *p_cur = GTK_WIDGET(p_sel->data);
   g_list_free(p_sel);
   GtkWidget *p_best = _nearest_cell_in_row_direction(p_grid, p_cur, i_dy);
   GFile     *p_f    = (p_best != NULL) ? _cell_file(p_best) : NULL;
   if (p_f != NULL) {
      _grid_select(p_grid, p_f);
   }
}

/* Borrowed pointer to the selected cell's file (NULL if nothing selected). */
GFile *
ggaze_grid_get_selected_file(GgazeGrid *p_grid) {
   g_return_val_if_fail(GGAZE_IS_GRID(p_grid), NULL);
   if (p_grid->p_flow == NULL) {
      return (NULL);
   }
   GList *p_sel =
      gtk_flow_box_get_selected_children(GTK_FLOW_BOX(p_grid->p_flow));
   if (p_sel == NULL) {
      return (NULL);
   }
   GFile *p_file = _cell_file(GTK_WIDGET(p_sel->data));
   g_list_free(p_sel);
   return (p_file); /* borrowed: owned by the cell's qdata */
}

/* Toggle just one cell's "ggaze-marked" css class to match the navigator's
 * mark set, without rebuilding the grid (so toggling a mark doesn't reflow or
 * re-request thumbnails). */
void
ggaze_grid_update_mark_badge(GgazeGrid *p_grid, GFile *p_file) {
   g_return_if_fail(GGAZE_IS_GRID(p_grid));
   if (p_file == NULL || p_grid->p_nav == NULL) {
      return;
   }
   GtkWidget *p_child = _find_cell(p_grid, p_file);
   GtkWidget *p_box   = (p_child != NULL) ? _cell_box(p_child) : NULL;
   if (p_box != NULL) {
      _set_marked(p_box, navigator_is_marked(p_grid->p_nav, p_file));
   }
}

void
ggaze_grid_refresh(GgazeGrid *p_grid) {
   g_return_if_fail(GGAZE_IS_GRID(p_grid));
   /* A rebuild discards every cell; cancel any in-flight thumbnail requests
    * for the old pictures so their finish callbacks don't paint into orphaned
    * widgets, then start a fresh cancellable for the new cells. */
   if (p_grid->p_cancel != NULL) {
      g_cancellable_cancel(p_grid->p_cancel);
      g_clear_object(&p_grid->p_cancel);
   }
   p_grid->p_cancel = g_cancellable_new();
   _clear_flow(p_grid);

   guint u_n = navigator_get_count(p_grid->p_nav);
   for (guint u_i = 0; u_i < u_n; u_i++) {
      GFile *p_file = navigator_get_file(p_grid->p_nav, u_i);
      if (p_file == NULL) {
         continue;
      }
      if (p_grid->b_hide_trashed &&
          navigator_is_removed(p_grid->p_nav, p_file)) {
         continue;
      }
      GtkWidget *p_child = _make_cell(p_grid, p_file);
      gtk_flow_box_append(GTK_FLOW_BOX(p_grid->p_flow), p_child);
   }
   _select_current(p_grid);
}

/* Update every cell's "ggaze-marked" badge from the navigator's mark set
 * in place (no rebuild), so mark changes and navigation don't blank the grid
 * by recreating cells. */
static void
_refresh_badge_step(GgazeGrid *p_grid, GtkWidget *p_child, gpointer p_data) {
   (void)p_data;
   GFile     *p_f   = _cell_file(p_child);
   GtkWidget *p_box = _cell_box(p_child);
   if (p_f != NULL && p_box != NULL) {
      _set_marked(p_box, navigator_is_marked(p_grid->p_nav, p_f));
   }
}

void
ggaze_grid_refresh_mark_badges(GgazeGrid *p_grid) {
   g_return_if_fail(GGAZE_IS_GRID(p_grid));
   if (p_grid->p_nav == NULL) {
      return;
   }
   _foreach_cell(p_grid, _refresh_badge_step, NULL);
}

/* Walk every cell and sync its dimmed/removed appearance to the
 * navigator's removed set in place (no rebuild), so a `d`/`D` trash or `u`
 * undo that changes only the removed set — not the listing count — still
 * dims or un-dims the right cell. Mirrors ggaze_grid_refresh_mark_badges for
 * the "ggaze-removed" class + opacity instead of the mark badge. */
static void
_refresh_removed_step(GgazeGrid *p_grid, GtkWidget *p_child, gpointer p_data) {
   (void)p_data;
   GFile     *p_f   = _cell_file(p_child);
   GtkWidget *p_box = _cell_box(p_child);
   if (p_f != NULL && p_box != NULL) {
      _set_removed(p_box, navigator_is_removed(p_grid->p_nav, p_f));
   }
}

static void
_refresh_removed_state(GgazeGrid *p_grid) {
   if (p_grid->p_nav == NULL) {
      return;
   }
   _foreach_cell(p_grid, _refresh_removed_step, NULL);
}

static void
_on_nav_changed(Navigator *p_nav, guint u_flags, gpointer p_data) {
   (void)p_nav;
   GgazeGrid *p_grid = GGAZE_GRID(p_data);
   if (p_grid->p_nav == NULL) {
      return;
   }
   /* A rebuilt listing always needs a full rebuild (cells may be new, gone
    * or renamed -- a same-count replace included). So does a removed-set
    * change while trashed items are hidden, because those cells must
    * physically leave the flowbox. Anything else is an in-place refresh:
    * rebuilding on every navigation keypress would recreate every
    * GtkPicture and blank the grid while each cell re-requested its
    * thumbnail. */
   if ((u_flags & GGAZE_NAV_LISTING) != 0 ||
       (p_grid->b_hide_trashed && (u_flags & GGAZE_NAV_REMOVED) != 0)) {
      ggaze_grid_refresh(p_grid);
      return;
   }
   if ((u_flags & GGAZE_NAV_MARKS) != 0) {
      ggaze_grid_refresh_mark_badges(p_grid);
   }
   if ((u_flags & GGAZE_NAV_REMOVED) != 0) {
      _refresh_removed_state(p_grid);
   }
   if ((u_flags & GGAZE_NAV_CURSOR) != 0) {
      _select_current(p_grid);
   }
}

/* --- GObject ------------------------------------------------------------- */

static void
ggaze_grid_dispose(GObject *p_obj) {
   GgazeGrid *p_grid = GGAZE_GRID(p_obj);
   if (p_grid->u_visible_idle != 0) {
      g_source_remove(p_grid->u_visible_idle);
      p_grid->u_visible_idle = 0;
   }
   if (p_grid->p_cancel != NULL) {
      g_cancellable_cancel(p_grid->p_cancel);
      g_clear_object(&p_grid->p_cancel);
   }
   if (p_grid->p_nav != NULL && p_grid->u_nav_handler != 0) {
      g_signal_handler_disconnect(p_grid->p_nav, p_grid->u_nav_handler);
      p_grid->u_nav_handler = 0;
   }
   p_grid->p_nav = NULL; /* detached; don't touch it after this */
   g_clear_pointer(&p_grid->p_scrolled, gtk_widget_unparent);
   G_OBJECT_CLASS(ggaze_grid_parent_class)->dispose(p_obj);
}

static void
ggaze_grid_finalize(GObject *p_obj) {
   GgazeGrid *p_grid = GGAZE_GRID(p_obj);
   /* p_nav/p_thumb are borrowed (owned by the window). */
   (void)p_grid;
   G_OBJECT_CLASS(ggaze_grid_parent_class)->finalize(p_obj);
}

static void
ggaze_grid_class_init(GgazeGridClass *p_klass) {
   GObjectClass   *p_oc = G_OBJECT_CLASS(p_klass);
   GtkWidgetClass *p_wc = GTK_WIDGET_CLASS(p_klass);
   p_oc->dispose        = ggaze_grid_dispose;
   p_oc->finalize       = ggaze_grid_finalize;
   /* GgazeGrid has a single child (the GtkScrolledWindow holding the
    * GtkFlowBox); GtkBinLayout allocates it to fill the grid so the flowbox
    * actually gets sized and renders instead of staying a 0x0 black area. */
   gtk_widget_class_set_layout_manager_type(p_wc, GTK_TYPE_BIN_LAYOUT);
   u_activate_signal =
      g_signal_new("activate", G_TYPE_FROM_CLASS(p_klass), G_SIGNAL_RUN_LAST, 0,
                   NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE, 0);
   u_navigate_signal = g_signal_new(
      "navigate", G_TYPE_FROM_CLASS(p_klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      g_cclosure_marshal_generic, G_TYPE_NONE, 1, G_TYPE_INT);
   u_mark_signal = g_signal_new("mark-requested", G_TYPE_FROM_CLASS(p_klass),
                                G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                g_cclosure_marshal_generic, G_TYPE_NONE, 0);
   u_mark_all_signal = g_signal_new(
      "mark-all-requested", G_TYPE_FROM_CLASS(p_klass), G_SIGNAL_RUN_LAST, 0,
      NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE, 0);
}

static void
ggaze_grid_init(GgazeGrid *p_grid) {
   p_grid->i_size         = 128;
   p_grid->b_hide_trashed = FALSE;
   /* Owned cancellable for in-flight thumbnail requests. detach/dispose
    * cancel it so a closing/replaced grid's worker threads and main-thread
    * finish callbacks stop touching (possibly freed) picture widgets. */
   p_grid->p_cancel   = g_cancellable_new();
   p_grid->p_scrolled = gtk_scrolled_window_new();
   gtk_widget_set_parent(p_grid->p_scrolled, GTK_WIDGET(p_grid));
   gtk_widget_set_hexpand(p_grid->p_scrolled, TRUE);
   gtk_widget_set_vexpand(p_grid->p_scrolled, TRUE);
   GtkAdjustment *p_hadjustment = gtk_scrolled_window_get_hadjustment(
      GTK_SCROLLED_WINDOW(p_grid->p_scrolled));
   GtkAdjustment *p_vadjustment = gtk_scrolled_window_get_vadjustment(
      GTK_SCROLLED_WINDOW(p_grid->p_scrolled));
   g_signal_connect(p_hadjustment, "changed",
                    G_CALLBACK(_on_adjustment_changed), p_grid);
   g_signal_connect(p_vadjustment, "changed",
                    G_CALLBACK(_on_adjustment_changed), p_grid);
   g_signal_connect(p_hadjustment, "value-changed",
                    G_CALLBACK(_on_adjustment_changed), p_grid);
   g_signal_connect(p_vadjustment, "value-changed",
                    G_CALLBACK(_on_adjustment_changed), p_grid);
   p_grid->p_flow = gtk_flow_box_new();
   gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(p_grid->p_flow), TRUE);
   gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(p_grid->p_flow),
                                             FALSE);
   g_signal_connect(p_grid->p_flow, "selected-children-changed",
                    G_CALLBACK(_on_selection_changed), p_grid);
   g_signal_connect(p_grid->p_flow, "child-activated",
                    G_CALLBACK(_on_child_activated), p_grid);
   GtkEventController *p_key = gtk_event_controller_key_new();
   gtk_event_controller_set_propagation_phase(p_key, GTK_PHASE_CAPTURE);
   g_signal_connect(p_key, "key-pressed", G_CALLBACK(_on_flow_key), p_grid);
   gtk_widget_add_controller(p_grid->p_flow, p_key);
   /* Middle-click on a cell toggles its mark (pointer-accessible marks). */
   GtkGesture *p_middle = gtk_gesture_click_new();
   gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(p_middle),
                                 GDK_BUTTON_MIDDLE);
   g_signal_connect(p_middle, "pressed", G_CALLBACK(_on_flow_middle_pressed),
                    p_grid);
   gtk_widget_add_controller(p_grid->p_flow, GTK_EVENT_CONTROLLER(p_middle));
   gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(p_grid->p_scrolled),
                                 p_grid->p_flow);
}

/* --- public ------------------------------------------------------------- */

GtkWidget *
ggaze_grid_new(Navigator *p_nav, Thumbnail *p_thumb, int i_size,
               gboolean b_hide_trashed) {
   g_return_val_if_fail(GGAZE_IS_NAVIGATOR(p_nav), NULL);
   GgazeGrid *p_grid      = GGAZE_GRID(g_object_new(GGAZE_TYPE_GRID, NULL));
   p_grid->p_nav          = p_nav;
   p_grid->p_thumb        = p_thumb;
   p_grid->i_size         = (i_size <= 0) ? 128 : i_size;
   p_grid->b_hide_trashed = b_hide_trashed;
   p_grid->u_nav_handler =
      g_signal_connect(p_nav, "changed", G_CALLBACK(_on_nav_changed), p_grid);
   ggaze_grid_refresh(p_grid);
   return (GTK_WIDGET(p_grid));
}

void
ggaze_grid_set_select_func(GgazeGrid *p_grid, GgazeGridSelectFunc fn,
                           gpointer p_user_data) {
   g_return_if_fail(GGAZE_IS_GRID(p_grid));
   p_grid->fn_select     = fn;
   p_grid->p_select_data = p_user_data;
}

/* Re-request the picture at the new size and forget that it was painted:
 * a different size may map to a different thumbnail bucket. */
static void
_resize_cell_step(GgazeGrid *p_grid, GtkWidget *p_child, gpointer p_data) {
   (void)p_data;
   GtkWidget *p_box = _cell_box(p_child);
   if (p_box == NULL) {
      return;
   }
   GtkWidget *p_pic = gtk_widget_get_first_child(p_box);
   if (p_pic != NULL) {
      gtk_widget_set_size_request(p_pic, p_grid->i_size, p_grid->i_size);
      CellData *p_d = _cell_data(p_pic);
      if (p_d != NULL) {
         p_d->b_done = FALSE;
      }
   }
   gtk_widget_set_size_request(p_box, p_grid->i_size + 4, p_grid->i_size + 18);
}

void
ggaze_grid_set_thumbnail_size(GgazeGrid *p_grid, int i_size) {
   g_return_if_fail(GGAZE_IS_GRID(p_grid));
   if (i_size <= 0) {
      i_size = 128;
   }
   if (p_grid->i_size == i_size) {
      return;
   }
   p_grid->i_size = i_size;
   _foreach_cell(p_grid, _resize_cell_step, NULL);
   gtk_widget_queue_resize(p_grid->p_flow);
   _queue_visible_thumbnails(p_grid);
}

int
ggaze_grid_get_thumbnail_size(GgazeGrid *p_grid) {
   g_return_val_if_fail(GGAZE_IS_GRID(p_grid), 0);
   return (p_grid->i_size);
}

void
ggaze_grid_set_hide_trashed(GgazeGrid *p_grid, gboolean b_hide) {
   g_return_if_fail(GGAZE_IS_GRID(p_grid));
   if (p_grid->b_hide_trashed == b_hide) {
      return;
   }
   p_grid->b_hide_trashed = b_hide;
   ggaze_grid_refresh(p_grid);
}

void
ggaze_grid_detach(GgazeGrid *p_grid) {
   g_return_if_fail(GGAZE_IS_GRID(p_grid));
   if (p_grid->p_cancel != NULL) {
      g_cancellable_cancel(p_grid->p_cancel);
      g_clear_object(&p_grid->p_cancel);
   }
   if (p_grid->p_nav != NULL && p_grid->u_nav_handler != 0) {
      g_signal_handler_disconnect(p_grid->p_nav, p_grid->u_nav_handler);
      p_grid->u_nav_handler = 0;
   }
   p_grid->p_nav = NULL;
}

guint
ggaze_grid_get_count(GgazeGrid *p_grid) {
   g_return_val_if_fail(GGAZE_IS_GRID(p_grid), 0);
   guint      u_count = 0;
   GtkWidget *p_child = gtk_widget_get_first_child(p_grid->p_flow);
   while (p_child != NULL) {
      u_count++;
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   return (u_count);
}
