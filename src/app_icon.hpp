// ztpp — src/app_icon.hpp: установка иконки приложения из встроенного логотипа (logo_data.hpp).
// SDL_SetWindowIcon — Win/Linux (на macOS no-op); Dock-иконка macOS — через ztppSetDockIconMac (launcher_osx.mm).
#pragma once
#ifndef ZTPP_NO_SDL
#include <SDL.h>
#include "logo_data.hpp"

#ifdef __APPLE__
void ztppSetDockIconMac(const void* png, unsigned len);   // launcher_osx.mm (ObjC++)
#endif

inline void ztppApplyWindowIcon(SDL_Window* win) {
    SDL_Surface* ic = SDL_CreateRGBSurfaceWithFormatFrom((void*)ZTPP_ICON64, 64, 64, 32, 64 * 4,
                                                         SDL_PIXELFORMAT_ARGB8888);
    if (ic) { SDL_SetWindowIcon(win, ic); SDL_FreeSurface(ic); }
#ifdef __APPLE__
    ztppSetDockIconMac(ZTPP_LOGO_PNG, ZTPP_LOGO_PNG_LEN);
#endif
}
#endif
