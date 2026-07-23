cmake_minimum_required(VERSION 3.16)
# ⭐ВЕРСИЯ ПРОЕКТА — источник истины (транслируется в код дефайном ZTPP_VERSION; см. src/version.hpp)
project(ztpp VERSION 0.8 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ZTPP_NO_SDL "Собрать без SDL (только режим --dump)" OFF)

# ── Multi-TU: исходники собираются из нескольких единиц трансляции ──────────────
# Раньше проект был unity-build (один main.cpp инклудил все .hpp). Теперь определения
# живут в .cpp (заголовки — только объявления), что даёт РАЗДЕЛЬНУЮ компиляцию и быструю
# инкрементальную пересборку. Слои: src/rom (ROM→ассеты декодер, переносим в новый движок),
# src/render (рендер/рантайм ztpp), src/ (приложение/точка входа).
file(GLOB_RECURSE ZTPP_CXX_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
# Платформенные нативные лаунчеры (.cpp) добавляются УСЛОВНО ниже (Win32/GTK) — исключаем из
# общего glob, иначе Windows/GTK-код собирался бы на всех ОС. (macOS-лаунчер — .mm, в glob не попадает.)
list(REMOVE_ITEM ZTPP_CXX_SOURCES
  ${CMAKE_SOURCE_DIR}/src/launcher_win.cpp
  ${CMAKE_SOURCE_DIR}/src/launcher_gtk.cpp)

# Nuked-OPN2 (Yamaha YM2612/YM3438) — vendored C-эмулятор чипа (LGPL 2.1).
add_executable(ztpp ${ZTPP_CXX_SOURCES} src/opn2/ym3438.c)

# Заголовки модулей резолвятся плоско: `#include "gfx.hpp"` находит src/rom/gfx.hpp и т.д.
# (физический каталог задаёт границу модуля; общий include-путь избавляет от правки всех #include).
target_include_directories(ztpp PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/rom
  ${CMAKE_SOURCE_DIR}/src/render)

# Версия из project(VERSION …) → код (src/version.hpp: ztppVersion())
target_compile_definitions(ztpp PRIVATE ZTPP_VERSION="${PROJECT_VERSION}")

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

# ── Нативный лаунчер выбора ROM на macOS (Cocoa) ───────────────────────────────
# Как в eduke32 (startup-window per-platform): на macOS используем реальные виджеты ОС
# (NSWindow/NSTableView/NSOpenPanel) вместо самодельного SDL-фреймбуфера. launcher::run()
# в launcher.hpp сам выбирает runNative() (этот .mm) на APPLE или runSdl() на прочих ОС.
if(APPLE AND NOT ZTPP_NO_SDL)
  enable_language(OBJCXX)
  target_sources(ztpp PRIVATE ${CMAKE_SOURCE_DIR}/src/launcher_osx.mm)
  set_source_files_properties(${CMAKE_SOURCE_DIR}/src/launcher_osx.mm PROPERTIES
    COMPILE_FLAGS "-fobjc-arc")
  target_link_libraries(ztpp PRIVATE "-framework Cocoa")
endif()

# Windows: нативный Win32-диалог (launcher_win.cpp, по образцу eduke32 startwin.game.c)
# + иконка exe (ztpp.rc → ztpp.ico из логотипа).
if(WIN32 AND NOT ZTPP_NO_SDL)
  enable_language(RC)
  target_sources(ztpp PRIVATE ${CMAKE_SOURCE_DIR}/src/launcher_win.cpp ${CMAKE_SOURCE_DIR}/ztpp.rc)
  target_link_libraries(ztpp PRIVATE comctl32 comdlg32 shell32)
endif()

# Linux: нативный GTK3-лаунчер (launcher_gtk.cpp, по образцу eduke32 startgtk.game.c) — ОПЦИОНАЛЬНО.
# Если gtk+-3.0 не найден, Linux остаётся на SDL-фолбэке. ⚠ best-effort, НЕ проверен сборкой.
if(UNIX AND NOT APPLE AND NOT ZTPP_NO_SDL)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(GTK3 QUIET IMPORTED_TARGET gtk+-3.0)
  endif()
  if(TARGET PkgConfig::GTK3)
    target_sources(ztpp PRIVATE ${CMAKE_SOURCE_DIR}/src/launcher_gtk.cpp)
    target_link_libraries(ztpp PRIVATE PkgConfig::GTK3)
    target_compile_definitions(ztpp PRIVATE ZTPP_GTK_LAUNCHER)  # диспетчер run() выберет runNative
    message(STATUS "ztpp: native GTK3 launcher enabled")
  else()
    message(STATUS "ztpp: gtk+-3.0 not found — Linux launcher uses SDL fallback")
  endif()
endif()
