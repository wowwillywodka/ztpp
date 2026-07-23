#pragma once
// ztpp — src/password_entry.hpp: АУТЕНТИЧНЫЙ экран ввода пароля Zero Tolerance (ROM sub_058054).
//   Раскладка ПО ДИЗАСМУ (VRAM-порядок 57d2e): ТИТУЛ ($e080) → СЕТКА 8×8 ($e200..$e900) → строка действий
//   DEL/CANCEL/DONE ($ea00) → ПОЛЕ ВВОДА снизу ($eb80). Сетка = display-строки @0x584E1 (64 симв, без I/l/0/1).
//   Курсор = СОБСТВЕННЫЙ маркер (ROM 0x581BA: 4-угловая спрайт-рамка вокруг ЯЧЕЙКИ, тайлы c243/ca43/d243/da43).
//   Неверный пароль → «INCORRECT PASSWORD» на месте поля ввода (ROM 0x58496, ~0x28 кадров), буфер НЕ очищается.
//   ⭐QoL сверх оригинала: ввод С КЛАВИАТУРЫ и КЛИК МЫШЬЮ по ячейкам. Валидация/применение — main (applyPassword).
// Подключается ПОСЛЕ ui.hpp (drawTextBig/menu::K/OY/RY), framebuffer.hpp.
#include "framebuffer.hpp"
#include "ui.hpp"
#include "rom/gamedata.hpp"   // SelFont (строчные буквы — пароль регистрозависим)
#include <string>
#include <cstddef>

namespace pwscr {

// Визуальная сетка (ROM display-строки @0x584E1+): 8 строк × 8 столбцов. Пропущены неоднозначные I/l/0/1.
inline const char GRID[8][8] = {
    {'A','B','C','D','E','F','G','H'},
    {'J','K','L','M','N','O','P','Q'},
    {'R','S','T','U','V','W','X','Y'},
    {'Z','a','b','c','d','e','f','g'},
    {'h','i','j','k','m','n','o','p'},
    {'q','r','s','t','u','v','w','x'},
    {'y','z','2','3','4','5','6','7'},
    {'8','9','?',')','!','/','-','*'},
};
inline constexpr int PW_LEN = 9;                      // длина пароля (ROM: 9 символов)

inline bool isPwChar(char c) {                        // символ входит в 64-символьный алфавит пароля?
    for (auto& row : GRID) for (char g : row) if (g == c) return true;
    return false;
}

enum Act { A_NONE = 0, A_TYPED, A_DELETED, A_SUBMIT, A_CANCEL };

struct State {
    bool active = false;
    int  cx = 0, cy = 0;         // курсор: cx 0..7 (столбец), cy 0..8 (8 = строка действий DEL/CANCEL/DONE)
    std::string buf;             // введённые символы (макс PW_LEN)
    std::string msg;             // сообщение на месте поля ввода msgT кадров (ROM 0x58496 ~0x28 кадров)
    int  msgT = 0;               // таймер сообщения
    bool msgOk = false;          // true = принят (зелёный), false = ошибка (красный)
    bool doneClose = false;      // закрыть экран после того как сообщение о принятии погаснет

    void open()  { active = true; cx = cy = 0; buf.clear(); msg.clear(); msgT = 0; doneClose = false; }
    void close() { active = false; }
    void fail()  { msg = "INCORRECT PASSWORD"; msgT = 90; msgOk = false; }   // ⭐буфер НЕ очищается (ROM: reprint)
    void ok()    { msg = "PASSWORD ACCEPTED";  msgT = 90; msgOk = true; doneClose = true; }

    char charAt() const { return (cy >= 0 && cy < 8) ? GRID[cy][cx] : 0; }
    int  actZone() const { return cx < 3 ? 0 : (cx < 5 ? 1 : 2); }   // 0=DEL 1=CANCEL 2=DONE

    void move(int dx, int dy) {
        if (dx) cx = (cx + dx + 8) & 7;
        if (dy) { cy += dy; if (cy < 0) cy = 8; else if (cy > 8) cy = 0; }
    }
    void append(char c) { if (isPwChar(c) && (int)buf.size() < PW_LEN) buf += c; }
    bool backspace()    { if (buf.empty()) return false; buf.pop_back(); return true; }

