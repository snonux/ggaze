/*:*
 * ggaze — Preferences dialog (AdwPreferencesDialog over org.buetow.ggaze)
 *
 * Every scalar row binds straight to its GSettings key; the enum combo rows
 * take their choices from the schema itself (g_settings_schema_key_get_range)
 * so there is no hand-mirrored nick table to keep in step. The four ordered
 * a(ss) lists (destinations, editors, scripts, enhance presets) get a list
 * group each with add / edit / move / remove, validated live in the entry
 * dialog. All per-dialog state is allocated per dialog and freed with it.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "prefs.h"

#include <adwaita.h>
#include <glib.h>
#include <gtk/gtk.h>

#include "ggaze-config.h"
#include "preset-strength.h"

/* --- enum <-> combo mapping ---------------------------------------------- */

/* The choices of one enum key, read from the schema at build time. */
typedef struct {
   char  *c_key;
   char **pp_nicks; /* NULL-terminated, schema order == enum value order */
   guint  u_n;
} EnumSpec;

static void
_enum_spec_free(gpointer p) {
   EnumSpec *p_e = (EnumSpec *)p;
   g_free(p_e->c_key);
   g_strfreev(p_e->pp_nicks);
   g_free(p_e);
}

/* "capture-time" -> "Capture time": the schema nick is the label's source. */
static char *
_label_from_nick(const char *c_nick) {
   char *c_lbl = g_strdup(c_nick);
   for (char *p = c_lbl; *p != '\0'; p++) {
      if (*p == '-') {
         *p = ' ';
      }
   }
   if (c_lbl[0] != '\0') {
      c_lbl[0] = (char)g_ascii_toupper(c_lbl[0]);
   }
   return (c_lbl);
}

/* Read the enum choices for c_key from the installed schema. */
static EnumSpec *
_enum_spec_new(GSettings *p_gs, const char *c_key) {
   GSettingsSchema *p_schema = NULL;
   g_object_get(p_gs, "settings-schema", &p_schema, NULL);
   GSettingsSchemaKey *p_k       = g_settings_schema_get_key(p_schema, c_key);
   GVariant           *p_range   = g_settings_schema_key_get_range(p_k);
   GVariant           *p_choices = NULL;
   const char         *c_kind    = NULL;
   g_variant_get(p_range, "(&sv)", &c_kind, &p_choices);
   EnumSpec *p_e = g_new0(EnumSpec, 1);
   p_e->c_key    = g_strdup(c_key);
   p_e->pp_nicks = g_variant_dup_strv(p_choices, NULL);
   p_e->u_n      = g_strv_length(p_e->pp_nicks);
   g_variant_unref(p_choices);
   g_variant_unref(p_range);
   g_settings_schema_key_unref(p_k);
   g_settings_schema_unref(p_schema);
   return (p_e);
}

/* GSettings enum keys are stored as nick strings; AdwComboRow:selected is a
 * guint index. These mappings convert between the two. */
static gboolean
_enum_get(GValue *p_val, GVariant *p_var, gpointer p_data) {
   const EnumSpec *p_e    = (const EnumSpec *)p_data;
   const gchar    *c_nick = g_variant_get_string(p_var, NULL);
   guint           u_sel  = 0;
   for (guint u = 0; u < p_e->u_n; u++) {
      if (g_str_equal(c_nick, p_e->pp_nicks[u])) {
         u_sel = u;
         break;
      }
   }
   g_value_set_uint(p_val, u_sel);
   return (TRUE);
}

static GVariant *
_enum_set(const GValue *p_val, const GVariantType *p_type, gpointer p_data) {
   (void)p_type;
   const EnumSpec *p_e = (const EnumSpec *)p_data;
   guint           u   = g_value_get_uint(p_val);
   if (u >= p_e->u_n) {
      u = 0;
   }
   return (g_variant_new_string(p_e->pp_nicks[u]));
}

/* int <-> double (AdwSpinRow:value is double; thumbnail-size is int). */
static gboolean
_int_get(GValue *p_val, GVariant *p_var, gpointer p_data) {
   (void)p_data;
   g_value_set_double(p_val, (gdouble)g_variant_get_int32(p_var));
   return (TRUE);
}

