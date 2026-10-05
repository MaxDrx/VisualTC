#!/usr/bin/env bash
# Builds VisualTC's third-party dependencies from pinned upstream tags, for
# Linux machines where neither vcpkg nor the Qt online installer is wanted
# (offline build hosts, containers, distro packaging).
#
#   scripts/build_deps_linux.sh [--headless] [PREFIX]
#
# PREFIX defaults to /opt/visualtc-deps. Afterwards configure VisualTC with
#   cmake --preset linux-local        (expects PREFIX=/opt/visualtc-deps)
# or  -DCMAKE_PREFIX_PATH="PREFIX;PREFIX/qt6".
#
# --headless builds Qt with only the "offscreen" platform plugin (no X11 /
# Wayland / OpenGL development headers needed). Such a Qt can run the test
# suite and the screenshot automation but cannot open a window: use it for CI
# containers, not for end users.
#
# Requirements: git, cmake >= 3.21, ninja, a C++20 compiler, perl, python3,
# and the development packages of zlib, bzip2, liblzma and OpenSSL.
# For a desktop Qt also: libgl1-mesa-dev libxkbcommon-dev libxkbcommon-x11-dev
# libxcb*-dev (see https://doc.qt.io/qt-6/linux-requirements.html).
set -euo pipefail

HEADLESS=0
if [[ "${1:-}" == "--headless" ]]; then
  HEADLESS=1
  shift
fi
PREFIX="${1:-/opt/visualtc-deps}"
WORK="${WORK:-$PWD/visualtc-deps-src}"
JOBS="$(nproc)"

GDCM_TAG=v3.0.24
CATCH2_TAG=v3.7.1
QT_TAG=v6.8.3
ZSTD_TAG=v1.5.7
LIBARCHIVE_TAG=v3.8.7

mkdir -p "$WORK" "$PREFIX"
cd "$WORK"

fetch() {  # fetch <dir> <url> <tag>
  if [[ ! -d "$1" ]]; then
    git clone --depth 1 --branch "$3" "$2" "$1"
  fi
}

fetch gdcm   https://github.com/malaterre/GDCM.git        "$GDCM_TAG"
fetch catch2 https://github.com/catchorg/Catch2.git       "$CATCH2_TAG"
fetch qtbase https://github.com/qt/qtbase.git             "$QT_TAG"
fetch qtsvg  https://github.com/qt/qtsvg.git              "$QT_TAG"
fetch qtimageformats https://github.com/qt/qtimageformats.git "$QT_TAG"
fetch zstd       https://github.com/facebook/zstd.git         "$ZSTD_TAG"
fetch libarchive https://github.com/libarchive/libarchive.git "$LIBARCHIVE_TAG"

echo "=== GDCM ${GDCM_TAG} (static, bundled OpenJPEG / CharLS / IJG / zlib / expat) ==="
cmake -G Ninja -S gdcm -B build-gdcm \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DGDCM_BUILD_SHARED_LIBS=OFF -DGDCM_BUILD_APPLICATIONS=OFF \
  -DGDCM_BUILD_TESTING=OFF -DGDCM_BUILD_EXAMPLES=OFF \
  -DGDCM_BUILD_DOCBOOK_MANPAGES=OFF -DGDCM_DOCUMENTATION=OFF \
  -DGDCM_USE_VTK=OFF -DGDCM_WRAP_PYTHON=OFF
cmake --build build-gdcm -j"$JOBS"
cmake --install build-gdcm

echo "=== zstd ${ZSTD_TAG} + libarchive ${LIBARCHIVE_TAG} (exames compactados) ==="
# Uses the distribution's zlib, bzip2, liblzma and OpenSSL (Ubuntu:
# zlib1g-dev libbz2-dev liblzma-dev libssl-dev).
cmake -G Ninja -S zstd/build/cmake -B build-zstd \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_TESTS=OFF
cmake --build build-zstd -j"$JOBS"
cmake --install build-zstd
cmake -G Ninja -S libarchive -B build-libarchive \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
  -DENABLE_OPENSSL=ON -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF -DENABLE_ZLIB=ON -DENABLE_BZip2=ON \
  -DENABLE_LZMA=ON -DENABLE_ZSTD=ON -DENABLE_LZ4=OFF -DENABLE_LZO=OFF -DENABLE_LIBB2=OFF \
  -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF -DENABLE_PCREPOSIX=OFF -DENABLE_PCRE2POSIX=OFF -DENABLE_ICONV=OFF \
  -DENABLE_ACL=OFF -DENABLE_XATTR=OFF -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF \
  -DENABLE_TEST=OFF -DENABLE_WERROR=OFF
cmake --build build-libarchive -j"$JOBS"
cmake --install build-libarchive

echo "=== Catch2 ${CATCH2_TAG} (tests only) ==="
cmake -G Ninja -S catch2 -B build-catch2 \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DBUILD_TESTING=OFF -DCATCH_INSTALL_DOCS=OFF
cmake --build build-catch2 -j"$JOBS"
cmake --install build-catch2

echo "=== Qt ${QT_TAG}: qtbase ==="
QT_FLAGS=(
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX/qt6"
  -DBUILD_SHARED_LIBS=ON -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF -DQT_BUILD_BENCHMARKS=OFF
  -DFEATURE_sql=OFF -DFEATURE_network=OFF -DFEATURE_printsupport=OFF
  -DINPUT_libjpeg=qt -DINPUT_pcre=qt -DINPUT_harfbuzz=qt
)
if [[ "$HEADLESS" == 1 ]]; then
  QT_FLAGS+=(
    -DINPUT_opengl=no -DFEATURE_opengl=OFF -DFEATURE_xcb=OFF -DFEATURE_dbus=OFF -DFEATURE_glib=OFF
    -DFEATURE_xml=OFF -DFEATURE_icu=OFF -DFEATURE_vulkan=OFF -DFEATURE_eglfs=OFF
    -DFEATURE_libinput=OFF -DFEATURE_evdev=OFF -DQT_QPA_DEFAULT_PLATFORM=offscreen
  )
fi
cmake -G Ninja -S qtbase -B build-qtbase "${QT_FLAGS[@]}"
cmake --build build-qtbase -j"$JOBS"
cmake --install build-qtbase

for module in qtsvg qtimageformats; do
  echo "=== Qt ${QT_TAG}: ${module} ==="
  cmake -G Ninja -S "$module" -B "build-$module" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX/qt6" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX/qt6" -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF
  cmake --build "build-$module" -j"$JOBS"
  cmake --install "build-$module"
done

echo "Dependências instaladas em $PREFIX"
