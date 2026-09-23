#ifndef GGAZE_VIEWLOAD_H
#define GGAZE_VIEWLOAD_H

/*:*
 * ggaze — view-load pipeline (large view)
 *
 * Everything between "navigator.current changed" and "a texture is on
 * screen": the bounded texture LRU, the one visible-load GCancellable, the
 * prefetch round for the two neighbours, the progressive low-res partial
 * marshalled from the decode thread, and the last-write-wins rule that
 * decides which result may touch the viewer. This is where AGENTS.md's
 * "one active load per window" and "viewer only shows a texture whose path
 * == navigator.current" invariants are enforced, so it lives in a plain-C
 * module that a headless unit test can drive (tests/test_viewload.c)
 * instead of inside GgazeWindow. The widget side is reached through a tiny
 * host vtable: show a texture, update the title, show a status line.
 *
 * Failure is reported, never swallowed: when the visible load of the current
 * file fails the canvas is cleared and a status line names the file and the
 * error, so the previous picture can never sit under the new file's title.
 *
 * Reference counted internally (the in-flight load contexts hold refs), so a
 * host that disposes mid-load is safe: viewload_dispose() cancels everything
 * and the callbacks then return without touching the host.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>

#include "navigator.h"

G_BEGIN_DECLS

typedef struct ViewLoad ViewLoad;

/* Host-side operations. p_host is borrowed for the duration of each call and
 * is never called after viewload_dispose(). */
typedef struct {
   /* Put p_tex -- the current file's decoded texture, from the cache or a
    * finished load -- on screen (NULL clears the canvas). The host applies
    * its own overrides here (e.g. an active enhance preview) and may
    * remember p_tex as the file's picture. */
   void (*show_texture)(gpointer p_host, GdkTexture *p_tex);
   /* Put a progressive loader's low-res partial of the file still decoding
    * on screen: a stand-in the full result replaces through show_texture,
    * never the file's picture -- the host must not remember it as such
    * (its size is not the image's). */
   void (*show_partial)(gpointer p_host, GdkTexture *p_tex);
   /* Refresh the title after the current file changed. */
   void (*update_header)(gpointer p_host);
   /* Transient status line for a load failure. */
   void (*show_status)(gpointer p_host, const char *c_msg);
} ViewLoadHostOps;

/* u_cache_cap is the texture LRU capacity (AGENTS.md: 4). */
ViewLoad *viewload_new(const ViewLoadHostOps *p_ops, gpointer p_host,
                       guint u_cache_cap);

/* Cancel every in-flight load and stop calling the host (host dispose). */
void viewload_dispose(ViewLoad *p_vl);

/* Drop the owner's reference (host finalize, after dispose). */
void viewload_delete(ViewLoad *p_vl);

/* The navigator whose current file is shown (borrowed; NULL when no folder
 * is open). Set on open, cleared before the navigator is released. */
void viewload_set_navigator(ViewLoad *p_vl, Navigator *p_nav);

/* Show navigator.current: synchronously from the cache when it is there,
 * else cancel the previous visible load and start a new async one. Updates
 * the header either way and prefetches the neighbours. With no current file
 * the canvas is cleared. */
void viewload_load_current(ViewLoad *p_vl);

/* The cached decoded texture for p_file (transfer none) or NULL. */
GdkTexture *viewload_get_cached(ViewLoad *p_vl, GFile *p_file);

/* Drop every cached texture (test seam: forces the next load onto the async
 * path). */
void viewload_clear_cache(ViewLoad *p_vl);

G_END_DECLS

#endif /* GGAZE_VIEWLOAD_H */
