#!/usr/bin/env bash
# Сборка и упаковка x86_64-релиза ztpp для Intel Mac / Rosetta 2.
# Требуется Intel Homebrew в /usr/local с cmake, pkgconf и sdl2.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

[ "$(uname -s)" = Darwin ] || { echo "этот скрипт предназначен для macOS" >&2; exit 1; }

INTEL_BREW=${INTEL_BREW:-/usr/local/bin/brew}
[ -x "$INTEL_BREW" ] || {
  echo "не найден Intel Homebrew: $INTEL_BREW" >&2
  echo "Установите его в /usr/local или укажите INTEL_BREW=/путь/к/brew." >&2
  exit 1
}

INTEL_PREFIX=$(arch -x86_64 "$INTEL_BREW" --prefix)
[ -x "$INTEL_PREFIX/bin/cmake" ] || {
  echo "в Intel Homebrew не найден cmake (arch -x86_64 $INTEL_BREW install cmake pkgconf sdl2)" >&2
  exit 1
}
[ -x "$INTEL_PREFIX/bin/pkg-config" ] || {
  echo "в Intel Homebrew не найден pkg-config (arch -x86_64 $INTEL_BREW install pkgconf)" >&2
  exit 1
}

intel_env=(
  arch -x86_64 /usr/bin/env
  "PATH=$INTEL_PREFIX/bin:/usr/bin:/bin:/usr/sbin:/sbin"
  "PKG_CONFIG_PATH=$INTEL_PREFIX/lib/pkgconfig:$INTEL_PREFIX/share/pkgconfig"
  "PKG_CONFIG_LIBDIR=$INTEL_PREFIX/lib/pkgconfig:$INTEL_PREFIX/share/pkgconfig"
)
"${intel_env[@]}" pkg-config --exists sdl2 || {
  echo "в Intel Homebrew не найдена SDL2 (arch -x86_64 $INTEL_BREW install sdl2)" >&2
  exit 1
}

BUILD=build-macos-x86_64
OUT="$ROOT/dist/ztpp-macos-x86_64"
"${intel_env[@]}" "$INTEL_PREFIX/bin/cmake" -S . -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DPKG_CONFIG_EXECUTABLE="$INTEL_PREFIX/bin/pkg-config"
"${intel_env[@]}" "$INTEL_PREFIX/bin/cmake" --build "$BUILD" --parallel
exec "$ROOT/tools/package_macos_portable.sh" "$BUILD" "$OUT" x86_64
