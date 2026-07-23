#!/bin/bash
# ztpp — сборка macOS .app-бандла «как в опенсорсе»: вложенный SDL2, .icns из логотипа,
# ad-hoc подпись (без Apple Developer). Запуск:  bash tools/make_app.sh [build-dir] [out-dir]
# Результат: <out>/ztpp.app + <out>/ztpp-<version>-macos.zip
set -euo pipefail
cd "$(dirname "$0")/.."                                   # корень ztpp/

BUILD="${1:-build}"
OUT="${2:-dist}"
BIN="$BUILD/ztpp"
[ -x "$BIN" ] || { echo "нет $BIN — сначала собери (cmake --build $BUILD)"; exit 1; }

VER=$(sed -n 's/^project(ztpp VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$VER" ] || VER=0.0
APP="$OUT/ztpp.app"
echo "== ztpp v$VER -> $APP"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Frameworks"
cp "$BIN" "$APP/Contents/MacOS/ztpp"

# ── Info.plist ──
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>ztpp</string>
  <key>CFBundleDisplayName</key><string>ztpp — Zero Tolerance Port</string>
  <key>CFBundleIdentifier</key><string>com.wowwillywodka.ztpp</string>
  <key>CFBundleVersion</key><string>$VER</string>
  <key>CFBundleShortVersionString</key><string>$VER</string>
  <key>CFBundleExecutable</key><string>ztpp</string>
  <key>CFBundleIconFile</key><string>ztpp</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST

# ── Иконка .icns из логотипа ──
if [ -f "ztpp logo.png" ]; then
  ICONSET=$(mktemp -d)/ztpp.iconset
  mkdir -p "$ICONSET"
  for sz in 16 32 64 128 256 512; do
    sips -z $sz $sz "ztpp logo.png" --out "$ICONSET/icon_${sz}x${sz}.png" >/dev/null
    sips -z $((sz*2)) $((sz*2)) "ztpp logo.png" --out "$ICONSET/icon_${sz}x${sz}@2x.png" >/dev/null
  done
  iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/ztpp.icns"
fi

# ── SDL2 внутрь бандла (@executable_path/../Frameworks) ──
SDL_SRC=$(otool -L "$APP/Contents/MacOS/ztpp" | awk '/libSDL2/{print $1; exit}')
if [ -n "${SDL_SRC:-}" ] && [ -f "$SDL_SRC" ]; then
  SDL_NAME=$(basename "$SDL_SRC")
  cp "$SDL_SRC" "$APP/Contents/Frameworks/$SDL_NAME"
  install_name_tool -change "$SDL_SRC" "@executable_path/../Frameworks/$SDL_NAME" "$APP/Contents/MacOS/ztpp"
  install_name_tool -id "@executable_path/../Frameworks/$SDL_NAME" "$APP/Contents/Frameworks/$SDL_NAME"
  echo "== SDL2 вложен: $SDL_NAME"
else
  echo "!! libSDL2 не найдена в otool -L — бандл будет требовать системный SDL2"
fi

# ── ad-hoc подпись (обязательна для arm64; Gatekeeper всё равно попросит right-click→Open) ──
codesign --force --deep -s - "$APP"
codesign --verify --deep "$APP" && echo "== подпись (ad-hoc) ок"

# ── zip для GitHub Release ──
( cd "$OUT" && rm -f "ztpp-$VER-macos.zip" && zip -qry "ztpp-$VER-macos.zip" ztpp.app )
echo "== готово: $OUT/ztpp-$VER-macos.zip"
echo "   юзеру: распаковать, ПОЛОЖИТЬ ROM РЯДОМ с ztpp.app (или drag&drop в лаунчер),"
echo "   первый запуск: правый клик -> Open (Gatekeeper, неподписанное приложение)."
