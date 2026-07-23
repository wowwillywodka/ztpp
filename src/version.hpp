// ztpp — версия проекта. ИСТОЧНИК ИСТИНЫ: CMakeLists.txt `project(ztpp VERSION …)` →
// компил-дефайн ZTPP_VERSION. Фолбэк здесь — на случай сборки вне CMake (не расходиться!).
#pragma once

#ifndef ZTPP_VERSION
#define ZTPP_VERSION "0.8"
#endif

inline const char* ztppVersion() { return ZTPP_VERSION; }
