# ggaze tests — two tracks (see docs/IMPLEMENTATION.md "Two test tracks").
#
#   unit        plain-C module tests, no display needed (>=80% coverage gate,
#               warn until M10 then fail).
#   integration  cross-module flows with real temp dirs + offscreen GTK.
#
# Run one track:   meson test -C build --suite unit
#                  meson test -C build --suite integration
# Coverage:        meson setup -Db_coverage=true build
#                  meson test -C build
#                  ninja -C build coverage   (needs lcov + genhtml)

# --- unit track ---
test_bootstrap = executable(
   'test_bootstrap',
   ['test_bootstrap.c', ggaze_conf_h],
   include_directories : inc,
   dependencies : [glib_dep],
   install : false,
)
test('bootstrap', test_bootstrap, suite : 'unit')

# test_app runs the ggaze binary as a subprocess; meson passes its path.
test_app = executable(
   'test_app',
   ['test_app.c', ggaze_conf_h],
   include_directories : inc,
   dependencies : [glib_dep, gio_dep],
   install : false,
)
test('app', test_app,
   suite : 'unit',
   args : [ggaze_exe.full_path()],
   depends : ggaze_exe,
)

# --- integration track (needs a display; CI uses xvfb-run) ---
test_window = executable(
   'test_window',
   ['test_window.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib,
   install : false,
)
test('window', test_window, suite : 'integration')

subdir('integration')