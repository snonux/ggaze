/* prefs.c — AdwPreferencesWindow bound to org.buetow.ggaze. */
#include "prefs.h"

#include <adwaita.h>
#include <glib.h>
#include <gtk/gtk.h>

/* --- enum <-> combo mapping ---------------------------------------------- */

typedef struct {
   const char        *c_key;
   const char *const *c_nicks;
   const char *const *c_labels;
   guint              u_n;
} EnumSpec;

/* GSettings enum keys are stored as nick strings; AdwComboRow:selected is a
 * guint index. These mappings convert between the two. */
static gboolean
_enum_get(GValue *p_val, GVariant *p_var, gpointer p_data) {
   const EnumSpec *p_e   = (const EnumSpec *)p_data;
   const gchar    *nick  = g_variant_get_string(p_var, NULL);
   guint           i_sel = 0;
   for (guint i = 0; i < p_e->u_n; i++) {
      if (g_str_equal(nick, p_e->c_nicks[i])) {
         i_sel = i;
         break;
      }
   }
   g_value_set_uint(p_val, i_sel);
   return (TRUE);
}

static GVariant *
_enum_set(const GValue *p_val, const GVariantType *p_type, gpointer p_data) {
   (void)p_type;
   const EnumSpec *p_e = (const EnumSpec *)p_data;
   guint           i   = g_value_get_uint(p_val);
   if (i >= p_e->u_n) {
      i = 0;
   }
   return (g_variant_new_string(p_e->c_nicks[i]));
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
_make_combo_row(const char *c_title, const EnumSpec *p_spec, GSettings *p_gs) {
   GtkStringList *p_list = gtk_string_list_new(NULL);
   for (guint i = 0; i < p_spec->u_n; i++) {
      gtk_string_list_append(p_list, p_spec->c_labels[i]);
   }
   AdwComboRow *p_row = ADW_COMBO_ROW(adw_combo_row_new());
   adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p_row), c_title);
   adw_combo_row_set_model(p_row, G_LIST_MODEL(p_list));
   g_settings_bind_with_mapping(p_gs, p_spec->c_key, p_row, "selected",
                                G_SETTINGS_BIND_DEFAULT, _enum_get, _enum_set,
                                (gpointer)p_spec, NULL);
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

typedef struct {
   Settings   *p_s;     /* borrowed */
   GSettings  *p_gs;    /* borrowed */
   GtkWidget  *p_group; /* AdwPreferencesGroup for this list */
   GPtrArray  *p_rows;  /* added row widgets (borrowed; owned by the group) */
   const char *c_title;
   const char *c_key;
   gboolean    b_require_path;
   GPtrArray *(*get)(Settings *);
   guint (*set)(Settings *, const GPtrArray *);
} ListSpec;

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

/* Read the current list, mutate, validate-persist, and rebuild the rows. */
static void _list_refresh(ListSpec *p_spec);

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
   guint      i     = p_ctx->u_index;
   gint       i_new = (gint)i + p_ctx->i_delta;
   if (i_new < 0 || i_new >= (gint)p_cur->len) {
      g_ptr_array_unref(p_cur);
      return;
   }
   gpointer p = g_ptr_array_index(p_cur, i);
   g_ptr_array_remove_index(p_cur, i);
   g_ptr_array_insert(p_cur, (guint)i_new, p);
   p_ctx->p_spec->set(p_ctx->p_spec->p_s, p_cur);
   g_ptr_array_unref(p_cur);
   _list_refresh(p_ctx->p_spec);
}

static GtkWidget *
_icon_button(const char *c_icon, const char *c_tip) {
   GtkWidget *p_btn = gtk_button_new_from_icon_name(c_icon);
   gtk_widget_add_css_class(p_btn, "flat");
   gtk_widget_set_tooltip_text(p_btn, c_tip);
   return (p_btn);
}

