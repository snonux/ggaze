#!/usr/bin/env bash
#
# Regenerate the README screenshots (docs/screenshots/*.png).
#
# Renders a folder of procedural images (make-images.py -- nothing personal,
# nothing copyrighted), runs the built ggaze on it inside Xvfb (never on the
# live desktop), drives it with xdotool and captures the root window with
# ImageMagick. Everything temporary lives under build/screenshots-tmp and is
# removed afterwards.
#
# Needs: a built tree (meson setup build && ninja -C build), python3 with
# Pillow + numpy, xvfb-run, xdotool, ImageMagick (magick + import).
#
# Usage: docs/screenshots/make-screenshots.sh   (from the repo root)

set -euo pipefail

readonly width=1280
readonly height=800

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly project_dir
readonly out_dir="${project_dir}/docs/screenshots"
readonly build_dir="${project_dir}/build"
readonly tmp_dir="${build_dir}/screenshots-tmp"

# Runs inside Xvfb: open the folder, shoot the grid, open the large view
# with the info card, shoot that. Sleeps are generous because thumbnails
# and the EXIF/histogram gather are asynchronous.
shoot() {
   local -r img_dir="$1"
   local pid win

   "${build_dir}/src/ggaze" "${img_dir}" &
   pid=$!
   win="$(xdotool search --sync --onlyvisible --pid "${pid}" | head -n 1)"
   xdotool windowmove "${win}" 0 0 windowsize "${win}" "${width}" "${height}"
   sleep 2
   # Park the pointer on the empty header-bar middle (no hover highlight on
   # a cell) and make the thumbnails bigger so the folder fills the frame.
   xdotool mousemove 400 27
   xdotool windowfocus --sync "${win}"
   xdotool key --window "${win}" plus plus
   sleep 4
   import -window root "${tmp_dir}/grid.png"

   # Open the large view on the first image, then toggle the info overlay.
   xdotool key --window "${win}" g Return
   sleep 2
   xdotool key --window "${win}" i
   sleep 3
   import -window root "${tmp_dir}/large.png"

   kill "${pid}"
   wait "${pid}" || true
}

# Strip metadata and squeeze the palette so each PNG stays well under 500 KB.
optimise() {
   local -r src="$1" dst="$2"

   magick "${src}" -strip -define png:compression-level=9 "${dst}"
}

main() {
   rm -rf "${tmp_dir}"
   mkdir -p "${tmp_dir}/runtime" "${tmp_dir}/cache" "${tmp_dir}/config"
   python3 "${out_dir}/make-images.py" "${tmp_dir}/images"

   # Private runtime dir so GDK cannot reach the desktop's Wayland socket;
   # private cache/config so the thumbnail cache and GSettings stay out of
   # the user's home; memory backend + build-tree schema like the tests.
   export -f shoot
   env -u WAYLAND_DISPLAY \
      XDG_RUNTIME_DIR="${tmp_dir}/runtime" \
      XDG_CACHE_HOME="${tmp_dir}/cache" \
      XDG_CONFIG_HOME="${tmp_dir}/config" \
      GDK_BACKEND=x11 \
      GSETTINGS_BACKEND=memory \
      GSETTINGS_SCHEMA_DIR="${build_dir}/data" \
      ADW_DEBUG_COLOR_SCHEME=prefer-dark \
      build_dir="${build_dir}" tmp_dir="${tmp_dir}" \
      width="${width}" height="${height}" \
      xvfb-run -a -s "-screen 0 ${width}x${height}x24" \
      bash -c 'shoot "$1"' _ "${tmp_dir}/images"

   optimise "${tmp_dir}/grid.png" "${out_dir}/grid.png"
   optimise "${tmp_dir}/large.png" "${out_dir}/large-view.png"
   rm -rf "${tmp_dir}"
}

main "$@"
