/*:*
 * ggaze — keyboard shortcut integration test
 *
 * Verifies that the GtkShortcutController installed on the window has
 * GLOBAL scope (the fix for the t/Esc not-firing bug) and that the keyboard
 * shortcut path actually routes the `t` and `Escape` keys to the
 * win.toggle-view / win.back actions and switches the large/grid stack.
 *
 * Why not real GdkKeyEvent injection? GTK 4.22.4 (the version this project
 * targets — see meson.build gtk4_dep) provides no public API to synthesize a
 * GdkKeyEvent: GdkEvent is opaque, has no public constructor, and
 * gtk_test_widget_send_key() is not exported by this GTK build (only
 * gtk_test_widget_wait_for_draw is present in gtktestutils.h). So the most
 * faithful alternative is to exercise the shortcut controller's *trigger
 * matching* directly — by reading each GtkShortcut's GtkKeyvalTrigger
 * keyval/modifiers and GtkNamedAction action name and asserting the key→action
 * binding the controller uses — and then dispatch the shortcut's action
 * through gtk_shortcut_action_activate(), which is the exact call the
 * controller makes when a keyval trigger matches a key press. Needs a display
 * (integration suite; xvfb).
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "viewer.h"
#include "window.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

/* --- helpers ------------------------------------------------------------ */

static GgazeWindow *
new_window(void) {
   return (GGAZE_WINDOW(g_object_new(GGAZE_TYPE_WINDOW, NULL)));
}

static GdkTexture *
viewer_texture(GgazeWindow *p_win) {
   GtkStack  *p_stack = ggaze_window_get_stack(p_win);
   GtkWidget *p_large = gtk_stack_get_child_by_name(p_stack, "large");
   return (ggaze_viewer_get_texture(GGAZE_VIEWER(p_large)));
}

