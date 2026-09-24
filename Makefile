# Convenience wrapper around meson/ninja for building and installing ggaze.
#
#   make                 release build in $(BUILDDIR)
#   make test            unit tests (integration needs a display: see AGENTS.md)
#   make install         install into ~/.local (no root needed)
#   make install PREFIX=/usr/local   system-wide (run with sudo)
#   make uninstall       remove what the last install put in place
#   make update          git pull --ff-only, then rebuild and install
#   make clean           delete $(BUILDDIR)
#
# Meson feature options (gegl, jxl, avif, heif, jpeg) stay `auto`: whatever
# -devel packages are present get used. Pass extra ones via MESON_OPTS, e.g.
#   make MESON_OPTS="-Dgegl=disabled"

PREFIX     ?= $(HOME)/.local
BUILDDIR   ?= build-release
MESON_OPTS ?=

.PHONY: all build setup test install uninstall update clean

all: build

# (Re)configure when the build dir is missing or PREFIX/options changed, so
# a later `make install PREFIX=...` does not silently use the old prefix.
setup:
	@if [ -d "$(BUILDDIR)" ]; then \
	   meson setup --reconfigure "$(BUILDDIR)" --prefix="$(PREFIX)" \
	      --buildtype=release $(MESON_OPTS); \
	else \
	   meson setup "$(BUILDDIR)" --prefix="$(PREFIX)" \
	      --buildtype=release $(MESON_OPTS); \
	fi

build: setup
	ninja -C "$(BUILDDIR)"

test: build
	meson test -C "$(BUILDDIR)" --suite unit

install: build
	meson install -C "$(BUILDDIR)"
	@case ":$$PATH:" in *":$(PREFIX)/bin:"*) ;; \
	   *) echo "note: $(PREFIX)/bin is not on your PATH";; esac

uninstall:
	ninja -C "$(BUILDDIR)" uninstall

update:
	git pull --ff-only
	$(MAKE) install

clean:
	rm -rf "$(BUILDDIR)"
