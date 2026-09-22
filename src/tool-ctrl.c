/*:*
 * ggaze — the interactive crop (c) and straighten (R) tools
 *
 * See tool-ctrl.h. The state machine (start / key / drag / apply / cancel /
 * abandon), the two overlays, and the glue between widget pixels (the
 * viewer's geometry) and image pixels (croprect.c / transform.c).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/
#include "tool-ctrl.h"

#include <math.h>

#include <glib.h>
#include <graphene.h>
#include <gtk/gtk.h>

#include "croprect.h"
#include "transform.h"

/* How close (widget px) the pointer must be to an edge or corner to grab it
 * rather than move the rectangle; scaled into image px per drag. */
#define _HIT_TOLERANCE_PX 12.0
/* Side of the corner handle squares, widget px. */
#define _HANDLE_PX 8.0
/* The straighten grid: lines at every 1/8 of the image. */
#define _GRID_DIVISIONS 8

static const char *_CROP_HINT =
   "Crop — drag to move or resize · h/l/j/k move, H/L/J/K resize "
   "· 1-4 aspect, 0 free · Enter applies, Esc cancels";
static const char *_RENDERING =
   "Preview still rendering — try again in a moment";
static const char *_FINISH_FIRST =
   "Finish the current tool first (Enter applies, Esc cancels)";

struct ToolCtrl {
   EnhanceCtrl           *p_ec;   /* borrowed: the transform being edited */
   const ToolCtrlHostOps *p_ops;  /* borrowed */
   gpointer               p_host; /* the window, borrowed */

   GgazeTool e_tool;         /* which tool has the view (NONE = idle) */
   GFile    *p_file;         /* owned: the file the tool started on, so a
                              * navigation away can be told from a rescan */
   Transform t_saved;        /* the transform at tool start (Esc restores) */
   Transform t_work;         /* the transform on the preview while editing */
   gboolean  b_crop_dropped; /* straighten: a committed crop was lost to the
                              * shrinking base this session (said in the
                              * status line until Enter / Esc) */

   /* crop */
   CropRect t_rect;     /* the rectangle, in base-image px */
   gboolean b_rect_set; /* t_rect has been laid out (needs the base size) */
   gdouble  d_aspect;   /* aspect lock w/h, 0 = free */
   gint     i_base_w;   /* the base size t_rect was laid out on (0 = none
                         * yet); also what the overlay checks the texture on
                         * screen against before drawing over it */
   gint i_base_h;

   /* an in-progress pointer drag */
   CropRectHit e_hit;        /* crop: what the drag grabbed */
   CropRect    t_drag_start; /* crop: the rectangle when it began */
   gdouble     d_drag_x0;    /* where it began, image px */
   gdouble     d_drag_y0;
   gboolean    b_line;    /* straighten: a horizon line is being drawn */
   gdouble     d_line_x1; /* its far end, image px (the near end is the
                           * drag start above) */
   gdouble d_line_y1;
};

/* --- host-op wrappers ---------------------------------------------------- */

static GgazeViewer *
_viewer(ToolCtrl *p_tc) {
   return (p_tc->p_ops->get_viewer(p_tc->p_host));
}

static void
_status(ToolCtrl *p_tc, const char *c_msg) {
   p_tc->p_ops->show_status(p_tc->p_host, c_msg);
}

static GFile *
_current_file(ToolCtrl *p_tc) {
   return (p_tc->p_ops->get_current_file(p_tc->p_host));
}

static void
_redraw(ToolCtrl *p_tc) {
   GgazeViewer *p_v = _viewer(p_tc);
   if (p_v != NULL) {
      gtk_widget_queue_draw(GTK_WIDGET(p_v));
   }
}

/* --- drawing helpers ------------------------------------------------------ */

static void
_fill(GtkSnapshot *p_snap, gdouble d_x, gdouble d_y, gdouble d_w, gdouble d_h,
      const GdkRGBA *p_col) {
   if (d_w <= 0.0 || d_h <= 0.0) {
      return;
   }
   graphene_rect_t t_r =
      GRAPHENE_RECT_INIT((float)d_x, (float)d_y, (float)d_w, (float)d_h);
   gtk_snapshot_append_color(p_snap, p_col, &t_r);
}

/* A 1-px-wide frame around the rectangle (drawn as a border so the four
 * sides are one node). */
static void
_frame(GtkSnapshot *p_snap, gdouble d_x, gdouble d_y, gdouble d_w, gdouble d_h,
       float f_width, const GdkRGBA *p_col) {
   GskRoundedRect t_rr;
   gsk_rounded_rect_init_from_rect(
      &t_rr,
      &GRAPHENE_RECT_INIT((float)d_x, (float)d_y, (float)d_w, (float)d_h),
      0.0f);
   const float   f_widths[4] = {f_width, f_width, f_width, f_width};
   const GdkRGBA t_cols[4]   = {*p_col, *p_col, *p_col, *p_col};
   gtk_snapshot_append_border(p_snap, &t_rr, f_widths, t_cols);
}