static GVariant *
_int_set(const GValue *p_val, const GVariantType *p_type, gpointer p_data) {
   (void)p_type;
   (void)p_data;
   return (g_variant_new_int32((gint)g_value_get_double(p_val)));
}

static GtkWidget *
_make_combo_row(const char *c_title, const char *c_key, GSettings *p_gs) {
   EnumSpec      *p_spec = _enum_spec_new(p_gs, c_key);
   GtkStringList *p_list = gtk_string_list_new(NULL);
   for (guint u = 0; u < p_spec->u_n; u++) {
      char *c_lbl = _label_from_nick(p_spec->pp_nicks[u]);
      gtk_string_list_append(p_list, c_lbl);
      g_free(c_lbl);
   }
   AdwComboRow *p_row = ADW_COMBO_ROW(adw_combo_row_new());
   adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p_row), c_title);
   adw_combo_row_set_model(p_row, G_LIST_MODEL(p_list));
   g_object_unref(p_list);
   /* The spec lives as long as the binding (the row). */
   g_settings_bind_with_mapping(p_gs, c_key, p_row, "selected",
                                G_SETTINGS_BIND_DEFAULT, _enum_get, _enum_set,
                                p_spec, _enum_spec_free);
   return (GTK_WIDGET(p_row));
}

static GtkWidget *
_make_spin_row(const char *c_title, gdouble d_min, gdouble d_max,
               gdouble d_step, const char *c_key, GSettings *p_gs,
               gboolean b_is_int) {
   GtkAdjustment *p_adj =
      gtk_adjustment_new(d_min, d_min, d_max, d_step, 0.0, 0.0);
   AdwSpinRow *p_row = ADW_SPIN_ROW(adw_spin_row_new(p_adj, d_step, 0));
   adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p_row), c_title);
   if (b_is_int) {
      adw_spin_row_set_digits(p_row, 0);
      g_settings_bind_with_mapping(p_gs, c_key, p_row, "value",
                                   G_SETTINGS_BIND_DEFAULT, _int_get, _int_set,
                                   NULL, NULL);
   } else {
      adw_spin_row_set_digits(p_row, 2);
      g_settings_bind(p_gs, c_key, p_row, "value", G_SETTINGS_BIND_DEFAULT);
   }
   return (GTK_WIDGET(p_row));
}

static GtkWidget *
_make_switch_row(const char *c_title, const char *c_key, GSettings *p_gs) {
   AdwSwitchRow *p_row = ADW_SWITCH_ROW(adw_switch_row_new());
   adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p_row), c_title);
   g_settings_bind(p_gs, c_key, p_row, "active", G_SETTINGS_BIND_DEFAULT);
   return (GTK_WIDGET(p_row));
}

/* --- ordered a(ss) list editor ------------------------------------------- */

/* One list group. Allocated per dialog (see PrefsLists) so two dialogs, or a
 * rebuilt one, never share row closures. */
typedef struct {
   Settings   *p_s;     /* borrowed */
   GtkWidget  *p_group; /* AdwPreferencesGroup for this list */
   GPtrArray  *p_rows;  /* added row widgets (borrowed; owned by the group) */
   const char *c_title;
   const char *c_description; /* what the list is for, under the title */
   const char *c_placeholder; /* value entry placeholder (per list) */
   gboolean    b_require_path;
   /* An extra check of the value beyond settings_pair_valid (NULL: none):
    * FALSE with *c_why_out (freed by the caller) saying what is wrong. The
    * enhance presets check their strength placeholder with it (8i2). */
   gboolean (*check)(const char *c_value, char **c_why_out);
   GPtrArray *(*get)(Settings *);
   guint (*set)(Settings *, const GPtrArray *);
} ListSpec;

#define GGAZE_PREFS_N_LISTS 4

/* Every per-dialog list editor, freed with the dialog. */
typedef struct {
   ListSpec t_specs[GGAZE_PREFS_N_LISTS];
   guint    u_n;
} PrefsLists;

