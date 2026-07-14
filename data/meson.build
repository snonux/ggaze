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