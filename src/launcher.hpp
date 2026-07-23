#pragma once
// ztpp — src/launcher.hpp: стартовое окно выбора ROM (а-ля rednukem/eduke32 startup window).
// Сканирует каталоги (cwd, родители, каталог бинарника) на ROM'ы Mega Drive семейства
// Zero Tolerance, детектит билд по заголовку, даёт выбрать, помнит последний выбор
// (ztpp_launcher.ini). Headless-проверка: `ztpp --launcherdump --dump out.ppm`.
//
// ⭐ UI-СЛОЙ ДВОЙНОЙ (как в eduke32 — на каждой ОС свой нативный startup-window):
//   • macOS  → НАТИВНОЕ окно Cocoa (реальные виджеты ОС: NSWindow/NSTableView/NSOpenPanel) —
//     launcher::runNative(), реализовано в launcher_osx.mm. CMake добавляет .mm только на APPLE.
//   • прочие → SDL-фолбэк runSdl() ниже (самодельный фреймбуфер + bitmap-шрифт).
//   launcher::run() выбирает нужную реализацию. Data-слой (Entry/scanRoms/probeRom/makeState/ini)
//   общий и вынесен в launcher_data.hpp (без рендера — чтобы ObjC++ не тянул проектный Rect,
//   конфликтующий с легаси-Rect из Cocoa/MacTypes.h).
//
// Пока запускаем только релиз ZT и ZTU; остальные билды (немецкая/прототипы BZT) показываются
// в списке как NOT SUPPORTED YET.

#include "launcher_data.hpp"    // DATA-слой: Entry/scanRoms/probeRom/makeState/State/ini (+ runNative decl)
#include "version.hpp"   // версия в заголовке лаунчера
#include "app_icon.hpp"  // иконка окна + лого в шапке
#include "framebuffer.hpp"      // FB + FBW/FBH (SDL-фолбэк рисует в фреймбуфер)
#include "ui.hpp"               // drawText/drawTextC (до загрузки ROM — public-domain FONT8X8)

