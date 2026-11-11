#!/bin/bash
# Downloads the dev headers/libs needed to build mpv-compare and extracts them
# into ./sysroot without installing anything system-wide (no root required).
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p debs sysroot linklib

PACKAGES=(
  libmpv-dev
  libx11-dev
  libegl-dev
  libegl1-mesa-dev
  libgles-dev
  libgles2-mesa-dev
  mesa-common-dev
  x11proto-dev
  libgl-dev
)

cd debs
apt-get download "${PACKAGES[@]}"
cd "$ROOT"

for deb in debs/*.deb; do
  dpkg-deb -x "$deb" sysroot
done

# Linker names: the runtime .so.N files exist on the system, the -dev symlinks
# are not installed system-wide, so provide them locally.
LIBDIR=/usr/lib/aarch64-linux-gnu
ln -sf "$LIBDIR/libmpv.so.1" linklib/libmpv.so
ln -sf "$LIBDIR/libEGL.so.1" linklib/libEGL.so
ln -sf "$LIBDIR/libGLESv2.so.2" linklib/libGLESv2.so
ln -sf "$LIBDIR/libX11.so.6" linklib/libX11.so

echo "Dependencies extracted to $ROOT/sysroot and $ROOT/linklib"
