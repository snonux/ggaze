#ifndef GGAZE_ENHANCER_H
#define GGAZE_ENHANCER_H

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

/* Export the enhanced buffer to a file (JPEG quality 95, EXIF Orientation=1).
 * Returns TRUE on success. */
gboolean enhancer_export(Enhancer *p_e, GeglBuffer *p_in,
                         const EnhancerPreset *p_preset, GFile *p_out,
                         GError **p_err);

G_END_DECLS

#endif