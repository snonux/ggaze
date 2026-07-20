/*:*
 * ggaze — thumbnail grid view
 *
 * GgazeGrid wraps a GtkFlowBox (in a GtkScrolledWindow): one cell per
 * navigator file, thumbnails loaded lazily (async, on realize) from the
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
   GFile *p_file;     /* owned ref */
   GFile *p_expected; /* the file this thumbnail request is for (owned) */
} CellData;

static const char *CELL_DATA_KEY = "ggaze-cell-data";

struct _GgazeGrid {
   GtkWidget     parent_instance;
   Navigator    *p_nav;
   Thumbnail    *p_thumb;
   GtkWidget    *p_flow;     /* GtkFlowBox */
   GtkWidget    *p_scrolled; /* GtkScrolledWindow */
   int           i_size;
   gboolean      b_hide_trashed;
   guint         u_nav_handler;
   guint         u_last_count; /* navigator count at last full refresh */
   GCancellable *p_cancel;     /* cancels pending thumbnails on dispose */
};

G_DEFINE_TYPE(GgazeGrid, ggaze_grid, GTK_TYPE_WIDGET)

static guint u_activate_signal = 0;

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
   GError     *p_err = NULL;
   GdkTexture *p_tex = thumbnail_get_finish(NULL, p_res, &p_err);
   if (p_tex != NULL) {
      /* Only apply if this request is still for this cell's file. */
      if (p_d->p_expected != NULL &&
          g_file_equal(p_d->p_expected, p_d->p_file)) {
         gtk_picture_set_paintable(GTK_PICTURE(p_pic), (GdkPaintable *)p_tex);
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
   if (p_d->p_expected != NULL) {
      return; /* already in flight */
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

static void
_on_pic_map(GtkWidget *p_pic, gpointer p_data) {
   (void)p_data;
   _request_thumbnail(p_pic);
}

/* --- cell construction --------------------------------------------------- */

static GtkWidget *
_make_picture(GgazeGrid *p_grid, GFile *p_file) {
   GtkWidget *p_pic = gtk_picture_new();
   gtk_widget_set_size_request(p_pic, p_grid->i_size, p_grid->i_size);
   gtk_picture_set_content_fit(GTK_PICTURE(p_pic), GTK_CONTENT_FIT_CONTAIN);
   CellData *p_d   = g_new(CellData, 1);
   p_d->p_file     = (GFile *)g_object_ref(p_file);
   p_d->p_expected = NULL;
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

   /* mark badge */
   if (navigator_is_marked(p_grid->p_nav, p_file)) {
      gtk_widget_add_css_class(p_box, "ggaze-marked");
   }
   /* dim removed */
   if (navigator_is_removed(p_grid->p_nav, p_file)) {
      gtk_widget_set_opacity(p_box, 0.35);
      gtk_widget_add_css_class(p_box, "ggaze-removed");
   }

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
      navigator_set_current_file(p_grid->p_nav, p_file);
   }
   g_signal_emit(p_grid, u_activate_signal, 0);
}

static gboolean
_on_flow_key(GtkEventControllerKey *p_key, guint u_kv, guint u_kc,
             GdkModifierType e_st, gpointer p_data) {
   (void)p_key;
   (void)u_kc;
   (void)e_st;
   GgazeGrid *p_grid = GGAZE_GRID(p_data);
   if (u_kv == GDK_KEY_Return || u_kv == GDK_KEY_KP_Enter) {
      /* Sync navigator.current to the highlighted cell before activating, so
       * Enter opens the arrow-selected image, not a stale current. */
      ggaze_grid_sync_current(p_grid);
      g_signal_emit(p_grid, u_activate_signal, 0);
      return (TRUE);
   }
   if (u_kv == GDK_KEY_j) {
      /* Move the cursor down one row (vim-style); h/l stay linear via the
       * global win.next/win.prev shortcuts. */
      ggaze_grid_move_cursor(p_grid, 1);
      return (TRUE);
   }
   if (u_kv == GDK_KEY_k) {
      ggaze_grid_move_cursor(p_grid, -1);
      return (TRUE);
   }
   if (u_kv == GDK_KEY_d) {
      /* `d` in the grid trashes the current file (window handles via action).
       */
      return (FALSE);
   }
   return (FALSE);
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
   GFile *p_cur = navigator_get_current(p_grid->p_nav);
   if (p_cur == NULL) {
      return;
   }
   GtkWidget *p_child = gtk_widget_get_first_child(p_grid->p_flow);
   while (p_child != NULL) {
      GFile *p_f = (GFile *)g_object_get_data(G_OBJECT(p_child), "file");
      if (p_f != NULL && g_file_equal(p_f, p_cur)) {
         GtkFlowBoxChild *p_fc = GTK_FLOW_BOX_CHILD(p_child);
         gtk_flow_box_select_child(GTK_FLOW_BOX(p_grid->p_flow), p_fc);
         gtk_widget_grab_focus(p_child);
         return;
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
}

/* Sync navigator.current to the flowbox's currently-selected child, so that
 * leaving the grid (Enter or toggle-to-large) opens the highlighted cell,
 * not a stale current left over from when the grid was entered. Mirrors the
 * sync _on_child_activated does for a double-click. Returns TRUE if a
 * selection was found and current was (or already was) that file. */
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
   GFile *p_file = (GFile *)g_object_get_data(G_OBJECT(p_sel->data), "file");
   g_list_free(p_sel);
   if (p_file == NULL) {
      return (FALSE);
   }
   return (navigator_set_current_file(p_grid->p_nav, p_file));
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
   graphene_rect_t r_cur;
   if (!gtk_widget_compute_bounds(p_cur, p_grid->p_flow, &r_cur) ||
       r_cur.size.width == 0) {
      return; /* not laid out yet */
   }
   GtkWidget *p_best  = NULL;
   float      best_dy = 0;
   float      best_dx = 0;
   GtkWidget *p_child = gtk_widget_get_first_child(p_grid->p_flow);
   while (p_child != NULL) {
      if (p_child != p_cur) {
         graphene_rect_t r;
         if (gtk_widget_compute_bounds(p_child, p_grid->p_flow, &r) &&
             r.size.width != 0) {
            float dy = r.origin.y - r_cur.origin.y;
            if ((i_dy > 0 && dy > 0) || (i_dy < 0 && dy < 0)) {
               float ady = ABS(dy);
               float adx = ABS(r.origin.x - r_cur.origin.x);
               if (p_best == NULL || ady < best_dy ||
                   (ady == best_dy && adx < best_dx)) {
                  p_best  = p_child;
                  best_dy = ady;
                  best_dx = adx;
               }
            }
         }
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   if (p_best != NULL) {
      GFile *p_f = (GFile *)g_object_get_data(G_OBJECT(p_best), "file");
      if (p_f != NULL) {
         navigator_set_current_file(p_grid->p_nav, p_f);
      }
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
   GFile *p_file = (GFile *)g_object_get_data(G_OBJECT(p_sel->data), "file");
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
   gboolean   b_marked = navigator_is_marked(p_grid->p_nav, p_file);
   GtkWidget *p_child  = gtk_widget_get_first_child(p_grid->p_flow);
   while (p_child != NULL) {
      GFile *p_f = (GFile *)g_object_get_data(G_OBJECT(p_child), "file");
      if (p_f != NULL && g_file_equal(p_f, p_file)) {
         GtkWidget *p_box =
            gtk_flow_box_child_get_child(GTK_FLOW_BOX_CHILD(p_child));
         if (p_box != NULL) {
            if (b_marked) {
               gtk_widget_add_css_class(p_box, "ggaze-marked");
            } else {
               gtk_widget_remove_css_class(p_box, "ggaze-marked");
            }
         }
         return;
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
}

void
ggaze_grid_refresh(GgazeGrid *p_grid) {
   g_return_if_fail(GGAZE_IS_GRID(p_grid));
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
   p_grid->u_last_count = u_n;
   _select_current(p_grid);
}

/* Update every cell's "ggaze-marked" badge from the navigator's mark set
 * in place (no rebuild), so mark changes and navigation don't blank the grid
 * by recreating cells. */
void
ggaze_grid_refresh_mark_badges(GgazeGrid *p_grid) {
   g_return_if_fail(GGAZE_IS_GRID(p_grid));
   if (p_grid->p_nav == NULL) {
      return;
   }
   GtkWidget *p_child = gtk_widget_get_first_child(p_grid->p_flow);
   while (p_child != NULL) {
      GFile     *p_f = (GFile *)g_object_get_data(G_OBJECT(p_child), "file");
      GtkWidget *p_box =
         gtk_flow_box_child_get_child(GTK_FLOW_BOX_CHILD(p_child));
      if (p_f != NULL && p_box != NULL) {
         if (navigator_is_marked(p_grid->p_nav, p_f)) {
            gtk_widget_add_css_class(p_box, "ggaze-marked");
         } else {
            gtk_widget_remove_css_class(p_box, "ggaze-marked");
         }
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
}

static void
_on_nav_changed(Navigator *p_nav, gpointer p_data) {
   (void)p_nav;
   GgazeGrid *p_grid = GGAZE_GRID(p_data);
   if (p_grid->p_nav == NULL) {
      return;
   }
   /* Only a structural change (files added/removed via rescan/trash/move)
    * needs a full rebuild. A mere current/mark change just moves the
    * selection and refreshes badges in place — rebuilding on every
    * navigation keypress would recreate every GtkPicture and blank the grid
    * while each cell re-requested its thumbnail. */
   if (navigator_get_count(p_grid->p_nav) != p_grid->u_last_count) {
      ggaze_grid_refresh(p_grid);
      return;
   }
   ggaze_grid_refresh_mark_badges(p_grid);
   _select_current(p_grid);
}

/* --- GObject ------------------------------------------------------------- */

static void
ggaze_grid_dispose(GObject *p_obj) {
   GgazeGrid *p_grid = GGAZE_GRID(p_obj);
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
}

static void
ggaze_grid_init(GgazeGrid *p_grid) {
   p_grid->i_size         = 128;
   p_grid->b_hide_trashed = FALSE;
   p_grid->p_scrolled     = gtk_scrolled_window_new();
   gtk_widget_set_parent(p_grid->p_scrolled, GTK_WIDGET(p_grid));
   gtk_widget_set_hexpand(p_grid->p_scrolled, TRUE);
   gtk_widget_set_vexpand(p_grid->p_scrolled, TRUE);
   p_grid->p_flow = gtk_flow_box_new();
   gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(p_grid->p_flow), TRUE);
   gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(p_grid->p_flow),
                                             FALSE);
   g_signal_connect(p_grid->p_flow, "child-activated",
                    G_CALLBACK(_on_child_activated), p_grid);
   GtkEventController *p_key = gtk_event_controller_key_new();
   g_signal_connect(p_key, "key-pressed", G_CALLBACK(_on_flow_key), p_grid);
   gtk_widget_add_controller(p_grid->p_flow, p_key);
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
ggaze_grid_set_thumbnail_size(GgazeGrid *p_grid, int i_size) {
   g_return_if_fail(GGAZE_IS_GRID(p_grid));
   if (i_size <= 0) {
      i_size = 128;
   }
   if (p_grid->i_size == i_size) {
      return;
   }
   p_grid->i_size = i_size;
   /* Update the size request of every cell's picture and reflow. */
   GtkWidget *p_child = gtk_widget_get_first_child(p_grid->p_flow);
   while (p_child != NULL) {
      GtkWidget *p_box =
         gtk_flow_box_child_get_child(GTK_FLOW_BOX_CHILD(p_child));
      if (p_box != NULL) {
         GtkWidget *p_pic = gtk_widget_get_first_child(p_box);
         if (p_pic != NULL) {
            gtk_widget_set_size_request(p_pic, i_size, i_size);
         }
         gtk_widget_set_size_request(p_box, i_size + 4, i_size + 18);
      }
      p_child = gtk_widget_get_next_sibling(p_child);
   }
   gtk_widget_queue_resize(p_grid->p_flow);
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

guint
ggaze_grid_activate_signal(void) {
   return (u_activate_signal);
}