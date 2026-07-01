cmake_minimum_required(VERSION 3.16)
project(ztpp CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ZTPP_NO_SDL "Собрать без SDL (только режим --dump)" OFF)

# Nuked-OPN2 (Yamaha YM2612/YM3438) — vendored C-эмулятор чипа (LGPL 2.1).
add_executable(ztpp src/main.cpp src/opn2/ym3438.c)
target_include_directories(ztpp PRIVATE src)

if(ZTPP_NO_SDL)
  target_compile_definitions(ztpp PRIVATE ZTPP_NO_SDL)
else()
  # SDL2 ставится через Homebrew: `brew install sdl2`.
  # Предпочитаем pkg-config: даёт dylib с абсолютным install-name (find_package на macOS
  # цепляет SDL2.framework через @rpath без RPATH -> ошибка загрузки в рантайме).
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
  endif()
  if(TARGET PkgConfig::SDL2)
    target_link_libraries(ztpp PRIVATE PkgConfig::SDL2)
  else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(ztpp PRIVATE SDL2::SDL2)
    # rpath для Homebrew SDL2 на macOS (framework-линковка); на Linux/Windows не нужно
    if(APPLE)
      set_target_properties(ztpp PROPERTIES BUILD_RPATH "/opt/homebrew/lib;/usr/local/lib")
    endif()
  endif()
endif()
