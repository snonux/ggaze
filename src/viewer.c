/*:*
 * ggaze — large single-image viewer
 *
 * Custom GtkWidget (decision #31). Draws the GdkTexture scaled into the widget
 * with letterboxing, zoom (fit / 100% / in / out), cursor-centered zoom,
 * drag-to-pan with clamping, and a dark background. M1: synchronous single
 * image. Compare-before/after (hold-Space) and tool overlays land in M9.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "viewer.h"

#include <glib.h>
#include <graphene.h>

#define GGAZE_ZOOM_FACTOR 1.25
#define GGAZE_ZOOM_MIN 0.02
#define GGAZE_ZOOM_MAX 64.0
#define GGAZE_PAN_STEP 24.0

struct _GgazeViewer {
   GtkWidget   parent_instance;
   GdkTexture *p_texture;
   gboolean    b_fit;   /* TRUE = fit-to-window; FALSE = use d_zoom */
   gdouble     d_zoom;  /* 1.0 = 100% (used when !b_fit) */
   gdouble     d_pan_x; /* offset from centred, in widget px */
   gdouble     d_pan_y;
   gdouble     d_drag_start_pan_x;
   gdouble     d_drag_start_pan_y;
};

G_DEFINE_TYPE(GgazeViewer, ggaze_viewer, GTK_TYPE_WIDGET)

/* --- geometry --------------------------------------------------------------
 */

static int
_tex_w(GgazeViewer *p_v) {
   return (p_v->p_texture != NULL) ? gdk_texture_get_width(p_v->p_texture) : 0;
}

static int
_tex_h(GgazeViewer *p_v) {
   return (p_v->p_texture != NULL) ? gdk_texture_get_height(p_v->p_texture) : 0;
}

/* Display scale + clamped top-left for the current state. Also writes the
 * clamped pan back so stored state matches what is drawn (no "dead zone" when
 * a new drag begins from a clamped edge). */
static void
_compute_geom(GgazeViewer *p_v, int i_w, int i_h, gdouble *p_scale,
              gdouble *p_x, gdouble *p_y, gdouble *p_dw, gdouble *p_dh) {
   int i_tw = _tex_w(p_v);
   int i_th = _tex_h(p_v);

   gdouble s;
   if (p_v->b_fit) {
      if (i_tw <= 0 || i_th <= 0 || i_w <= 0 || i_h <= 0) {
         s = 1.0;
      } else {
         s = MIN((gdouble)i_w / i_tw, (gdouble)i_h / i_th);
      }
   } else {
      s = p_v->d_zoom;
   }

   gdouble dw = (gdouble)i_tw * s;
   gdouble dh = (gdouble)i_th * s;
   gdouble x  = ((gdouble)i_w - dw) / 2.0 + p_v->d_pan_x;
   gdouble y  = ((gdouble)i_h - dh) / 2.0 + p_v->d_pan_y;

   /* Clamp so the image can't drift off-screen. */
   gdouble cx =
      CLAMP(x, MIN(0.0, (gdouble)i_w - dw), MAX(0.0, (gdouble)i_w - dw));
   gdouble cy =
      CLAMP(y, MIN(0.0, (gdouble)i_h - dh), MAX(0.0, (gdouble)i_h - dh));
   /* Write clamped pan back so the stored state tracks the drawn position. */
   p_v->d_pan_x = cx - ((gdouble)i_w - dw) / 2.0;
   p_v->d_pan_y = cy - ((gdouble)i_h - dh) / 2.0;

   if (p_scale != NULL) {
      *p_scale = s;
   }
   if (p_x != NULL) {
      *p_x = cx;
   }
   if (p_y != NULL) {
      *p_y = cy;
   }
   if (p_dw != NULL) {
      *p_dw = dw;
   }
   if (p_dh != NULL) {
      *p_dh = dh;
   }
}