static void
_list_refresh(ListSpec *p_spec) {
   /* Remove previously-added rows (tracked in p_spec->p_rows) before
    * rebuilding; AdwPreferencesGroup exposes no list-box accessor, so the
    * row widgets are remembered here. */
   if (p_spec->p_rows != NULL) {
      for (guint i = 0; i < p_spec->p_rows->len; i++) {
         GtkWidget *p_row = g_ptr_array_index(p_spec->p_rows, i);
         adw_preferences_group_remove(ADW_PREFERENCES_GROUP(p_spec->p_group),
                                      p_row);
      }
      g_ptr_array_set_size(p_spec->p_rows, 0);
   }

   GPtrArray *p_cur = p_spec->get(p_spec->p_s);
   for (guint i = 0; i < p_cur->len; i++) {
      const SettingsPair *pr    = g_ptr_array_index(p_cur, i);
      AdwActionRow       *p_row = ADW_ACTION_ROW(adw_action_row_new());
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p_row), pr->c_name);
      adw_action_row_set_subtitle(p_row, pr->c_value);
      /* Each suffix button gets its own RowCtx copy freed with the closure, so
       * removing/refreshing rows never leaves dangling callbacks. */
      RowCtx *p_up     = g_new(RowCtx, 1);
      p_up->p_spec     = p_spec;
      p_up->u_index    = i;
      p_up->i_delta    = -1;
      GtkWidget *p_upb = _icon_button("go-up-symbolic", "Move up");
      g_signal_connect_data(p_upb, "clicked", G_CALLBACK(_list_move), p_up,
                            (GClosureNotify)_row_ctx_free, 0);
      RowCtx *p_dn     = g_new(RowCtx, 1);
      p_dn->p_spec     = p_spec;
      p_dn->u_index    = i;
      p_dn->i_delta    = 1;
      GtkWidget *p_dnb = _icon_button("go-down-symbolic", "Move down");
      g_signal_connect_data(p_dnb, "clicked", G_CALLBACK(_list_move), p_dn,
                            (GClosureNotify)_row_ctx_free, 0);
      RowCtx *p_rm     = g_new(RowCtx, 1);
      p_rm->p_spec     = p_spec;
      p_rm->u_index    = i;
      p_rm->i_delta    = 0;
      GtkWidget *p_rmb = _icon_button("edit-delete-symbolic", "Remove");
      g_signal_connect_data(p_rmb, "clicked", G_CALLBACK(_list_remove), p_rm,
                            (GClosureNotify)_row_ctx_free, 0);
      adw_action_row_add_suffix(p_row, p_upb);
      adw_action_row_add_suffix(p_row, p_dnb);
      adw_action_row_add_suffix(p_row, p_rmb);
      adw_preferences_group_add(ADW_PREFERENCES_GROUP(p_spec->p_group),
                                GTK_WIDGET(p_row));
      g_ptr_array_add(p_spec->p_rows, p_row);
   }
   g_ptr_array_unref(p_cur);
}

/* Add-entry dialog: prompts for name + value, validates, appends. */
typedef struct {
   ListSpec    *p_spec;
   GtkEditable *p_name;
   GtkEditable *p_value;
} AddCtx;

static void
_add_confirm_cb(GObject *p_src, GAsyncResult *p_res, gpointer p_data) {
   AddCtx     *p_ctx = (AddCtx *)p_data;
   const char *c_resp =
      adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(p_src), p_res);
   gboolean b_ok = (c_resp != NULL && g_str_equal(c_resp, "ok"));
   if (b_ok) {
      const char *c_name  = gtk_editable_get_text(p_ctx->p_name);
      const char *c_value = gtk_editable_get_text(p_ctx->p_value);
      if (settings_pair_valid(c_name, c_value, p_ctx->p_spec->b_require_path)) {
         GPtrArray    *p_cur = p_ctx->p_spec->get(p_ctx->p_spec->p_s);
         SettingsPair *pr    = g_new(SettingsPair, 1);
         pr->c_name          = g_strdup(c_name);
         pr->c_value         = g_strdup(c_value);
         g_ptr_array_add(p_cur, pr);
         p_ctx->p_spec->set(p_ctx->p_spec->p_s, p_cur);
         g_ptr_array_unref(p_cur);
         _list_refresh(p_ctx->p_spec);
      }
   }
   g_free(p_ctx);
}