    Act activate() {   // подтверждение текущей ячейки (Enter/Space/клик по сетке)
        if (cy < 8) { int n0 = (int)buf.size(); append(charAt()); return (int)buf.size() > n0 ? A_TYPED : A_NONE; }
        switch (actZone()) { case 0: return backspace() ? A_DELETED : A_NONE;   // DEL
                             case 1: return A_CANCEL;                            // CANCEL
                             default: return A_SUBMIT; }                         // DONE
    }
    Act typeChar(char c) { int n0 = (int)buf.size(); append(c); return (int)buf.size() > n0 ? A_TYPED : A_NONE; }
    Act keyBackspace()   { return backspace() ? A_DELETED : A_NONE; }
};

inline State& st() { static State s; return s; }

// ── Единая ГЕОМЕТРИЯ (общая для рендера и мыши) — fb-координаты, база 448-региона (menu::RY). ──
struct Layout { int k, cellW, cellH, gridL, gridT, actT, bufT; };
inline Layout layout() {
    using namespace menu;
    // ⚠КООРДИНАТЫ ОТ 0 (как экран выбора): g_view экрана = (0,0,640,448·K); OY-центрированный RY уводил низ (строку
    //   действий) за 448 → «нет кнопок DONE/DEL/CANCEL». Здесь y·K от верха окна.
    Layout L; L.k = K();
    L.cellW = 30 * L.k; L.cellH = 30 * L.k;                 // шаг ячейки
    L.gridL = FBW / 2 - 8 * L.cellW / 2;
    L.bufT  = 44 * L.k;                                     // ⭐ПОЛЕ ВВОДА — под заголовком (видно, что набираешь)
    L.gridT = 80 * L.k;
    L.actT  = L.gridT + 8 * L.cellH + 6 * L.k;              // строка действий под сеткой (в пределах 448)
    return L;
}
// центры зон строки действий (DEL/CANCEL/DONE): 1/8, 1/2, 7/8 ширины сетки (совпадает с hit-зонами)
inline int actCenter(const Layout& L, int z) { int W = 8 * L.cellW; const int c[3] = {L.gridL + W/8, L.gridL + W/2, L.gridL + 7*W/8}; return c[z]; }
// клетка-цель под курсором (для рамки/мыши): grid cy<8, либо зона действий (cy==8)
inline Rect cellRect(const Layout& L, int cx, int cy) {
    if (cy < 8) return { L.gridL + cx * L.cellW, L.gridT + cy * L.cellH, L.cellW, L.cellH };
    const int zx0[3] = { L.gridL, L.gridL + 3*L.cellW, L.gridL + 5*L.cellW };
    const int zw [3] = { 3*L.cellW, 2*L.cellW, 3*L.cellW };
    int z = cx < 3 ? 0 : (cx < 5 ? 1 : 2);
    return { zx0[z], L.actT, zw[z], L.cellH };
}

// Мышь: координата fb → (cx,cy). Возвращает true и ставит курсор s.cx/s.cy на попавшую ячейку/зону.
inline bool hitTest(int mx, int my) {
    Layout L = layout(); State& s = st();
    if (my >= L.gridT && my < L.gridT + 8 * L.cellH) {           // сетка
        int cy = (my - L.gridT) / L.cellH, cx = (mx - L.gridL) / L.cellW;
        if (cx >= 0 && cx < 8 && cy >= 0 && cy < 8) { s.cx = cx; s.cy = cy; return true; }
    }
    if (my >= L.actT && my < L.actT + L.cellH) {                 // строка действий
        int z = (mx < L.gridL + 3*L.cellW) ? 0 : (mx < L.gridL + 5*L.cellW) ? 1 : 2;
        if (mx >= L.gridL && mx < L.gridL + 8*L.cellW) { s.cx = (z == 0 ? 1 : z == 1 ? 4 : 6); s.cy = 8; return true; }
    }
    return false;
}

// Отрисовка поверх чёрного фона. fnt = selFont (регистро-корректный); фолбэк — Font_grph.
inline void render(FB& fb, const SelFont& fnt) {
    using namespace menu;
    Layout L = layout(); int k = L.k;
    fb.clear(0xFF000000u);
    State& s = st();

    auto ch1 = [&](int x, int y, char c, int sc) {              // один символ selFont через палитру опций
        unsigned uc = (unsigned char)c;
        if (fnt.have && uc >= 0x20 && uc <= 0x7f) {
            int e = (int)uc - 0x20;
            for (int ry = 0; ry < 16; ++ry) for (int rx = 0; rx < 8; ++rx) {
                uint8_t idx = fnt.idx[e][ry * 8 + rx]; if (!idx) continue;
                uint32_t col = g_uiOptPal ? g_uiOptPal[idx] : fnt.glyph[e][ry * 8 + rx];
                for (int sy = 0; sy < sc; ++sy) for (int sx = 0; sx < sc; ++sx) fb.put(x + rx * sc + sx, y + ry * sc + sy, col);
            }
        } else { char s2[2] = {c, 0}; drawTextBig(fb, x, y, s2, 0, sc, false, g_uiOptPal); }
    };
    auto textC = [&](int cxp, int y, const char* str, int sc) {
        int w = 0; for (const char* p = str; *p; ++p) ++w;
        int x = cxp - w * 8 * sc / 2;
        for (const char* p = str; *p; ++p) { ch1(x, y, *p, sc); x += 8 * sc; }
    };

    textC(FBW / 2, 14*k, "ENTER PASSWORD", 2*k);

    // ⭐ПОЛЕ ВВОДА (вверху, видно набор): 9 слотов, введённое + '_'-плейсхолдеры. Сообщение (ROM 0x58496) на этом же
    //   месте — ТЕМ ЖЕ цветом, что буквы набора (без раскраски принят/ошибка, по просьбе юзера).
    if (s.msgT > 0 && !s.msg.empty())
        textC(FBW / 2, L.bufT, s.msg.c_str(), 2*k);
    else {
        std::string disp = s.buf; while ((int)disp.size() < PW_LEN) disp += '_';
        textC(FBW / 2, L.bufT, disp.c_str(), 2*k);
    }

    char cell[2] = {0, 0};                                       // СЕТКА 8×8
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c) {
            cell[0] = GRID[r][c];
            textC(L.gridL + c * L.cellW + L.cellW / 2, L.gridT + r * L.cellH + 6*k, cell, 2*k);
        }

