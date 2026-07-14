// ztpp — src/rom/gamedata.cpp: наполнение GameData из ROM.
// ЕДИНСТВЕННОЕ место с ROM-адресами билдов (BuildAddrs/addrsForBuild). Декодеры экрана/шрифтов —
// приватные (static), зовутся раз при старте. Оркестратор — loadGameDataFromRom.
#include "gamedata.hpp"
#include <cstdio>

const char* buildName(Build b) {
    switch (b) {
        case Build::ZT:        return "Zero Tolerance (релиз)";
        case Build::ZTU:       return "Zero Tolerance Underground";
        case Build::BZT_June:  return "Beyond ZT (прото 1995-06-23)";
        case Build::BZT_July:  return "Beyond ZT (прото 1995-07-14)";
        case Build::ZT_German: return "Zero Tolerance (нем.)";
        default:               return "неизвестный";
    }
}

// ── Per-build таблица ROM-адресов (ЕДИНСТВЕННОЕ место с адресами) ──
struct BuildAddrs {
    std::vector<size_t> sig;            // ZMAP-сигнатуры по эпизодам
    size_t wallBank = 0; int wallTiles = 0; size_t wallPal = 0, shadeRampBase = 0;
    size_t objBank = 0; int objTiles = 0;   // банк графики объектов/декора (billboard-тайлы 32×32)
    size_t fcTemplateBase = 0;          // база таблицы шаблонов пол/потолок (5×0xA0), -0x7152 в сеттере 0x1d66
    size_t hudGfx = 0, hudTm = 0, hudPal = 0; int hudLineAdd = 0;  // HUD/кокпит (экран 0x1F72 40×28)
    size_t pauseGfx = 0, pauseTm = 0, pausePal = 0; int pauseLineAdd = 0;  // фон меню паузы (экран 0x1F72)
    size_t levelNameTable = 0;          // таблица имён уровней (16 симв./шт)
    size_t heldBank = 0; int heldBlocks = 0;   // банк held-графики оружия (блоки по 672 б) + число блоков
    size_t heldPalAddr = 0, heldTable = 0;     // палитра оружия (0x20D2) + таблица id→графика (15 лонгов @0x11C98)
    size_t hudIconBank = 0; int hudIconCount = 0;  // банк HUD-иконок инвентаря (14 иконок 32×32, шаг 0x200)
    size_t hudDigitFont = 0;                       // шрифт цифр боезапаса (0x15A06E, 0x78 б; резолвер 0x1DF54)
    size_t fontLettersBank = 0; int fontLettersCount = 0;  // шрифт Letters (8×8 4bpp; ZT 0x16E758, 40 глифов)
    size_t fontNumBank = 0;     int fontNumCount = 0;      // шрифт Numbers (8×8; ZT 0x16E618, 10 цифр) — HP/враги
    size_t fontAltBank = 0;     int fontAltCount = 0;      // шрифт Font2 (8×8; ZT 0x12EAA6, 36: 0-9 A-Z) — назв.уровня
    size_t fontBigBank = 0, fontBigTable = 0, fontBigPal = 0; int fontBigCount = 0;  // Font_grph 8×16 (ZT 0x125116, табл 0x1DD3A, пал жёлтая 0xCD47A)
    size_t animTableOff = 0x5914;       // смещение таблицы анимаций стен от ZMAP (ZT 0x5914, BZT 0x1914)
    bool wired = false;                 // адреса этого билда заведены
};
static BuildAddrs addrsForBuild(Build b) {
    BuildAddrs a;
    switch (b) {
        case Build::ZT:
        case Build::ZT_German:          // нем. = пересборка US: базовые адреса те же (objdef отличается — позже)
            a.sig = {0x15A106, 0x160420, 0x166028};
            a.wallBank = 0x12EF26; a.wallTiles = 255; a.wallPal = 0x20F2; a.shadeRampBase = 0x3392;
            a.objBank = 0x10E9BE; a.objTiles = 66;   // банк объектов/декора (палитра — линия 0 = wallPal)
            a.fcTemplateBase = 0x14ED26;   // env1 Dim; env-таблица не по порядку (см. ниже)
            a.hudGfx = 0x156CAE; a.hudTm = 0x1563EE; a.hudPal = 0x2072; a.hudLineAdd = 3;  // HUD/кокпит
            a.pauseGfx = 0x12DD06; a.pauseTm = 0x12D446; a.pausePal = 0x2072; a.pauseLineAdd = 3;  // ФОН меню паузы (руки+карта)
            a.levelNameTable = 0x30B3;   // имена уровней: 48 × 16 симв (DOCKING BAY 1 / BRIDGE 1 / ...)
            a.heldBank = 0x16C2B8; a.heldBlocks = 9;   // 9 блоков held-графики (кулаки + 8 стволов)
            a.heldPalAddr = 0x20D2; a.heldTable = 0x11C98;  // палитра оружия + таблица id→графика
            a.hudIconBank = 0x15846E; a.hudIconCount = 14;  // 14 HUD-иконок инвентаря (палитра = held 0x20D2)
            a.hudDigitFont = 0x15A06E;                       // шрифт цифр боезапаса (сразу после 14 иконок)
            a.fontLettersBank = 0x16E758; a.fontLettersCount = 40;  // Letters: A-Z(0-25) 0-9(26-35) .-:?(36-39)
            a.fontNumBank = 0x16E618; a.fontNumCount = 10;          // Numbers: 0-9
            a.fontAltBank = 0x12EAA6; a.fontAltCount = 36;          // Font2: 0-9(0-9) A-Z(10-35)
            a.fontBigBank = 0x125116; a.fontBigTable = 0x1DD3A; a.fontBigCount = 150; a.fontBigPal = 0xCD47A;  // Font_grph 8×16 (char→верх|низ; пал жёлтая)
            a.animTableOff = 0x5914;       // таблица анимаций стен сразу после env (sig+0x5904+16)
            a.wired = true; break;
        case Build::ZTU:                // TODO: банки/сигнатуры ZTU переразмечены
        case Build::BZT_June:           // TODO: прото-адреса + форматы
        case Build::BZT_July:
        default: break;                 // не заведено → fallback на ZT-раскладку (warn)
    }
    return a;
}

