Name:    ggaze
Version: 0.1.0
Release: 1%{?dist}
Summary: Fast native image viewer for culling camera shoots

License: GPL-3.0-or-later
URL:     https://codeberg.org/snonux/ggaze
Source0: %{url}/archive/%{version}.tar.gz

BuildRequires: meson ninja-build gcc
BuildRequires: gtk4-devel glib2-devel libadwaita-devel gdk-pixbuf2-devel
BuildRequires: libexif-devel libjpeg-turbo-devel
# Optional (built if available):
BuildRequires: gegl04-devel babl-devel
BuildRequires: libjxl-devel libheif-devel
# BuildRequires: libavif-devel  # uncomment when available

Requires: gtk4 glib2 libadwaita gdk-pixbuf2 libexif libjpeg-turbo

%description
ggaze (GNOME Gaze) is a small, fast, native image viewer for Fedora Linux.
It opens a folder of pictures, shows a thumbnail grid, and lets you flip
through the shoot with vi-style keys, check EXIF, trash rejects, and move
on. No library, no database, no sidecars.

%prep
%setup -q

%build
%meson -Dgegl=auto -Djxl=auto -Davif=auto -Dheif=auto
%meson_build

%install
%meson_install

%check
%meson_test

%files
%{_bindir}/ggaze
%{_datadir}/applications/org.buetow.ggaze.desktop
%{_datadir}/glib-2.0/schemas/org.buetow.ggaze.gschema.xml
%{_datadir}/metainfo/org.buetow.ggaze.metainfo.xml
%{_mandir}/man1/ggaze.1*
%license LICENSE

%changelog
* Sat Jul 12 2026 ggaze contributors <paul@buetow.org> - 0.1.0-1
- Initial RPM package