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
# Every test that constructs a GgazeWindow calls g_settings_new(); point it at
# the build-tree compiled schema and use the in-process memory backend so no
# test touches the user's dconf and the schema is found without installing.
fixtures_env.set('GSETTINGS_BACKEND', 'memory')
fixtures_env.set('GSETTINGS_SCHEMA_DIR', ggaze_gschema_dir)
# tu0: test_window.c and test_enhance_flow.c (both built on fixtures_env) now
# exercise real GtkPopovers (fontconfig/pango leak the first time a popover's
# labels are measured) and, when GEGL is enabled, GEGL's own worker-thread
# processing (gegl_jpg_load's gio_source_init leaks a small fixed buffer per
# jpeg_read_header call) -- both third-party, both already enumerated in
# lsan_suppressions.txt (the same file open_external_env/move_undo_env/
# runner_rescan_env already point ASan at for their own popover tests).
fixtures_env.set('LSAN_OPTIONS',
   'suppressions=' + join_paths(meson.project_source_root(), 'tests',
                               'lsan_suppressions.txt'))

# GSettings test environment: memory backend (in-process, no dconf) + the
# schema compiled into the build tree (data/gschemas.compiled). Used by the
# plain-C settings unit test and the integration test that changes settings.
gsettings_env = environment()
gsettings_env.set('GIO_USE_VFS', 'local')
gsettings_env.set('GGAZE_FIXTURES_DIR',
   join_paths(meson.project_source_root(), 'tests', 'fixtures'))
gsettings_env.set('GGAZE_SAMPLE_DIR',
   join_paths(meson.project_source_root(), 'sample-images'))
gsettings_env.set('GSETTINGS_BACKEND', 'memory')
gsettings_env.set('GSETTINGS_SCHEMA_DIR', ggaze_gschema_dir)

# Shared test helpers (AGENTS.md: "Shared helpers go in tests/helpers/").
# Grid-cell activation and alert-dialog driving are used by more than one
# suite, so they live here instead of being copy-pasted per binary.
test_helpers_inc = include_directories('helpers')
test_helpers_lib = static_library(
   'ggaze_test_helpers',
   ['helpers/gtk_helpers.c', ggaze_conf_h],
   include_directories : [inc, src_inc, test_helpers_inc],
   dependencies : ggaze_deps,
   install : false,
)

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

if avif_dep.found()
   test_loader_avif = executable(
      'test_loader_avif',
      ['test_loader_avif.c', ggaze_conf_h],
      include_directories : [inc, src_inc],
      dependencies : [gdkpixbuf_dep, gtk4_dep, glib_dep, gio_dep,
         avif_dep],
      link_with : ggaze_lib,
      install : false,
   )
   test('loader_avif', test_loader_avif, suite : 'unit', env : fixtures_env)
endif

if jxl_dep.found()
   test_loader_jxl = executable(
      'test_loader_jxl',
      ['test_loader_jxl.c', ggaze_conf_h],
      include_directories : [inc, src_inc],
      dependencies : [gdkpixbuf_dep, gtk4_dep, glib_dep, gio_dep,
         jxl_dep],
      link_with : ggaze_lib,
      install : false,
   )
   test('loader_jxl', test_loader_jxl, suite : 'unit', env : fixtures_env)
endif

if heif_dep.found()
   test_loader_heif = executable(
      'test_loader_heif',
      ['test_loader_heif.c', ggaze_conf_h],
      include_directories : [inc, src_inc],
      dependencies : [gdkpixbuf_dep, gtk4_dep, glib_dep, gio_dep,
         heif_dep],
      link_with : ggaze_lib,
      install : false,
   )
   test('loader_heif', test_loader_heif, suite : 'unit', env : fixtures_env)
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

