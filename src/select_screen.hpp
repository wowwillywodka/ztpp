// select_screen.hpp — ЭКРАН ВЫБОРА БОЙЦА (PLANET DEFENSE CORPS), ТОЧНО как оригинал ZT.
// Референс: MAME-дамп VRAM (plane B @0xE000 = портрет+текст, plane A @0xC000 = DECEASED-панель для мёртвого).
// Layout из VRAM-адресов ZT (sub_0c6934/c69b8): портрет @(8,8) 128×96; статы шрифтом 8×16 @x144 (имя/код/класс/рост/вес/дата,
// шаг 16px); био 6 строк @x8 снизу (y104+). ВСЁ в native 320×224; fb reference = ×2 (640×448). Шрифт — фирменный ZtFontBig 8×16.
// Подключается ПОСЛЕ FB, ui.hpp (drawTextBig/ZtFontBig), rom/gamedata.hpp.
#pragma once
#include "rom/gamedata.hpp"

struct SelectState {
    int  cursor = 0;             // текущий боец 0..4 (ZT $FF1030)
    bool dead[5] = { false, false, false, false, false };  // DECEASED (порт: true=мёртв; ZT $FF28EC 0=мёртв)
    int  slideT = 0;             // таймер слайд-перехода (ZT c683c/c6896: 0x20=32 кадра, скролл +8/кадр)
    int  slideDir = 0;           // направление слайда (+1 вправо/−1 влево)
    int  slidePrev = 0;          // индекс уезжающего бойца
    bool allDead() const { for (bool d : dead) if (!d) return false; return true; }
    void move(int dir, int n) {  // ZT: смена бойца запускает слайд-переход
        if (n <= 0) return;
        slidePrev = cursor; cursor = (cursor + dir + n) % n; slideDir = dir; slideT = 32;   // 0x20 кадров
    }
    void tickSlide() { if (slideT > 0) --slideT; }
};

// Текст фирменным шрифтом экрана выбора (8×16, родная сине-белая палитра). Позиции в native, масштаб K.
// Билды без вскрытого SelFont (ZTU): фолбэк на 8×8-шрифт UI (тёмно-синий, по центру строки 8×16).
inline void drawSelText(FB& fb, const SelFont& fnt, int nx, int ny, const std::string& s, int K) {
    if (!fnt.have) { drawText(fb, nx * K, ny * K + 4 * K, s.c_str(), 0xFF20386Cu, K); return; }
    int px = nx * K;
    for (char chc : s) {
        int e = (unsigned char)chc - 0x20;
        if (e >= 0 && e < 96) {
            const uint32_t* g = fnt.glyph[e];
            for (int r = 0; r < 16; ++r) for (int c = 0; c < 8; ++c) {
                uint32_t col = g[r * 8 + c]; if (!(col >> 24)) continue;
                for (int sy = 0; sy < K; ++sy) for (int sx = 0; sx < K; ++sx)
                    fb.put(px + c * K + sx, ny * K + r * K + sy, col);
            }
        }
        px += 8 * K;
    }
}

