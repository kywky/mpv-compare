#!/bin/bash
# Builds an arm64 .deb for mpv-compare. Run on the target after
# scripts/fetch-deps.sh (or with the dev packages installed).
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

VERSION="${VERSION:-0.1.0}"
ARCH="$(dpkg --print-architecture)"
PKG="mpv-compare"
STAGE="$ROOT/build/${PKG}_${VERSION}_${ARCH}"
OUT="$ROOT/dist"

make -j"$(nproc)"

rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN" "$STAGE/usr/bin" "$STAGE/usr/share/doc/$PKG"

install -m755 "$ROOT/mpv-compare" "$STAGE/usr/bin/$PKG"

install -m644 README.md WORKLOG.md "$STAGE/usr/share/doc/$PKG/"

SIZE_KB="$(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)"
cat > "$STAGE/DEBIAN/control" <<CONTROL
Package: $PKG
Version: $VERSION
Architecture: $ARCH
Maintainer: Kaiyu Wang <1151865094@qq.com>
Installed-Size: $SIZE_KB
Depends: libc6 (>= 2.31), libmpv1, libegl1, libgles2, libx11-6
Recommends: fonts-noto-cjk
Section: video
Priority: optional
Homepage: https://github.com/kywky/mpv-compare
Description: dual-video comparison player with draggable wipe
 mpv-compare plays two videos side by side in a single window and lets you
 drag a vertical wipe line to compare them. libmpv handles decoding and
 rendering with hardware acceleration when available, and the two frames are
 composited with a small OpenGL ES shader.
 .
 Features: progress bar, file names, seek, speed, loop, frame stepping,
 screenshots, fullscreen, and automatic A/V position resync.
CONTROL

mkdir -p "$OUT"
dpkg-deb --root-owner-group -Zxz --build "$STAGE" "$OUT/${PKG}_${VERSION}_${ARCH}.deb"
echo "built $OUT/${PKG}_${VERSION}_${ARCH}.deb"
