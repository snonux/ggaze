#ifndef GGAZE_OPENER_H
#define GGAZE_OPENER_H

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
   char *c_name;
   char *c_command; /* may contain %f */
} OpenerProg;

typedef struct Opener Opener;

Opener          *opener_new(void);
void             opener_delete(Opener *p_o);
void             opener_set_progs(Opener *p_o, const GPtrArray *p_progs);
const GPtrArray *opener_get_progs(Opener *p_o);

/* Launch the program with %f expanded to the file's path. Detached. Returns
 * TRUE if the process was started. */
gboolean opener_launch(Opener *p_o, GFile *p_file, const OpenerProg *p_prog,
                       GError **p_err);

G_END_DECLS

#endif