test_mover = executable(
   'test_mover', ['test_mover.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : [glib_dep, gio_dep],
   link_with : ggaze_lib, install : false,
)
test('mover', test_mover, suite : 'unit', env : fixtures_env)

test_settings = executable(
   'test_settings', ['test_settings.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : [glib_dep, gio_dep],
   link_with : ggaze_lib, install : false,
)
test('settings', test_settings, suite : 'unit', env : gsettings_env,
   depends : ggaze_gschemas)

test_opener = executable(
   'test_opener', ['test_opener.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : [glib_dep, gio_dep],
   link_with : ggaze_lib, install : false,
)
test('opener', test_opener, suite : 'unit', env : fixtures_env)

test_runner = executable(
   'test_runner', ['test_runner.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : [glib_dep, gio_dep],
   link_with : ggaze_lib, install : false,
)
test('runner', test_runner, suite : 'unit', env : fixtures_env)

if gegl_dep.found()
   test_enhancer = executable(
      'test_enhancer', ['test_enhancer.c', ggaze_conf_h],
      include_directories : [inc, src_inc],
      dependencies : [glib_dep, gegl_dep, gtk4_dep],
      link_with : ggaze_lib, install : false,
   )
   enhancer_env = environment()
   enhancer_env.set('GIO_USE_VFS', 'local')
   enhancer_env.set('GGAZE_FIXTURES_DIR',
      join_paths(meson.project_source_root(), 'tests', 'fixtures'))
   enhancer_env.set('LSAN_OPTIONS',
      'suppressions=' + join_paths(meson.project_source_root(), 'tests',
                                  'lsan_suppressions.txt'))
   test('enhancer', test_enhancer, suite : 'unit', env : enhancer_env)
endif

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
#
# Lane determinism (1w0) -- why these tests are NOT pinned to a backend.
#
# The integration suites used to pass or fail depending on whether a display
# happened to be reachable, which made every "all four lanes green" claim
# unfalsifiable. Two separate causes, both fixed, neither by pinning a
# backend:
#
#  1. Displayless runs reported OK while running nothing. gtk_init_check()
#     failed, the suite exited 0 with an empty "1..0" TAP plan, and meson
#     counted it as a pass. Each main() now returns 77 instead, so meson
#     prints SKIP and the lane numbers say what actually executed. A lane
#     with 21 SKIPs is not the same result as a lane with 21 OKs, and it no
#     longer looks like one.
#
#  2. With a display, GTK's GDK backend leaked third-party memory that no
#     suppression covered, so the ASan lanes aborted. tests/lsan_suppressions
#     .txt now covers both the Wayland and the X11/EGL paths; its "GDK display
#     backends" block is the canonical write-up, including the two ways those
#     entries can look dead when they are not. Read it before editing it.
#
#  3. Load, not display: the dialog waits in tests/helpers/gtk_helpers.c spent
#     a fixed number of poll iterations rather than wall-clock time, so a full
#     parallel ASan lane could fail a wait that passed with the suite run
#     alone. Fixed there; see ggtest_wait_for_dialog(). Verify with FULL lane
#     runs, never a single suite -- running one suite alone is exactly what
#     kept this invisible.
#
# Pinning GDK_BACKEND was considered and rejected. This GTK build offers
# broadway, wayland and x11 (`pkg-config --variable=targets gtk4`) and none of
# them is headless: broadway needs a broadwayd daemon and x11 needs an X
# server, so forcing one cannot make behaviour identical everywhere -- on a
# host without that server it would turn every integration suite into a silent
# skip, trading a red lane for lost coverage. It would not even remove the
# need for the suppressions: X11 leaks ~92 KB of Mesa/EGL state deterministically
# at display-open time, and the Wayland wl_proxy leak is racy *within* a single
# backend (a GTK-only probe with no ggaze code leaks it on ~4 runs in 10), so
# no choice of backend is leak-free. Suppressions are load-bearing either way.
#
# The presenting suites are left presenting for real. test_grid_select_gate's
# present_and_lay_out() exists to get true laid-out coordinates out of
# gtk_widget_compute_bounds(), and test_settings_ui asserts on the root window
# adw_dialog_present() creates; a backend that never lays out would gut both.
#
# KNOWN, PRE-EXISTING, NOT FIXED HERE: the header above says CI uses xvfb-run,
# and under plain xvfb three suites (open_external, runner_rescan, move_undo)
# abort on assertions like "'p_pop' should not be NULL". xvfb has no DRI3
# ("libEGL warning: DRI3 error: Could not get DRI3 device"), so GTK popovers
# never materialise there. Verified pre-existing by re-running all three at
# 7a0f3d2, before any 1w0 change. So a green xvfb lane is not achievable today
# for reasons that have nothing to do with leaks -- worth knowing before
# trusting "CI uses xvfb-run" as a statement about a passing lane.
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

if libjpeg_dep.found()
   test_progress_lww = executable(
      'test_progress_lww',
      ['test_progress_lww.c', ggaze_conf_h],
      include_directories : [inc, src_inc],
      dependencies : ggaze_deps,
      link_with : ggaze_lib,
      install : false,
   )
   test('progress_lww', test_progress_lww,
      suite : 'integration', env : fixtures_env)
endif

test_full_lifecycle = executable(
   'test_full_lifecycle', ['test_full_lifecycle.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib, install : false,
)
test('full_lifecycle', test_full_lifecycle,
   suite : 'integration', env : fixtures_env)

test_shortcut = executable(
   'test_shortcut', ['test_shortcut.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib, install : false,
)
test('shortcut', test_shortcut,
   suite : 'integration', env : fixtures_env)

test_clipboard = executable(
   'test_clipboard', ['test_clipboard.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib, install : false,
)
test('clipboard', test_clipboard,
   suite : 'integration', env : fixtures_env)

test_copy = executable(
   'test_copy', ['test_copy.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib, install : false,
)
test('copy', test_copy, suite : 'integration', env : fixtures_env)

test_marks = executable(
   'test_marks', ['test_marks.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib, install : false,
)
test('marks', test_marks, suite : 'integration', env : fixtures_env)

test_delete_safety = executable(
   'test_delete_safety', ['test_delete_safety.c', ggaze_conf_h],
   include_directories : [inc, src_inc, test_helpers_inc],
   dependencies : ggaze_deps,
   link_with : [ggaze_lib, test_helpers_lib], install : false,
)
# aw0 added four subtests that drive the REAL confirm dialog (raise it, close
# the window under it, dispose under it, answer it), each of which opens a
# window, decodes an image and drains the main loop for an async
# GtkAlertDialog: ~1.3-2 s apiece, taking the suite from ~3 s to 8.9 s
# measured on the ASan lane (8.0 s plain). That is still inside meson's
# default 30 s, but at only ~3.4x it is thinner than this suite ever needed
# before, so the budget is stated explicitly -- 60 s is ~6.7x on the measured
# figure, generous for a loaded CI box and tight enough that a dialog which
# never appears is caught in a minute rather than five. Re-measure when
# subtests are added: the margin shrinks with every one.
test('delete_safety', test_delete_safety,
   suite : 'integration', env : fixtures_env, timeout : 60)

test_delete_nav = executable(
   'test_delete_nav', ['test_delete_nav.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib, install : false,
)
test('delete_nav', test_delete_nav,
   suite : 'integration', env : fixtures_env)

test_grid_cancel = executable(
   'test_grid_cancel',
   ['test_grid_cancel.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib,
   install : false,
)
test('grid_cancel', test_grid_cancel,
   suite : 'integration', env : fixtures_env)

test_grid_select_gate = executable(
   'test_grid_select_gate',
   ['test_grid_select_gate.c', ggaze_conf_h],
   include_directories : [inc, src_inc, test_helpers_inc],
   dependencies : ggaze_deps,
   link_with : [ggaze_lib, test_helpers_lib],
   install : false,
)
test('grid_select_gate', test_grid_select_gate,
   suite : 'integration', env : fixtures_env)

test_settings_ui = executable(
   'test_settings_ui', ['test_settings_ui.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib, install : false,
)
test('settings_ui', test_settings_ui,
   suite : 'integration', env : fixtures_env, depends : ggaze_gschemas)

test_open_external = executable(
   'test_open_external', ['test_open_external.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib, install : false,
)
open_external_env = environment()
open_external_env.set('GIO_USE_VFS', 'local')
open_external_env.set('GGAZE_FIXTURES_DIR',
   join_paths(meson.project_source_root(), 'tests', 'fixtures'))
open_external_env.set('GGAZE_SAMPLE_DIR',
   join_paths(meson.project_source_root(), 'sample-images'))
open_external_env.set('GSETTINGS_BACKEND', 'memory')
open_external_env.set('GSETTINGS_SCHEMA_DIR', ggaze_gschema_dir)
open_external_env.set('LSAN_OPTIONS',
   'suppressions=' + join_paths(meson.project_source_root(), 'tests',
                               'lsan_suppressions.txt'))
test('open_external', test_open_external,
   suite : 'integration', env : open_external_env, depends : ggaze_gschemas)

test_move_undo = executable(
   'test_move_undo', ['test_move_undo.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib, install : false,
)
move_undo_env = environment()
move_undo_env.set('GIO_USE_VFS', 'local')
move_undo_env.set('GGAZE_FIXTURES_DIR',
   join_paths(meson.project_source_root(), 'tests', 'fixtures'))
move_undo_env.set('GGAZE_SAMPLE_DIR',
   join_paths(meson.project_source_root(), 'sample-images'))
move_undo_env.set('GSETTINGS_BACKEND', 'memory')
move_undo_env.set('GSETTINGS_SCHEMA_DIR', ggaze_gschema_dir)
move_undo_env.set('LSAN_OPTIONS',
   'suppressions=' + join_paths(meson.project_source_root(), 'tests',
                               'lsan_suppressions.txt'))
test('move_undo', test_move_undo,
   suite : 'integration', env : move_undo_env, depends : ggaze_gschemas)

test_runner_rescan = executable(
   'test_runner_rescan', ['test_runner_rescan.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib, install : false,
)
runner_rescan_env = environment()
runner_rescan_env.set('GIO_USE_VFS', 'local')
runner_rescan_env.set('GGAZE_FIXTURES_DIR',
   join_paths(meson.project_source_root(), 'tests', 'fixtures'))
runner_rescan_env.set('GGAZE_SAMPLE_DIR',
   join_paths(meson.project_source_root(), 'sample-images'))
runner_rescan_env.set('GSETTINGS_BACKEND', 'memory')
runner_rescan_env.set('GSETTINGS_SCHEMA_DIR', ggaze_gschema_dir)
runner_rescan_env.set('LSAN_OPTIONS',
   'suppressions=' + join_paths(meson.project_source_root(), 'tests',
                               'lsan_suppressions.txt'))
test('runner_rescan', test_runner_rescan,
   suite : 'integration', env : runner_rescan_env, depends : ggaze_gschemas)

# tu0: enhance-flow integration coverage, gated the same way as the
# test_enhancer unit test above -- built (and run) only when GEGL is
# actually enabled, so the minimal lane skips it cleanly by never building
# it at all.
if gegl_dep.found()
   test_enhance_flow = executable(
      'test_enhance_flow',
      ['test_enhance_flow.c', ggaze_conf_h],
      include_directories : [inc, src_inc, test_helpers_inc],
      dependencies : ggaze_deps + [gegl_dep],
      link_with : [ggaze_lib, test_helpers_lib],
      install : false,
   )
   # Drives the real Save/Discard/Cancel prompt, and its move-continuation
   # subtest configures move destinations through GSettings, so it needs the
   # compiled schema like test_move_undo/test_settings_ui do. Each subtest
   # opens a window, decodes an image, runs a GEGL chain and then drains the
   # main loop for the async dialog, so ~1.5 s apiece is the floor; the
   # round-4 subtests that wait out the folder monitor's 250 ms debounce cost
   # ~3.5 s each. The default 30 s budget stopped being enough at ~20
   # subtests. Slowest measured run: 71.7 s (ASan lane, 39 subtests; the
   # plain lane needs 69.5 s for the same set), so 180 s is ~2.5x headroom --
   # generous enough for a loaded CI box, tight enough that a real hang is
   # caught in three minutes rather than five. Re-measure this when the
   # subtest count grows: the margin shrinks with every one added.
   test('enhance_flow', test_enhance_flow,
      suite : 'integration', env : fixtures_env, depends : ggaze_gschemas,
      timeout : 180)
endif

subdir('integration')
