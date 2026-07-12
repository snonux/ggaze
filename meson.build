# ggaze — GNOME Gaze
#
# A small, fast, native image viewer for Fedora Linux, written in C with
# GTK4. Full design lives in docs/; this file is the build root. Phase 0
# establishes the build/test/CI/convention harness — no production sources
# yet (they arrive from M0; see docs/roadmap.md).

project('ggaze', 'c',
   version : '0.1.0',
   license : 'GPL-3.0-or-later',
   meson_version : '>= 1.1',
   default_options : [
      'warning_level=2',   # -Wall -Wextra
      'c_std=c11',
      'b_pie=true',
   ],
)

# --- Always-required dependencies (the core viewer needs all of these) ---
gtk4_dep    = dependency('gtk4')
glib_dep    = dependency('glib-2.0')
gio_dep     = dependency('gio-2.0')
adwaita_dep = dependency('libadwaita-1')

# --- Optional backends (feature: auto/disabled/enabled). See meson_options.txt.
gegl_dep = dependency('gegl-0.4', required : get_option('gegl'))
jxl_dep  = dependency('libjxl',   required : get_option('jxl'))
avif_dep = dependency('libavif', required : get_option('avif'))
heif_dep = dependency('libheif',  required : get_option('heif'))

# --- Generated config header (app id, version, feature flags) ---
conf = configuration_data()
conf.set_quoted('GGAZE_VERSION', meson.project_version())
conf.set_quoted('GGAZE_APP_ID', 'org.buetow.ggaze')
conf.set('GGAZE_HAVE_GEGL', gegl_dep.found() ? 1 : 0)
conf.set('GGAZE_HAVE_JXL',  jxl_dep.found()  ? 1 : 0)
conf.set('GGAZE_HAVE_AVIF', avif_dep.found() ? 1 : 0)
conf.set('GGAZE_HAVE_HEIF', heif_dep.found() ? 1 : 0)

ggaze_conf_h = configure_file(
   input         : 'ggaze-config.h.in',
   output        : 'ggaze-config.h',
   configuration : conf,
)

# Include paths: source root (generated header) + src/ (module headers).
inc = include_directories('.')
src_inc = include_directories('src')

# --- ggaze library: the app + window (shared between the executable and
#     tests so the GApplication/GtkApplicationWindow code is exercised without
#     duplicating sources). Plain-C modules will join this library as they
#     land (navigator, trash, mover, ...).
ggaze_lib = static_library('ggaze',
   files('src/app.c', 'src/window.c'),
   include_directories : [inc, src_inc],
   dependencies : [gtk4_dep, glib_dep, gio_dep, adwaita_dep],
   install : false,
)

ggaze_deps = [gtk4_dep, glib_dep, gio_dep, adwaita_dep]

subdir('src')
subdir('data')
subdir('tests')