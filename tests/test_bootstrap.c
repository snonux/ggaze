/*:*
 * ggaze — bootstrap sanity test
 *
 * Confirms the build/test harness is wired: g_test_init runs, and the
 * generated ggaze-config.h exposes the app id and version. No display,
 * no GTK — this is the smallest proof that the unit-test track compiles
 * and executes under `meson test --suite unit`.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#include "ggaze-config.h"

#include <glib.h>
#include <string.h>

static void
test_app_id(void) {
   g_assert_cmpstr(GGAZE_APP_ID, ==, "org.buetow.ggaze");
}

static void
test_version(void) {
   g_assert_cmpstr(GGAZE_VERSION, !=, NULL);
   g_assert_cmpint((int)strlen(GGAZE_VERSION), >, 0);
}

static void
test_feature_flags_are_binary(void) {
   /* Feature flags must be 0 or 1, never undefined (compile-time guard). */
   g_assert_cmpint(GGAZE_HAVE_GEGL, >=, 0);
   g_assert_cmpint(GGAZE_HAVE_GEGL, <=, 1);
   g_assert_cmpint(GGAZE_HAVE_JXL, >=, 0);
   g_assert_cmpint(GGAZE_HAVE_JXL, <=, 1);
   g_assert_cmpint(GGAZE_HAVE_AVIF, >=, 0);
   g_assert_cmpint(GGAZE_HAVE_AVIF, <=, 1);
   g_assert_cmpint(GGAZE_HAVE_HEIF, >=, 0);
   g_assert_cmpint(GGAZE_HAVE_HEIF, <=, 1);
}

int
main(int i_argc, char **c_argv) {
   g_test_init(&i_argc, &c_argv, NULL);
   g_test_add_func("/bootstrap/app_id", test_app_id);
   g_test_add_func("/bootstrap/version", test_version);
   g_test_add_func("/bootstrap/feature_flags", test_feature_flags_are_binary);
   return (g_test_run());
}