    static const char* ACT[3] = {"DEL", "CANCEL", "DONE"};       // строка действий
    for (int a = 0; a < 3; ++a) textC(actCenter(L, a), L.actT + 6*k, ACT[a], 2*k);

    // ⭐МАРКЕР — РЕАЛЬНАЯ ГРАФИКА ROM (тайл 0x243 @gd.pwCursor, угол ┌): 4 зеркальные копии по углам ячейки.
    Rect cr = cellRect(L, s.cx, s.cy);
    Rect mk = (s.cy < 8) ? Rect{ cr.x + 2*k, cr.y + 2*k, cr.w - 4*k, cr.h - 4*k }           // вокруг символа (вся ячейка)
                         : Rect{ cr.x + 2*k, cr.y + 2*k, cr.w - 4*k, cr.h - 4*k };          // вокруг метки действия
    int cs = k;                                                 // масштаб угол-тайла 8×8 (раздельные уголки)
    if (g_pwCursor) {
        auto corner = [&](int px, int py, bool fx, bool fy) {
            for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
                uint32_t c = g_pwCursor[(fy ? 7 - y : y) * 8 + (fx ? 7 - x : x)];
                if (!(c >> 24)) continue;
                for (int sy = 0; sy < cs; ++sy) for (int sx = 0; sx < cs; ++sx) fb.put(px + x*cs + sx, py + y*cs + sy, c);
            }
        };
        corner(mk.x,               mk.y,               false, false);  // ┌
        corner(mk.x + mk.w - 8*cs, mk.y,               true,  false);  // ┐
        corner(mk.x,               mk.y + mk.h - 8*cs, false, true);   // └
        corner(mk.x + mk.w - 8*cs, mk.y + mk.h - 8*cs, true,  true);   // ┘
    } else {                                                     // фолбэк (ZTU): векторные уголки
        const uint32_t MC = 0xFFF0D040u; int bl = mk.h / 3, th = 2*k, x0 = mk.x, y0 = mk.y, x1 = mk.x + mk.w, y1 = mk.y + mk.h;
        auto blk = [&](int ax, int ay, int w, int h) { for (int yy = ay; yy < ay + h; ++yy) for (int xx = ax; xx < ax + w; ++xx) fb.put(xx, yy, MC); };
        blk(x0, y0, bl, th); blk(x0, y0, th, bl); blk(x1 - bl, y0, bl, th); blk(x1 - th, y0, th, bl);
        blk(x0, y1 - th, bl, th); blk(x0, y1 - bl, th, bl); blk(x1 - bl, y1 - th, bl, th); blk(x1 - th, y1 - bl, th, bl);
    }

    drawTextBigC(fb, FBW / 2, 424*k, "ARROWS/CLICK + ENTER  OR TYPE   ESC=CANCEL", 0xFF8090A0u, k, true, nullptr);
}

} // namespace pwscr