static void
_prefs_lists_free(gpointer p) {
   PrefsLists *p_l = (PrefsLists *)p;
   for (guint u = 0; u < p_l->u_n; u++) {
      g_clear_pointer(&p_l->t_specs[u].p_rows, g_ptr_array_unref);
   }
   g_free(p_l);
}

typedef struct {
   ListSpec *p_spec;
   guint     u_index;
   gint      i_delta; /* -1 up, +1 down, 0 = remove */
} RowCtx;

static void
_row_ctx_free(gpointer p, GClosure *p_c) {
   (void)p_c;
   g_free(p);
}

static void _list_refresh(ListSpec *p_spec);
static void _list_edit_dialog(ListSpec *p_spec, gint i_index);

static void
_list_remove(GtkButton *p_btn, gpointer p_data) {
   (void)p_btn;
   RowCtx    *p_ctx = (RowCtx *)p_data;
   GPtrArray *p_cur = p_ctx->p_spec->get(p_ctx->p_spec->p_s);
   if (p_ctx->u_index < p_cur->len) {
      g_ptr_array_remove_index(p_cur, p_ctx->u_index);
   }
   p_ctx->p_spec->set(p_ctx->p_spec->p_s, p_cur);
   g_ptr_array_unref(p_cur);
   _list_refresh(p_ctx->p_spec);
}

static void
_list_move(GtkButton *p_btn, gpointer p_data) {
   (void)p_btn;
   RowCtx    *p_ctx = (RowCtx *)p_data;
   GPtrArray *p_cur = p_ctx->p_spec->get(p_ctx->p_spec->p_s);
   guint      u     = p_ctx->u_index;
   gint       i_new = (gint)u + p_ctx->i_delta;
   if (i_new < 0 || i_new >= (gint)p_cur->len) {
      g_ptr_array_unref(p_cur);
      return;
   }
   gpointer p = g_ptr_array_index(p_cur, u);
   g_ptr_array_remove_index(p_cur, u);
   g_ptr_array_insert(p_cur, (guint)i_new, p);
   p_ctx->p_spec->set(p_ctx->p_spec->p_s, p_cur);
   g_ptr_array_unref(p_cur);
   _list_refresh(p_ctx->p_spec);
}

static void
_list_edit(GtkButton *p_btn, gpointer p_data) {
   (void)p_btn;
   RowCtx *p_ctx = (RowCtx *)p_data;
   _list_edit_dialog(p_ctx->p_spec, (gint)p_ctx->u_index);
}

/* One flat icon suffix button on a row, with its own RowCtx freed with the
 * closure so removing/refreshing rows never leaves dangling callbacks. */
static GtkWidget *
_row_button(ListSpec *p_spec, guint u_index, gint i_delta, const char *c_icon,
            const char *c_tip, GCallback fn) {
   RowCtx *p_ctx  = g_new(RowCtx, 1);
   p_ctx->p_spec  = p_spec;
   p_ctx->u_index = u_index;
   p_ctx->i_delta = i_delta;
   GtkWidget *p_b = gtk_button_new_from_icon_name(c_icon);
   gtk_widget_add_css_class(p_b, "flat");
   gtk_widget_set_tooltip_text(p_b, c_tip);
   g_signal_connect_data(p_b, "clicked", fn, p_ctx,
                         (GClosureNotify)_row_ctx_free, 0);
   return (p_b);
}