/* A straight line of f_width between two widget points (the only diagonal
 * thing drawn: the horizon), with a translucent dark halo so it reads on
 * any picture. */
static void
_line(GtkSnapshot *p_snap, gdouble d_x0, gdouble d_y0, gdouble d_x1,
      gdouble d_y1) {
   GskPathBuilder *p_b = gsk_path_builder_new();
   gsk_path_builder_move_to(p_b, (float)d_x0, (float)d_y0);
   gsk_path_builder_line_to(p_b, (float)d_x1, (float)d_y1);
   GskPath      *p_path  = gsk_path_builder_free_to_path(p_b);
   GskStroke    *p_halo  = gsk_stroke_new(5.0f);
   GskStroke    *p_core  = gsk_stroke_new(2.0f);
   const GdkRGBA t_dark  = {0.0f, 0.0f, 0.0f, 0.55f};
   const GdkRGBA t_light = {1.0f, 0.85f, 0.2f, 1.0f};
   gtk_snapshot_append_stroke(p_snap, p_path, p_halo, &t_dark);
   gtk_snapshot_append_stroke(p_snap, p_path, p_core, &t_light);
   gsk_stroke_free(p_halo);
   gsk_stroke_free(p_core);
   gsk_path_unref(p_path);
}

/* --- the crop overlay ----------------------------------------------------- */

/* Dim everything outside the rectangle, draw thirds inside it, a black-
 * haloed white frame, and the four corner handles. Nothing is drawn while
 * the texture on screen is not the base the rectangle was laid out on (the
 * base preview is still rendering): a rectangle over the wrong image would
 * lie about what Enter will crop. */
static void
_draw_crop(ToolCtrl *p_tc, GtkSnapshot *p_snap, const GgazeViewerGeom *p_g) {
   if (!p_tc->b_rect_set || p_g->i_img_w != p_tc->i_base_w ||
       p_g->i_img_h != p_tc->i_base_h) {
      return;
   }
   const GdkRGBA t_dim    = {0.0f, 0.0f, 0.0f, 0.55f};
   const GdkRGBA t_white  = {1.0f, 1.0f, 1.0f, 1.0f};
   const GdkRGBA t_black  = {0.0f, 0.0f, 0.0f, 0.6f};
   const GdkRGBA t_thirds = {1.0f, 1.0f, 1.0f, 0.35f};
   gdouble       d_s      = p_g->d_scale;
   gdouble       d_x      = p_g->d_x + p_tc->t_rect.d_x * d_s;
   gdouble       d_y      = p_g->d_y + p_tc->t_rect.d_y * d_s;
   gdouble       d_w      = p_tc->t_rect.d_w * d_s;
   gdouble       d_h      = p_tc->t_rect.d_h * d_s;
   gdouble       d_iw     = p_g->i_img_w * d_s;
   gdouble       d_ih     = p_g->i_img_h * d_s;
   _fill(p_snap, p_g->d_x, p_g->d_y, d_iw, d_y - p_g->d_y, &t_dim);
   _fill(p_snap, p_g->d_x, d_y + d_h, d_iw, p_g->d_y + d_ih - (d_y + d_h),
         &t_dim);
   _fill(p_snap, p_g->d_x, d_y, d_x - p_g->d_x, d_h, &t_dim);
   _fill(p_snap, d_x + d_w, d_y, p_g->d_x + d_iw - (d_x + d_w), d_h, &t_dim);
   for (gint i = 1; i < 3; i++) {
      _fill(p_snap, d_x + d_w * i / 3.0 - 0.5, d_y, 1.0, d_h, &t_thirds);
      _fill(p_snap, d_x, d_y + d_h * i / 3.0 - 0.5, d_w, 1.0, &t_thirds);
   }
   _frame(p_snap, d_x - 1.5, d_y - 1.5, d_w + 3.0, d_h + 3.0, 1.0f, &t_black);
   _frame(p_snap, d_x - 0.5, d_y - 0.5, d_w + 1.0, d_h + 1.0, 1.0f, &t_white);
   gdouble d_hh = _HANDLE_PX / 2.0;
   _fill(p_snap, d_x - d_hh, d_y - d_hh, _HANDLE_PX, _HANDLE_PX, &t_white);
   _fill(p_snap, d_x + d_w - d_hh, d_y - d_hh, _HANDLE_PX, _HANDLE_PX,
         &t_white);
   _fill(p_snap, d_x - d_hh, d_y + d_h - d_hh, _HANDLE_PX, _HANDLE_PX,
         &t_white);
   _fill(p_snap, d_x + d_w - d_hh, d_y + d_h - d_hh, _HANDLE_PX, _HANDLE_PX,
         &t_white);
}

