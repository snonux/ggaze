# ggaze data: .desktop entry + GSettings schema. Full AppStream/icon install
# lands in M10; this wires the basics so `meson install` lays out the app and
# compiles the schema. gnome.post_install also refreshes the desktop database.

install_data('org.buetow.ggaze.desktop',
   install_dir : join_paths(get_option('datadir'), 'applications'))

install_data('org.buetow.ggaze.gschema.xml',
   install_dir : join_paths(get_option('datadir'), 'glib-2.0', 'schemas'))

gnome = import('gnome')
gnome.post_install(
   glib_compile_schemas : true,
   update_desktop_database : true,
)