static gdouble
_current_scale(GgazeViewer *p_v) {
   if (p_v->b_fit) {
      gdouble s = 1.0;
      _compute_geom(p_v, gtk_widget_get_width(GTK_WIDGET(p_v)),
                    gtk_widget_get_height(GTK_WIDGET(p_v)), &s, NULL, NULL,
                    NULL, NULL);
      return (s);
   }
   return (p_v->d_zoom);
}

/* Zoom around widget point (d_cx, d_cy), keeping that point over the same
 * image pixel. _compute_geom clamps on the next draw. */
static void
_zoom_at(GgazeViewer *p_v, gdouble d_cx, gdouble d_cy, gdouble d_new_zoom) {
   if (p_v->p_texture == NULL) {
      return;
   }
   d_new_zoom = CLAMP(d_new_zoom, GGAZE_ZOOM_MIN, GGAZE_ZOOM_MAX);

   int     i_w = gtk_widget_get_width(GTK_WIDGET(p_v));
   int     i_h = gtk_widget_get_height(GTK_WIDGET(p_v));
   gdouble s_old;
   gdouble x_old, y_old;
   _compute_geom(p_v, i_w, i_h, &s_old, &x_old, &y_old, NULL, NULL);

   /* Image-space pixel under the cursor before zoom. */
   gdouble img_x = (s_old > 0.0) ? (d_cx - x_old) / s_old : 0.0;
   gdouble img_y = (s_old > 0.0) ? (d_cy - y_old) / s_old : 0.0;

   p_v->b_fit  = FALSE;
   p_v->d_zoom = d_new_zoom;

   gdouble s_new  = d_new_zoom;
   gdouble want_x = d_cx - img_x * s_new;
   gdouble want_y = d_cy - img_y * s_new;
   p_v->d_pan_x = want_x - ((gdouble)i_w - (gdouble)_tex_w(p_v) * s_new) / 2.0;
   p_v->d_pan_y = want_y - ((gdouble)i_h - (gdouble)_tex_h(p_v) * s_new) / 2.0;

   gtk_widget_queue_draw(GTK_WIDGET(p_v));
}

/* --- GtkWidget vfuncs ----------------------------------------------------- */

static void
ggaze_viewer_measure(GtkWidget *p_widget, GtkOrientation o, int i_for_size,
                     int *p_min, int *p_nat, int *p_min_bl, int *p_nat_bl) {
   (void)p_widget;
   (void)o;
   (void)i_for_size;
   /* Fit-to-window fills the allocation via hexpand/vexpand; the viewer's
    * own natural size is small so a large image doesn't grow the window. */
   *p_min    = 1;
   *p_nat    = 1;
   *p_min_bl = -1;
   *p_nat_bl = -1;
}

static void
ggaze_viewer_snapshot(GtkWidget *p_widget, GtkSnapshot *p_snap) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_widget);
   int          i_w = gtk_widget_get_width(p_widget);
   int          i_h = gtk_widget_get_height(p_widget);

   /* Dark background (configurable via settings in M10). */
   static const GdkRGBA BG = {0.07f, 0.07f, 0.07f, 1.0f};
   graphene_rect_t      bg_rect =
      GRAPHENE_RECT_INIT(0.f, 0.f, (float)i_w, (float)i_h);
   gtk_snapshot_append_color(p_snap, &BG, &bg_rect);

   if (p_v->p_texture == NULL) {
      return;
   }

   gdouble x, y, dw, dh;
   _compute_geom(p_v, i_w, i_h, NULL, &x, &y, &dw, &dh);
   if (dw <= 0.0 || dh <= 0.0) {
      return;
   }
   graphene_rect_t rect =
      GRAPHENE_RECT_INIT((float)x, (float)y, (float)dw, (float)dh);
   gtk_snapshot_append_texture(p_snap, p_v->p_texture, &rect);
}

static void
ggaze_viewer_dispose(GObject *p_obj) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_obj);
   g_clear_object(&p_v->p_texture);
   G_OBJECT_CLASS(ggaze_viewer_parent_class)->dispose(p_obj);
}