/* --- the straighten overlay ----------------------------------------------- */

/* A grid over the whole image (the centre lines a little stronger) to judge
 * the level against, and the horizon line while one is being dragged. */
static void
_draw_straighten(ToolCtrl *p_tc, GtkSnapshot *p_snap,
                 const GgazeViewerGeom *p_g) {
   const GdkRGBA t_faint  = {1.0f, 1.0f, 1.0f, 0.3f};
   const GdkRGBA t_centre = {1.0f, 1.0f, 1.0f, 0.6f};
   gdouble       d_iw     = p_g->i_img_w * p_g->d_scale;
   gdouble       d_ih     = p_g->i_img_h * p_g->d_scale;
   for (gint i = 1; i < _GRID_DIVISIONS; i++) {
      const GdkRGBA *p_c = (i * 2 == _GRID_DIVISIONS) ? &t_centre : &t_faint;
      gdouble        d_f = (gdouble)i / _GRID_DIVISIONS;
      _fill(p_snap, p_g->d_x + d_iw * d_f - 0.5, p_g->d_y, 1.0, d_ih, p_c);
      _fill(p_snap, p_g->d_x, p_g->d_y + d_ih * d_f - 0.5, d_iw, 1.0, p_c);
   }
   if (p_tc->b_line) {
      gdouble d_s = p_g->d_scale;
      _line(p_snap, p_g->d_x + p_tc->d_drag_x0 * d_s,
            p_g->d_y + p_tc->d_drag_y0 * d_s, p_g->d_x + p_tc->d_line_x1 * d_s,
            p_g->d_y + p_tc->d_line_y1 * d_s);
   }
}

/* The viewer's overlay callbacks (viewer.h). */
static void
_draw_cb(GtkSnapshot *p_snap, const GgazeViewerGeom *p_g, gpointer p_data) {
   ToolCtrl *p_tc = (ToolCtrl *)p_data;
   if (p_tc->e_tool == GGAZE_TOOL_CROP) {
      _draw_crop(p_tc, p_snap, p_g);
   } else if (p_tc->e_tool == GGAZE_TOOL_STRAIGHTEN) {
      _draw_straighten(p_tc, p_snap, p_g);
   }
}

static void
_drag_cb(GgazeViewerDragPhase e_phase, gdouble d_x, gdouble d_y,
         gpointer p_data) {
   tool_ctrl_drag((ToolCtrl *)p_data, e_phase, d_x, d_y);
}

/* --- session -------------------------------------------------------------- */

/* Drop the overlay and every per-session field; the transform is left to the
 * caller (apply commits it, cancel restores it, abandon leaves it). */
static void
_leave(ToolCtrl *p_tc) {
   p_tc->e_tool = GGAZE_TOOL_NONE;
   p_tc->b_line = FALSE;
   p_tc->e_hit  = CROPRECT_HIT_NONE;
   g_clear_object(&p_tc->p_file);
   GgazeViewer *p_v = _viewer(p_tc);
   if (p_v != NULL) {
      ggaze_viewer_set_overlay(p_v, NULL, NULL, NULL);
   }
}

/* Common start: needs a current file; records what Esc will restore, puts
 * the large view up and installs the overlay. */
static gboolean
_begin(ToolCtrl *p_tc, GgazeTool e_tool) {
   GFile *p_cur = _current_file(p_tc);
   if (p_cur == NULL) {
      _status(p_tc, "Nothing open to crop or straighten");
      return (FALSE);
   }
   p_tc->p_ops->ensure_large_view(p_tc->p_host);
   p_tc->e_tool = e_tool;
   g_set_object(&p_tc->p_file, p_cur);
   p_tc->t_saved        = *enhance_ctrl_get_transform(p_tc->p_ec);
   p_tc->t_work         = p_tc->t_saved;
   p_tc->b_crop_dropped = FALSE;
   ggaze_viewer_set_overlay(_viewer(p_tc), _draw_cb, _drag_cb, p_tc);
   return (TRUE);
}

/* Lay the rectangle out on the base image (the whole of it, or the crop the
 * tool started with), or re-clamp it when the base changed size under it.
 * FALSE while the base size is not known yet. */
static gboolean
_ensure_rect(ToolCtrl *p_tc) {
   gint i_w, i_h;
   if (!enhance_ctrl_get_base_size(p_tc->p_ec, &i_w, &i_h) || i_w < 1 ||
       i_h < 1) {
      return (FALSE);
   }
   if (!p_tc->b_rect_set) {
      croprect_init_full(&p_tc->t_rect, i_w, i_h);
      p_tc->b_rect_set = TRUE;
   } else if (i_w != p_tc->i_base_w || i_h != p_tc->i_base_h) {
      croprect_clamp(&p_tc->t_rect, i_w, i_h);
   }
   p_tc->i_base_w = i_w;
   p_tc->i_base_h = i_h;
   return (TRUE);
}

