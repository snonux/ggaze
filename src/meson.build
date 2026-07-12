# ggaze executable. The app/window code lives in libggae (see meson.build); the
# executable only adds the entry point and the --version CLI option.

ggaze_exe = executable('ggaze',
   'main.c',
   include_directories : [inc, src_inc],
   dependencies : ggaze_deps,
   link_with : ggaze_lib,
   install : true,
)