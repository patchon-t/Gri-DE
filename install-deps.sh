#!/usr/bin/env bash
# Install GriDE's build and runtime dependencies on Debian 13 (trixie) or
# LMDE 7, both of which ship wlroots 0.18. Every package comes from apt.
#
# Usage: ./install-deps.sh
set -euo pipefail

# Phase 1: the compositor. Later phases add to this list.
packages=(
	build-essential      # C compiler and make
	meson                # build system (>= 1.3)
	ninja-build
	pkg-config
	python3              # generates gride-tokens.h from tokens.json
	wayland-protocols
	libwayland-dev
	libwlroots-0.18-dev  # the compositor library, pinned to 0.18
	libxkbcommon-dev     # keyboard layouts
	libinput-dev         # touchpad settings (tap to click)
	libpixman-1-dev
	libdrm-dev           # drm_fourcc.h pixel formats
	foot                 # default terminal for Super+Enter
	fonts-jetbrains-mono # the GriDE mono font
)

if [[ -r /etc/os-release ]]; then
	. /etc/os-release
fi

if ! apt-cache show libwlroots-0.18-dev >/dev/null 2>&1; then
	echo "This system's apt has no libwlroots-0.18-dev, which GriDE needs."
	if [[ "${ID:-}" == "linuxmint" && -n "${UBUNTU_CODENAME:-}" ]]; then
		echo "Linux Mint ${VERSION_ID:-} is based on Ubuntu ${UBUNTU_CODENAME}, which does not package wlroots 0.18."
		echo "LMDE 7 (based on Debian 13) does. See README.md, section \"Supported systems\"."
	else
		echo "Supported: Debian 13 (trixie) and LMDE 7. Run 'sudo apt update' first if you are on one of them."
	fi
	exit 1
fi

SUDO=""
if [[ $EUID -ne 0 ]]; then
	SUDO="sudo"
fi

echo "Installing: ${packages[*]}"
$SUDO apt-get install -y --no-install-recommends "${packages[@]}"
echo "Done. Next: make && make dev"
