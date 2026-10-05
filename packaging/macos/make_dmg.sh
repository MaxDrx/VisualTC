#!/usr/bin/env bash
# Builds the macOS disk image (runs on macOS):
#
#   packaging/macos/make_dmg.sh OUT.dmg APP [APP2]
#
#   APP, APP2  VisualTC.app already processed by macdeployqt. With two bundles
#              (one built on Apple Silicon, one on Intel) the program files
#              are merged with lipo into a single universal app, which runs
#              natively on both kinds of Mac. Qt's frameworks and plugins are
#              already universal and are kept as they are.
#
# The image opens a window with the app, an arrow and the Applications
# folder ("Arraste o VisualTC para a pasta Aplicativos"). Requires dmgbuild
# (pip install dmgbuild).
#
# Signing, all optional (environment variables):
#   MACOS_SIGN_IDENTITY  "Developer ID Application: Nome (TEAMID)" from a
#                        keychain; without it the app gets an ad-hoc signature
#                        and macOS asks the user to confirm the first opening.
#   APPLE_ID, APPLE_TEAM_ID, APPLE_APP_PASSWORD
#                        notarization with Apple (needs MACOS_SIGN_IDENTITY);
#                        a notarized image opens with a plain double-click.
set -euo pipefail

if [[ $# -lt 2 ]]; then
  sed -n '2,22p' "$0"
  exit 2
fi
OUT=$1
APP=$2
APP2=${3:-}
HERE=$(cd "$(dirname "$0")" && pwd)
IDENTITY=${MACOS_SIGN_IDENTITY:-}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
UNI="$WORK/VisualTC.app"
ditto "$APP" "$UNI"

is_macho() { file -b "$1" | grep -q 'Mach-O'; }
has_both() { local a; a=$(lipo -archs "$1" 2>/dev/null || true); [[ "$a" == *arm64* && "$a" == *x86_64* ]]; }

if [[ -n "$APP2" ]]; then
  echo "=== Universal app (arm64 + x86_64) ==="
  merged=0
  while IFS= read -r -d '' f; do
    rel=${f#"$UNI"/}
    other="$APP2/$rel"
    if [[ -L "$f" || ! -f "$other" ]] || ! is_macho "$f" || has_both "$f"; then
      continue
    fi
    if has_both "$other"; then
      cp -f "$other" "$f"
    elif [[ "$(lipo -archs "$f")" == "$(lipo -archs "$other")" ]]; then
      echo "ERRO: $rel tem a mesma arquitetura nos dois pacotes ($(lipo -archs "$f"))" >&2
      exit 1
    else
      lipo -create "$f" "$other" -output "$f.universal"
      mv -f "$f.universal" "$f"
      merged=$((merged + 1))
    fi
  done < <(find "$UNI" -type f -print0)
  echo "$merged arquivos combinados com lipo"
fi

# Every executable file must run on both kinds of Mac (a single thin
# plugin would make that feature fail on the other architecture).
thin=0
while IFS= read -r -d '' f; do
  if [[ ! -L "$f" ]] && is_macho "$f" && ! has_both "$f"; then
    echo "  só $(lipo -archs "$f"): ${f#"$UNI"/}"
    thin=$((thin + 1))
  fi
done < <(find "$UNI" -type f -print0)
if [[ -n "$APP2" && $thin -gt 0 ]]; then
  echo "ERRO: $thin arquivos não são universais" >&2
  exit 1
fi
lipo -archs "$UNI/Contents/MacOS/VisualTC"
lipo -archs "$UNI/Contents/MacOS/visualtc-worker"

echo "=== Signature (${IDENTITY:-ad-hoc}) ==="
# Inside out: libraries and plugins, frameworks, the worker, then the app.
SIGN=(codesign --force --sign "${IDENTITY:--}")
if [[ -n "$IDENTITY" ]]; then
  SIGN+=(--options runtime --timestamp)
fi
find "$UNI/Contents" -type f \( -name '*.dylib' -o -name '*.so' \) -print0 | xargs -0 -n 20 "${SIGN[@]}"
if [[ -d "$UNI/Contents/Frameworks" ]]; then
  find "$UNI/Contents/Frameworks" -maxdepth 1 -name '*.framework' -print0 | xargs -0 -n 1 "${SIGN[@]}"
fi
"${SIGN[@]}" "$UNI/Contents/MacOS/visualtc-worker"
"${SIGN[@]}" "$UNI"
codesign --verify --deep --strict --verbose=2 "$UNI"

echo "=== Disk image ==="
tiffutil -cathidpicheck "$HERE/dmg-background.png" "$HERE/dmg-background@2x.png" -out "$WORK/background.tiff"
rm -f "$OUT"
dmgbuild -s "$HERE/dmg_settings.py" -D app="$UNI" -D background="$WORK/background.tiff" \
  -D icon="$HERE/VisualTC.icns" "VisualTC" "$OUT"

if [[ -n "$IDENTITY" ]]; then
  codesign --force --sign "$IDENTITY" --timestamp "$OUT"
  if [[ -n "${APPLE_ID:-}" && -n "${APPLE_TEAM_ID:-}" && -n "${APPLE_APP_PASSWORD:-}" ]]; then
    echo "=== Notarization ==="
    xcrun notarytool submit "$OUT" --apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" \
      --password "$APPLE_APP_PASSWORD" --wait
    xcrun stapler staple "$OUT"
    spctl --assess --type open --context context:primary-signature --verbose=2 "$OUT"
  fi
fi
hdiutil verify "$OUT"
ls -la "$OUT"
