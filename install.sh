#!/bin/sh
# Build and install ewm and its dependencies:
#   curl -fsSL https://raw.githubusercontent.com/almqv/ewm/main/install.sh | sh
# Run from a checkout to install that checkout instead.
# Environment: PREFIX (default /usr/local), EWM_REF (branch or tag, default main)
set -eu

repo=https://github.com/almqv/ewm
prefix=${PREFIX:-/usr/local}

if [ "$(id -u)" -eq 0 ]; then
	sudo=
elif command -v sudo >/dev/null 2>&1; then
	sudo=sudo
elif command -v doas >/dev/null 2>&1; then
	sudo=doas
else
	echo "ewm: run as root or install sudo" >&2
	exit 1
fi

if command -v apt-get >/dev/null 2>&1; then
	$sudo apt-get update
	$sudo apt-get install -y build-essential git pkg-config libx11-dev \
		libxft-dev libxinerama-dev libfontconfig-dev libyajl-dev liblua5.4-dev
elif command -v dnf >/dev/null 2>&1; then
	$sudo dnf install -y gcc make git pkgconf-pkg-config libX11-devel \
		libXft-devel libXinerama-devel fontconfig-devel yajl-devel lua-devel
elif command -v pacman >/dev/null 2>&1; then
	$sudo pacman -Syu --needed --noconfirm base-devel git libx11 libxft \
		libxinerama fontconfig yajl lua
elif command -v zypper >/dev/null 2>&1; then
	$sudo zypper install -y gcc make git pkgconf libX11-devel libXft-devel \
		libXinerama-devel fontconfig-devel libyajl-devel lua54-devel
elif command -v xbps-install >/dev/null 2>&1; then
	$sudo xbps-install -Sy base-devel git pkg-config libX11-devel \
		libXft-devel libXinerama-devel fontconfig-devel yajl-devel lua54-devel
elif command -v apk >/dev/null 2>&1; then
	$sudo apk add build-base git pkgconf libx11-dev libxft-dev \
		libxinerama-dev fontconfig-dev yajl-dev lua5.4-dev
else
	echo "ewm: unknown package manager; install a C compiler, make, git," \
		"pkg-config and the development files of libX11, libXft," \
		"libXinerama, fontconfig, yajl and Lua 5.4" >&2
fi

if [ -f src/ewm.c ] && [ -f Makefile ]; then
	src=$PWD
else
	src=$(mktemp -d)
	trap 'rm -rf "$src"' EXIT
	git clone --depth 1 --branch "${EWM_REF:-main}" "$repo" "$src"
fi

make -C "$src" PREFIX="$prefix"
$sudo make -C "$src" install PREFIX="$prefix" XSESSIONDIR=/usr/share/xsessions

# hotswap a running ewm (from a terminal inside the session)
if "$prefix/bin/ewm-msg" run_command restart >/dev/null 2>&1; then
	echo "ewm updated and restarted in place."
	exit 0
fi

cat <<EOF

ewm is installed in $prefix. Log out and pick "ewm" in your display manager,
or put "exec ewm" in ~/.xinitrc. ~/.config/ewm/config.lua is created on the
first start; ewm reloads it whenever you save it.
EOF