static EngineProfile profileForBuild(Build b) {
    EngineProfile p;
    switch (b) {
        case Build::BZT_June: p.hasPcm = false; break;         // June: нет PCM-сэмплов
        case Build::BZT_July: p.bgCompressed = true; break;    // July: фоны byte-pair
        default: break;
    }
    return p;
}

// Детект билда (эвристика; уточняется при подключении конкретного билда). Для текущего ROM (ZT, 2МБ) → ZT.
static Build detectBuild(const Rom& rom) {
    size_t n = rom.size();
    if (n >= 0x2C0000) return Build::BZT_July;   // ~3МБ → прото (July; June уточнить по дате)
    return Build::ZT;                            // 2МБ: ZT/ZTU/нем. — различение позже (заголовок/маркеры)
}

// Общий декод экрана 0x1F72 (40×28 MD-тайлов 8×8 + nametable + 64-цв палитра) → 320×224 ARGB.
static std::vector<uint32_t> decodeScreen320(const Rom& rom, size_t gfx, size_t tm, size_t palAddr, int lineAdd) {
    std::vector<uint32_t> out;
    if (!gfx || !tm || !palAddr) return out;
    uint32_t pal[64];
    for (int i = 0; i < 64; ++i) pal[i] = cramToArgb(rom.u16(palAddr + i * 2));
    out.assign((size_t)HUD_W * HUD_H, 0xFF000000u);
    const int TW = 40, TH = 28;
    for (int ty = 0; ty < TH; ++ty)
        for (int tx = 0; tx < TW; ++tx) {
            uint16_t e = rom.u16(tm + (size_t)(ty * TW + tx) * 2);
            int idx  = e & 0x7FF;
            int line = ((e >> 13) + lineAdd) & 3;
            bool hflip = (e & 0x800) != 0, vflip = (e & 0x1000) != 0;
            size_t toff = gfx + (size_t)idx * 32;            // MD-тайл 8×8 = 32 байта (4 байта/строка)
            int pbase = line * 16;
            for (int r = 0; r < 8; ++r) {
                int sr = vflip ? (7 - r) : r;
                uint8_t row8[8];
                for (int c = 0; c < 4; ++c) {
                    uint8_t b = rom.u8(toff + (size_t)sr * 4 + c);
                    row8[c * 2] = b >> 4; row8[c * 2 + 1] = b & 0x0F;   // hi=левый px
                }
                if (hflip) for (int k = 0; k < 4; ++k) { uint8_t t = row8[k]; row8[k] = row8[7 - k]; row8[7 - k] = t; }
                uint32_t* dst = &out[(size_t)(ty * 8 + r) * HUD_W + tx * 8];
                for (int c = 0; c < 8; ++c) dst[c] = pal[pbase + row8[c]];
            }
        }
    return out;
}
static void decodeHud(GameData& gd, const Rom& rom, const BuildAddrs& a) {
    gd.hud = decodeScreen320(rom, a.hudGfx, a.hudTm, a.hudPal, a.hudLineAdd);
    gd.pauseBg = decodeScreen320(rom, a.pauseGfx, a.pauseTm, a.pausePal, a.pauseLineAdd);  // фон меню паузы (руки+карта-PDA)
}

