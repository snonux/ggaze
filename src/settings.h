#ifndef GGAZE_SETTINGS_H
#define GGAZE_SETTINGS_H

/*:*
 * ggaze — GSettings wrapper
 *
 * Settings is a plain-C wrapper over the `org.buetow.ggaze` GSettings schema.
 * It owns a single GSettings instance and exposes typed accessors for every
 * key: scalar preferences (sort, wrap, background, scroll behavior, slideshow
 * delay, thumbnail size, hide-trashed, hide-raw-sidecars) and the ordered
 * a(ss) lists (destinations, editors, scripts, enhance-presets). List setters
 * validate each (name, value) pair and persist only the accepted entries, so
 * bad data round-tripped from gsettings / dconf is normalized on write.
 *
 * Owns no GtkWidget and needs no display, so it is unit-testable standalone.
 * The Preferences UI (prefs.c) binds widgets to the underlying GSettings via
 * settings_get_gsettings(); list editing goes through these accessors so the
 * validation path is the only path. See docs/architecture.md "settings".
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
:*/

#include "navigator.h"

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

#define GGAZE_SETTINGS_SCHEMA_ID "org.buetow.ggaze"
#define GGAZE_SETTINGS_PATH "/org/buetow/ggaze/"

/* Background colour behind the image in large view. Order matches the
 * `org.buetow.ggaze.background` schema enum (black=0 ... checker=3). */
typedef enum {
   GGAZE_BG_BLACK = 0,
   GGAZE_BG_DARK,
   GGAZE_BG_GREY,
   GGAZE_BG_CHECKER
} GgazeBackground;

/* What the mouse scroll wheel does in large view. Order matches the
 * `org.buetow.ggaze.scroll-behavior` schema enum (zoom=0 ... navigate=2). */
typedef enum {
   GGAZE_SCROLL_ZOOM = 0,
   GGAZE_SCROLL_PAN_WHEN_ZOOMED,
   GGAZE_SCROLL_NAVIGATE
} GgazeScrollBehavior;

#include "settings-pair.h"

typedef struct Settings Settings;

/* Create a wrapper over the default GSettings instance for the schema. */
Settings *settings_new(void);
/* Create a wrapper adopting an existing GSettings (for tests; takes a ref). */
Settings *settings_new_for_gsettings(GSettings *p_gs);
void      settings_delete(Settings *p_s);

/* Borrowed underlying GSettings, for g_settings_bind in the Preferences UI. */
GSettings *settings_get_gsettings(Settings *p_s);

/* --- scalar keys --------------------------------------------------------- */
GgazeSort       settings_get_sort(Settings *p_s);
void            settings_set_sort(Settings *p_s, GgazeSort e_sort);
gboolean        settings_get_wrap(Settings *p_s);
void            settings_set_wrap(Settings *p_s, gboolean b_wrap);
GgazeBackground settings_get_background(Settings *p_s);
void            settings_set_background(Settings *p_s, GgazeBackground e_bg);
GgazeScrollBehavior settings_get_scroll_behavior(Settings *p_s);
void     settings_set_scroll_behavior(Settings *p_s, GgazeScrollBehavior e_sb);
gdouble  settings_get_slideshow_delay(Settings *p_s);
void     settings_set_slideshow_delay(Settings *p_s, gdouble d_sec);
gint     settings_get_thumbnail_size(Settings *p_s);
void     settings_set_thumbnail_size(Settings *p_s, gint i_px);
gboolean settings_get_hide_trashed(Settings *p_s);
void     settings_set_hide_trashed(Settings *p_s, gboolean b_hide);
gboolean settings_get_hide_raw(Settings *p_s);
void     settings_set_hide_raw(Settings *p_s, gboolean b_hide);
gboolean settings_get_enhance_preview_thumbnails(Settings *p_s);
void settings_set_enhance_preview_thumbnails(Settings *p_s, gboolean b_enabled);

/* --- ordered a(ss) lists ------------------------------------------------- */
/* Each getter returns a freshly-allocated GPtrArray of SettingsPair* (transfer
 * full; unref with g_ptr_array_unref — the free func calls settings_pair_free).
 * Empty list -> a zero-length array (never NULL). */
GPtrArray *settings_get_destinations(Settings *p_s);
GPtrArray *settings_get_editors(Settings *p_s);
GPtrArray *settings_get_scripts(Settings *p_s);
GPtrArray *settings_get_enhance_presets(Settings *p_s);

/* Replace the stored list with the given pairs, validating each entry. Invalid
 * entries (empty name / empty value, or non-absolute path when b_require_path)
 * are dropped; the rest are persisted in order. p_pairs may be NULL or empty
 * to clear the key. Returns the number of entries actually stored. The
 * destinations key requires absolute paths; editors/scripts/enhance-presets
 * accept any non-empty command/graph string. */
guint settings_set_destinations(Settings *p_s, const GPtrArray *p_pairs);
guint settings_set_editors(Settings *p_s, const GPtrArray *p_pairs);
guint settings_set_scripts(Settings *p_s, const GPtrArray *p_pairs);
guint settings_set_enhance_presets(Settings *p_s, const GPtrArray *p_pairs);

/* Validate one (name, value) pair. b_require_path additionally requires value
 * to be an absolute filesystem path. Exposed for tests and the Preferences UI
 * to give immediate feedback before a write. */
gboolean settings_pair_valid(const char *c_name, const char *c_value,
                             gboolean b_require_path);

G_END_DECLS

#endif /* GGAZE_SETTINGS_H */
