# GriDE developer entry points. The real build is Meson + Ninja.
#
#   make                 build into ./build
#   make dev             run GriDE nested in a window of your current desktop
#   sudo make install    install to $(PREFIX) (default /usr/local)
#
# Variables: PREFIX, BUILDDIR, MOD (super|alt), THEME (light|dark),
# GRID (e.g. 4x3), GRID_MODE (free|auto), TERMINAL (command for Super+Enter),
# DEV_CMD (run once GriDE is up).

BUILDDIR ?= build
PREFIX   ?= /usr/local
MOD      ?= super
THEME    ?= light
GRID     ?= 4x3
GRID_MODE ?= free
TERMINAL ?=
DEV_CMD  ?=

.PHONY: all build dev install uninstall clean

all: build

$(BUILDDIR)/build.ninja:
	meson setup $(BUILDDIR) --prefix=$(PREFIX)

build: $(BUILDDIR)/build.ninja
	ninja -C $(BUILDDIR)

# wlroots picks its nested backend from WAYLAND_DISPLAY or DISPLAY.
dev: build
	@if [ -z "$$WAYLAND_DISPLAY" ] && [ -z "$$DISPLAY" ]; then \
		echo "make dev opens GriDE in a window, so run it from inside a desktop session."; \
		echo "To run GriDE on a bare TTY instead: $(BUILDDIR)/wm/gride-wm"; \
		exit 1; \
	fi
	$(BUILDDIR)/wm/gride-wm -d -m $(MOD) -T $(THEME) -G $(GRID) -M $(GRID_MODE) \
		$(if $(TERMINAL),-t '$(TERMINAL)') $(if $(DEV_CMD),-s '$(DEV_CMD)')

install: build
	meson install -C $(BUILDDIR)

uninstall:
	ninja -C $(BUILDDIR) uninstall

clean:
	rm -rf $(BUILDDIR)
