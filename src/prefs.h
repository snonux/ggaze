#ifndef GGAZE_PREFS_H
#define GGAZE_PREFS_H

/*:*
 * ggaze — preferences window
 *
 * prefs_show() builds an AdwPreferencesWindow bound to the org.buetow.ggaze
 * GSettings schema: scalar keys (sort, wrap, background, scroll behavior,
 * slideshow delay, thumbnail size, hide-trashed) via g_settings_bind, and the
 * ordered a(ss) lists (destinations, editors, scripts, enhance presets) via a
 * row-per-entry editor with add / remove / move-up / move-down that persists
 * through the Settings wrapper's validated setters. This is a UI module: it
 * owns GtkWidget and needs a display; the settings logic stays in the plain-C
 * Settings wrapper. See docs/architecture.md "settings".
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
:*/

#include "settings.h"

#include <adwaita.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Build (but do not present) an AdwPreferencesDialog bound to the schema. The
 * caller owns the returned dialog (unref with g_object_unref / adw_dialog…).
 * Exposed so tests can exercise construction without presenting. */
AdwPreferencesDialog *prefs_build_dialog(Settings *p_settings);

/* Build and present a modal AdwPreferencesDialog parented to p_parent.
 * p_settings is borrowed for the lifetime of the dialog (the dialog closes
 * before the caller may free it). */
void prefs_show(Settings *p_settings, GtkWidget *p_parent);

G_END_DECLS

#endif /* GGAZE_PREFS_H */