static void
ggaze_viewer_class_init(GgazeViewerClass *p_klass) {
   GtkWidgetClass *p_wc = GTK_WIDGET_CLASS(p_klass);
   GObjectClass   *p_oc = G_OBJECT_CLASS(p_klass);
   p_wc->measure        = ggaze_viewer_measure;
   p_wc->snapshot       = ggaze_viewer_snapshot;
   p_oc->dispose        = ggaze_viewer_dispose;
   gtk_widget_class_set_css_name(p_wc, "ggazeviewer");
}

/* --- controllers ---------------------------------------------------------- */

static void
drag_begin_cb(GtkGestureDrag *p_gesture, gdouble d_x, gdouble d_y,
              gpointer p_data) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_data);
   (void)p_gesture;
   (void)d_x;
   (void)d_y;
   p_v->d_drag_start_pan_x = p_v->d_pan_x;
   p_v->d_drag_start_pan_y = p_v->d_pan_y;
}

static void
drag_update_cb(GtkGestureDrag *p_gesture, gdouble d_dx, gdouble d_dy,
               gpointer p_data) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_data);
   (void)p_gesture;
   p_v->d_pan_x = p_v->d_drag_start_pan_x + d_dx;
   p_v->d_pan_y = p_v->d_drag_start_pan_y + d_dy;
   gtk_widget_queue_draw(GTK_WIDGET(p_v));
}

static gboolean
scroll_cb(GtkEventControllerScroll *p_scroll, gdouble d_dx, gdouble d_dy,
          gpointer p_data) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_data);
   if (p_v->p_texture == NULL) {
      return (FALSE);
   }
   (void)d_dx;

   gdouble   d_cx = (gdouble)gtk_widget_get_width(GTK_WIDGET(p_v)) / 2.0;
   gdouble   d_cy = (gdouble)gtk_widget_get_height(GTK_WIDGET(p_v)) / 2.0;
   GdkEvent *p_event =
      gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(p_scroll));
   if (p_event != NULL) {
      gdk_event_get_position(p_event, &d_cx, &d_cy);
   }

   gdouble d_factor =
      (d_dy < 0.0) ? GGAZE_ZOOM_FACTOR : 1.0 / GGAZE_ZOOM_FACTOR;
   _zoom_at(p_v, d_cx, d_cy, _current_scale(p_v) * d_factor);
   return (TRUE);
}

static gboolean
key_cb(GtkEventControllerKey *p_key, guint u_keyval, guint u_keycode,
       GdkModifierType e_state, gpointer p_data) {
   GgazeViewer *p_v = GGAZE_VIEWER(p_data);
   (void)p_key;
   (void)u_keycode;
   (void)e_state;
   gboolean b_handled = TRUE;

   switch (u_keyval) {
   case GDK_KEY_plus:
   case GDK_KEY_equal:
      ggaze_viewer_zoom_in(p_v);
      break;
   case GDK_KEY_minus:
   case GDK_KEY_underscore:
      ggaze_viewer_zoom_out(p_v);
      break;
   case GDK_KEY_0:
      ggaze_viewer_toggle_fit_100(p_v);
      break;
   case GDK_KEY_j:
      ggaze_viewer_pan(p_v, 0.0, GGAZE_PAN_STEP);
      break;
   case GDK_KEY_k:
      ggaze_viewer_pan(p_v, 0.0, -GGAZE_PAN_STEP);
      break;
   case GDK_KEY_H:
      ggaze_viewer_pan(p_v, -GGAZE_PAN_STEP, 0.0);
      break;
   case GDK_KEY_L:
      ggaze_viewer_pan(p_v, GGAZE_PAN_STEP, 0.0);
      break;
   default:
      b_handled = FALSE;
      break;
   }
   return (b_handled);
}