static void
_list_add(GtkButton *p_btn, gpointer p_data) {
   (void)p_btn;
   ListSpec  *p_spec = (ListSpec *)p_data;
   GtkWidget *p_box  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
   gtk_widget_set_margin_start(p_box, 12);
   gtk_widget_set_margin_end(p_box, 12);
   gtk_widget_set_margin_top(p_box, 12);
   gtk_widget_set_margin_bottom(p_box, 12);
   GtkWidget *p_name = gtk_entry_new();
   gtk_entry_set_placeholder_text(GTK_ENTRY(p_name), "Name");
   GtkWidget *p_value = gtk_entry_new();
   gtk_entry_set_placeholder_text(GTK_ENTRY(p_value),
                                  p_spec->b_require_path
                                     ? "Absolute path (e.g. /home/me/Photos)"
                                     : "Command / graph (e.g. gimp %f)");
   gtk_box_append(GTK_BOX(p_box), p_name);
   gtk_box_append(GTK_BOX(p_box), p_value);

   AdwDialog *p_dlg = adw_alert_dialog_new("Add entry", NULL);
   adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(p_dlg), p_box);
   adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(p_dlg), "cancel", "Cancel",
                                  "ok", "Add", NULL);
   adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(p_dlg), "ok",
                                            ADW_RESPONSE_SUGGESTED);
   adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(p_dlg), "ok");
   adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(p_dlg), "cancel");

   AddCtx *p_ctx  = g_new(AddCtx, 1);
   p_ctx->p_spec  = p_spec;
   p_ctx->p_name  = GTK_EDITABLE(p_name);
   p_ctx->p_value = GTK_EDITABLE(p_value);
   adw_alert_dialog_choose(ADW_ALERT_DIALOG(p_dlg), p_spec->p_group, NULL,
                           _add_confirm_cb, p_ctx);
}

static GtkWidget *
_build_list_group(ListSpec *p_spec) {
   g_clear_pointer(&p_spec->p_rows, g_ptr_array_unref);
   p_spec->p_rows  = g_ptr_array_new();
   p_spec->p_group = adw_preferences_group_new();
   adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(p_spec->p_group),
                                   p_spec->c_title);
   /* "Add" button in the group header suffix. */
   GtkWidget *p_add = gtk_button_new_from_icon_name("list-add-symbolic");
   gtk_widget_add_css_class(p_add, "flat");
   gtk_widget_set_tooltip_text(p_add, "Add entry");
   g_signal_connect(p_add, "clicked", G_CALLBACK(_list_add), p_spec);
   adw_preferences_group_set_header_suffix(
      ADW_PREFERENCES_GROUP(p_spec->p_group), p_add);
   _list_refresh(p_spec);
   return p_spec->p_group;
}

/* --- pages --------------------------------------------------------------- */

