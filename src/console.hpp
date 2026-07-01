#pragma once
// ── ВНУТРИИГРОВАЯ КОНСОЛЬ (GZDoom-style) ──────────────────────────────────────────────────────────
// Открытие по `(grave). Команды (give/spawn/god/…), tab-автодополнение, история ↑/↓, скролл PgUp/PgDn,
// лог-вывод (сообщения игры тоже сюда). Команды РЕГИСТРИРУЮТСЯ из main.cpp лямбдами (захват состояния игры).
// Раскладко-независимо: команды/история — по scancode (onKey), печать символов — по SDL_TEXTINPUT (onText).
#include "ui.hpp"
#include <functional>
#include <vector>
#include <string>
#include <cctype>
#include <SDL.h>

namespace con {

struct Cmd { std::string name, help; std::function<void(const std::vector<std::string>&)> fn; };
inline std::vector<Cmd>&         cmds()    { static std::vector<Cmd> v; return v; }
inline std::vector<std::string>& logBuf()  { static std::vector<std::string> v; return v; }
inline std::vector<std::string>& history() { static std::vector<std::string> v; return v; }
inline bool&        open_()   { static bool b = false; return b; }
inline std::string& line()    { static std::string s; return s; }
inline int&         histPos() { static int p = -1; return p; }   // -1 = редактируемая строка
inline int&         scroll()  { static int s = 0;  return s; }   // прокрутка лога (0 = низ)

inline bool isOpen() { return open_(); }
inline void log(const std::string& m) { auto& L = logBuf(); L.push_back(m); if (L.size() > 800) L.erase(L.begin()); }
inline void toggle() {
    open_() = !open_();
    if (open_()) { histPos() = -1; scroll() = 0; SDL_StartTextInput(); }
    else SDL_StopTextInput();
}
inline void registerCmd(const std::string& n, const std::string& h,
                        std::function<void(const std::vector<std::string>&)> fn) {
    cmds().push_back({n, h, std::move(fn)});
}
inline std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> t; std::string cur;
    for (char c : s) { if (c == ' ' || c == '\t') { if (!cur.empty()) { t.push_back(cur); cur.clear(); } } else cur += c; }
    if (!cur.empty()) t.push_back(cur);
    return t;
}
inline void exec(const std::string& ln) {
    log("] " + ln);
    auto t = tokenize(ln); if (t.empty()) return;
    std::string name = t[0]; for (auto& c : name) c = (char)std::tolower((unsigned char)c);
    if (name == "help") { log("commands:"); for (auto& c : cmds()) log("  " + c.name + " - " + c.help);
                          log("  help  clear"); return; }
    if (name == "clear") { logBuf().clear(); return; }
    std::vector<std::string> args(t.begin() + 1, t.end());
    for (auto& c : cmds()) if (c.name == name) { c.fn(args); return; }
    log("unknown command: " + name + "  (type 'help')");
}
inline void onText(const char* txt) {                 // печать символов (раскладко-зависимо — верно для ввода)
    for (const char* p = txt; *p; ++p) if (*p != '`' && (unsigned char)*p >= 32 && (unsigned char)*p < 127) line() += *p;
}
inline void onKey(SDL_Scancode k, bool /*shift*/) {
    auto& ln = line();
    switch (k) {
        case SDL_SCANCODE_BACKSPACE: if (!ln.empty()) ln.pop_back(); break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER:
            if (!ln.empty()) { history().push_back(ln); exec(ln); ln.clear(); histPos() = -1; scroll() = 0; }
            break;
        case SDL_SCANCODE_TAB: {                       // автодополнение имени команды + общий префикс
            auto t = tokenize(ln);
            if (t.size() <= 1) {
                std::string pre = t.empty() ? "" : t[0]; for (auto& c : pre) c = (char)std::tolower((unsigned char)c);
                std::vector<std::string> m; for (auto& c : cmds()) if (c.name.rfind(pre, 0) == 0) m.push_back(c.name);
                if (m.size() == 1) ln = m[0] + " ";
                else if (m.size() > 1) {
                    std::string row; for (auto& x : m) row += x + " "; log(row);
                    std::string cp = m[0];                 // дополнить до общего префикса
                    for (auto& x : m) { size_t i = 0; while (i < cp.size() && i < x.size() && cp[i] == x[i]) ++i; cp = cp.substr(0, i); }
                    ln = cp;
                }
            }
            break;
        }
        case SDL_SCANCODE_UP: { auto& H = history(); if (!H.empty()) { if (histPos() < 0) histPos() = (int)H.size();
                                if (histPos() > 0) histPos()--; ln = H[histPos()]; } break; }
        case SDL_SCANCODE_DOWN: { auto& H = history(); if (histPos() >= 0) { histPos()++;
                                  if (histPos() >= (int)H.size()) { histPos() = -1; ln.clear(); } else ln = H[histPos()]; } break; }
        case SDL_SCANCODE_PAGEUP:   scroll() += 4; break;
        case SDL_SCANCODE_PAGEDOWN: scroll() -= 4; if (scroll() < 0) scroll() = 0; break;
        default: break;
    }
}
// Отрисовка панели (верхняя ~55% экрана), затемнённый фон + лог + строка ввода. sc = render scale (шрифт).
inline void draw(FB& fb, int W, int H, int sc) {
    if (!open_()) return;
    int ph = (int)(H * 0.55);
    for (int y = 0; y < ph; ++y) for (int x = 0; x < W; ++x) {           // затемнение фона
        uint32_t& p = fb.px[(size_t)y * W + x]; p = (((p >> 1) & 0x007F7F7Fu)) | 0xFF000000u;
    }
    for (int x = 0; x < W; ++x) { fb.put(x, ph - 1, 0xFF40C0FFu); fb.put(x, ph - 2, 0xFF206090u); }
    int lh = 9 * sc;
    int iy = ph - lh - 2 * sc;
    drawText(fb, 4 * sc, iy, ("]" + line() + "_").c_str(), 0xFFE8F4FFu, sc);   // строка ввода
    auto& L = logBuf();                                                        // лог снизу вверх
    int y = iy - lh, idx = (int)L.size() - 1 - scroll();
    while (y > -lh && idx >= 0) { drawText(fb, 4 * sc, y, L[idx].c_str(), 0xFFA8C0D0u, sc); y -= lh; --idx; }
}

}  // namespace con