// Отрисовать ОДНОГО бойца (портрет+статы+био+DECEASED) со сдвигом по Y (native px). K = масштаб. clipT/clipB = границы fb-Y.
inline void drawFighterCard(FB& fb, const GameData& gd, int idx, bool isDead, int nyoff, int K, int clipT, int clipB) {
    if (idx < 0 || idx >= (int)gd.fighters.size()) return;
    const FighterCard& f = gd.fighters[idx];
    auto putc = [&](int fx, int fy, uint32_t c) { if (fy >= clipT && fy < clipB) fb.put(fx, fy, c); };
    // ПОРТРЕТ @ native (8,8) + верт.сдвиг
    const int pw = f.pw > 0 ? f.pw : 128, ph = f.ph > 0 ? f.ph : 96;
    if (!f.portrait.empty())
        for (int y = 0; y < ph * K; ++y) for (int x = 0; x < pw * K; ++x)
            putc(8 * K + x, (8 + nyoff) * K + y, f.portrait[(size_t)(y / K) * pw + (x / K)]);
    // СТАТЫ + БИО фирменным шрифтом
    auto T = [&](int nx, int ny, const std::string& s) {
        if (!gd.selFont.have) {           // билд без SelFont (ZTU): фолбэк 8×8 UI-шрифтом с тем же клипом
            int px = nx * K, base = (ny + nyoff + 4) * K;
            for (char chc : s) { unsigned char uc = (unsigned char)chc; if (uc < 32 || uc > 127) uc = '?';
                const uint8_t* g8 = FONT8X8[uc - 32];
                for (int r = 0; r < 8; ++r) for (int c = 0; c < 8; ++c) if (g8[r] & (1 << c))
                    for (int sy = 0; sy < K; ++sy) for (int sx = 0; sx < K; ++sx)
                        putc(px + c * K + sx, base + r * K + sy, 0xFF20386Cu);
                px += 8 * K; }
            return;
        }
        int px = nx * K, base = (ny + nyoff) * K;
        for (char chc : s) { int e = (unsigned char)chc - 0x20;
            if (e >= 0 && e < 96) { const uint32_t* g = gd.selFont.glyph[e];
                for (int r = 0; r < 16; ++r) for (int c = 0; c < 8; ++c) { uint32_t col = g[r * 8 + c]; if (!(col >> 24)) continue;
                    for (int sy = 0; sy < K; ++sy) for (int sx = 0; sx < K; ++sx) putc(px + c * K + sx, base + r * K + sy, col); } }
            px += 8 * K; }
    };
    T(144,  8, f.name);   T(144, 24, f.code);  T(144, 40, f.cls);
    T(144, 56, f.height); T(144, 72, f.weight); T(144, 88, f.dob);
    for (int b = 0; b < 6; ++b) if (!f.bio[b].empty()) T(8, 104 + b * 16, f.bio[b]);
    // DECEASED-панель поверх (ZT c6a16, native-позиция bbox + верт.сдвиг)
    if (isDead && !gd.deceasedGfx.empty() && gd.deceasedW > 0) {
        int bx = gd.deceasedX, by = gd.deceasedY + nyoff;
        for (int y = 0; y < gd.deceasedH; ++y) for (int x = 0; x < gd.deceasedW; ++x) {
            uint32_t c = gd.deceasedGfx[(size_t)y * gd.deceasedW + x]; if (!(c >> 24)) continue;
            for (int sy = 0; sy < K; ++sy) for (int sx = 0; sx < K; ++sx) putc((bx + x) * K + sx, (by + y) * K + sy, c);
        }
    }
    // СПРАЙТЫ (ZT: часть страницы бойца — СКРОЛЛЯТСЯ вместе с ним через nyoff): стрелка↑ @(288,39), push @(284,91), стрелка↓ @(288,143).
    auto spr = [&](const std::vector<uint32_t>& s, int w, int h, int nx, int ny, bool vf) {
        if (s.empty()) return;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            uint32_t c = s[(size_t)(vf ? (h - 1 - y) : y) * w + x]; if (!(c >> 24)) continue;
            for (int sy = 0; sy < K; ++sy) for (int sx = 0; sx < K; ++sx) putc((nx + x) * K + sx, (ny + nyoff + y) * K + sy, c);
        }
    };
    spr(gd.selArrow, 24, 32, 288, 39,  false);
    spr(gd.selPush,  32, 32, 284, 91,  false);
    spr(gd.selArrow, 24, 32, 288, 143, true);
}

// ── РЕНДЕР экрана выбора: ТОЧНЫЙ layout оригинала + вертик.слайд + спрайты (стрелки/push-start) + белый фон. ──
inline void drawSelectScreen(FB& fb, const GameData& gd, const SelectState& st) {
    const int K = 2;                                                     // native 320×224 → fb 640×448
    // ФОН = backdrop CRAM[0] (светло-серый, как оригинал; НЕ чёрный).
    for (int y = 0; y < 224 * K; ++y) for (int x = 0; x < FBW; ++x) fb.px[(size_t)y * FBW + x] = gd.selBg;
    for (int y = 224 * K; y < FBH; ++y) for (int x = 0; x < FBW; ++x) fb.px[(size_t)y * FBW + x] = 0xFF000000u;
    if (gd.fighters.empty()) { drawSelText(fb, gd.selFont, 90, 100, "NO FIGHTER DATA", K); return; }
    const int SCR = 224;                                                 // высота экрана (native) — дистанция верт.слайда
    if (st.slideT > 0) {
        int off = (32 - st.slideT) * SCR / 32;                           // 0→SCR
        int newFrom = st.slideDir > 0 ? SCR - off : -SCR + off;
        int oldTo   = st.slideDir > 0 ? -off : off;
        drawFighterCard(fb, gd, st.slidePrev, st.dead[st.slidePrev], oldTo,   K, 0, 224 * K);
        drawFighterCard(fb, gd, st.cursor,    st.dead[st.cursor],    newFrom, K, 0, 224 * K);
    } else {
        drawFighterCard(fb, gd, st.cursor, st.dead[st.cursor], 0, K, 0, 224 * K);
    }
}
