#!/usr/bin/env bash
# setup.sh — configure the yos host build for arm64 Apple TV (real device).
#
#   ./build-tools/tvos/setup.sh
#
# Produces build-tvos-arm64/, an out-of-tree meson build that
# cross-compiles libyos.a against the AppleTVOS SDK. The deploy.sh
# script then bolts that lib into a signed .app bundle and installs
# it to a paired Apple TV via `xcrun devicectl`.
#
# Optional deps disabled at meson time (no AppleTVOS SDK packages
# for these):
#   - msgpack-c     → -Dwith_yctl=disabled
#   - libpython3.12 → -Dwith_libpython=disabled
#
# Builds yos as a STATIC LIBRARY, not an executable: tvOS apps must
# enter through UIApplicationMain in the bundle's main(), which
# launcher.m supplies. See -Dbuild_static_lib=true.

set -euo pipefail

cd "$(dirname "$0")/../.."
REPO="$PWD"

SDK="$(xcrun --sdk appletvos --show-sdk-path)"
CLANG="$(xcrun --sdk appletvos --find clang)"
TOOLCHAIN_BIN="$(dirname "$CLANG")"
TOOLCHAIN_AR="$TOOLCHAIN_BIN/ar"
TOOLCHAIN_STRIP="$TOOLCHAIN_BIN/strip"
TOOLCHAIN_RANLIB="$TOOLCHAIN_BIN/ranlib"

BUILD_DIR="$REPO/build-tvos-arm64"
CROSS_TMPL="$REPO/build-tools/tvos/cross-arm64-tvos.txt"
CROSS_OUT="$REPO/build-tools/tvos/cross-arm64-tvos.generated.txt"

sed \
    -e "s|@SDK@|$SDK|g" \
    -e "s|@CLANG@|$CLANG|g" \
    -e "s|@TOOLCHAIN_AR@|$TOOLCHAIN_AR|g" \
    -e "s|@TOOLCHAIN_STRIP@|$TOOLCHAIN_STRIP|g" \
    -e "s|@TOOLCHAIN_RANLIB@|$TOOLCHAIN_RANLIB|g" \
    "$CROSS_TMPL" > "$CROSS_OUT"

echo "[tvos] SDK   : $SDK"
echo "[tvos] clang : $CLANG"
echo "[tvos] cross : $CROSS_OUT"
echo "[tvos] build : $BUILD_DIR"

MESON_FLAGS=(
    --cross-file "$CROSS_OUT"
    -Dwith_yctl=disabled
    -Dwith_libpython=disabled
    -Dbuild_static_lib=true
)

if [ -d "$BUILD_DIR" ]; then
    meson setup --reconfigure "${MESON_FLAGS[@]}" "$BUILD_DIR"
else
    meson setup "${MESON_FLAGS[@]}" "$BUILD_DIR"
fi

echo
echo "[tvos] setup ok. Compile with:"
echo "    meson compile -C $BUILD_DIR src/yos/yos"
echo
echo "[tvos] then deploy with:"
echo "    ./build-tools/tvos/deploy.sh"