static void
drain_main(guint u_ms) {
   for (guint u = 0; u < u_ms; u++) {
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
}

static void
copy_fixture(const char *c_dir, const char *c_name) {
   const gchar *c_fx = g_getenv("GGAZE_FIXTURES_DIR");
   g_assert_nonnull(c_fx);
   char   *c_src = g_build_filename(c_fx, c_name, NULL);
   char   *c_dst = g_build_filename(c_dir, c_name, NULL);
   GFile  *p_src = g_file_new_for_path(c_src);
   GFile  *p_dst = g_file_new_for_path(c_dst);
   GError *p_err = NULL;
   g_assert_true(g_file_copy(p_src, p_dst, G_FILE_COPY_OVERWRITE, NULL, NULL,
                             NULL, &p_err));
   g_assert_no_error(p_err);
   g_object_unref(p_src);
   g_object_unref(p_dst);
   g_free(c_src);
   g_free(c_dst);
}

static void
cleanup_temp_dir(char *c_dir) {
   GFile           *p_dir = g_file_new_for_path(c_dir);
   GFileEnumerator *p_e =
      g_file_enumerate_children(p_dir, "standard::name,standard::type",
                                G_FILE_QUERY_INFO_NONE, NULL, NULL);
   if (p_e != NULL) {
      GFileInfo *p_info;
      while ((p_info = g_file_enumerator_next_file(p_e, NULL, NULL)) != NULL) {
         GFile *p_child = g_file_get_child(p_dir, g_file_info_get_name(p_info));
         g_file_delete(p_child, NULL, NULL);
         g_object_unref(p_child);
         g_object_unref(p_info);
      }
      g_object_unref(p_e);
   }
   g_file_delete(p_dir, NULL, NULL);
   g_object_unref(p_dir);
   g_free(c_dir);
}

/* Find the GtkShortcutController attached to p_widget that owns the ggaze
 * keybinding shortcuts (identified by having a "win.toggle-view" shortcut).
 * GtkApplicationWindow may also install its own built-in shortcut controller
 * (for mnemonics, LOCAL scope); we want ours, not that one. Returns a new ref.
 */
static const char *shortcut_action_name(GtkShortcut *p_s);

static GtkShortcutController *
find_shortcut_controller(GtkWidget *p_widget) {
   GListModel            *p_ctrls = gtk_widget_observe_controllers(p_widget);
   guint                  u_n     = g_list_model_get_n_items(p_ctrls);
   GtkShortcutController *p_sc    = NULL;
   for (guint i = 0; i < u_n && p_sc == NULL; i++) {
      GObject *p_obj = g_list_model_get_item(p_ctrls, i);
      if (GTK_IS_SHORTCUT_CONTROLLER(p_obj)) {
         GtkShortcutController *p_cand = (GtkShortcutController *)p_obj;
         guint u_m = g_list_model_get_n_items(G_LIST_MODEL(p_cand));
         for (guint j = 0; j < u_m; j++) {
            GObject    *p_s    = g_list_model_get_item(G_LIST_MODEL(p_cand), j);
            const char *c_name = shortcut_action_name((GtkShortcut *)p_s);
            g_object_unref(p_s);
            if (g_strcmp0(c_name, "win.toggle-view") == 0) {
               p_sc = p_cand; /* steal the ref; stop both loops */
               break;
            }
         }
         if (p_sc == NULL) {
            g_object_unref(p_cand); /* not ours */
         }
      } else {
         g_object_unref(p_obj);
      }
   }
   g_object_unref(p_ctrls);
   return (p_sc);
}

/* Return the action name of p_s if it is a GtkNamedAction, else NULL. */
static const char *
shortcut_action_name(GtkShortcut *p_s) {
   GtkShortcutAction *p_a = gtk_shortcut_get_action(p_s);
   if (p_a == NULL || !G_TYPE_CHECK_INSTANCE_TYPE(p_a, GTK_TYPE_NAMED_ACTION)) {
      return (NULL);
   }
   return (gtk_named_action_get_action_name(
      G_TYPE_CHECK_INSTANCE_CAST(p_a, GTK_TYPE_NAMED_ACTION, GtkNamedAction)));
}

/* Return TRUE if p_s's trigger is a GtkKeyvalTrigger matching u_keyval+e_mods.
 */
static gboolean
shortcut_trigger_matches(GtkShortcut *p_s, guint u_keyval,
                         GdkModifierType e_mods) {
   GtkShortcutTrigger *p_t = gtk_shortcut_get_trigger(p_s);
   if (p_t == NULL ||
       !G_TYPE_CHECK_INSTANCE_TYPE(p_t, GTK_TYPE_KEYVAL_TRIGGER)) {
      return (FALSE);
   }
   GtkKeyvalTrigger *p_kt = G_TYPE_CHECK_INSTANCE_CAST(
      p_t, GTK_TYPE_KEYVAL_TRIGGER, GtkKeyvalTrigger);
   return (gtk_keyval_trigger_get_keyval(p_kt) == u_keyval &&
           gtk_keyval_trigger_get_modifiers(p_kt) == e_mods);
}

/* Find the shortcut on p_sc whose action name equals c_action. New ref. */
static GtkShortcut *
find_shortcut_by_action(GtkShortcutController *p_sc, const char *c_action) {
   guint u_n = g_list_model_get_n_items(G_LIST_MODEL(p_sc));
   for (guint i = 0; i < u_n; i++) {
      GObject    *p_s    = g_list_model_get_item(G_LIST_MODEL(p_sc), i);
      const char *c_name = shortcut_action_name((GtkShortcut *)p_s);
      if (g_strcmp0(c_name, c_action) == 0) {
         return ((GtkShortcut *)p_s);
      }
      g_object_unref(p_s);
   }
   return (NULL);
}

/* Find the shortcut on p_sc whose keyval trigger is u_keyval+e_mods. New ref.
 */
static GtkShortcut *
find_shortcut_by_key(GtkShortcutController *p_sc, guint u_keyval,
                     GdkModifierType e_mods) {
   guint u_n = g_list_model_get_n_items(G_LIST_MODEL(p_sc));
   for (guint i = 0; i < u_n; i++) {
      GObject *p_s = g_list_model_get_item(G_LIST_MODEL(p_sc), i);
      if (shortcut_trigger_matches((GtkShortcut *)p_s, u_keyval, e_mods)) {
         return ((GtkShortcut *)p_s);
      }
      g_object_unref(p_s);
   }
   return (NULL);
}

/* Dispatch p_s's action through the GtkShortcutAction path — the same
 * gtk_shortcut_action_activate() the controller calls when a keyval trigger
 * matches a real key press. */
static void
fire_shortcut(GtkWidget *p_widget, GtkShortcut *p_s) {
   GtkShortcutAction *p_a = gtk_shortcut_get_action(p_s);
   g_assert_nonnull(p_a);
   g_assert_true(gtk_shortcut_action_activate(p_a, 0, p_widget, NULL));
}

/* --- subtests ----------------------------------------------------------- */

/* The fix: the window's shortcut controller must be GLOBAL scope so its
 * triggers are consulted for every key event at the toplevel, before child
 * key controllers (the viewer has its own) consume them. */
static void
test_shortcut_controller_scope(void) {
   GgazeWindow *p_win = new_window();

   GtkShortcutController *p_sc = find_shortcut_controller(GTK_WIDGET(p_win));
   g_assert_nonnull(p_sc);
   g_assert_cmpint(gtk_shortcut_controller_get_scope(p_sc), ==,
                   GTK_SHORTCUT_SCOPE_GLOBAL);
   g_object_unref(p_sc);

   g_object_unref(p_win);
   drain_main(200);
}

/* The keyboard shortcut path: the `t` key must be bound to win.toggle-view
 * and `Escape` to win.back, and dispatching those shortcuts must switch the
 * stack between "large" and "grid". Exercises the controller's trigger
 * matching (keyval→action binding) plus the GtkShortcutAction dispatch. */
static void
test_shortcut_keypath_toggle_and_back(void) {
   GError *p_err = NULL;
   char   *c_dir = g_dir_make_tmp("ggaze-sc-XXXXXX", &p_err);
   g_assert_no_error(p_err);
   copy_fixture(c_dir, "plain.jpg");
   copy_fixture(c_dir, "rot6.jpg");
   copy_fixture(c_dir, "small.png");

   char  *c_plain_path = g_build_filename(c_dir, "plain.jpg", NULL);
   GFile *p_plain      = g_file_new_for_path(c_plain_path);
   g_free(c_plain_path);

   GgazeWindow *p_win = new_window();
   ggaze_window_open(p_win, p_plain);

   /* Wait for the FULL image to load. plain.jpg decodes to 6x3 once the
    * JPEG backend's full-decode phase completes; the low-res 1/8 preview
    * (1x1) arrives earlier via the progress callback and only sets the
    * viewer texture, not the stack. Waiting for 6x3 guarantees the
    * load-completion callback (_show_texture) has run and re-asserted the
    * "large" stack, so no later callback will yank the stack back to
    * "large" after we toggle. (See window.c _load_finish_cb/_show_texture;
    * that stack-reset-on-late-load is tracked as a separate window.c
    * concern, out of scope for the shortcut fix.) */
   for (guint u = 0; u < 3000; u++) {
      GdkTexture *p_t = viewer_texture(p_win);
      if (p_t != NULL && gdk_texture_get_width(p_t) == 6 &&
          gdk_texture_get_height(p_t) == 3) {
         break;
      }
      g_main_context_iteration(g_main_context_default(), FALSE);
      g_usleep(1000);
   }
   g_assert_cmpint(gdk_texture_get_width(viewer_texture(p_win)), ==, 6);
   g_assert_cmpint(gdk_texture_get_height(viewer_texture(p_win)), ==, 3);

   GtkStack *p_stack = ggaze_window_get_stack(p_win);
   g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "large");

   GtkShortcutController *p_sc = find_shortcut_controller(GTK_WIDGET(p_win));
   g_assert_nonnull(p_sc);
   g_assert_cmpint(gtk_shortcut_controller_get_scope(p_sc), ==,
                   GTK_SHORTCUT_SCOPE_GLOBAL);

   /* `t` → win.toggle-view: verify the key→action binding both ways, then
    * dispatch the shortcut and assert the stack flips to "grid". */
   {
      GtkShortcut *p_by_key = find_shortcut_by_key(p_sc, GDK_KEY_t, 0);
      g_assert_nonnull(p_by_key);
      g_assert_cmpstr(shortcut_action_name(p_by_key), ==, "win.toggle-view");
      g_object_unref(p_by_key);

      GtkShortcut *p_by_act = find_shortcut_by_action(p_sc, "win.toggle-view");
      g_assert_nonnull(p_by_act);
      g_assert_true(shortcut_trigger_matches(p_by_act, GDK_KEY_t, 0));
      fire_shortcut(GTK_WIDGET(p_win), p_by_act);
      g_object_unref(p_by_act);
   }
   drain_main(200);
   g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "grid");

   /* `t` again → back to "large". */
   {
      GtkShortcut *p_s = find_shortcut_by_action(p_sc, "win.toggle-view");
      g_assert_nonnull(p_s);
      g_assert_true(shortcut_trigger_matches(p_s, GDK_KEY_t, 0));
      fire_shortcut(GTK_WIDGET(p_win), p_s);
      g_object_unref(p_s);
   }
   drain_main(200);
   g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "large");

   /* `Escape` → win.back: large → grid. Verify binding both ways, dispatch. */
   {
      GtkShortcut *p_by_key = find_shortcut_by_key(p_sc, GDK_KEY_Escape, 0);
      g_assert_nonnull(p_by_key);
      g_assert_cmpstr(shortcut_action_name(p_by_key), ==, "win.back");
      g_object_unref(p_by_key);

      GtkShortcut *p_by_act = find_shortcut_by_action(p_sc, "win.back");
      g_assert_nonnull(p_by_act);
      g_assert_true(shortcut_trigger_matches(p_by_act, GDK_KEY_Escape, 0));
      fire_shortcut(GTK_WIDGET(p_win), p_by_act);
      g_object_unref(p_by_act);
   }
   drain_main(200);
   g_assert_cmpstr(gtk_stack_get_visible_child_name(p_stack), ==, "grid");

   g_object_unref(p_sc);
   g_object_unref(p_plain);
   g_object_unref(p_win);
   drain_main(500);
   cleanup_temp_dir(c_dir);
}

