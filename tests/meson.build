# ggaze tests — two tracks (see docs/IMPLEMENTATION.md "Two test tracks").
#
#   unit        plain-C module tests, no display needed (>=80% coverage gate,
#               warn until M10 then fail).
#   integration cross-module flows with real temp dirs and REAL GTK on a REAL
#               display. Nothing here is offscreen and GTK4 offers no headless
#               backend: each main() returns 77 (meson SKIP) when
#               gtk_init_check() fails. Both tracks' sources live directly in
#               tests/ -- the split is the suite tag below, not a directory
#               (tests/integration/ contains only a meson.build). See the
#               integration-track note further down for the X11/xvfb lane CI
#               actually runs, and why five suites are is_parallel : false.
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
# The xvfb (X11) lane, and why popover suites are special (5w0).
#
# CI really does run this track under xvfb-run (.woodpecker/ci.yml, last step:
# `xvfb-run -a meson test -C build --suite integration`, on fedora:40 where no
# Wayland socket exists), so the X11 GDK backend is the backend CI uses. An
# earlier note here blamed the three aborts on xvfb's missing DRI3. That was
# wrong -- DRI3 is a renderer concern and forcing GSK_RENDERER=cairo/gl/ngl/
# vulkan changed nothing. Two unrelated causes, both now fixed:
#
#  1. No focus widget on a never-presented toplevel. gtk_popover_show() ends
#     with gtk_widget_child_focus() for an autohide popover, and
#     gtk_popover_focus()'s autohide arm (gtk/gtkpopover.c:1119-1127 -- NOT
#     the "Empty popover" branch at :1106-1110, which returns FALSE without
#     touching focus) passes a NULL gtk_root_get_focus() to
#     gtk_widget_is_ancestor(). Fixed in the suites via ggtest_focus_viewer();
#     the mechanism is written up in full in tests/helpers/gtk_helpers.h.
#
#  2. X11 seat-grab contention, handled below with is_parallel : false.
#
# REPRODUCING THE X11 LANE LOCALLY IS A TRAP. On a Wayland desktop,
# `xvfb-run -a meson test ...` does NOT test the X11 backend: GDK still finds
# the Wayland socket and the whole lane silently runs on Wayland (21/21 green,
# proving nothing about CI). Unsetting WAYLAND_DISPLAY is not enough either --
# libwayland falls back to $XDG_RUNTIME_DIR/wayland-0. Both are needed:
#
#   env -u WAYLAND_DISPLAY XDG_RUNTIME_DIR=$(mktemp -d) \
#     xvfb-run -a meson test -C build --suite integration
#
# A LIVE COMPOSITOR IS NOT A NEUTRAL DISPLAY EITHER (8w0). The mirror image of
# that trap: the session display has a window manager, and a window manager can
# close a window a test is asserting on. GTK turns such a close into a negative
# dialog response -- gtkmain.c GDK_DELETE -> gtk_window_emit_close_request ->
# gtk/deprecated/gtkdialog.c gtk_dialog_close_request ->
# gtk_dialog_response(GTK_RESPONSE_DELETE_EVENT) -> gtkalertdialog.c
# response_cb -- and what that reports depends on the dialog: with a cancel
# button configured, as the `D` >1-mark delete confirm configures one, it comes
# back as that button's index, i.e. a plain "Cancel was pressed" with NOTHING
# logged, since it is neither a dismissal (GTK_DIALOG_ERROR_DISMISSED needs no
# cancel button, task 2w0) nor a cancellation. That silence is what left this
# task with nothing to look at, and it is why the GEGL-only Save prompt now
# sets no cancel button (see _save_prompt_show): a WM close there arrives as
# DISMISSED and gets a g_message. Escape reaches the same signal via
# gtk_window_close(), so a close and an Escape stay indistinguishable either
# way -- the split that was won is "a button was pressed" vs "nobody pressed
# anything".
#
# Measured at 9aeb902 with 8 concurrent copies of test_enhance_flow against one
# display: on the session's Wayland compositor, 1 run in 32 failed exactly that
# way (a Save prompt asserted present by its "Cancel" button, its "Discard"
# button gone a few lines later); on Xvfb, which runs no window manager at all,
# the same binary produced 0 failures in 30 runs at 6-way concurrency and the
# two subtests 8w0 was filed against produced 0 in 120.
#
# What is immune is the RECIPE, not the lane: no run under the xvfb command
# above can reach that state, because there is no window manager on that
# display and no user typing into it. A plain `meson test -C build` on a live
# session is NOT immune -- neither mechanism (a WM closing the dialog, a stray
# key on a focused dialog) needs a second process, so serialising the suite
# (is_parallel : false since 5w0, and --repeat runs a non-parallel test strictly
# one at a time: mesonbuild/mtest.py _run_tests awaits every outstanding future
# before AND after such a runner) lowers the odds without conferring immunity.
# Click or type during enhance_flow's ~75 s and you can hit it. So soak
# -- and run day to day -- with the xvfb command above, not with the session
# display.