/* `c`: show the base (the committed transform minus its crop) and lay the
 * rectangle out on it -- the previous crop, if there was one, so it can be
 * adjusted rather than redrawn. The base is a PREVIEW override, not a
 * commit: the committed crop keeps counting as work while the tool is open,
 * so `s` in the tool still exports it and navigating away still prompts
 * for it (committing "no crop" here used to lose it silently). */
static void
_start_crop(ToolCtrl *p_tc) {
   if (!_begin(p_tc, GGAZE_TOOL_CROP)) {
      return;
   }
   p_tc->b_rect_set    = p_tc->t_saved.b_crop;
   p_tc->t_rect        = p_tc->t_saved.t_crop;
   p_tc->d_aspect      = 0.0;
   p_tc->i_base_w      = 0;
   p_tc->i_base_h      = 0;
   p_tc->t_work.b_crop = FALSE;
   enhance_ctrl_set_preview_transform(p_tc->p_ec, &p_tc->t_work);
   _ensure_rect(p_tc); /* now if the size is known, else on first use */
   _redraw(p_tc);
   _status(p_tc, _CROP_HINT);
}

/* The angle as the status lines say it: "0.0°", "1.5° CW", "2.0° CCW" --
 * a zero angle has no direction (it used to read "0.0° CW"). g_ascii_formatd
 * keeps a decimal point regardless of locale. Caller frees. */
static char *
_angle_text(gdouble d_degrees) {
   char c_num[G_ASCII_DTOSTR_BUF_SIZE];
   g_ascii_formatd(c_num, sizeof(c_num), "%.1f", fabs(d_degrees));
   if (d_degrees == 0.0) {
      return (g_strdup_printf("%s°", c_num));
   }
   return (g_strdup_printf("%s° %s", c_num, d_degrees < 0.0 ? "CCW" : "CW"));
}

/* The straighten status line: the current angle, the keys, the auto-crop
 * state. Re-shown on every change so the angle is always readable. */
static void
_straighten_status(ToolCtrl *p_tc) {
   char *c_angle = _angle_text(p_tc->t_work.d_degrees);
   char *c_msg =
      g_strdup_printf("Straighten %s — drag along the horizon · h/l nudge "
                      "½° · A auto-crop %s · Enter applies, Esc cancels%s",
                      c_angle, p_tc->t_work.b_autocrop ? "on" : "off",
                      p_tc->b_crop_dropped ? " · crop removed (nothing of "
                                             "it left at this angle)"
                                           : "");
   _status(p_tc, c_msg);
   g_free(c_msg);
   g_free(c_angle);
}

static void
_start_straighten(ToolCtrl *p_tc) {
   if (!_begin(p_tc, GGAZE_TOOL_STRAIGHTEN)) {
      return;
   }
   _redraw(p_tc);
   _straighten_status(p_tc);
}

/* Push the working transform to the preview (straighten: every nudge and
 * horizon drag renders live). */
static void
_push_work(ToolCtrl *p_tc) {
   enhance_ctrl_set_transform(p_tc->p_ec, &p_tc->t_work);
   _redraw(p_tc);
}

/* --- crop editing --------------------------------------------------------- */

/* One keyboard step: 1% of the base's shorter side, at least a pixel, so a
 * press moves a visible amount on any image size. */
static gdouble
_nudge_step(ToolCtrl *p_tc) {
   return (MAX(1.0, floor(MIN(p_tc->i_base_w, p_tc->i_base_h) / 100.0)));
}

static void
_rect_move(ToolCtrl *p_tc, gdouble d_dx, gdouble d_dy) {
   croprect_move(&p_tc->t_rect, d_dx, d_dy, p_tc->i_base_w, p_tc->i_base_h);
   _redraw(p_tc);
}

static void
_rect_resize(ToolCtrl *p_tc, CropRectHit e_edge, gdouble d_dx, gdouble d_dy) {
   croprect_resize(&p_tc->t_rect, e_edge, d_dx, d_dy, p_tc->d_aspect,
                   p_tc->i_base_w, p_tc->i_base_h);
   _redraw(p_tc);
}

static void
_rect_aspect(ToolCtrl *p_tc, gdouble d_aspect) {
   p_tc->d_aspect = d_aspect;
   croprect_set_aspect(&p_tc->t_rect, d_aspect, p_tc->i_base_w, p_tc->i_base_h);
   _redraw(p_tc);
}

/* The aspect presets: 1-4 lock 1:1 / 3:2 / 4:3 / 16:9, 0 frees. TRUE iff
 * u_keyval is one of them, with the ratio in *p_aspect. */