static void
_list_refresh(ListSpec *p_spec) {
   /* Remove previously-added rows (tracked in p_rows) before rebuilding;
    * AdwPreferencesGroup exposes no list-box accessor. */
   for (guint u = 0; u < p_spec->p_rows->len; u++) {
      adw_preferences_group_remove(ADW_PREFERENCES_GROUP(p_spec->p_group),
                                   g_ptr_array_index(p_spec->p_rows, u));
   }
   g_ptr_array_set_size(p_spec->p_rows, 0);

   GPtrArray *p_cur = p_spec->get(p_spec->p_s);
   for (guint u = 0; u < p_cur->len; u++) {
      const SettingsPair *p_pr  = g_ptr_array_index(p_cur, u);
      AdwActionRow       *p_row = ADW_ACTION_ROW(adw_action_row_new());
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p_row), p_pr->c_name);
      adw_action_row_set_subtitle(p_row, p_pr->c_value);
      adw_action_row_add_suffix(
         p_row, _row_button(p_spec, u, 0, "document-edit-symbolic", "Edit",
                            G_CALLBACK(_list_edit)));
      adw_action_row_add_suffix(p_row,
                                _row_button(p_spec, u, -1, "go-up-symbolic",
                                            "Move up", G_CALLBACK(_list_move)));
      adw_action_row_add_suffix(
         p_row, _row_button(p_spec, u, 1, "go-down-symbolic", "Move down",
                            G_CALLBACK(_list_move)));
      adw_action_row_add_suffix(
         p_row, _row_button(p_spec, u, 0, "edit-delete-symbolic", "Remove",
                            G_CALLBACK(_list_remove)));
      adw_preferences_group_add(ADW_PREFERENCES_GROUP(p_spec->p_group),
                                GTK_WIDGET(p_row));
      g_ptr_array_add(p_spec->p_rows, p_row);
   }
   g_ptr_array_unref(p_cur);
}

/* --- add / edit entry dialog -------------------------------------------- */

typedef struct {
   ListSpec    *p_spec;
   AdwDialog   *p_dlg;
   GtkEditable *p_name;
   GtkEditable *p_value;
   GtkWidget   *p_hint;  /* why the entry is not valid yet */
   gint         i_index; /* row being edited, -1 = adding */
} EditCtx;

#if GGAZE_HAVE_GEGL
/* The enhance presets' extra check: a strength placeholder, if the graph
 * has one, must be well formed -- the message says what is wrong. (GEGL
 * builds only, like the preset list itself.) */
static gboolean
_check_preset_graph(const char *c_value, char **c_why_out) {
   GError *p_err = NULL;
   if (preset_strength_parse(c_value, NULL, NULL, &p_err)) {
      return (TRUE);
   }
   *c_why_out = g_strdup(p_err->message);
   g_error_free(p_err);
   return (FALSE);
}
#endif

/* TRUE iff (c_name, c_value) may be stored in p_spec's list; otherwise
 * *c_why_out (caller frees) says why, for the hint line. */
static gboolean
_pair_ok(const ListSpec *p_spec, const char *c_name, const char *c_value,
         char **c_why_out) {
   *c_why_out = NULL;
   if (c_name == NULL || *c_name == '\0') {
      *c_why_out = g_strdup("A name is required.");
   } else if (c_value == NULL || *c_value == '\0') {
      *c_why_out =
         g_strdup(p_spec->b_require_path ? "A folder path is required."
                                         : "A command is required.");
   } else if (!settings_pair_valid(c_name, c_value, p_spec->b_require_path)) {
      *c_why_out = g_strdup("The path must be absolute (start with /).");
   } else if (p_spec->check != NULL) {
      p_spec->check(c_value, c_why_out);
   }
   return (*c_why_out == NULL);
}

/* Live validation: the OK response is enabled only for a valid pair, and
 * the hint says what is missing, instead of the dialog silently dropping an
 * invalid entry on OK (which read as "Add does nothing"). */
static void
_edit_validate(GtkEditable *p_e, gpointer p_data) {
   (void)p_e;
   EditCtx *p_ctx = (EditCtx *)p_data;
   char    *c_why = NULL;
   gboolean b_ok = _pair_ok(p_ctx->p_spec, gtk_editable_get_text(p_ctx->p_name),
                            gtk_editable_get_text(p_ctx->p_value), &c_why);
   gtk_label_set_text(GTK_LABEL(p_ctx->p_hint), c_why != NULL ? c_why : "");
   g_free(c_why);
   adw_alert_dialog_set_response_enabled(ADW_ALERT_DIALOG(p_ctx->p_dlg), "ok",
                                         b_ok);
}

