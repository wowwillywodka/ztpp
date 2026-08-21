#!/usr/bin/env bash
# Упаковка обычного macOS-бинарника (не .app) с SDL2 рядом с ним.
# Использование: tools/package_macos_portable.sh <build-dir> <output-dir> <arch>
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:?"нужен каталог сборки"}
OUT=${2:?"нужен каталог результата"}
ARCH=${3:?"нужна архитектура (arm64 или x86_64)"}

case "$BUILD" in
  /*) ;;
  *) BUILD="$ROOT/$BUILD" ;;
esac

case "$ARCH" in
  arm64|x86_64) ;;
  *) echo "неподдерживаемая архитектура: $ARCH" >&2; exit 2 ;;
esac

case "$OUT" in
  "$ROOT"/dist/*) ;;
  *) echo "каталог результата должен находиться внутри $ROOT/dist" >&2; exit 2 ;;
esac

BIN="$BUILD/ztpp"
[ -x "$BIN" ] || { echo "нет исполняемого файла: $BIN" >&2; exit 1; }

VER=$(sed -n 's/^project(ztpp VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")
[ -n "$VER" ] || { echo "не удалось определить версию из CMakeLists.txt" >&2; exit 1; }

BIN_ARCHS=$(lipo -archs "$BIN")
case " $BIN_ARCHS " in
  *" $ARCH "*) ;;
  *) echo "ztpp собран не для $ARCH (архитектуры: $BIN_ARCHS)" >&2; exit 1 ;;
esac

# В актуальных Homebrew-сборках путь может указывать как на sdl2, так и на
# sdl2-compat. Берём именно install-name, записанный линкером в ztpp.
SDL_SRC=$(otool -L "$BIN" | awk '$1 ~ /libSDL2.*\.dylib$/ { print $1; exit }')
[ -n "${SDL_SRC:-}" ] || { echo "SDL2 не найдена среди зависимостей $BIN" >&2; exit 1; }
[ -f "$SDL_SRC" ] || { echo "SDL2 по пути $SDL_SRC недоступна" >&2; exit 1; }

SDL_NAME=$(basename "$SDL_SRC")
ARCHIVE="$ROOT/dist/ztpp-$VER-macos-$ARCH.zip"

# Начиная с Homebrew SDL2 = sdl2-compat. Это прослойка, которая загружает
# SDL3 во время выполнения, а не через обычную запись в `otool -L`. Её rpath
# ведёт в Homebrew Cellar, поэтому в переносимом архиве SDL3 нужно положить
# рядом с SDL2 и переназначить этот rpath на @loader_path.
SDL3_RPATH=$(otool -l "$SDL_SRC" | awk '
  $1 == "cmd" && $2 == "LC_RPATH" { want_path = 1; next }
  want_path && $1 == "path" {
    if ($2 ~ /\/sdl3\/lib$/) { print $2; exit }
    want_path = 0
  }
')
SDL3_SRC=
SDL3_NAME=libSDL3.dylib
if [ -n "${SDL3_RPATH:-}" ]; then
  case "$SDL3_RPATH" in
    @loader_path/*)
      SDL_REAL_DIR=$(cd "$(dirname "$SDL_SRC")" && pwd -P)
      SDL3_DIR=$(cd "$SDL_REAL_DIR/${SDL3_RPATH#@loader_path/}" && pwd -P)
      ;;
    /*) SDL3_DIR=$SDL3_RPATH ;;
    *) echo "неподдерживаемый rpath SDL3: $SDL3_RPATH" >&2; exit 1 ;;
  esac
  SDL3_SRC="$SDL3_DIR/$SDL3_NAME"
  [ -f "$SDL3_SRC" ] || { echo "SDL3 по пути $SDL3_SRC недоступна" >&2; exit 1; }
fi

mkdir -p "$OUT"
rm -f "$OUT/ztpp" "$OUT/$SDL_NAME" "$OUT/$SDL3_NAME" "$ARCHIVE"
cp "$BIN" "$OUT/ztpp"
# -L важен: Homebrew часто предоставляет libSDL2 через символическую ссылку.
cp -L "$SDL_SRC" "$OUT/$SDL_NAME"

SDL_ARCHS=$(lipo -archs "$OUT/$SDL_NAME")
case " $SDL_ARCHS " in
  *" $ARCH "*) ;;
  *) echo "SDL2 не содержит $ARCH (архитектуры: $SDL_ARCHS)" >&2; exit 1 ;;
esac

# Убираем абсолютный путь Homebrew: dyld найдёт библиотеку рядом с ztpp.
install_name_tool -change "$SDL_SRC" "@executable_path/$SDL_NAME" "$OUT/ztpp"
install_name_tool -id "@executable_path/$SDL_NAME" "$OUT/$SDL_NAME"

if [ -n "$SDL3_SRC" ]; then
  cp -L "$SDL3_SRC" "$OUT/$SDL3_NAME"
  SDL3_ARCHS=$(lipo -archs "$OUT/$SDL3_NAME")
  case " $SDL3_ARCHS " in
    *" $ARCH "*) ;;
    *) echo "SDL3 не содержит $ARCH (архитектуры: $SDL3_ARCHS)" >&2; exit 1 ;;
  esac
  install_name_tool -rpath "$SDL3_RPATH" "@loader_path" "$OUT/$SDL_NAME"
  install_name_tool -id "@loader_path/$SDL3_NAME" "$OUT/$SDL3_NAME"
fi

if ! otool -L "$OUT/ztpp" | awk '{print $1}' | grep -qx "@executable_path/$SDL_NAME"; then
  echo "не удалось заменить путь SDL2 на относительный" >&2
  exit 1
fi

# После install_name_tool прежняя ad-hoc подпись недействительна. Подписываем
# библиотеку и бинарник заново; Apple Developer certificate для этого не нужен.
if [ -n "$SDL3_SRC" ]; then
  codesign --force --sign - "$OUT/$SDL3_NAME"
fi
codesign --force --sign - "$OUT/$SDL_NAME"
codesign --force --sign - "$OUT/ztpp"
codesign --verify --verbose=1 "$OUT/ztpp"

# Не архивируем каталог целиком: в нём пользователь может держать ROM для
# локальной проверки, а ROM'ы не должны случайно попасть в GitHub Release.
(
  cd "$ROOT/dist"
  if [ -n "$SDL3_SRC" ]; then
    zip -q "$(basename "$ARCHIVE")" "$(basename "$OUT")/ztpp" \
      "$(basename "$OUT")/$SDL_NAME" "$(basename "$OUT")/$SDL3_NAME"
  else
    zip -q "$(basename "$ARCHIVE")" "$(basename "$OUT")/ztpp" \
      "$(basename "$OUT")/$SDL_NAME"
  fi
)
echo "== готово: $ARCHIVE"
if [ -n "$SDL3_SRC" ]; then
  echo "   внутри: $(basename "$OUT")/ztpp + $SDL_NAME + $SDL3_NAME"
else
  echo "   внутри: $(basename "$OUT")/ztpp + $SDL_NAME"
fi