static void
ggaze_viewer_init(GgazeViewer *p_v) {
   p_v->p_texture = NULL;
   p_v->b_fit     = TRUE;
   p_v->d_zoom    = 1.0;
   p_v->d_pan_x   = 0.0;
   p_v->d_pan_y   = 0.0;

   gtk_widget_set_focusable(GTK_WIDGET(p_v), TRUE);

   GtkGesture *p_drag = gtk_gesture_drag_new();
   gtk_widget_add_controller(GTK_WIDGET(p_v), GTK_EVENT_CONTROLLER(p_drag));
   g_signal_connect(p_drag, "drag-begin", G_CALLBACK(drag_begin_cb), p_v);
   g_signal_connect(p_drag, "drag-update", G_CALLBACK(drag_update_cb), p_v);

   GtkEventController *p_scroll =
      gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
   gtk_widget_add_controller(GTK_WIDGET(p_v), p_scroll);
   g_signal_connect(p_scroll, "scroll", G_CALLBACK(scroll_cb), p_v);

   GtkEventController *p_key = gtk_event_controller_key_new();
   gtk_widget_add_controller(GTK_WIDGET(p_v), p_key);
   g_signal_connect(p_key, "key-pressed", G_CALLBACK(key_cb), p_v);
}

/* --- public API ----------------------------------------------------------- */

GtkWidget *
ggaze_viewer_new(void) {
   return (GTK_WIDGET(g_object_new(GGAZE_TYPE_VIEWER, NULL)));
}

void
ggaze_viewer_set_texture(GgazeViewer *p_viewer, GdkTexture *p_texture) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   g_set_object(&p_viewer->p_texture, p_texture);
   p_viewer->b_fit   = TRUE;
   p_viewer->d_zoom  = 1.0;
   p_viewer->d_pan_x = 0.0;
   p_viewer->d_pan_y = 0.0;
   gtk_widget_queue_draw(GTK_WIDGET(p_viewer));
}

GdkTexture *
ggaze_viewer_get_texture(GgazeViewer *p_viewer) {
   g_return_val_if_fail(GGAZE_IS_VIEWER(p_viewer), NULL);
   return (p_viewer->p_texture); /* (transfer none) */
}

void
ggaze_viewer_zoom_in(GgazeViewer *p_viewer) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   _zoom_at(p_viewer, (gdouble)gtk_widget_get_width(GTK_WIDGET(p_viewer)) / 2.0,
            (gdouble)gtk_widget_get_height(GTK_WIDGET(p_viewer)) / 2.0,
            _current_scale(p_viewer) * GGAZE_ZOOM_FACTOR);
}

void
ggaze_viewer_zoom_out(GgazeViewer *p_viewer) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   _zoom_at(p_viewer, (gdouble)gtk_widget_get_width(GTK_WIDGET(p_viewer)) / 2.0,
            (gdouble)gtk_widget_get_height(GTK_WIDGET(p_viewer)) / 2.0,
            _current_scale(p_viewer) / GGAZE_ZOOM_FACTOR);
}

void
ggaze_viewer_toggle_fit_100(GgazeViewer *p_viewer) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   if (p_viewer->b_fit) {
      p_viewer->b_fit   = FALSE;
      p_viewer->d_zoom  = 1.0;
      p_viewer->d_pan_x = 0.0;
      p_viewer->d_pan_y = 0.0;
   } else {
      p_viewer->b_fit = TRUE;
   }
   gtk_widget_queue_draw(GTK_WIDGET(p_viewer));
}

void
ggaze_viewer_fit(GgazeViewer *p_viewer) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   p_viewer->b_fit   = TRUE;
   p_viewer->d_pan_x = 0.0;
   p_viewer->d_pan_y = 0.0;
   gtk_widget_queue_draw(GTK_WIDGET(p_viewer));
}

void
ggaze_viewer_pan(GgazeViewer *p_viewer, gdouble d_dx, gdouble d_dy) {
   g_return_if_fail(GGAZE_IS_VIEWER(p_viewer));
   p_viewer->d_pan_x += d_dx;
   p_viewer->d_pan_y += d_dy;
   gtk_widget_queue_draw(GTK_WIDGET(p_viewer));
}