static void
_edit_confirm_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   EditCtx    *p_ctx = (EditCtx *)p_data;
   const char *c_resp =
      adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(p_src), p_res);
   if (c_resp != NULL && g_str_equal(c_resp, "ok")) {
      const char *c_name  = gtk_editable_get_text(p_ctx->p_name);
      const char *c_value = gtk_editable_get_text(p_ctx->p_value);
      char       *c_why   = NULL;
      gboolean    b_ok    = _pair_ok(p_ctx->p_spec, c_name, c_value, &c_why);
      g_free(c_why);
      if (b_ok) {
         GPtrArray *p_cur = p_ctx->p_spec->get(p_ctx->p_spec->p_s);
         if (p_ctx->i_index >= 0 && (guint)p_ctx->i_index < p_cur->len) {
            settings_pair_free(g_ptr_array_index(p_cur, p_ctx->i_index));
            g_ptr_array_index(p_cur, p_ctx->i_index) =
               settings_pair_new(c_name, c_value);
         } else {
            g_ptr_array_add(p_cur, settings_pair_new(c_name, c_value));
         }
         p_ctx->p_spec->set(p_ctx->p_spec->p_s, p_cur);
         g_ptr_array_unref(p_cur);
         _list_refresh(p_ctx->p_spec);
      }
   }
   g_free(p_ctx);
}

/* The entry form: name + value entries with the list's own placeholder and
 * a hint line. Returns the box; the entries are handed back via p_ctx. */
static GtkWidget *
_edit_form(ListSpec *p_spec, EditCtx *p_ctx, const SettingsPair *p_initial) {
   GtkWidget *p_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
   gtk_widget_set_margin_start(p_box, 12);
   gtk_widget_set_margin_end(p_box, 12);
   gtk_widget_set_margin_top(p_box, 12);
   gtk_widget_set_margin_bottom(p_box, 12);
   GtkWidget *p_name = gtk_entry_new();
   gtk_entry_set_placeholder_text(GTK_ENTRY(p_name),
                                  "Name (shown in the list)");
   GtkWidget *p_value = gtk_entry_new();
   gtk_entry_set_placeholder_text(GTK_ENTRY(p_value), p_spec->c_placeholder);
   if (p_initial != NULL) {
      gtk_editable_set_text(GTK_EDITABLE(p_name), p_initial->c_name);
      gtk_editable_set_text(GTK_EDITABLE(p_value), p_initial->c_value);
   }
   GtkWidget *p_hint = gtk_label_new("");
   gtk_widget_add_css_class(p_hint, "dim-label");
   gtk_label_set_wrap(GTK_LABEL(p_hint), TRUE);
   gtk_widget_set_halign(p_hint, GTK_ALIGN_START);
   gtk_box_append(GTK_BOX(p_box), p_name);
   gtk_box_append(GTK_BOX(p_box), p_value);
   gtk_box_append(GTK_BOX(p_box), p_hint);
   p_ctx->p_name  = GTK_EDITABLE(p_name);
   p_ctx->p_value = GTK_EDITABLE(p_value);
   p_ctx->p_hint  = p_hint;
   return (p_box);
}

/* Add (i_index < 0) or edit (row i_index) an entry of p_spec's list. */
static void
_list_edit_dialog(ListSpec *p_spec, gint i_index) {
   GPtrArray          *p_cur     = p_spec->get(p_spec->p_s);
   const SettingsPair *p_initial = NULL;
   if (i_index >= 0 && (guint)i_index < p_cur->len) {
      p_initial = g_ptr_array_index(p_cur, i_index);
   }
   EditCtx *p_ctx   = g_new0(EditCtx, 1);
   p_ctx->p_spec    = p_spec;
   p_ctx->i_index   = p_initial != NULL ? i_index : -1;
   GtkWidget *p_box = _edit_form(p_spec, p_ctx, p_initial);
   g_ptr_array_unref(p_cur);

   AdwDialog *p_dlg = adw_alert_dialog_new(
      p_initial != NULL ? "Edit entry" : "Add entry", p_spec->c_description);
   p_ctx->p_dlg = p_dlg;
   adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(p_dlg), p_box);
   adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(p_dlg), "cancel", "Cancel",
                                  "ok", p_initial != NULL ? "Save" : "Add",
                                  NULL);
   adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(p_dlg), "ok",
                                            ADW_RESPONSE_SUGGESTED);
   adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(p_dlg), "ok");
   adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(p_dlg), "cancel");
   g_signal_connect(p_ctx->p_name, "changed", G_CALLBACK(_edit_validate),
                    p_ctx);
   g_signal_connect(p_ctx->p_value, "changed", G_CALLBACK(_edit_validate),
                    p_ctx);
   _edit_validate(NULL, p_ctx);
   adw_alert_dialog_choose(ADW_ALERT_DIALOG(p_dlg), p_spec->p_group, NULL,
                           _edit_confirm_cb, p_ctx);
}

