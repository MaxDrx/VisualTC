#!/usr/bin/env bash
# Builds the Linux packages from a configured and compiled build tree:
#
#   packaging/linux/make_packages.sh BUILD_DIR QMAKE [OUT_DIR]
#
#   BUILD_DIR  e.g. build/linux-release
#   QMAKE      qmake of the Qt used in the build (linuxdeploy-plugin-qt finds
#              the Qt libraries and plugins through it)
#   OUT_DIR    default: dist
#
# Produces, with fixed names (the download page links to them):
#   OUT_DIR/visualtc_amd64.deb        double-click on Ubuntu, Debian, Mint, Pop!_OS…
#   OUT_DIR/VisualTC-x86_64.AppImage  any distribution, no installation
#
# Both carry their own Qt, GDCM and libarchive. The AppImage uses the static
# type2 runtime, so the user's system does not need libfuse2.
#
# Tools (linuxdeploy, its Qt plugin and appimagetool) are downloaded once to
# TOOLS_DIR (default .packaging-tools); set them in advance for offline hosts.
# EXTRA_PLATFORM_PLUGINS ("a.so;b.so") adds Qt platform plugins besides xcb;
# by default only offscreen (for --screenshot automation without a display).
# Wayland desktops run VisualTC through XWayland: shipping the Wayland plugin
# would also require its shell-integration plugins (EXTRA_QT_MODULES=
# waylandcompositor in linuxdeploy-plugin-qt), or windows may never appear.
set -euo pipefail

if [[ $# -lt 2 ]]; then
  sed -n '2,20p' "$0"
  exit 2
fi
BUILD_DIR=$(cd "$1" && pwd)
QMAKE_BIN=$2
OUT_DIR=${3:-dist}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TOOLS_DIR=${TOOLS_DIR:-$PWD/.packaging-tools}
VERSION=$(grep -m1 -oP '^\s+VERSION \K[0-9.]+' "$ROOT/CMakeLists.txt")
MAINTAINER=${VISUALTC_MAINTAINER:-"VisualTC Maintainers <maintainers@example.invalid>"}
# The packaging tools are AppImages themselves: run them without FUSE (CI
# containers, build hosts without libfuse2).
export APPIMAGE_EXTRACT_AND_RUN=1

mkdir -p "$TOOLS_DIR" "$OUT_DIR"
OUT_DIR=$(cd "$OUT_DIR" && pwd)
fetch_tool() {  # fetch_tool <file> <url>
  if [[ ! -x "$TOOLS_DIR/$1" ]]; then
    curl -fsSL --retry 3 -o "$TOOLS_DIR/$1" "$2"
    chmod +x "$TOOLS_DIR/$1"
  fi
}
fetch_tool linuxdeploy-x86_64.AppImage \
  https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
fetch_tool linuxdeploy-plugin-qt-x86_64.AppImage \
  https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
fetch_tool appimagetool-x86_64.AppImage \
  https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
export PATH="$TOOLS_DIR:$PATH"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
APPDIR="$WORK/AppDir"

echo "=== AppDir (VisualTC ${VERSION}) ==="
cmake --install "$BUILD_DIR" --prefix "$APPDIR/usr"
export QMAKE="$QMAKE_BIN"
# linuxdeploy resolves the Qt libraries through the loader search path.
export LD_LIBRARY_PATH="$("$QMAKE_BIN" -query QT_INSTALL_LIBS)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [[ -z "${EXTRA_PLATFORM_PLUGINS:-}" ]]; then
  QT_PLUGINS=$("$QMAKE_BIN" -query QT_INSTALL_PLUGINS)
  extra=()
  for plugin in libqoffscreen.so; do
    [[ -f "$QT_PLUGINS/platforms/$plugin" ]] && extra+=("$plugin")
  done
  EXTRA_PLATFORM_PLUGINS=$(IFS=';'; echo "${extra[*]}")
  export EXTRA_PLATFORM_PLUGINS
fi
linuxdeploy-x86_64.AppImage --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/VisualTC" --executable "$APPDIR/usr/bin/visualtc-worker" \
  --desktop-file "$APPDIR/usr/share/applications/visualtc.desktop" \
  --icon-file "$ROOT/packaging/linux/visualtc.png" \
  --plugin qt

echo "=== AppImage ==="
ARCH=x86_64 VERSION="$VERSION" appimagetool-x86_64.AppImage --no-appstream "$APPDIR" \
  "$OUT_DIR/VisualTC-x86_64.AppImage"
chmod +x "$OUT_DIR/VisualTC-x86_64.AppImage"

echo "=== .deb ==="
# Same self-contained tree under /opt/visualtc (the Qt of the LTS
# distributions is older than the one VisualTC needs), plus the menu entry,
# the icon and a "visualtc" command.
DEB="$WORK/deb"
mkdir -p "$DEB/DEBIAN" "$DEB/opt/visualtc" "$DEB/usr/bin" "$DEB/usr/share/applications" \
         "$DEB/usr/share/icons/hicolor/scalable/apps" "$DEB/usr/share/icons/hicolor/256x256/apps" \
         "$DEB/usr/share/doc/visualtc"
cp -a "$APPDIR/usr/." "$DEB/opt/visualtc/"
rm -rf "$DEB/opt/visualtc/share/applications" "$DEB/opt/visualtc/share/icons"
ln -s /opt/visualtc/bin/VisualTC "$DEB/usr/bin/visualtc"
sed 's|^Exec=.*|Exec=/opt/visualtc/bin/VisualTC %F|' "$ROOT/packaging/linux/visualtc.desktop" \
  > "$DEB/usr/share/applications/visualtc.desktop"
cp "$ROOT/resources/icons/app.svg" "$DEB/usr/share/icons/hicolor/scalable/apps/visualtc.svg"
cp "$ROOT/packaging/linux/visualtc.png" "$DEB/usr/share/icons/hicolor/256x256/apps/visualtc.png"
cp "$ROOT/THIRD_PARTY_LICENSES.md" "$DEB/usr/share/doc/visualtc/"
INSTALLED_KB=$(du -sk "$DEB" | cut -f1)
cat > "$DEB/DEBIAN/control" <<EOF
Package: visualtc
Version: ${VERSION}
Architecture: amd64
Maintainer: ${MAINTAINER}
Installed-Size: ${INSTALLED_KB}
Depends: libc6 (>= 2.35), libgl1, libegl1, libfontconfig1, libfreetype6, libxkbcommon0, libxkbcommon-x11-0, libxcb-cursor0, libxcb-icccm4, libxcb-image0, libxcb-keysyms1, libxcb-randr0, libxcb-render-util0, libxcb-shape0, libxcb-xinerama0, libdbus-1-3
Section: science
Priority: optional
Description: VisualTC - visualizador de imagens médicas DICOM
 Visualizador DICOM offline para tomografia, ressonância e outras
 modalidades: abre CDs, pastas e exames compactados (ZIP, RAR, 7z),
 com ordenação espacial, janela/nível, medidas, ROI em HU e MPR.
EOF
dpkg-deb --root-owner-group -Zxz --build "$DEB" "$OUT_DIR/visualtc_amd64.deb" >/dev/null
dpkg-deb --info "$OUT_DIR/visualtc_amd64.deb" | sed -n '1,12p'

ls -la "$OUT_DIR"