namespace launcher {

// ---- Геометрия SDL-списка (VIS_ROWS — в launcher_data.hpp, нужен makeState/clampScroll) ----
inline constexpr int LNC_W = 640, LNC_H = 480;
inline constexpr int LIST_X = 16, LIST_Y = 72, LIST_W = 608, ROW_H = 34;

inline std::string truncLeft(const std::string& s, size_t n) {  // хвост пути виднее начала
    return s.size() <= n ? s : "..." + s.substr(s.size() - (n - 3));
}
inline std::string truncRight(const std::string& s, size_t n) {
    return s.size() <= n ? s : s.substr(0, n - 1) + "~";
}

inline void drawLauncher(FB& fb, const State& st) {
    const uint32_t BG = 0xFF0C0C10, PANEL = 0xFF15151C, SELBG = 0xFF20301E, ACCENT = 0xFF6CD86C,
                   WHITE = 0xFFE8E8E8, GREY = 0xFF8A8A92, DGREY = 0xFF55555C, RED = 0xFFE86050,
                   YELLOW = 0xFFE8D06C;
    fb.clear(BG);
    // Шапка
    fb.rect(0, 0, LNC_W, 56, PANEL);
    fb.rect(0, 56, LNC_W, 2, ACCENT);
    // ⭐ЛОГОТИП (logo_data.hpp, 64×64 → 40×40 nearest) вместо текстового «ZTPP»
    for (int y = 0; y < 40; ++y) for (int x = 0; x < 40; ++x) {
        uint32_t c = ZTPP_ICON64[(y * 64 / 40) * 64 + (x * 64 / 40)];
        if (c >> 24 >= 0x80) fb.put(14 + x, 6 + y, c | 0xFF000000u);
    }
    { char lv[24]; std::snprintf(lv, sizeof lv, "ZTPP V%s", ztppVersion()); drawText(fb, 60, 30, lv, GREY, 1); }
    drawText(fb, 128, 16, "ZERO TOLERANCE PORT", WHITE, 2);
    drawText(fb, 128, 38, "SELECT A ROM TO LAUNCH", GREY, 1);

    // Панель списка
    fb.rect(LIST_X, LIST_Y, LIST_W, VIS_ROWS * ROW_H, PANEL);
    if (st.entries.empty()) {
        drawTextC(fb, LNC_W / 2, LIST_Y + 120, "NO ZERO TOLERANCE ROMS FOUND", YELLOW, 2);
        drawTextC(fb, LNC_W / 2, LIST_Y + 150, "PUT A ROM (.GEN/.BIN/.MD) NEXT TO THE ZTPP BINARY", GREY, 1);
        drawTextC(fb, LNC_W / 2, LIST_Y + 164, "OR DRAG & DROP A ROM FILE INTO THIS WINDOW", GREY, 1);
    }
    for (int r = 0; r < VIS_ROWS; ++r) {
        int idx = st.scroll + r;
        if (idx >= (int)st.entries.size()) break;
        const Entry& e = st.entries[idx];
        int y = LIST_Y + r * ROW_H;
        if (idx == st.sel) {
            fb.rect(LIST_X, y, LIST_W, ROW_H, SELBG);
            fb.rect(LIST_X, y, 4, ROW_H, ACCENT);
        }
        uint32_t nameCol = e.supported ? WHITE : DGREY;
        uint32_t infoCol = e.supported ? ACCENT : GREY;
        drawText(fb, LIST_X + 12, y + 4, truncRight(e.file, 36).c_str(), nameCol, 2);
        char info[96];
        std::snprintf(info, sizeof(info), "%-28s %4.1f MB  %s",
                      buildLabel(e.build), (double)e.size / (1024.0 * 1024.0),
                      launcher::buildStatus(e.build));
        drawText(fb, LIST_X + 12, y + 22, info, infoCol, 1);
    }
    // Скроллбар
    if ((int)st.entries.size() > VIS_ROWS) {
        int barH = VIS_ROWS * ROW_H * VIS_ROWS / (int)st.entries.size();
        if (barH < 16) barH = 16;
        int barY = LIST_Y + (VIS_ROWS * ROW_H - barH) * st.scroll
                   / ((int)st.entries.size() - VIS_ROWS);
        fb.rect(LIST_X + LIST_W - 4, LIST_Y, 4, VIS_ROWS * ROW_H, BG);
        fb.rect(LIST_X + LIST_W - 4, barY, 4, barH, DGREY);
    }

    // Детали выбранного
    int dy = LIST_Y + VIS_ROWS * ROW_H + 8;
    if (st.sel >= 0 && st.sel < (int)st.entries.size()) {
        const Entry& e = st.entries[st.sel];
        char det[128];
        std::snprintf(det, sizeof(det), "TITLE: %s   SERIAL: %s",
                      truncRight(e.title, 40).c_str(), e.serial.c_str());
        drawText(fb, LIST_X, dy, det, GREY, 1);
        drawText(fb, LIST_X, dy + 12, ("PATH: " + truncLeft(e.path, 74)).c_str(), GREY, 1);
    }

    // Подвал: подсказки + мигающее сообщение
    fb.rect(0, LNC_H - 34, LNC_W, 34, PANEL);
    drawTextC(fb, LNC_W / 2, LNC_H - 28, "UP/DOWN: SELECT   ENTER: LAUNCH   ESC: QUIT   DRAG & DROP: ADD ROM", GREY, 1);
    if (st.msgTimer > 0)
        drawTextC(fb, LNC_W / 2, LNC_H - 14, st.msg.c_str(), RED, 1);
}

// ---- Headless-кадр лаунчера в PPM (для отладки без окна: --launcherdump --dump out.ppm) ----
inline bool dumpFrame(const char* outPath) {
    int ow = FBW, oh = FBH;
    FBW = LNC_W; FBH = LNC_H;
    {
        FB fb;
        State st = makeState("");
        drawLauncher(fb, st);
        FILE* f = std::fopen(outPath, "wb");
        if (!f) { FBW = ow; FBH = oh; std::fprintf(stderr, "launcher: cannot write %s\n", outPath); return false; }
        std::fprintf(f, "P6\n%d %d\n255\n", LNC_W, LNC_H);
        for (int i = 0; i < LNC_W * LNC_H; ++i) {
            uint32_t c = fb.px[(size_t)i];
            uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
            std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
        std::printf("launcher: dumped %s\n", outPath);
    }
    FBW = ow; FBH = oh;
    return true;
}

// ---- SDL-фолбэк окна выбора ROM (не-macOS). Возвращает путь или "" (закрыли/ESC = выход) ----
inline std::string runSdl(const std::string& preselect) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "launcher: SDL_Init failed (%s), falling back to default ROM\n", SDL_GetError());
        return preselect;   // без видео окна не будет — тихий fallback на автопоиск
    }
    int ow = FBW, oh = FBH;
    FBW = LNC_W; FBH = LNC_H;
    std::string result;
    {
        char lt[64]; std::snprintf(lt, sizeof lt, "ztpp v%s - select ROM", ztppVersion());
        SDL_Window* win = SDL_CreateWindow(lt,
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, LNC_W, LNC_H, SDL_WINDOW_SHOWN);
        ztppApplyWindowIcon(win);       // ⭐иконка ZTPP
        SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED) : nullptr;
        SDL_Texture* tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                                   SDL_TEXTUREACCESS_STREAMING, LNC_W, LNC_H) : nullptr;
        if (!tex) {
            std::fprintf(stderr, "launcher: window/renderer failed (%s)\n", SDL_GetError());
            if (ren) SDL_DestroyRenderer(ren);
            if (win) SDL_DestroyWindow(win);
            FBW = ow; FBH = oh;
            return preselect;
        }
        FB fb;
        State st = makeState(preselect);
        bool running = true;
        uint32_t lastClickTicks = 0; int lastClickRow = -1;   // распознавание двойного клика
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) running = false;
                else if (ev.type == SDL_DROPFILE) {
                    addDropped(st, ev.drop.file);
                    SDL_free(ev.drop.file);
                }
                else if (ev.type == SDL_KEYDOWN) {
                    switch (ev.key.keysym.scancode) {       // по scancode (не keysym: раскладка)
                        case SDL_SCANCODE_ESCAPE:   running = false; break;
                        case SDL_SCANCODE_UP:       st.sel--; clampScroll(st); break;
                        case SDL_SCANCODE_DOWN:     st.sel++; clampScroll(st); break;
                        case SDL_SCANCODE_PAGEUP:   st.sel -= VIS_ROWS; clampScroll(st); break;
                        case SDL_SCANCODE_PAGEDOWN: st.sel += VIS_ROWS; clampScroll(st); break;
                        case SDL_SCANCODE_HOME:     st.sel = 0; clampScroll(st); break;
                        case SDL_SCANCODE_END:      st.sel = (int)st.entries.size() - 1; clampScroll(st); break;
                        case SDL_SCANCODE_RETURN:
                        case SDL_SCANCODE_KP_ENTER:
                            if (tryLaunch(st, result)) running = false;
                            break;
                        default: break;
                    }
                }
                else if (ev.type == SDL_MOUSEWHEEL) {
                    st.scroll -= ev.wheel.y;
                    int maxScroll = (int)st.entries.size() - VIS_ROWS;
                    if (st.scroll > maxScroll) st.scroll = maxScroll;
                    if (st.scroll < 0) st.scroll = 0;
                }
                else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
                    int mx = ev.button.x, my = ev.button.y;
                    if (mx >= LIST_X && mx < LIST_X + LIST_W && my >= LIST_Y && my < LIST_Y + VIS_ROWS * ROW_H) {
                        int row = st.scroll + (my - LIST_Y) / ROW_H;
                        if (row < (int)st.entries.size()) {
                            uint32_t now = SDL_GetTicks();
                            bool dbl = (row == lastClickRow && row == st.sel && now - lastClickTicks < 400);
                            st.sel = row; clampScroll(st);
                            if (dbl && tryLaunch(st, result)) running = false;
                            lastClickTicks = now; lastClickRow = row;
                        }
                    }
                }
            }
            if (st.msgTimer > 0) st.msgTimer--;
            drawLauncher(fb, st);
            SDL_UpdateTexture(tex, nullptr, fb.px.data(), LNC_W * 4);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, nullptr, nullptr);
            SDL_RenderPresent(ren);
            SDL_Delay(16);
        }
        SDL_DestroyTexture(tex);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
    }
    FBW = ow; FBH = oh;   // main выставит 640*RS заново перед созданием игрового FB
    if (!result.empty()) std::printf("launcher: selected %s\n", result.c_str());
    else                 std::printf("launcher: no ROM selected, exit\n");
    return result;
}

// ---- Точка входа: выбор реализации окна выбора ROM по платформе (ZTPP_HAS_NATIVE_LAUNCHER
//      ставится в launcher_data.hpp: macOS/Cocoa, Windows/Win32, Linux/GTK при наличии) ----
inline std::string run(const std::string& preselect) {
#ifdef ZTPP_HAS_NATIVE_LAUNCHER
    return runNative(preselect);   // реальные виджеты ОС (как per-platform startup-window eduke32)
#else
    return runSdl(preselect);      // фолбэк: самодельное SDL-окно
#endif
}

} // namespace launcher