static void
_list_add(GtkButton *p_btn, gpointer p_data) {
   (void)p_btn;
   _list_edit_dialog((ListSpec *)p_data, -1);
}

static GtkWidget *
_build_list_group(ListSpec *p_spec) {
   p_spec->p_rows  = g_ptr_array_new();
   p_spec->p_group = adw_preferences_group_new();
   adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(p_spec->p_group),
                                   p_spec->c_title);
   adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(p_spec->p_group),
                                         p_spec->c_description);
   /* "Add" button in the group header suffix. */
   GtkWidget *p_add = gtk_button_new_from_icon_name("list-add-symbolic");
   gtk_widget_add_css_class(p_add, "flat");
   gtk_widget_set_tooltip_text(p_add, "Add entry");
   g_signal_connect(p_add, "clicked", G_CALLBACK(_list_add), p_spec);
   adw_preferences_group_set_header_suffix(
      ADW_PREFERENCES_GROUP(p_spec->p_group), p_add);
   _list_refresh(p_spec);
   return (p_spec->p_group);
}

/* --- pages --------------------------------------------------------------- */

static AdwPreferencesPage *
_build_general_page(GSettings *p_gs) {
   AdwPreferencesPage *p_page =
      ADW_PREFERENCES_PAGE(adw_preferences_page_new());
   adw_preferences_page_set_title(p_page, "General");
   adw_preferences_page_set_icon_name(p_page, "preferences-system-symbolic");

   AdwPreferencesGroup *p_grp =
      ADW_PREFERENCES_GROUP(adw_preferences_group_new());
   adw_preferences_group_set_title(p_grp, "View");
   adw_preferences_group_add(p_grp,
                             _make_combo_row("Sort order", "sort", p_gs));
   adw_preferences_group_add(
      p_grp, _make_switch_row("Wrap at folder ends", "wrap", p_gs));
   adw_preferences_group_add(p_grp,
                             _make_combo_row("Background", "background", p_gs));
   adw_preferences_group_add(
      p_grp, _make_combo_row("Scroll behavior", "scroll-behavior", p_gs));
   adw_preferences_group_add(p_grp, _make_spin_row("Slideshow delay (s)", 0.1,
                                                   60.0, 0.5, "slideshow-delay",
                                                   p_gs, FALSE));
   adw_preferences_group_add(p_grp,
                             _make_spin_row("Thumbnail size (px)", 64, 512, 32,
                                            "thumbnail-size", p_gs, TRUE));
   adw_preferences_group_add(
      p_grp, _make_switch_row("Hide trashed items", "hide-trashed", p_gs));
   adw_preferences_group_add(
      p_grp, _make_switch_row("Hide RAW sidecars", "hide-raw-sidecars", p_gs));
#if GGAZE_HAVE_GEGL
   adw_preferences_group_add(
      p_grp, _make_switch_row("Enhance preview thumbnails (GEGL)",
                              "enhance-preview-thumbnails", p_gs));
#endif
   adw_preferences_page_add(p_page, p_grp);
   return (ADW_PREFERENCES_PAGE(p_page));
}

/* The four list editors. The GEGL preset list only appears in a GEGL build:
 * configuring presets that can never run was a trap. */