static gboolean
_aspect_for_key(guint u_keyval, gdouble *p_aspect) {
   static const struct {
      guint   u_key;
      gdouble d_aspect;
   } ASPECTS[] = {{GDK_KEY_1, 1.0},
                  {GDK_KEY_2, 3.0 / 2.0},
                  {GDK_KEY_3, 4.0 / 3.0},
                  {GDK_KEY_4, 16.0 / 9.0},
                  {GDK_KEY_0, 0.0}};
   for (gsize u = 0; u < G_N_ELEMENTS(ASPECTS); u++) {
      if (ASPECTS[u].u_key == u_keyval) {
         *p_aspect = ASPECTS[u].d_aspect;
         return (TRUE);
      }
   }
   return (FALSE);
}

/* TRUE iff u_keyval is one of the crop tool's own editing keys: the moves
 * and resizes below plus the aspect presets. */
static gboolean
_is_crop_key(guint u_keyval) {
   gdouble d_unused;
   switch (u_keyval) {
   case GDK_KEY_h:
   case GDK_KEY_l:
   case GDK_KEY_j:
   case GDK_KEY_k:
   case GDK_KEY_H:
   case GDK_KEY_L:
   case GDK_KEY_J:
   case GDK_KEY_K:
      return (TRUE);
   default:
      return (_aspect_for_key(u_keyval, &d_unused));
   }
}

/* The crop tool's own keys (the common Enter/Esc/tool keys are handled
 * before this). A key that is not the tool's is never consumed, so q, s,
 * ?, t, Space ... keep their meaning at every moment of the session.
 * Every edit needs the rectangle laid out, which needs the base size:
 * until that is known the tool's OWN keys are consumed with a status line
 * rather than passed on to, say, win.prev (an earlier version swallowed
 * every plain key in that state, and `q` could not quit). */
static gboolean
_crop_key(ToolCtrl *p_tc, guint u_keyval) {
   if (!_is_crop_key(u_keyval)) {
      return (FALSE);
   }
   if (!_ensure_rect(p_tc)) {
      _status(p_tc, _RENDERING);
      return (TRUE);
   }
   gdouble d_step = _nudge_step(p_tc);
   gdouble d_aspect;
   switch (u_keyval) {
   case GDK_KEY_h:
      _rect_move(p_tc, -d_step, 0.0);
      return (TRUE);
   case GDK_KEY_l:
      _rect_move(p_tc, d_step, 0.0);
      return (TRUE);
   case GDK_KEY_k:
      _rect_move(p_tc, 0.0, -d_step);
      return (TRUE);
   case GDK_KEY_j:
      _rect_move(p_tc, 0.0, d_step);
      return (TRUE);
   case GDK_KEY_H: /* the right edge moves left: narrower */
      _rect_resize(p_tc, CROPRECT_HIT_RIGHT, -d_step, 0.0);
      return (TRUE);
   case GDK_KEY_L:
      _rect_resize(p_tc, CROPRECT_HIT_RIGHT, d_step, 0.0);
      return (TRUE);
   case GDK_KEY_K: /* the bottom edge moves up: shorter */
      _rect_resize(p_tc, CROPRECT_HIT_BOTTOM, 0.0, -d_step);
      return (TRUE);
   case GDK_KEY_J:
      _rect_resize(p_tc, CROPRECT_HIT_BOTTOM, 0.0, d_step);
      return (TRUE);
   default:
      break;
   }
   if (_aspect_for_key(u_keyval, &d_aspect)) {
      _rect_aspect(p_tc, d_aspect);
   }
   return (TRUE); /* _is_crop_key said so: nothing else reaches here */
}

/* A drag over the crop rectangle: BEGIN decides what was grabbed, every
 * later phase re-derives the rectangle from the one at BEGIN plus the total
 * offset (croprect_drag), so a drag never accumulates clamping error. The
 * whole gesture is ignored while the texture on screen is not the base the
 * rectangle is laid out on (the base preview is still rendering): pointer
 * pixels mapped through that other image's geometry would land on the
 * wrong image pixels -- the same guard under which _draw_crop hides the
 * rectangle, so nothing invisible can be edited. */
static void
_crop_drag(ToolCtrl *p_tc, const GgazeViewerGeom *p_g,
           GgazeViewerDragPhase e_phase, gdouble d_ix, gdouble d_iy) {
   if (!_ensure_rect(p_tc) || p_g->i_img_w != p_tc->i_base_w ||
       p_g->i_img_h != p_tc->i_base_h) {
      return;
   }
   if (e_phase == GGAZE_VIEWER_DRAG_BEGIN) {
      p_tc->e_hit        = croprect_hit(&p_tc->t_rect, d_ix, d_iy,
                                        _HIT_TOLERANCE_PX / p_g->d_scale);
      p_tc->t_drag_start = p_tc->t_rect;
      p_tc->d_drag_x0    = d_ix;
      p_tc->d_drag_y0    = d_iy;
      return;
   }
   if (p_tc->e_hit == CROPRECT_HIT_NONE) {
      return; /* the drag started outside the rectangle */
   }
   croprect_drag(&p_tc->t_rect, &p_tc->t_drag_start, p_tc->e_hit,
                 d_ix - p_tc->d_drag_x0, d_iy - p_tc->d_drag_y0, p_tc->d_aspect,
                 p_tc->i_base_w, p_tc->i_base_h);
   if (e_phase == GGAZE_VIEWER_DRAG_END) {
      p_tc->e_hit = CROPRECT_HIT_NONE;
   }
   _redraw(p_tc);
}

