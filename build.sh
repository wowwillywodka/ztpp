#!/usr/bin/env bash
# Сборка прототипа ztpp. Требуется: cmake, clang, SDL2 (brew install sdl2).
set -euo pipefail
cd "$(dirname "$0")"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
echo
echo "Готово: ./build/ztpp"
echo "Запуск:  ./build/ztpp \"../Zero Tolerance (USA, Europe) (Rev A).gen\""
echo "Дамп:    ./build/ztpp \"../Zero Tolerance (USA, Europe) (Rev A).gen\" --dump out.ppm --ep 1 --floor 0 --mode 0"
