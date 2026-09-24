/*:*
 * ggaze — which gdk-pixbuf modules this machine has, for the test suites
 * (AGENTS.md: "Shared helpers go in tests/helpers/")
 *
 * gdk-pixbuf2 itself ships png/gif/jpeg; webp/tiff/ico/jxl come from
 * packages of their own, and CI's fedora:40 installs none of them. A suite
 * whose fixture only such a module decodes asks here BEFORE opening it:
 * without the module the load can only fail, and a readiness wait on it
 * would time out instead of skipping. test_loader_pixbuf and test_viewer
 * both need this; it is the one copy.
 *
 * Copyright (c) 2026 ggaze contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *:*/

#ifndef GGAZE_TEST_PIXBUF_MODULES_H
#define GGAZE_TEST_PIXBUF_MODULES_H

#include <glib.h>

/* TRUE iff the gdk-pixbuf module c_module ("webp", "tiff", ...) is
 * installed on this machine. */
gboolean ggtest_pixbuf_module_available(const char *c_module);

#endif /* GGAZE_TEST_PIXBUF_MODULES_H */
