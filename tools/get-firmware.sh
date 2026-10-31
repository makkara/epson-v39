#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Download Epson's Windows driver package for the Perfection V39, extract
# the scanner firmware (esfw010c.bin) and install it where the epsonv39
# SANE backend looks for it.
#
# The firmware is copyrighted by Epson and is not part of this repository;
# this script fetches it from Epson's own download server.
#
# usage: tools/get-firmware.sh [--system | --dest DIR]
#   (default)  install for the current user: ~/.local/share/sane/epsonv39/
#   --system   install for all users: /usr/share/sane/epsonv39/ (run with sudo)
#   --dest DIR install into DIR
#
# Needs: curl or wget, 7z (7zip / p7zip), sha256sum.
set -eu

URL="https://ftp.epson.com/drivers/V39_EScan_3950_AM.exe"
FW_SHA256="14986e0823fb805f97099605e381686aded9a86f58228c508675b5e42b4a6211"

dest="${XDG_DATA_HOME:-$HOME/.local/share}/sane/epsonv39"
case "${1:-}" in
    --system) dest=/usr/share/sane/epsonv39 ;;
    --dest) dest="${2:?--dest needs a directory}" ;;
    "") ;;
    -h|--help) sed -n '3,15p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
esac

need() { command -v "$1" >/dev/null 2>&1 || { echo "error: '$1' not found. $2" >&2; exit 1; }; }
SEVENZ=$(command -v 7z || command -v 7zz || command -v 7za || true)
[ -n "$SEVENZ" ] || { echo "error: 7z not found. Install 7zip (Fedora/Arch: 7zip or p7zip, Debian/Ubuntu: 7zip or p7zip-full)." >&2; exit 1; }
need sha256sum "Install coreutils."

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

echo "Downloading Epson's V39 driver package (about 30 MB)..."
if command -v curl >/dev/null 2>&1; then
    curl -fL --progress-bar -o "$work/epson.exe" "$URL"
else
    need wget "Install curl or wget."
    wget -q --show-progress -O "$work/epson.exe" "$URL"
fi

echo "Extracting esfw010c.bin..."
"$SEVENZ" e -y -o"$work" "$work/epson.exe" ModUsd.cab >/dev/null
"$SEVENZ" e -y -o"$work" "$work/ModUsd.cab" esfw010c.bin >/dev/null

sum=$(sha256sum "$work/esfw010c.bin" | cut -d' ' -f1)
if [ "$sum" != "$FW_SHA256" ]; then
    echo "error: esfw010c.bin has an unexpected checksum:" >&2
    echo "  got      $sum" >&2
    echo "  expected $FW_SHA256" >&2
    echo "Epson may have changed the package. The backend was tested with the expected file." >&2
    exit 1
fi

install -d "$dest"
install -m 644 "$work/esfw010c.bin" "$dest/esfw010c.bin"
echo "Installed $dest/esfw010c.bin (firmware checksum OK)."
