#!/usr/bin/env bash
# Builds duplicates-ui.AppImage from an already-built binary.
# One recipe used by both CI and local runs, so a release can be reproduced by hand.
#
#   usage: packaging/make-appimage.sh <path-to-duplicates-ui> <output-dir>
set -euo pipefail

BIN="${1:?usage: make-appimage.sh <binary> <outdir>}"
OUT="${2:?usage: make-appimage.sh <binary> <outdir>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

APPDIR="$WORK/AppDir"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps"

install -m755 "$BIN" "$APPDIR/usr/bin/duplicates-ui"
install -m755 "$HERE/AppRun" "$APPDIR/AppRun"

# appimagetool wants the desktop file and icon at the AppDir root as well as in
# the usual share/ locations, so desktop integration works once installed.
install -m644 "$HERE/duplicates-ui.desktop" "$APPDIR/duplicates-ui.desktop"
install -m644 "$HERE/duplicates-ui.desktop" "$APPDIR/usr/share/applications/duplicates-ui.desktop"
install -m644 "$HERE/duplicates-ui.png" "$APPDIR/duplicates-ui.png"
install -m644 "$HERE/duplicates-ui.png" \
    "$APPDIR/usr/share/icons/hicolor/256x256/apps/duplicates-ui.png"

TOOL="${APPIMAGETOOL:-$WORK/appimagetool}"
if [ ! -x "$TOOL" ]; then
    curl -fsSL -o "$TOOL" \
        "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"
    chmod +x "$TOOL"
fi

mkdir -p "$OUT"
# --appimage-extract-and-run avoids needing FUSE, which CI runners do not have.
ARCH=x86_64 "$TOOL" --appimage-extract-and-run "$APPDIR" "$OUT/duplicates-ui-x86_64.AppImage"
echo "built $OUT/duplicates-ui-x86_64.AppImage"