static AdwPreferencesPage *
_build_general_page(Settings *p_s, GSettings *p_gs) {
   (void)p_s;
   AdwPreferencesPage *p_page =
      ADW_PREFERENCES_PAGE(adw_preferences_page_new());
   adw_preferences_page_set_title(p_page, "General");
   adw_preferences_page_set_icon_name(p_page, "preferences-system-symbolic");

   static const char *const sort_nicks[]  = {"name", "capture-time", "size"};
   static const char *const sort_labels[] = {"Name", "Capture time", "Size"};
   static const char *const bg_nicks[]   = {"black", "dark", "grey", "checker"};
   static const char *const bg_labels[]  = {"Black", "Dark", "Grey", "Checker"};
   static const char *const scr_nicks[]  = {"zoom", "pan-when-zoomed",
                                            "navigate"};
   static const char *const scr_labels[] = {"Zoom", "Pan when zoomed",
                                            "Navigate"};
   static EnumSpec          e_sort       = {"sort", sort_nicks, sort_labels, 3};
   static EnumSpec          e_bg = {"background", bg_nicks, bg_labels, 4};
   static EnumSpec e_scr = {"scroll-behavior", scr_nicks, scr_labels, 3};

   AdwPreferencesGroup *p_grp =
      ADW_PREFERENCES_GROUP(adw_preferences_group_new());
   adw_preferences_group_set_title(p_grp, "View");
   adw_preferences_group_add(p_grp,
                             _make_combo_row("Sort order", &e_sort, p_gs));
   adw_preferences_group_add(
      p_grp, _make_switch_row("Wrap at folder ends", "wrap", p_gs));
   adw_preferences_group_add(p_grp, _make_combo_row("Background", &e_bg, p_gs));
   adw_preferences_group_add(p_grp,
                             _make_combo_row("Scroll behavior", &e_scr, p_gs));
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
   adw_preferences_page_add(p_page, p_grp);
   return (ADW_PREFERENCES_PAGE(p_page));
}

static AdwPreferencesPage *
_build_lists_page(Settings *p_s, GSettings *p_gs) {
   AdwPreferencesPage *p_page =
      ADW_PREFERENCES_PAGE(adw_preferences_page_new());
   adw_preferences_page_set_title(p_page, "Commands");
   adw_preferences_page_set_icon_name(p_page, "system-run-symbolic");

   static ListSpec specs[4];
   specs[0] = (ListSpec){.p_s            = p_s,
                         .p_gs           = p_gs,
                         .c_title        = "Move destinations",
                         .c_key          = "destinations",
                         .b_require_path = TRUE,
                         .get            = settings_get_destinations,
                         .set            = settings_set_destinations};
   specs[1] = (ListSpec){.p_s            = p_s,
                         .p_gs           = p_gs,
                         .c_title        = "External editors",
                         .c_key          = "editors",
                         .b_require_path = FALSE,
                         .get            = settings_get_editors,
                         .set            = settings_set_editors};
   specs[2] = (ListSpec){.p_s            = p_s,
                         .p_gs           = p_gs,
                         .c_title        = "Shell scripts",
                         .c_key          = "scripts",
                         .b_require_path = FALSE,
                         .get            = settings_get_scripts,
                         .set            = settings_set_scripts};
   specs[3] = (ListSpec){.p_s            = p_s,
                         .p_gs           = p_gs,
                         .c_title        = "Enhance presets (GEGL)",
                         .c_key          = "enhance-presets",
                         .b_require_path = FALSE,
                         .get            = settings_get_enhance_presets,
                         .set            = settings_set_enhance_presets};
   for (guint i = 0; i < G_N_ELEMENTS(specs); i++) {
      adw_preferences_page_add(
         p_page, ADW_PREFERENCES_GROUP(_build_list_group(&specs[i])));
   }
   /* Keep the static specs alive for the dialog lifetime: they hold no heap
    * pointers beyond the borrowed Settings/GSettings, so a static array is
    * fine. The ListSpec pointers are referenced by row/signal closures only
    * while the dialog exists. */
   return (ADW_PREFERENCES_PAGE(p_page));
}

AdwPreferencesDialog *
prefs_build_dialog(Settings *p_settings) {
   g_return_val_if_fail(p_settings != NULL, NULL);
   GSettings            *p_gs = settings_get_gsettings(p_settings);
   AdwPreferencesDialog *p_win =
      ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
   adw_preferences_dialog_add(p_win, _build_general_page(p_settings, p_gs));
   adw_preferences_dialog_add(p_win, _build_lists_page(p_settings, p_gs));
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