# ggaze data: .desktop entry + GSettings schema + AppStream metainfo + man page.
# `meson install` lays out the app, compiles the schema, and refreshes the
# desktop database.

install_data('org.buetow.ggaze.desktop',
   install_dir : join_paths(get_option('datadir'), 'applications'))

install_data('org.buetow.ggaze.metainfo.xml',
   install_dir : join_paths(get_option('datadir'), 'metainfo'))

install_data('org.buetow.ggaze.gschema.xml',
   install_dir : join_paths(get_option('datadir'), 'glib-2.0', 'schemas'))

install_man('ggaze.1')

gnome = import('gnome')
gnome.post_install(
   glib_compile_schemas : true,
   update_desktop_database : true,
)

# Compile the schema into the build tree so unit/integration tests can load it
# via GSETTINGS_SCHEMA_DIR without installing (and GSETTINGS_BACKEND=memory
# keeps writes in-process so tests never touch the user's dconf).
glib_compile_schemas = find_program('glib-compile-schemas')
ggaze_gschemas = custom_target(
   'ggaze-gschemas.compiled',
   input  : files('org.buetow.ggaze.gschema.xml'),
   output : 'gschemas.compiled',
   command: [glib_compile_schemas, '--targetdir', '@OUTDIR@',
             meson.current_source_dir()],
   build_by_default : true,
)
# Directory containing the compiled gschemas.compiled (consumed by tests).
ggaze_gschema_dir = meson.current_build_dir()