/* Preserve existing action behavior: assert the FULL set of win.* actions
 * from src/shortcuts.c SHORTCUTS[] is registered on the controller. Guards
 * against a future table/registration regression where a binding is dropped
 * or rewired. We assert by action name (not by keyval) because GTK4
 * normalizes uppercase keyvals in GtkKeyvalTrigger — e.g. GDK_KEY_G with no
 * modifier is stored as GDK_KEY_g/0, which collides with the plain 'g'
 * win.first binding. That G/last-vs-g/first conflict is a pre-existing
 * keybinding bug in shortcuts.c, out of scope for this (t/Esc scope) task and
 * preserved as-is here. */
static void
test_shortcut_full_table_registered(void) {
   static const char *ACTIONS[] = {
      "win.prev",         "win.next",        "win.first",    "win.last",
      "win.open",         "win.quit",        "win.trash",    "win.delete",
      "win.undo",         "win.toggle-view", "win.mark",     "win.mark-all",
      "win.shortcuts",    "win.zoom-in",     "win.zoom-out", "win.fullscreen",
      "win.slideshow",    "win.info",        "win.back",     "win.enhance",
      "win.enhance-save",
   };
   GgazeWindow           *p_win = new_window();
   GtkShortcutController *p_sc  = find_shortcut_controller(GTK_WIDGET(p_win));
   g_assert_nonnull(p_sc);
   for (gsize i = 0; i < G_N_ELEMENTS(ACTIONS); i++) {
      GtkShortcut *p_s = find_shortcut_by_action(p_sc, ACTIONS[i]);
      g_assert_nonnull(p_s);
      g_object_unref(p_s);
   }
   /* The SHORTCUTS[] table has 25 rows now (some actions appear twice, e.g.
    * win.prev for h and Left; win.zoom-in for plus and equal). */
   g_assert_cmpint(g_list_model_get_n_items(G_LIST_MODEL(p_sc)), ==, 25);
   g_object_unref(p_sc);
   g_object_unref(p_win);
   drain_main(200);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
   if (!gtk_init_check()) {
      g_test_skip("no display available (run under xvfb)");
      return (g_test_run());
   }
   g_test_add_func("/shortcut/controller_scope",
                   test_shortcut_controller_scope);
   g_test_add_func("/shortcut/keypath_toggle_and_back",
                   test_shortcut_keypath_toggle_and_back);
   g_test_add_func("/shortcut/full_table_registered",
                   test_shortcut_full_table_registered);
   return (g_test_run());
}