// ── Декод шрифтов ZT (1-бит маска по «не-фон»; фон = самый частый ниббл глифа) ──
static void ztFontClear(ZtFont& f, int h) {
    f.have = false; f.h = h;
    for (int i = 0; i < 128; ++i) { f.supported[i] = false; for (int r = 0; r < 16; ++r) f.glyph[i][r] = 0; }
}
static uint8_t ztTileNib(const Rom& rom, size_t base, int r, int c) {
    uint8_t b = rom.u8(base + (size_t)r * 4 + c / 2);
    return (c & 1) ? (b & 0x0F) : (uint8_t)(b >> 4);
}
static int ztTileBg(const Rom& rom, size_t base) {            // самый частый ниббл = фон
    int cnt[16] = {0};
    for (int r = 0; r < 8; ++r) for (int c = 0; c < 8; ++c) cnt[ztTileNib(rom, base, r, c)]++;
    int bg = 0; for (int n = 1; n < 16; ++n) if (cnt[n] > cnt[bg]) bg = n;
    return bg;
}
// forceBg≥0 → использовать этот индекс как ФОН (для шрифта чисел 0x16E618 фон = idx 0xF). -1 = авто.
static ZtFont decodeZtFont8(const Rom& rom, size_t bank, const char* order, int forceBg = -1) {
    ZtFont f; ztFontClear(f, 8);
    for (int g = 0; order[g]; ++g) {
        size_t base = bank + (size_t)g * 32;
        int bg = (forceBg >= 0) ? forceBg : ztTileBg(rom, base);
        int ch = (unsigned char)order[g];
        for (int r = 0; r < 8; ++r) {
            uint8_t bits = 0;
            for (int c = 0; c < 8; ++c) if (ztTileNib(rom, base, r, c) != bg) bits |= (uint8_t)(1 << c);
            f.glyph[ch][r] = bits;
        }
        f.supported[ch] = true;
    }
    f.have = true; return f;
}
// Font_grph 8×16 (ЦВЕТНОЙ): таблица char(0x20+)→long (верх<<16|низ), тайлы VRAM-абсолютные, gfx = tile−1.
static ZtFontBig decodeZtFontBig(const Rom& rom, size_t bank, size_t table, int count, size_t palAddr) {
    ZtFontBig f;
    f.have = false;
    for (int i = 0; i < 128; ++i) {
        f.supported[i] = false;
        for (int r = 0; r < 16; ++r) for (int c = 0; c < 8; ++c) f.pix[i][r][c] = 0;
    }
    Palette pal = readPalette(rom, palAddr);
    for (int i = 0; i < 16; ++i) f.pal[i] = pal.c[i];
    for (int ch = 0x20; ch <= 0x7E; ++ch) {
        uint32_t e = rom.u32(table + (size_t)(ch - 0x20) * 4);
        int tile[2] = { (int)((e >> 16) & 0x7FF) - 1, (int)(e & 0x7FF) - 1 };  // [верх, низ] gfx-индексы
        bool any = false;
        for (int half = 0; half < 2; ++half) {
            int gi = tile[half];
            if (gi < 0 || gi >= count) continue;
            size_t base = bank + (size_t)gi * 32;
            for (int r = 0; r < 8; ++r)
                for (int c = 0; c < 8; ++c)
                    f.pix[ch][half * 8 + r][c] = ztTileNib(rom, base, r, c);
            any = true;
        }
        if (any) f.supported[ch] = true;
    }
    f.have = true; return f;
}

