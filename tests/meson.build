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
#
# Fixture dirs are passed to tests via env vars:
#   GGAZE_FIXTURES_DIR  committed tests/fixtures/ (CI-portable baseline)
#   GGAZE_SAMPLE_DIR    local ./sample-images (not git-tracked; tests skip
#                       cleanly when absent so CI stays green)
#   GIO_USE_VFS=local   avoid gvfs daemon-VFS internal leaks under ASan

fixtures_env = environment()
fixtures_env.set('GIO_USE_VFS', 'local')
fixtures_env.set('GGAZE_FIXTURES_DIR',
   join_paths(meson.project_source_root(), 'tests', 'fixtures'))
fixtures_env.set('GGAZE_SAMPLE_DIR',
   join_paths(meson.project_source_root(), 'sample-images'))

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

test_detect = executable(
   'test_detect',
   ['test_detect.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : [glib_dep],
   link_with : ggaze_lib,
   install : false,
)
test('detect', test_detect, suite : 'unit', env : fixtures_env)

test_loader_pixbuf = executable(
   'test_loader_pixbuf',
   ['test_loader_pixbuf.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : [gdkpixbuf_dep, gtk4_dep, glib_dep, gio_dep],
   link_with : ggaze_lib,
   install : false,
)
test('loader_pixbuf', test_loader_pixbuf, suite : 'unit', env : fixtures_env)

if libjpeg_dep.found()
   test_loader_jpeg = executable(
      'test_loader_jpeg',
      ['test_loader_jpeg.c', ggaze_conf_h],
      include_directories : [inc, src_inc],
      dependencies : [gdkpixbuf_dep, gtk4_dep, glib_dep, gio_dep],
      link_with : ggaze_lib,
      install : false,
   )
   test('loader_jpeg', test_loader_jpeg, suite : 'unit', env : fixtures_env)
endif

test_navigator = executable(
   'test_navigator',
   ['test_navigator.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : [glib_dep, gio_dep],
   link_with : ggaze_lib,
   install : false,
)
test('navigator', test_navigator, suite : 'unit', env : fixtures_env)

test_trash = executable(
   'test_trash',
   ['test_trash.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : [glib_dep, gio_dep],
   link_with : ggaze_lib,
   install : false,
)
test('trash', test_trash, suite : 'unit', env : fixtures_env)

test_thumbnail = executable(
   'test_thumbnail',
   ['test_thumbnail.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : [gdkpixbuf_dep, gtk4_dep, glib_dep, gio_dep],
   link_with : ggaze_lib,
   install : false,
)
test('thumbnail', test_thumbnail, suite : 'unit', env : fixtures_env)

test_info = executable(
   'test_info',
   ['test_info.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib,
   install : false,
)
test('info', test_info, suite : 'unit', env : fixtures_env)

test_texturecache = executable(
   'test_texturecache',
   ['test_texturecache.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : [gdkpixbuf_dep, gtk4_dep, glib_dep, gio_dep],
   link_with : ggaze_lib,
   install : false,
)
test('texturecache', test_texturecache, suite : 'unit', env : fixtures_env)

# --- integration track (needs a display; CI uses xvfb-run) ---
test_window = executable(
   'test_window',
   ['test_window.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib,
   install : false,
)
test('window', test_window, suite : 'integration', env : fixtures_env)

test_open_and_show = executable(
   'test_open_and_show',
   ['test_open_and_show.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib,
   install : false,
)
test('open_and_show', test_open_and_show,
   suite : 'integration',
   env : fixtures_env)

test_walk_folder = executable(
   'test_walk_folder',
   ['test_walk_folder.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib,
   install : false,
)
test('walk_folder', test_walk_folder, suite : 'integration', env : fixtures_env)

test_responsive_nav = executable(
   'test_responsive_nav',
   ['test_responsive_nav.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib,
   install : false,
)
test('responsive_nav', test_responsive_nav,
   suite : 'integration',
   env : fixtures_env)

test_grid_cull = executable(
   'test_grid_cull',
   ['test_grid_cull.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib,
   install : false,
)
test('grid_cull', test_grid_cull, suite : 'integration', env : fixtures_env)

test_progressive_jpeg = executable(
   'test_progressive_jpeg',
   ['test_progressive_jpeg.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib,
   install : false,
)
test('progressive_jpeg', test_progressive_jpeg,
   suite : 'integration', env : fixtures_env)

subdir('integration')