/* Enter in the crop tool: commit the rectangle (none, if it still covers the
 * whole base) and leave. Refused while the base preview is rendering: the
 * rectangle was laid out on a size the screen does not show yet. */
static gboolean
_apply_crop(ToolCtrl *p_tc) {
   if (enhance_ctrl_is_pending(p_tc->p_ec) || !_ensure_rect(p_tc)) {
      _status(p_tc, _RENDERING);
      return (FALSE);
   }
   Transform t_new = p_tc->t_work;
   CropRect  t_r   = p_tc->t_rect;
   croprect_round(&t_r);
   if (croprect_is_full(&t_r, p_tc->i_base_w, p_tc->i_base_h)) {
      t_new.b_crop = FALSE;
   } else {
      t_new.b_crop = TRUE;
      t_new.t_crop = t_r;
   }
   _leave(p_tc);
   enhance_ctrl_set_transform(p_tc->p_ec, &t_new);
   _status(p_tc, t_new.b_crop ? "Cropped — s saves a copy, Esc discards "
                                "the preview"
                              : "Crop removed (the whole image)");
   return (TRUE);
}

/* --- straighten editing --------------------------------------------------- */

/* Every change of the angle or the auto-crop flag goes through here: the
 * base image changes size with it, so a crop committed earlier is kept over
 * the same content (transform_rebase_crop, anchored on the centre the
 * straighten turns about) or, when nothing of it is left at this angle,
 * dropped and said so in the status line -- never silently, and never with
 * a title still claiming "crop". Esc restores the crop with the rest. */
static void
_change_straighten(ToolCtrl *p_tc, gdouble d_degrees, gboolean b_autocrop) {
   Transform t_old         = p_tc->t_work;
   p_tc->t_work.d_degrees  = d_degrees;
   p_tc->t_work.b_autocrop = b_autocrop;
   gint i_ow, i_oh;
   if (p_tc->t_work.b_crop &&
       enhance_ctrl_get_orig_size(p_tc->p_ec, &i_ow, &i_oh) &&
       !transform_rebase_crop(&p_tc->t_work, &t_old, i_ow, i_oh)) {
      p_tc->b_crop_dropped = TRUE;
   }
   _push_work(p_tc);
   _straighten_status(p_tc);
}

static void
_nudge(ToolCtrl *p_tc, gdouble d_delta) {
   _change_straighten(p_tc,
                      transform_clamp_angle(p_tc->t_work.d_degrees + d_delta),
                      p_tc->t_work.b_autocrop);
}

static gboolean
_straighten_key(ToolCtrl *p_tc, guint u_keyval) {
   switch (u_keyval) {
   case GDK_KEY_h:
   case GDK_KEY_minus:
   case GDK_KEY_underscore:
      _nudge(p_tc, -TRANSFORM_ANGLE_STEP); /* counter-clockwise */
      return (TRUE);
   case GDK_KEY_l:
   case GDK_KEY_plus:
   case GDK_KEY_equal:
      _nudge(p_tc, TRANSFORM_ANGLE_STEP); /* clockwise */
      return (TRUE);
   case GDK_KEY_A:
      _change_straighten(p_tc, p_tc->t_work.d_degrees,
                         !p_tc->t_work.b_autocrop);
      return (TRUE);
   default:
      return (FALSE);
   }
}

/* A drag in the straighten tool draws the horizon; on END the line's slope
 * (relative to the preview as it is now, so it ADDS to the current angle)
 * becomes the new angle and the image levels. An UPDATE or END without a
 * BEGIN -- the drag started before `R` was pressed, or a stale end -- has
 * no line to level by and is ignored, the way _crop_drag ignores a drag
 * that began outside the rectangle (it used to apply whatever the start
 * coordinates last held: a lone END at (300, 200) levelled by 35 degrees). */