static void
_init_lists(PrefsLists *p_l, Settings *p_s) {
   p_l->t_specs[p_l->u_n++] =
      (ListSpec){.p_s            = p_s,
                 .c_title        = "Move destinations",
                 .c_description  = "Folders the m key offers",
                 .c_placeholder  = "Absolute folder path "
                                   "(e.g. /home/me/Photos/keep)",
                 .b_require_path = TRUE,
                 .get            = settings_get_destinations,
                 .set            = settings_set_destinations};
   p_l->t_specs[p_l->u_n++] =
      (ListSpec){.p_s           = p_s,
                 .c_title       = "External editors",
                 .c_description = "Programs the e key offers; "
                                  "%f is the image path",
                 .c_placeholder = "Command (e.g. gimp %f)",
                 .get           = settings_get_editors,
                 .set           = settings_set_editors};
   p_l->t_specs[p_l->u_n++] =
      (ListSpec){.p_s           = p_s,
                 .c_title       = "Shell scripts",
                 .c_description = "Run with the ! key via "
                                  "/bin/sh; %f is the image, "
                                  "%d its folder",
                 .c_placeholder = "Shell command (e.g. "
                                  "exiftool -P %f > %d/meta.txt)",
                 .get           = settings_get_scripts,
                 .set           = settings_set_scripts};
#if GGAZE_HAVE_GEGL
   p_l->t_specs[p_l->u_n++] =
      (ListSpec){.p_s           = p_s,
                 .c_title       = "Enhance presets",
                 .c_description = "Extra presets for the a "
                                  "chooser: GEGL operations with "
                                  "prop=value settings, in order. "
                                  "One number may be marked tunable "
                                  "with {s:DEFAULT:MIN..MAX} or "
                                  "{s:DEFAULT:MIN..MAX:STEP} in its "
                                  "place (tunable in the panel once "
                                  "it lists user presets)",
                 .c_placeholder = "e.g. gegl:saturation "
                                  "scale={s:1.3:0..2:0.1} "
                                  "gegl:unsharp-mask std-dev=1.5",
                 .check         = _check_preset_graph,
                 .get           = settings_get_enhance_presets,
                 .set           = settings_set_enhance_presets};
#endif
}

static AdwPreferencesPage *
_build_lists_page(Settings *p_s, PrefsLists *p_l) {
   AdwPreferencesPage *p_page =
      ADW_PREFERENCES_PAGE(adw_preferences_page_new());
   adw_preferences_page_set_title(p_page, "Commands");
   adw_preferences_page_set_icon_name(p_page, "system-run-symbolic");
   _init_lists(p_l, p_s);
   for (guint u = 0; u < p_l->u_n; u++) {
      adw_preferences_page_add(
         p_page, ADW_PREFERENCES_GROUP(_build_list_group(&p_l->t_specs[u])));
   }
   return (ADW_PREFERENCES_PAGE(p_page));
}

AdwPreferencesDialog *
prefs_build_dialog(Settings *p_settings) {
   g_return_val_if_fail(p_settings != NULL, NULL);
   GSettings            *p_gs = settings_get_gsettings(p_settings);
   AdwPreferencesDialog *p_win =
      ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
   PrefsLists *p_l = g_new0(PrefsLists, 1);
   /* The list editors' state belongs to this dialog and dies with it. */
   g_object_set_data_full(G_OBJECT(p_win), "ggaze-prefs-lists", p_l,
                          _prefs_lists_free);
   adw_preferences_dialog_add(p_win, _build_general_page(p_gs));
   adw_preferences_dialog_add(p_win, _build_lists_page(p_settings, p_l));
   return (p_win);
}

void
prefs_show(Settings *p_settings, GtkWidget *p_parent) {
   g_return_if_fail(p_settings != NULL);
   g_return_if_fail(GTK_IS_WIDGET(p_parent));
   AdwPreferencesDialog *p_win = prefs_build_dialog(p_settings);
   if (p_win == NULL) {
      return;
   }
   adw_dialog_present(ADW_DIALOG(p_win), p_parent);
}
