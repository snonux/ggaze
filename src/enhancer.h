#ifndef GGAZE_ENHANCER_H
#define GGAZE_ENHANCER_H

#include "ggaze-config.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <gegl.h>

G_BEGIN_DECLS

typedef struct {
   char *c_name;
   char *c_graph; /* GEGL graph string (for user presets) or NULL (built-in) */
   int   i_builtin; /* 1 if built-in (programmatic), 0 if user (graph text) */
} EnhancerPreset;

typedef struct Enhancer Enhancer;

Enhancer *enhancer_new(void);
void      enhancer_delete(Enhancer *p_e);

void enhancer_set_presets(Enhancer *p_e, const GPtrArray *p_presets);
const GPtrArray *enhancer_get_presets(Enhancer *p_e);

/* Apply a preset to a GeglBuffer (returns a new buffer, or NULL on error). */
GeglBuffer *enhancer_apply(Enhancer *p_e, GeglBuffer *p_in,
                           const EnhancerPreset *p_preset, GError **p_err);

/* Apply a chain of the enabled built-in presets (bit i of u_mask -> preset i)
 * in array order, composing them. Returns a new buffer, or NULL if none are
 * enabled / on error. */
GeglBuffer *enhancer_apply_chain(Enhancer *p_e, GeglBuffer *p_in,
                                 const GPtrArray *p_presets, guint8 u_mask,
                                 GError **p_err);

/* Export the enhanced buffer to a file. The saver is chosen from p_out's
 * extension: .jpg/.jpeg -> gegl:jpg-save (quality 95), .png -> gegl:png-save,
 * .webp -> gegl:webp-save (if available). Other extensions fail with
 * G_IO_ERROR_NOT_SUPPORTED. Success is verified by a real stat of the output
 * (not pre-existence). Returns TRUE on success. */
gboolean enhancer_export(Enhancer *p_e, GeglBuffer *p_in,
                         const EnhancerPreset *p_preset, GFile *p_out,
                         GError **p_err);

/* Export p_in with the enabled-preset chain (u_mask) composed, to p_out. */
gboolean enhancer_export_chain(Enhancer *p_e, GeglBuffer *p_in,
                               const GPtrArray *p_presets, guint8 u_mask,
                               GFile *p_out, GError **p_err);

#if GGAZE_HAVE_GEGL
/* Load a file into a GeglBuffer via the gegl:load op. Returns a new buffer
 * (caller unrefs) or NULL with p_err set. */
GeglBuffer *enhancer_load(GFile *p_file, GError **p_err);

/* Convert a GeglBuffer to a GdkTexture for preview (RGBA8 bytes). Returns a
 * new GdkTexture (caller unrefs) or NULL with p_err set. Needs no display. */
GdkTexture *enhancer_buffer_to_texture(GeglBuffer *p_buf, GError **p_err);

/* Async: load p_file, apply the enabled-preset chain (u_mask), and convert
 * the result to a GdkTexture, all off the calling thread (a GTask worker) so
 * a caller with a main loop (e.g. the window) is never blocked by GEGL's
 * CPU-heavy processing (tu0). p_presets is snapshotted internally before the
 * worker starts, so a concurrent enhancer_set_presets() (Preferences apply)
 * cannot race it. p_cancel may be NULL. Since GEGL processing itself cannot
 * be interrupted mid-flight, cancellation only skips work that has not
 * started yet -- a caller that needs last-write-wins semantics (e.g. a newer
 * apply superseding this one) must still check that on its own before using
 * the finished result. */
void enhancer_apply_chain_async(Enhancer *p_e, GFile *p_file,
                                const GPtrArray *p_presets, guint8 u_mask,
                                GCancellable       *p_cancel,
                                GAsyncReadyCallback p_cb, gpointer p_data);

/* Finish enhancer_apply_chain_async(). Returns a new GdkTexture (caller
 * unrefs) or NULL with p_err set. */
GdkTexture *enhancer_apply_chain_finish(GAsyncResult *p_res, GError **p_err);
#endif /* GGAZE_HAVE_GEGL */

G_END_DECLS

#endif