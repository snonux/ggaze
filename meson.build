# ggaze — GTK Gaze
#
# A small, fast, native image viewer for Fedora Linux, written in C with
# GTK4. Full design lives in docs/; this file is the build root: the static
# ggaze library (shared by the executable and the tests), the optional
# backends behind meson feature options, and the generated config header.

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
# >= 4.10: GdkTextureDownloader + gdk_texture_get_format (histogram.c reads a
# texture's pixels in its native layout through them; both are 4.10 API).
gtk4_dep       = dependency('gtk4', version : '>= 4.10')
# >= 2.74: G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC, the sub-second mtime the
# texture cache's stamp compares (texturecache.c). Pinned here rather than
# left to whatever GLib GTK happens to pull in.
glib_dep       = dependency('glib-2.0', version : '>= 2.74')
gio_dep        = dependency('gio-2.0', version : '>= 2.74')
adwaita_dep    = dependency('libadwaita-1')
gdkpixbuf_dep  = dependency('gdk-pixbuf-2.0')  # pixbuf loader backend
libexif_dep    = dependency('libexif')           # EXIF gather (info overlay, M4)
# libm for the plain-C geometry modules (croprect/transform use sin/cos/
# floor, gesture-math fabs); GTK pulls it in transitively everywhere else,
# but the unit tests that link only glib + the static ggaze library need it
# spelled out.
cc    = meson.get_compiler('c')
m_dep = cc.find_library('m', required : false)

# --- Optional backends (feature: auto/disabled/enabled). See meson_options.txt.
gegl_dep    = dependency('gegl-0.4', required : get_option('gegl'))
jxl_dep     = dependency('libjxl',   required : get_option('jxl'))
avif_dep    = dependency('libavif',  required : get_option('avif'))
heif_dep    = dependency('libheif',  required : get_option('heif'))
# libjpeg-turbo direct JPEG (progressive low-res preview); without it the
# GdkPixbuf backend decodes JPEG.
libjpeg_dep = dependency('libjpeg',  required : get_option('jpeg'))

# --- Generated config header (app id, version, feature flags) ---
conf = configuration_data()
conf.set_quoted('GGAZE_VERSION', meson.project_version())
conf.set_quoted('GGAZE_APP_ID', 'org.buetow.ggaze')
conf.set('GGAZE_HAVE_GEGL', gegl_dep.found() ? 1 : 0)
conf.set('GGAZE_HAVE_JXL',  jxl_dep.found()  ? 1 : 0)
conf.set('GGAZE_HAVE_AVIF', avif_dep.found() ? 1 : 0)
conf.set('GGAZE_HAVE_HEIF', heif_dep.found() ? 1 : 0)
conf.set('GGAZE_HAVE_JPEG', libjpeg_dep.found() ? 1 : 0)

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
ggaze_src = files(
      'src/app.c',
      'src/window.c',
      'src/viewer.c',
      'src/navigator.c',
      'src/shortcuts.c',
      'src/popup_list.c',
      'src/texturecache.c',
      'src/viewload.c',
      'src/info-overlay.c',
      'src/histogram.c',
      'src/histogram-view.c',
      'src/undo.c',
      'src/thumbnail.c',
      'src/trash.c',
      'src/gridview.c',
      'src/info.c',
      'src/icc.c',
      'src/streamread.c',
      'src/delete-confirm.c',
      'src/fileops.c',
      'src/dialog-util.c',
      'src/save-gate.c',
      'src/mover.c',
      'src/opener.c',
      'src/pathutil.c',
      'src/croprect.c',
      'src/gesture-math.c',
      'src/transform.c',
      'src/edit-history.c',
      'src/runner.c',
      'src/settings.c',
      'src/settings-pair.c',
      'src/prefs.c',
      'src/clipboard.c',
      'src/loader/loader.c',
      'src/loader/detect.c',
      'src/loader/animation.c',
      'src/loader/pixbuf-util.c',
      'src/loader/backends/pixbuf.c',
   )
ggaze_c_args = []
ggaze_extra_deps = []

if jxl_dep.found()
   ggaze_src += files('src/loader/backends/jxl.c')
   ggaze_extra_deps += jxl_dep
endif
if avif_dep.found()
   ggaze_src += files('src/loader/backends/avif.c')
   ggaze_extra_deps += avif_dep
endif
if heif_dep.found()
   ggaze_src += files('src/loader/backends/heif.c')
   ggaze_extra_deps += heif_dep
endif
if libjpeg_dep.found()
   ggaze_src += files('src/loader/backends/jpeg.c')
   ggaze_extra_deps += libjpeg_dep
endif
if gegl_dep.found()
   # loader/intact.c vouches for a file before GEGL's loaders see it
   # (decision #45); nothing else calls it, so the minimal build carries
   # none of it. streamread.c stays above: icc.c walks with it in every
   # build.
   ggaze_src += files('src/enhancer.c', 'src/enhance-ui.c', 'src/enhance-ctrl.c',
      'src/tool-ctrl.c', 'src/edit-mode.c', 'src/loader/intact.c')
   ggaze_extra_deps += gegl_dep
   # lcms2 is babl's own dependency (babl-devel requires it), so a GEGL
   # build has it already. enhancer.c opens a CMYK profile with it before
   # babl does: babl keeps a failed LCMS transform and crashes on its first
   # conversion (decision #45).
   ggaze_extra_deps += dependency('lcms2')
endif

ggaze_lib = static_library('ggaze',
   ggaze_src,
   include_directories : [inc, src_inc],
   dependencies : [gtk4_dep, glib_dep, gio_dep, adwaita_dep, gdkpixbuf_dep,
      libexif_dep, m_dep] + ggaze_extra_deps,
   c_args : ggaze_c_args,
   install : false,
)

ggaze_deps = [gtk4_dep, glib_dep, gio_dep, adwaita_dep, gdkpixbuf_dep, libexif_dep,
   m_dep]

subdir('src')
subdir('data')
subdir('tests')