# --- suites that need exclusive use of the display's seat grab (5w0) -------
#
# A GtkPopover with autohide (all four of ggaze's `e`/`!`/`m`/`a` popups) is a
# GdkPopup, and on X11 gdk_x11_surface_present_popup() takes a gdk_seat_grab()
# for it. An X seat grab is display-global: only ONE client can hold it. When
# it fails, gdk_seat_default_grab() hides the popup surface it just mapped
# (gdk/gdkseatdefault.c: `if (status != GDK_GRAB_SUCCESS && !was_visible)
# gdk_surface_hide (surface)`), which arrives as notify::mapped on the popup
# surface, and GtkPopover turns that into gtk_widget_set_visible (FALSE) ->
# "closed" -> ggaze's own _open_ext_destroy()/_run_script_destroy()/
# _move_destroy() unparent the popover. The suite then finds NULL a couple of
# milliseconds after firing the action.
#
# CI runs every integration binary in parallel against ONE xvfb display, so
# these suites were racing each other for that single grab. Measured on this
# tree: 6 concurrent copies of /open_external/popup_structure on one Xvfb
# failed 60 times out of 72; the same lane with --num-processes 1 was green.
# The grab is taken by gdk_x11_surface_present_popup(), so the race is X11-only
# to begin with -- and on Wayland there was additionally nothing to contend
# for, because every popover subtest but one builds a toplevel it never
# presents and a popup whose parent surface is unrealized never presents there
# either (see gtk_helpers.h). That "but one" is /open_external/popup_really_maps
# (cw0), which does present; it lives in a suite that is is_parallel : false
# below, so it cannot reintroduce contention. Either way, only the X11 lane
# ever showed the race.
#
# is_parallel : false says exactly what is true -- these tests need exclusive
# use of a display-global resource. It is not a timing workaround and no
# assertion was weakened to accommodate it. The listed suites are every suite
# that pops an autohide popover, INCLUDING test_window (its `a` subtest opens
# the GEGL preset popover) and test_enhance_flow (its move-continuation
# subtest fires win.move): a suite that merely *takes* the grab still breaks
# whichever suite is asserting on a popover at that moment, so being a
# grabber is enough to require serialisation, not just being an asserter.
#
# Cost: the track goes from ~75 s to ~100 s wall-clock. Re-check that trade if
# more popover suites appear -- serialising a long suite (enhance_flow is 70 s
# on its own) is what dominates it.
#
# SCOPE OF THE GUARANTEE: is_parallel : false serialises tests WITHIN ONE
# `meson test` INVOCATION, and that is all it can do (dw0). Meson has no
# cross-process lock on the display; two invocations racing each other for the
# same X seat grab are exactly the contention this setting exists to prevent,
# and neither one can see the other. CI is safe because .woodpecker/ci.yml runs
# one invocation per lane, each under its own `xvfb-run -a`, i.e. its own
# display. A DEVELOPER IS NOT: AGENTS.md lists `meson test -C build` and
# `meson test -C build-asan` as separate commands, so running two lanes at once
# against one display reintroduces the race in full.
#
# Measured on this tree, one Xvfb display shared by two concurrent
# invocations -- `meson test -C build-minimal --suite integration` and
# `meson test -C build-min-asan --suite integration`:
#
#   lane 1 (build-minimal)  : Ok 20, Fail 0
#   lane 2 (build-min-asan) : Ok 19, Fail 1
#     integration - ggaze:move_undo  FAIL  killed by signal 6 SIGABRT
#     not ok /move/popup_structure - ERROR:../tests/test_move_undo.c:644:
#            test_move_popup_structure: 'p_pop' should not be NULL
#
# That is the seat-grab signature described above -- the popover was unparented
# out from under the assertion because the other lane held the grab -- but what
# a developer SEES is "SIGABRT" and a NULL widget, which reads like a product
# crash rather than two test runs fighting over a display. Hence this note.
# The fix is a display each, not a meson setting: give the second lane its own
# `xvfb-run -a`, or just run the lanes one after another.
test_window = executable(
   'test_window',
   ['test_window.c', ggaze_conf_h],
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib,
   install : false,
)
# is_parallel : false -- see "suites that need exclusive use of the
# display's seat grab" above (its `a` subtest opens the preset popover).
#
# It only takes that grab in the GEGL lanes, though (dw0). The `a` subtest is
# test_enhance_a_is_safe_with_and_without_gegl, and its popover only opens
# under #if GGAZE_HAVE_GEGL; in build-minimal and build-min-asan there is no
# p_enhancer, so win.enhance returns early, no popover opens and nothing
# contends for the seat grab. The serialisation therefore buys nothing in
# those two lanes and costs them 2.3 s.
#
# Left unconditional deliberately. Meson would need the flag varied by
# get_option('gegl'), which is a `feature` defaulting to auto -- so the
# condition would have to be resolved the same way the C code resolves it, in
# a second place, to save 2.3 s in two lanes. A wrong answer there is a
# reintroduced flake in the GEGL lanes; the right answer is worth 2.3 s. Do
# not "optimise" this without re-reading this paragraph.
test('window', test_window, suite : 'integration', env : fixtures_env,
   is_parallel : false)

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
   include_directories : [inc, src_inc, test_helpers_inc],
   dependencies : ggaze_deps,
   link_with : [ggaze_lib, test_helpers_lib], install : false,
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
   suite : 'integration', env : open_external_env,
   depends : ggaze_gschemas, is_parallel : false)

test_move_undo = executable(
   'test_move_undo', ['test_move_undo.c', ggaze_conf_h],
   include_directories : [inc, src_inc, test_helpers_inc],
   dependencies : ggaze_deps,
   link_with : [ggaze_lib, test_helpers_lib], install : false,
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
   suite : 'integration', env : move_undo_env,
   depends : ggaze_gschemas, is_parallel : false)

test_runner_rescan = executable(
   'test_runner_rescan', ['test_runner_rescan.c', ggaze_conf_h],
   include_directories : [inc, src_inc, test_helpers_inc],
   dependencies : ggaze_deps,
   link_with : [ggaze_lib, test_helpers_lib], install : false,
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
   suite : 'integration', env : runner_rescan_env,
   depends : ggaze_gschemas, is_parallel : false)

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
      timeout : 180, is_parallel : false)
endif

subdir('integration')