static void
_straighten_drag(ToolCtrl *p_tc, GgazeViewerDragPhase e_phase, gdouble d_ix,
                 gdouble d_iy) {
   if (e_phase == GGAZE_VIEWER_DRAG_BEGIN) {
      p_tc->b_line    = TRUE;
      p_tc->d_drag_x0 = d_ix;
      p_tc->d_drag_y0 = d_iy;
   } else if (!p_tc->b_line) {
      return;
   }
   p_tc->d_line_x1 = d_ix;
   p_tc->d_line_y1 = d_iy;
   if (e_phase == GGAZE_VIEWER_DRAG_END) {
      p_tc->b_line  = FALSE;
      gdouble d_deg = transform_horizon_degrees(p_tc->d_drag_x0,
                                                p_tc->d_drag_y0, d_ix, d_iy);
      if (d_deg != 0.0) {
         _change_straighten(
            p_tc, transform_clamp_angle(p_tc->t_work.d_degrees + d_deg),
            p_tc->t_work.b_autocrop);
      }
   }
   _redraw(p_tc);
}

/* Enter in the straighten tool: the angle is already on the preview (every
 * nudge pushed it), so committing is leaving. */
static gboolean
_apply_straighten(ToolCtrl *p_tc) {
   char *c_angle = _angle_text(p_tc->t_work.d_degrees);
   char *c_msg   = g_strdup_printf("Straightened %s — s saves a "
                                   "copy, Esc discards the preview",
                                   c_angle);
   _leave(p_tc);
   _status(p_tc, c_msg);
   g_free(c_msg);
   g_free(c_angle);
   return (TRUE);
}

/* --- keys shared by both tools -------------------------------------------- */

/* Enter / Esc, and the tool keys themselves: the same tool's key cancels
 * (a toggle, like `a` for the panel), the other tool's key and the quarter
 * turns are refused until this tool is finished. */
static gboolean
_common_key(ToolCtrl *p_tc, guint u_keyval) {
   switch (u_keyval) {
   case GDK_KEY_Return:
   case GDK_KEY_KP_Enter:
      tool_ctrl_apply(p_tc);
      return (TRUE);
   case GDK_KEY_Escape:
      tool_ctrl_cancel(p_tc);
      return (TRUE);
   case GDK_KEY_c:
      if (p_tc->e_tool == GGAZE_TOOL_CROP) {
         tool_ctrl_cancel(p_tc);
      } else {
         _status(p_tc, _FINISH_FIRST);
      }
      return (TRUE);
   case GDK_KEY_R:
      if (p_tc->e_tool == GGAZE_TOOL_STRAIGHTEN) {
         tool_ctrl_cancel(p_tc);
      } else {
         _status(p_tc, _FINISH_FIRST);
      }
      return (TRUE);
   case GDK_KEY_bracketleft:
   case GDK_KEY_bracketright:
      _status(p_tc, _FINISH_FIRST);
      return (TRUE);
   default:
      return (FALSE);
   }
}

/* --- public API ----------------------------------------------------------- */

ToolCtrl *
tool_ctrl_new(EnhanceCtrl *p_ec, const ToolCtrlHostOps *p_ops,
              gpointer p_host) {
   g_return_val_if_fail(p_ec != NULL && p_ops != NULL, NULL);
   ToolCtrl *p_tc = g_new0(ToolCtrl, 1);
   p_tc->p_ec     = p_ec;
   p_tc->p_ops    = p_ops;
   p_tc->p_host   = p_host;
   transform_init(&p_tc->t_saved);
   transform_init(&p_tc->t_work);
   return (p_tc);
}

void
tool_ctrl_delete(ToolCtrl *p_tc) {
   if (p_tc == NULL) {
      return;
   }
   g_clear_object(&p_tc->p_file);
   g_free(p_tc);
}

void
tool_ctrl_dispose(ToolCtrl *p_tc) {
   if (p_tc != NULL && p_tc->e_tool != GGAZE_TOOL_NONE) {
      _leave(p_tc);
   }
}

GgazeTool
tool_ctrl_get_tool(ToolCtrl *p_tc) {
   g_return_val_if_fail(p_tc != NULL, GGAZE_TOOL_NONE);
   return (p_tc->e_tool);
}

void
tool_ctrl_toggle_crop(ToolCtrl *p_tc) {
   g_return_if_fail(p_tc != NULL);
   if (p_tc->e_tool == GGAZE_TOOL_CROP) {
      tool_ctrl_cancel(p_tc);
   } else if (p_tc->e_tool == GGAZE_TOOL_STRAIGHTEN) {
      _status(p_tc, _FINISH_FIRST);
   } else {
      _start_crop(p_tc);
   }
}

void
tool_ctrl_toggle_straighten(ToolCtrl *p_tc) {
   g_return_if_fail(p_tc != NULL);
   if (p_tc->e_tool == GGAZE_TOOL_STRAIGHTEN) {
      tool_ctrl_cancel(p_tc);
   } else if (p_tc->e_tool == GGAZE_TOOL_CROP) {
      _status(p_tc, _FINISH_FIRST);
   } else {
      _start_straighten(p_tc);
   }
}

