#!/usr/bin/env bash
# Сборка и упаковка нативного релиза ztpp для Apple Silicon.
# Требуется: Xcode Command Line Tools, CMake и SDL2 из Homebrew.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

[ "$(uname -s)" = Darwin ] || { echo "этот скрипт предназначен для macOS" >&2; exit 1; }
[ "$(uname -m)" = arm64 ] || {
  echo "Apple Silicon-сборку нужно запускать нативно на Mac с ARM (не через Rosetta)." >&2
  exit 1
}

command -v cmake >/dev/null || { echo "не найден cmake" >&2; exit 1; }
command -v pkg-config >/dev/null || { echo "не найден pkg-config (brew install pkgconf)" >&2; exit 1; }
pkg-config --exists sdl2 || { echo "не найдена SDL2 (brew install sdl2)" >&2; exit 1; }

BUILD=build-macos-arm64
OUT="$ROOT/dist/ztpp-macos-arm64"

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build "$BUILD" --parallel
exec "$ROOT/tools/package_macos_portable.sh" "$BUILD" "$OUT" arm64