// ── Наполнение из ROM ──
bool loadGameDataFromRom(GameData& gd, const Rom& rom) {
    gd.build   = detectBuild(rom);
    gd.profile = profileForBuild(gd.build);
    BuildAddrs a = addrsForBuild(gd.build);
    if (!a.wired) {                               // билд ещё не заведён → ZT-раскладка как fallback
        std::fprintf(stderr, "ВНИМАНИЕ: билд «%s» ещё не подключён — пробую ZT-раскладку\n", buildName(gd.build));
        a = addrsForBuild(Build::ZT);
    }

    gd.levels.clear(); gd.levels.resize(a.sig.size());
    for (size_t i = 0; i < a.sig.size(); ++i) loadLevelFromRom(gd.levels[i], rom, a.sig[i], a.animTableOff);
    gd.profile.episodes = (int)gd.levels.size();

    gd.wallPal = readPalette(rom, a.wallPal);
    gd.wall.count = a.wallTiles;
    gd.wall.data.assign(static_cast<size_t>(a.wallTiles) * 512, 0);
    for (size_t i = 0; i < gd.wall.data.size(); ++i) gd.wall.data[i] = rom.u8(a.wallBank + i);

    // Банк объектов/декора (billboard-тайлы 32×32 column-major, тот же формат что стены).
    gd.obj.count = a.objTiles;
    gd.obj.data.assign(static_cast<size_t>(a.objTiles) * 512, 0);
    for (size_t i = 0; i < gd.obj.data.size(); ++i) gd.obj.data[i] = rom.u8(a.objBank + i);

    gd.bgCity  = loadZtBackground(rom, 0);
    gd.bgSpace = loadZtBackground(rom, 1);

    decodeHud(gd, rom, a);              // HUD/кокпит (320×224) + фон меню паузы
    gd.levelNames.clear();             // имена уровней (для меню паузы): 48 × 16 симв, обрезаем пробелы
    if (a.levelNameTable) for (int i = 0; i < 48; ++i) {
        std::string s; for (int c = 0; c < 16; ++c) { char ch = (char)rom.u8(a.levelNameTable + (size_t)i * 16 + c); if (ch >= 32 && ch < 127) s += ch; }
        size_t b = s.find_first_not_of(' '), e = s.find_last_not_of(' ');
        gd.levelNames.push_back(b == std::string::npos ? "" : s.substr(b, e - b + 1));
    }

    // ОРУЖИЕ В РУКАХ: банк held-графики (heldBlocks × 672 б) + палитра (0x20D2) + таблица id→блок.
    // Таблица @0x11C98 (15 лонгов, weapon-id→указатель графики) → индекс блока = (ptr−heldBank)/672.
    if (a.heldBank && a.heldBlocks > 0) {
        const size_t HELD_BLOCK = 672;             // 0x2A0 = 21 тайл × 32 б
        gd.heldBlocks = a.heldBlocks;
        gd.heldGfx.assign((size_t)a.heldBlocks * HELD_BLOCK, 0);
        for (size_t i = 0; i < gd.heldGfx.size(); ++i) gd.heldGfx[i] = rom.u8(a.heldBank + i);
        // Блок ДЕЙСТВИЯ кулаков (удар/кик/замах) @ heldBank−0x280 (0x16c038): idle-кулаки в heldBlock0 не годятся
        // для поз удара. ZT DMA-свапит этот блок в VRAM при ударе.
        gd.fistAction.assign(HELD_BLOCK, 0);
        for (size_t i = 0; i < HELD_BLOCK; ++i) gd.fistAction[i] = rom.u8(a.heldBank - 0x280 + i);
        gd.heldPal = readPalette(rom, a.heldPalAddr ? a.heldPalAddr : a.wallPal);
        for (int id = 0; id < 15; ++id) {
            size_t ptr = a.heldTable ? rom.u32(a.heldTable + (size_t)id * 4) : a.heldBank;
            long blk = a.heldBank ? (long)((ptr - a.heldBank) / HELD_BLOCK) : 0;
            gd.heldBlockForId[id] = (blk >= 0 && blk < a.heldBlocks) ? (uint8_t)blk : 0;
        }
    }

    // HUD-иконки инвентаря (банк 0x15846E, 14 × 0x200). Палитра — та же held (0x20D2).
    if (a.hudIconBank && a.hudIconCount > 0) {
        gd.hudIconCount = a.hudIconCount;
        gd.hudIcons.assign((size_t)a.hudIconCount * 0x200, 0);
        for (size_t i = 0; i < gd.hudIcons.size(); ++i) gd.hudIcons[i] = rom.u8(a.hudIconBank + i);
    }
    if (a.hudDigitFont) for (int i = 0; i < 0x78; ++i) gd.digitFont[i] = rom.u8(a.hudDigitFont + i);  // шрифт цифр боезапаса

    // ШРИФТЫ ZT (1-бит маски в формате FONT8X8 → drawChar/drawCharBig единообразны).
    if (a.fontLettersBank) gd.font    = decodeZtFont8(rom, a.fontLettersBank, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:?");
    if (a.fontNumBank)     gd.fontNum = decodeZtFont8(rom, a.fontNumBank,     "0123456789", 0xF);  // фон чисел = idx 0xF (#000000)
    if (a.fontAltBank)     gd.fontAlt = decodeZtFont8(rom, a.fontAltBank,     "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    if (a.fontBigBank && a.fontBigTable)
        gd.fontBig = decodeZtFontBig(rom, a.fontBigBank, a.fontBigTable, a.fontBigCount, a.fontBigPal);

    gd.shadeRamps.assign(0x1000, 0);
    for (size_t i = 0; i < 0x1000; ++i) gd.shadeRamps[i] = rom.u8(a.shadeRampBase + i);

    // Шаблоны пол/потолок (ОТДЕЛЬНЫЙ СЛОЙ, не скейлер). Сеттер 0x1d66 ставит указатель -0x7152 по env:
    //   env0 Bright=base+0x140, env1 Dim=base+0x00, env2 Haze=base+0xA0, env3 NoCeil=base+0x1E0,
    //   env4 Black=base+0x280 (в ROM лежат не по порядку env). Каждый = 0xA0 б = 2 верт. паттерна по
    //   80 строк (нечёт/чёт колонка = гориз. дизер); байт = 2px 4bpp (hi=левый). Раскладываем по env 0..4.
    gd.fcTemplates.assign(5 * 0xA0, 0);
    if (a.fcTemplateBase) {
        static const size_t envOff[5] = { 0x140, 0x000, 0x0A0, 0x1E0, 0x280 };  // env0..4 → смещение в ROM
        for (int e = 0; e < 5; ++e)
            for (size_t i = 0; i < 0xA0; ++i)
                gd.fcTemplates[(size_t)e * 0xA0 + i] = rom.u8(a.fcTemplateBase + envOff[e] + i);
    }

    setActiveCellTable(cellTableForBuild((int)gd.build));   // классификация клеток по билду (сейчас ZT)
    loadZAngLUT(rom);                                       // LUT углов 0x8124 (cos/sin ×256) — для fixed-point рендера стен
    if (gd.build == Build::ZT || gd.build == Build::ZT_German)
        decodeEnemySprites(rom, gd.wallPal);               // реальные спрайты врагов (дерево 0x1B7B38… пал 0x20F2)

    gd.valid = !gd.levels.empty() && gd.levels[0].valid();
    return gd.valid;
}
