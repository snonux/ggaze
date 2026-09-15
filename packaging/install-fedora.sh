#!/usr/bin/env bash
# Install ggaze on a Fedora laptop for the current user, including a GNOME
# application launcher entry and app icon.
#
# Usage:
#   ./packaging/install-fedora.sh
#   PREFIX=/usr/local ./packaging/install-fedora.sh   # system-wide (needs root)
#   SKIP_DEPS=1 ./packaging/install-fedora.sh         # skip dnf packages

set -euo pipefail

declare script_dir project_dir
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "${script_dir}/.." && pwd)"
declare -r script_dir project_dir
declare -r prefix="${PREFIX:-${HOME}/.local}"
declare -r build_dir="${BUILD_DIR:-${project_dir}/build-install}"
declare -r skip_deps="${SKIP_DEPS:-0}"

_install::log() {
   printf '==> %s\n' "$*"
}

_install::need_cmd() {
   local -r cmd="$1"
   if ! command -v "${cmd}" >/dev/null 2>&1; then
      printf 'error: required command not found: %s\n' "${cmd}" >&2
      return 1
   fi
}

_install::deps() {
   if [[ "${skip_deps}" == "1" ]]; then
      _install::log "Skipping dependency install (SKIP_DEPS=1)"
      return 0
   fi

   _install::need_cmd dnf
   _install::log "Installing Fedora build/runtime packages (may ask for sudo)"

   local -a pkgs=(
      meson ninja-build gcc pkgconf-pkg-config
      gtk4-devel glib2-devel libadwaita-devel gdk-pixbuf2-devel
      libexif-devel libjpeg-turbo-devel
      # optional backends (auto-detected by meson)
      gegl04-devel babl-devel libjxl-devel libheif-devel libavif-devel
      desktop-file-utils
   )

   if [[ "$(id -u)" -eq 0 ]]; then
      dnf install -y "${pkgs[@]}"
   else
      sudo dnf install -y "${pkgs[@]}"
   fi
}

_install::build() {
   _install::need_cmd meson
   _install::need_cmd ninja

   _install::log "Configuring ${build_dir} (prefix=${prefix})"
   if [[ ! -d "${build_dir}" ]]; then
      meson setup "${build_dir}" "${project_dir}" \
         --prefix="${prefix}" \
         -Dgegl=auto -Djxl=auto -Davif=auto -Dheif=auto
   else
      meson setup --reconfigure "${build_dir}" "${project_dir}" \
         --prefix="${prefix}" \
         -Dgegl=auto -Djxl=auto -Davif=auto -Dheif=auto
   fi

   _install::log "Building"
   meson compile -C "${build_dir}"
}

_install::install() {
   _install::log "Installing into ${prefix}"
   meson install -C "${build_dir}"

   local -r desktop_dir="${prefix}/share/applications"
   local -r icon_dir="${prefix}/share/icons/hicolor"

   if command -v update-desktop-database >/dev/null 2>&1; then
      update-desktop-database "${desktop_dir}" 2>/dev/null || true
   fi
   if command -v gtk-update-icon-cache >/dev/null 2>&1 \
      && [[ -d "${icon_dir}" ]]; then
      gtk-update-icon-cache -f -t "${icon_dir}" 2>/dev/null || true
   fi

   _install::log "Installed:"
   printf '    binary : %s\n' "${prefix}/bin/ggaze"
   printf '    desktop: %s/org.buetow.ggaze.desktop\n' "${desktop_dir}"
   printf '    icon   : %s/scalable/apps/ggaze.svg\n' "${icon_dir}"
   printf '\n'
   printf 'Open the GNOME Activities overview and search for "ggaze".\n'
   if [[ "${prefix}" == "${HOME}/.local" ]]; then
      printf 'Ensure ~/.local/bin is on PATH if launching from a terminal.\n'
   fi
}

main() {
   _install::deps
   _install::build
   _install::install
}

main "$@"
