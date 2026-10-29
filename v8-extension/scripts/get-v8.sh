#!/usr/bin/env bash
set -euo pipefail

# Downloads a PREBUILT V8 monolith (headers + libv8_monolith.a) and lays it out
# at third_party/v8-install. No compilation — just a ~10 MB download.
#
# Source of the prebuilt binaries:
#   https://github.com/kuoruan/libv8/releases   (V8 monolith, Linux/macOS/Win)
#
# Usage:
#   scripts/get-v8.sh
#   cmake -S . -B build -DV8_ROOT="$PWD/third_party/v8-install"
#   cmake --build build
#
# Env: V8_VERSION=<tag> overrides the pinned release.

V8_VERSION="${V8_VERSION:-v13.1.201.22}"
BASE_URL="https://github.com/kuoruan/libv8/releases/download/${V8_VERSION}"

case "$(uname -s)-$(uname -m)" in
  Linux-x86_64)  ARCHIVE="v8_Linux_x64.tar.xz" ;;
  Darwin-arm64)  ARCHIVE="v8_macOS_arm64.tar.xz" ;;
  Darwin-x86_64) ARCHIVE="v8_macOS_x64.tar.xz" ;;
  *)
    echo "unsupported platform: $(uname -s)-$(uname -m)" >&2
    exit 1
    ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL="$ROOT/third_party/v8-install"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "==> Downloading prebuilt V8 ($V8_VERSION, $ARCHIVE)"
curl -fSL -o "$TMP/$ARCHIVE" "$BASE_URL/$ARCHIVE"

echo "==> Extracting to $INSTALL"
rm -rf "$INSTALL"
mkdir -p "$INSTALL/lib"
tar -xf "$TMP/$ARCHIVE" -C "$TMP"
# The archive extracts to ./include and ./libv8_monolith.a (plus gn-args).
mv "$TMP/include" "$INSTALL/include"
mv "$TMP/libv8_monolith.a" "$INSTALL/lib/"
cp "$TMP/gn-args_Linux.txt" "$INSTALL/" 2>/dev/null || true

echo "==> Done. V8 install root: $INSTALL"
echo "    Next:"
echo "      cmake -S . -B build -DV8_ROOT=$INSTALL"
echo "      cmake --build build"
echo "      ./build/v8ext-demo"