gboolean
tool_ctrl_key(ToolCtrl *p_tc, guint u_keyval, GdkModifierType e_state) {
   g_return_val_if_fail(p_tc != NULL, FALSE);
   if (p_tc->e_tool == GGAZE_TOOL_NONE) {
      return (FALSE);
   }
   if ((e_state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) != 0) {
      return (FALSE); /* a chord (Ctrl+q, Ctrl+c, ...) is never ours */
   }
   if (_common_key(p_tc, u_keyval)) {
      return (TRUE);
   }
   if (p_tc->e_tool == GGAZE_TOOL_CROP) {
      return (_crop_key(p_tc, u_keyval));
   }
   return (_straighten_key(p_tc, u_keyval));
}

void
tool_ctrl_drag(ToolCtrl *p_tc, GgazeViewerDragPhase e_phase, gdouble d_x,
               gdouble d_y) {
   g_return_if_fail(p_tc != NULL);
   if (p_tc->e_tool == GGAZE_TOOL_NONE) {
      return;
   }
   GgazeViewer    *p_v = _viewer(p_tc);
   GgazeViewerGeom t_g;
   if (p_v == NULL || !ggaze_viewer_get_geometry(p_v, &t_g) ||
       t_g.d_scale <= 0.0) {
      return;
   }
   gdouble d_ix = (d_x - t_g.d_x) / t_g.d_scale;
   gdouble d_iy = (d_y - t_g.d_y) / t_g.d_scale;
   if (p_tc->e_tool == GGAZE_TOOL_CROP) {
      _crop_drag(p_tc, &t_g, e_phase, d_ix, d_iy);
   } else {
      _straighten_drag(p_tc, e_phase, d_ix, d_iy);
   }
}

gboolean
tool_ctrl_apply(ToolCtrl *p_tc) {
   g_return_val_if_fail(p_tc != NULL, FALSE);
   switch (p_tc->e_tool) {
   case GGAZE_TOOL_CROP:
      return (_apply_crop(p_tc));
   case GGAZE_TOOL_STRAIGHTEN:
      return (_apply_straighten(p_tc));
   case GGAZE_TOOL_NONE:
   default:
      return (FALSE);
   }
}

void
tool_ctrl_cancel(ToolCtrl *p_tc) {
   g_return_if_fail(p_tc != NULL);
   if (p_tc->e_tool == GGAZE_TOOL_NONE) {
      return;
   }
   GgazeTool e_was   = p_tc->e_tool;
   Transform t_saved = p_tc->t_saved;
   _leave(p_tc);
   if (e_was == GGAZE_TOOL_CROP) {
      /* The crop tool never committed anything: dropping its base override
       * is the whole restore (and keeps a saved preview saved). */
      enhance_ctrl_set_preview_transform(p_tc->p_ec, NULL);
   } else {
      enhance_ctrl_set_transform(p_tc->p_ec, &t_saved);
   }
   _status(p_tc, e_was == GGAZE_TOOL_CROP ? "Crop cancelled"
                                          : "Straighten cancelled");
}

void
tool_ctrl_abandon(ToolCtrl *p_tc) {
   g_return_if_fail(p_tc != NULL);
   if (p_tc->e_tool == GGAZE_TOOL_NONE) {
      return;
   }
   GgazeTool e_was = p_tc->e_tool;
   _leave(p_tc);
   if (e_was == GGAZE_TOOL_CROP) {
      /* The view left the large page under the tool: the committed crop
       * must be back on the preview when the view returns (the override
       * only re-renders, it never switches views). */
      enhance_ctrl_set_preview_transform(p_tc->p_ec, NULL);
   }
}

void
tool_ctrl_discarded(ToolCtrl *p_tc) {
   g_return_if_fail(p_tc != NULL);
   if (p_tc->e_tool == GGAZE_TOOL_NONE) {
      return;
   }
   /* Only leave: the discard that called this resets the transform and a
    * crop tool's override together, so restoring t_saved or clearing the
    * override here would just render once more for nothing. Before this
    * hook the tool stayed up with its t_work / t_saved intact, and the
    * next nudge or Enter re-applied the "discarded" angle or turn. */
   _leave(p_tc);
}

void
tool_ctrl_nav_changed(ToolCtrl *p_tc) {
   g_return_if_fail(p_tc != NULL);
   if (p_tc->e_tool == GGAZE_TOOL_NONE) {
      return;
   }
   GFile *p_cur = _current_file(p_tc);
   if (p_cur == NULL || p_tc->p_file == NULL ||
       !g_file_equal(p_cur, p_tc->p_file)) {
      /* Another file: only leave. The controller's own nav_changed, which
       * runs right after this, resets the transform AND a crop tool's
       * override together; clearing the override here would render the
       * old transform onto the new file first and make that reset think
       * the preview already belongs to it. */
      _leave(p_tc);
   }
}
