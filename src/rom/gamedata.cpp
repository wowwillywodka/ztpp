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

const char* buildKey(Build b) {
    switch (b) {
        case Build::ZT:        return "zt";
        case Build::ZTU:       return "ztu";
        case Build::BZT_June:  return "bzt_june";
        case Build::BZT_July:  return "bzt_july";
        case Build::ZT_German: return "zt_german";
        default:               return "unknown";
    }
}

// Детекция билда по заголовку (единая точка: и загрузка, и лаунчер; = mdgfx/rom.py::detect_version).
Build detectBuildFromHeader(const uint8_t* hdr, size_t hdrLen, size_t fileSize) {
    auto find = [&](size_t off, size_t len, const char* s) {
        size_t sl = 0; while (s[sl]) ++sl;
        if (off + len > hdrLen || sl > len) return false;
        for (size_t i = off; i + sl <= off + len; ++i) {
            size_t j = 0; while (j < sl && hdr[i + j] == (uint8_t)s[j]) ++j;
            if (j == sl) return true;
        }
        return false;
    };
    if (!find(0x100, 0x10, "SEGA"))  return Build::Unknown;   // не MD-ROM
    if (!find(0x180, 0x0E, "T-119")) return Build::Unknown;   // MD-ROM, но не семейство ZT
    if (fileSize >= 0x300000) return Build::BZT_July;         // 3 МБ — только июльский прототип
    if (find(0x180, 0x0E, "-01")) return Build::ZT;
    if (find(0x180, 0x0E, "-00")) {
        if (find(0x3170, 0x20, "SUBWAY"))      return Build::ZTU;       // таблица имён уровней @0x3174
        if (find(0x30B0, 0x14, "DOCKING BAY")) return Build::ZT_German; // таблица имён релиза @0x30B2
        return Build::BZT_June;
    }
    return Build::Unknown;
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
    size_t idCardTable = 0;                    // ⭐таблица ID-карт U-RON (5 long → объекты 0x1F72; ZT 0x2CC8)
    size_t hudIconBank = 0; int hudIconCount = 0;  // банк HUD-иконок инвентаря (14 иконок 32×32, шаг 0x200)
    size_t hudDigitFont = 0;                       // шрифт цифр боезапаса (0x15A06E, 0x78 б; резолвер 0x1DF54)
    size_t fontLettersBank = 0; int fontLettersCount = 0;  // шрифт Letters (8×8 4bpp; ZT 0x16E758, 40 глифов)
    size_t fontNumBank = 0;     int fontNumCount = 0;      // шрифт Numbers (8×8; ZT 0x16E618, 10 цифр) — HP/враги
    size_t fontAltBank = 0;     int fontAltCount = 0;      // шрифт Font2 (8×8; ZT 0x12EAA6, 36: 0-9 A-Z) — назв.уровня
    size_t fontBigBank = 0, fontBigTable = 0, fontBigPal = 0; int fontBigCount = 0;  // Font_grph 8×16 (ZT 0x125116, табл 0x1DD3A, пал жёлтая 0xCD47A)
    size_t animTableOff = 0x5914;       // смещение таблицы анимаций стен от ZMAP (ZT 0x5914, BZT 0x1914)
    // ── Per-build адреса, ранее зашитые в код (ZT-дефолты). ZTU-значения: известные из реверса
    //    (ztu-underground) + байт-поиск ИДЕНТИЧНЫХ таблиц ZT→ZTU (уникальные совпадения = verified).
    size_t nvPalAddr = 0x2072;          // ночник: полная зелёная CRAM (ZTU 0x2134)
    size_t pauseTextPalAddr = 0x1CDAE;  // белая палитра заголовка паузы (ZTU 0x1D77A, байт-в-байт)
    size_t angLUTAddr = 0x8124;         // LUT углов 512×{cos,sin} (ZTU 0x81E6, байт-в-байт)
    size_t spriteClutAddr = 0x10D1BE;   // CLUT-шейд спрайтов 12×0x100 (ZTU 0xDEB66, байт-в-байт)
    size_t enemyGfx[10] = {0};          // 10 банков спрайтов врагов по слотам (все 0 = ZT-дефолты декодера)
    size_t enemyAltFH = 0x1C258A;       // альт-банк FH «коммандо» (ZTU: нет → 0)
    size_t startInvAddr = 0xF98;        // стартовый инвентарь 5×8 б (ZTU 0x107E, байт-в-байт)
    size_t fighterBase = 0;             // карточки бойцов 5×0x13E ASCII (ZT 0xC6B36, ZTU 0x974D6)
    size_t portraitTable = 0;           // таблица портретов-ID-карт экрана выбора (ZT 0xC634E; 0 = взять gd.idCards)
    size_t sndSamp = 0x5E51C, sndPatch = 0x5A804, sndSeq = 0x5B0CC, sndSfx = 0xC5EB4;  // GEMS-банки
    bool hasPanorama = true;            // фоны-панорамы (ZTU: нет — все уровни с потолком)
    // ── Экран выбора/титул/заставки: ZTU = байт-идентичные ZT-блоки со СДВИГОМ (verified 2026-07-19,
    //    100% совпадение titleGfx 46450 Б, DECEASED/selFont/спрайты/пал — все 1:1 по дельте).
    long selDelta = 0;                  // сдвиг блока экрана выбора+DECEASED (ZTU −0x2F660: 0xC7196→0x97B36…)
    long titleGfxDelta = 0;             // сдвиг титул-графики [0x119B8E..0x125100) (ZTU −0x24858 → 0xF5336)
    long titleTabDelta = 0;             // сдвиг титул-таблиц [0x1D400..0x1DE00) (ZTU +0x9C8 → 0x1DDC8)
    bool hasTitle = false;              // титульный экран есть (ZT/German/ZTU — у ZTU идентичен ZT)
    size_t copyrightBase = 0, copyrightPal = 0;   // копирайт-экран (объект 0x1F72) + палитра
    size_t logoTiles = 0, logoNt[2] = {0, 0}, logoPal[2] = {0, 0};   // лого мода (ZTU: PIKO + разработчик)
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
            a.idCardTable = 0x2CC8;                          // ⭐ID-карты отряда (пауза-экран, ROM 2acc)
            a.hudIconBank = 0x15846E; a.hudIconCount = 14;  // 14 HUD-иконок инвентаря (палитра = held 0x20D2)
            a.hudDigitFont = 0x15A06E;                       // шрифт цифр боезапаса (сразу после 14 иконок)
            a.fontLettersBank = 0x16E758; a.fontLettersCount = 40;  // Letters: A-Z(0-25) 0-9(26-35) .-:?(36-39)
            a.fontNumBank = 0x16E618; a.fontNumCount = 10;          // Numbers: 0-9
            a.fontAltBank = 0x12EAA6; a.fontAltCount = 36;          // Font2: 0-9(0-9) A-Z(10-35)
            a.fontBigBank = 0x125116; a.fontBigTable = 0x1DD3A; a.fontBigCount = 150; a.fontBigPal = 0xCD47A;  // Font_grph 8×16 (char→верх|низ; пал жёлтая)
            a.animTableOff = 0x5914;       // таблица анимаций стен сразу после env (sig+0x5904+16)
            a.fighterBase = 0xC6B36; a.portraitTable = 0xC634E;
            a.hasTitle = true;
            a.copyrightBase = 0x116DBE; a.copyrightPal = 0x1CD8E;
            a.wired = true; break;
        case Build::ZTU: {              // Underground v1.5: движок релиза, всё переразмечено (ztu-underground)
            a.sig = {0x1736AE};         // ОДИН ZMAP (эпизод 0 = 16 этажей субвея); раскладка офсетов = ZT
            // Стены: 3 банка по (level>>4)&3 (level-load @0xE62/E78/E84, RAM -$58e0); играется только
            // эпизод 0 → субвей 0x10A6CE (индустрия 0x12A6CE / тёмный 0x1494CE — задел мода).
            a.wallBank = 0x10A6CE; a.wallTiles = 255;
            a.wallPal = 0x21B4;         // палитра спрайтов/стен (level-load 0x116fe: 0x2134→0x21B4; подтв. юзером)
            a.nvPalAddr = 0x2134;       // ночник-аналог (US 0x2072→0x20F2)
            a.shadeRampBase = 0x3454;   // CLUT стен (байт-поиск ZT 0x3392: уникальное совпадение)
            a.objBank = 0xE0366; a.objTiles = 0x91;   // банк объектов/предметов (ZTU_ITEMS_BASE, ~145 тайлов)
            a.fcTemplateBase = 0x1682CE;              // шаблоны пол/потолок (байт-поиск ZT 0x14ED26)
            // Кокпит-HUD = полноэкранный 0x1F72-объект @0x16F98E (tm=+8, gfx=+0x8C8); палитра-блок 0x2134.
            a.hudGfx = 0x16F98E + 0x8C8; a.hudTm = 0x16F98E + 8; a.hudPal = 0x2134; a.hudLineAdd = 3;
            // ⭐Фон меню паузы (руки+PDA) = объект @0x108BE6 (2026-07-19: байт-поиск ZT-тайлов 0x12DD06 →
            // 0x1094AE = base+0x8C8; прежняя пометка экстрактора «кокпит-перспектива» неверна).
            a.pauseGfx = 0x108BE6 + 0x8C8; a.pauseTm = 0x108BE6 + 8; a.pausePal = 0x2134; a.pauseLineAdd = 3;
            a.levelNameTable = 0x3174;  // « SUBWAY LEVEL 1»… (16 б/имя, ведущий пробел)
            a.heldBank = 0x17936C; a.heldBlocks = 9;         // руки в синих перчатках + 8 стволов (шаг 0x2A0)
            a.heldPalAddr = 0x2194; a.heldTable = 0x1264A;   // HUD-палитра (0x21B4−0x20) + таблица id→графика
            a.idCardTable = 0x2D8A;                          // 5 ID-карт U-RON (объекты 0x1F72 @0x101B7E…)
            a.hudIconBank = 0x171A16; a.hudIconCount = 14;   // иконки инвентаря (таблица @0x10F40 → база)
            a.hudDigitFont = 0x173616;                       // = 0x171A16+14·0x200 (байт-в-байт с ZT 0x15A06E)
            a.fontLettersBank = 0x17B80C; a.fontLettersCount = 40;
            a.fontNumBank = 0x17B6CC;     a.fontNumCount = 10;
            a.fontAltBank = 0x10A24E;     a.fontAltCount = 36;
            a.fontBigBank = 0x1008BE; a.fontBigTable = 0x1E702; a.fontBigCount = 150;
            a.fontBigPal = 0x9E2B6;      // жёлтая палитра Font_grph (байт-в-байт с ZT 0xCD47A)
            a.pauseTextPalAddr = 0x1D77A;                    // байт-в-байт с ZT 0x1CDAE
            a.angLUTAddr = 0x81E6; a.spriteClutAddr = 0xDEB66;
            // Спрайты врагов по слотам (0Sgt 1FH 2Imp 3Hydaca 4Reven 5Boss1 6Dog 7FH-SF 8Boss3 9Boss2):
            // ZTU_CELLTYPE_ENEMY (celltype→слот = US-порядок), альт-банка FH в ZTU нет.
            { static const size_t G[10] = { 0x1C4BEC, 0x17BD0C, 0x1B48DC, 0x1B9D26, 0x19A12C,
                                            0x190C48, 0x1855A6, 0x188B34, 0x1A0FB4, 0x1AA5C0 };
              for (int i = 0; i < 10; ++i) a.enemyGfx[i] = G[i]; }
            a.enemyAltFH = 0;
            a.startInvAddr = 0x107E;    // байт-в-байт с ZT 0xF98
            a.fighterBase = 0x974D6;    // SATOE ISHII… (формат = ZT: 5×0x13E; = 0xC6B36−0x2F660)
            a.portraitTable = 0x96CEE;  // select-портреты (= ZT 0xC634E−0x2F660; ptr'ы → те же карты 0x101B7E…)
            a.sndSamp = 0x2EE77; a.sndPatch = 0x2981E; a.sndSeq = 0x2A288; a.sndSfx = 0x96848;  // GEMS-init @0x96492
            a.hasPanorama = false;      // фонов в игре нет (все уровни с потолком)
            // Экран выбора/DECEASED/титул: байт-идентичные ZT-блоки со сдвигом (verified байт-поиском).
            a.selDelta = -0x2F660; a.titleGfxDelta = -0x24858; a.titleTabDelta = 0x9C8; a.hasTitle = true;
            a.copyrightBase = 0xF2566; a.copyrightPal = 0x1D75A;   // копирайт = ZT-объект (head байт-в-байт)
            // ⭐Лого мода (оркестратор 0x2638E: тайлы→VRAM, 2 экрана по 0xB4=180 кадров, фейд 0x1d5d0):
            a.logoTiles = 0x2444A;
            a.logoNt[0] = 0x2616A; a.logoPal[0] = 0x2442A;   // PIKO
            a.logoNt[1] = 0x261FE; a.logoPal[1] = 0x2440A;   // эмблема разработчика
            a.wired = true; break;
        }
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

// Детект билда — по заголовку ROM (общая точка с лаунчером: detectBuildFromHeader).
static Build detectBuild(const Rom& rom) {
    if (rom.size() < 0x3200) return Build::Unknown;
    Build b = detectBuildFromHeader(rom.data.data(), 0x3200, rom.size());
    return (b == Build::Unknown) ? Build::ZT : b;   // неопознанный 2МБ — пробуем ZT-раскладку (прежнее поведение)
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
// ⭐Декод полноэкранной картинки 40×28 из СЫРЫХ тайлов + nametable (index+flip) + 16-цв. палитры.
// Формат стартовых заставок (tiles@tilesAddr, nametable@ntAddr — 40×28 слов idx&0x7FF, bit11 hflip, bit12 vflip;
// baseSub: индекс тайла = (слово&0x7FF) − min (тайлы в VRAM грузятся с базой = min индекс).  → 320×224 ARGB.
static std::vector<uint32_t> decodeTileScreen(const Rom& rom, size_t tilesAddr, size_t ntAddr, size_t palAddr, bool baseSub) {
    std::vector<uint32_t> out((size_t)HUD_W * HUD_H, 0xFF000000u);
    uint32_t pal[16]; for (int i = 0; i < 16; ++i) pal[i] = cramToArgb(rom.u16(palAddr + i * 2));
    const int TW = 40, TH = 28;
    int base = 0x7FFF;
    if (baseSub) for (int i = 0; i < TW * TH; ++i) { int idx = rom.u16(ntAddr + (size_t)i * 2) & 0x7FF; if (idx < base) base = idx; }
    else base = 0;
    for (int ty = 0; ty < TH; ++ty)
        for (int tx = 0; tx < TW; ++tx) {
            uint16_t e = rom.u16(ntAddr + (size_t)(ty * TW + tx) * 2);
            int idx = (e & 0x7FF) - base; if (idx < 0) continue;
            bool hf = (e & 0x800) != 0, vf = (e & 0x1000) != 0;
            size_t toff = tilesAddr + (size_t)idx * 32; if (toff + 32 > rom.size()) continue;
            for (int r = 0; r < 8; ++r) {
                int sr = vf ? (7 - r) : r; uint8_t row8[8];
                for (int c = 0; c < 4; ++c) { uint8_t b = rom.u8(toff + (size_t)sr * 4 + c); row8[c*2] = b >> 4; row8[c*2+1] = b & 0xF; }
                if (hf) for (int k = 0; k < 4; ++k) { uint8_t t = row8[k]; row8[k] = row8[7-k]; row8[7-k] = t; }
                uint32_t* dst = &out[(size_t)(ty * 8 + r) * HUD_W + tx * 8];
                for (int c = 0; c < 8; ++c) dst[c] = pal[row8[c]];
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
    // ⭐НИЧЬЯ краска/фон (у 'B' Letters ровно 32/32) выбирала КРАСКУ → глиф инвертировался
    // («буква B засвеченная»). Тай-брейк: нижний-левый угол тайла — у глифов всегда фон.
    int corner = ztTileNib(rom, base, 7, 0);
    if (cnt[corner] == cnt[bg]) bg = corner;
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
    for (size_t i = 0; i < a.sig.size(); ++i) {
        loadLevelFromRom(gd.levels[i], rom, a.sig[i], a.animTableOff);
        gd.levels[i].area = (int)i;    // ROM -$58e4 = (levelByte>>4)&3 = эпизод (набор «небесных» cellID 13c36)
    }
    gd.profile.episodes = (int)gd.levels.size();

    gd.wallPal = readPalette(rom, a.wallPal);
    gd.nvPal   = readPalette(rom, a.nvPalAddr);         // ночник: зелёная палитра (ROM 29ee; ZTU 0x2134)
    gd.pauseTextPal = readPalette(rom, a.pauseTextPalAddr);
    gd.sndSamp = a.sndSamp; gd.sndPatch = a.sndPatch; gd.sndSeq = a.sndSeq; gd.sndSfx = a.sndSfx;
    gd.pauseNames.clear();                              // ⭐576aa: 48 строк по 25 байт «DOCKING BAY LEVEL 1 : »
    if (gd.build == Build::ZT || gd.build == Build::ZT_German)
        for (int i = 0; i < 48; ++i) {
            size_t o = 0x576AA + (size_t)i * 0x19; std::string t;
            for (int j = 0; j < 0x19 && rom.u8(o + j); ++j) t += (char)rom.u8(o + j);
            gd.pauseNames.push_back(t);
        }        // ⭐пауза-заголовок: глифы 57bc2 несут pal2, а CRAM line2 на
                                                        //  паузе = 0x1CDAE («белая» Font_grph) — VERIFIED MAME pause_cram
    // (0x16E618 = ШРИФТ ЦИФР HP (d98e рисует font|mask), НЕ флеш-градиент — прежняя flashTable удалена;
    //  вспышка урона/смерти = $FF1072 → CRAM-слот 63, см. VBlank 0xB12 и player().flashCram.)
    // ⭐SEGA-заставка (sub_05711e): базовая палитра @0x570C2, кольцо палитро-анимации @0x570CA (текстура —
    //  тайлы 0xF7..0xFE банка стен, уже в gd.wall). Только ZT (в June/July boot другой).
    if (gd.build == Build::ZT || gd.build == Build::ZT_German) {
        for (int i = 0; i < 16; ++i) gd.segaPal[i]  = rom.u16(0x570C2 + (size_t)i * 2);
        for (int i = 0; i < 34; ++i) gd.segaAnim[i] = rom.u16(0x570CA + (size_t)i * 2);
        gd.hasSega = true;
    }
    // ⭐ТИТУЛ (1cf36): спрайт-оверлеи. Тайлы VRAM 0x355+ = 0x123716 (182) + 0x124DD6 (26); column-major.
    // ZTU: те же блоки со сдвигом (графика −0x24858, таблицы +0x9C8) — контент байт-идентичен ZT.
    if (a.hasTitle) {
        const long GD_ = a.titleGfxDelta, TD_ = a.titleTabDelta;
        auto tileAddr = [&](int idx) -> size_t {
            if (idx < 0x355) return 0;
            int off = idx - 0x355;
            if (off < 182) return (size_t)(0x123716 + GD_) + (size_t)off * 32;
            off -= 182;
            if (off < 26) return (size_t)(0x124DD6 + GD_) + (size_t)off * 32;
            return 0;
        };
        uint32_t spal[4][16];
        for (int l = 0; l < 4; ++l) for (int i = 0; i < 16; ++i) spal[l][i] = cramToArgb(rom.u16((size_t)(0x1DCF4 + TD_) + l * 32 + i * 2));
        auto blitSprites = [&](size_t tbl, int n, std::vector<uint32_t>& out) {
            out.assign((size_t)HUD_W * HUD_H, 0);
            for (int i = 0; i < n; ++i) {
                int Y = rom.u16(tbl + i * 8) - 128, sz = rom.u16(tbl + i * 8 + 2);
                int attr = rom.u16(tbl + i * 8 + 4), X = rom.u16(tbl + i * 8 + 6) - 128;
                int w = ((sz >> 10) & 3) + 1, h = ((sz >> 8) & 3) + 1;
                int ti = attr & 0x7FF, pl = (attr >> 13) & 3;
                for (int cx = 0; cx < w; ++cx)
                    for (int cy = 0; cy < h; ++cy) {
                        size_t t = tileAddr(ti + cx * h + cy); if (!t) continue;
                        for (int r = 0; r < 8; ++r)
                            for (int c = 0; c < 4; ++c) {
                                uint8_t b = rom.u8(t + (size_t)r * 4 + c);
                                for (int hh = 0; hh < 2; ++hh) {
                                    int v = hh ? (b & 0xF) : (b >> 4); if (!v) continue;
                                    int px = X + cx * 8 + c * 2 + hh, py = Y + cy * 8 + r;
                                    if (px >= 0 && px < HUD_W && py >= 0 && py < HUD_H)
                                        out[(size_t)py * HUD_W + px] = spal[pl][v];
                                }
                            }
                    }
            }
        };
        blitSprites((size_t)(0x1D5A2 + TD_), 16, gd.titleFighter);
        blitSprites((size_t)(0x1D4D6 + TD_), 4,  gd.titleMenu);
        gd.titleCursor.assign(16 * 8, 0);                 // курсор-стрелка: 2×1 тайла (0x40B..0x40C), pal0
        for (int cx = 0; cx < 2; ++cx) {
            size_t t = tileAddr(0x40B + cx); if (!t) continue;
            for (int r = 0; r < 8; ++r)
                for (int c = 0; c < 4; ++c) {
                    uint8_t b = rom.u8(t + (size_t)r * 4 + c);
                    for (int hh = 0; hh < 2; ++hh) {
                        int v = hh ? (b & 0xF) : (b >> 4); if (!v) continue;
                        gd.titleCursor[(size_t)r * 16 + cx * 8 + c * 2 + hh] = spal[0][v];
                    }
                }
        }
    }
    // ⭐КОПИРАЙТ = объект блиттера 1f72 (VERIFIED): заголовок = [nTiles][tileOff=0x8C8][w=40][h=28];
    // NT-слова @+8 (по строкам, тайл-база 0 → БЕЗ base-subtract); тайлы @+0x8C8 (в VRAM с 0).
    // ZT @0x116DBE (пал 0x1CD8E); ZTU @0xF2566 (head байт-в-байт с ZT, пал 0x1D75A).
    if (a.copyrightBase)
        gd.copyrightScreen = decodeTileScreen(rom, a.copyrightBase + 0x8C8, a.copyrightBase + 0x8, a.copyrightPal, /*baseSub*/false);
    // ⭐ЛОГО-ЭКРАНЫ МОДА (ZTU-оркестратор 0x2638E → писатель NT 0x2656E: {u16 x,y,w,h}+w×h слов; тайлы в
    // VRAM с 0; экран = тайлы 0x2444A + палитра своего лого). Рендерим в 320×224 ARGB по (x,y) тайлов.
    gd.logoFrames.clear();
    if (a.logoTiles) for (int L = 0; L < 2; ++L) {
        if (!a.logoNt[L]) continue;
        int lx = rom.u16(a.logoNt[L]), ly = rom.u16(a.logoNt[L] + 2),
            lw = rom.u16(a.logoNt[L] + 4), lh = rom.u16(a.logoNt[L] + 6);
        if (lw < 1 || lw > 40 || lh < 1 || lh > 28) continue;
        uint32_t lpal[16]; for (int i = 0; i < 16; ++i) lpal[i] = cramToArgb(rom.u16(a.logoPal[L] + (size_t)i * 2));
        // ⭐ФОН = BACKDROP-цвет (ROM 2638E: рег 0x87→CRAM-цвет 0 линии лого): PIKO = 0x0EEE БЕЛЫЙ,
        // разработчик = 0x0000 чёрный. Была жёсткая чёрная заливка — PIKO терял белый фон (юзер 2026-07-24).
        std::vector<uint32_t> fr((size_t)HUD_W * HUD_H, lpal[0]);
        for (int ty = 0; ty < lh; ++ty)
            for (int tx = 0; tx < lw; ++tx) {
                int e = rom.u16(a.logoNt[L] + 8 + (size_t)(ty * lw + tx) * 2), idx = e & 0x7FF;
                size_t t = a.logoTiles + (size_t)idx * 32; if (t + 32 > rom.size()) continue;
                for (int r = 0; r < 8; ++r)
                    for (int c = 0; c < 4; ++c) {
                        uint8_t b = rom.u8(t + (size_t)r * 4 + c);
                        int px = (lx + tx) * 8 + c * 2, py = (ly + ty) * 8 + r;
                        if (px < 0 || px + 1 >= HUD_W || py < 0 || py >= HUD_H) continue;
                        fr[(size_t)py * HUD_W + px]     = lpal[b >> 4];
                        fr[(size_t)py * HUD_W + px + 1] = lpal[b & 0x0F];
                    }
            }
        gd.logoFrames.push_back(std::move(fr));
    }
    // ⭐СТАРТОВЫЕ ЗАСТАВКИ (только ZT): TECHNOPOP-анимация (22 кадра) + ACCOLADE.
    if (gd.build == Build::ZT || gd.build == Build::ZT_German) {
        static const uint32_t IF_TILES[22] = {0x2057E,0x2295E,0x24CBE,0x2707E,0x293DE,0x2B9FE,0x2E25E,0x307DE,
            0x32BDE,0x34F9E,0x3741E,0x399BE,0x3BEBE,0x3E3BE,0x4085E,0x42C5E,0x44FBE,0x4767E,0x49E5E,0x4C67E,0x4EDFE,0x5131E};
        static const uint32_t IF_NT[22] = {0x1FCBE,0x2209E,0x243FE,0x267BE,0x28B1E,0x2B13E,0x2D99E,0x2FF1E,
            0x3231E,0x346DE,0x36B5E,0x390FE,0x3B5FE,0x3DAFE,0x3FF9E,0x4239E,0x446FE,0x46DBE,0x4959E,0x4BDBE,0x4E53E,0x50A5E};
        gd.introFrames.clear();
        for (int i = 0; i < 22; ++i)
            gd.introFrames.push_back(decodeTileScreen(rom, IF_TILES[i], IF_NT[i], 0x1FC3E, /*baseSub*/true));
        // (титул-блоки titleGfxBlk/titleTabBlk грузятся ниже общим per-build блоком a.hasTitle.)
        // ⭐ACCOLADE (sub_052C7E, между копирайтом и Technopop): 15 шагов «печатной машинки» A-C-C-O-L-A-D-E.
        // Тайлы: 409 сырых 4bpp @0x53DA0 (VRAM с тайла 0). Кадр шага s: блок 4 ряда × 32 слова @0x52DA0+s·0x100,
        // ⚠ NT-слова LITTLE-ENDIAN (внешний PC-инструментарий Accolade; код свапает байты @52d2c). Палитра
        // 16 цв. BE @0x52D80. Блок пишется в план A @$8508 (кол.4, ряды 10-13) = экранные px (32,80), 256×32.
        gd.accoladeFrames.clear();
        {
            uint32_t apal[16]; for (int i = 0; i < 16; ++i) apal[i] = cramToArgb(rom.u16(0x52D80 + (size_t)i * 2));
            for (int s = 0; s < 15; ++s) {
                std::vector<uint32_t> fr((size_t)HUD_W * HUD_H, 0xFF000000u);
                for (int ty = 0; ty < 4; ++ty)
                    for (int tx = 0; tx < 32; ++tx) {
                        size_t ea = 0x52DA0 + (size_t)s * 0x100 + (size_t)(ty * 32 + tx) * 2;
                        uint16_t e = (uint16_t)(rom.u8(ea) | (rom.u8(ea + 1) << 8));   // LE-слово
                        int idx = e & 0x7FF;
                        bool hf = (e & 0x800) != 0, vf = (e & 0x1000) != 0;
                        size_t toff = 0x53DA0 + (size_t)idx * 32; if (toff + 32 > rom.size()) continue;
                        for (int r = 0; r < 8; ++r) {
                            int sr = vf ? (7 - r) : r; uint8_t row8[8];
                            for (int c = 0; c < 4; ++c) { uint8_t b = rom.u8(toff + (size_t)sr * 4 + c); row8[c*2] = b >> 4; row8[c*2+1] = b & 0xF; }
                            if (hf) for (int k = 0; k < 4; ++k) { uint8_t t = row8[k]; row8[k] = row8[7-k]; row8[7-k] = t; }
                            uint32_t* dst = &fr[(size_t)(80 + ty * 8 + r) * HUD_W + 32 + tx * 8];
                            for (int c = 0; c < 8; ++c) dst[c] = apal[row8[c]];
                        }
                    }
                gd.accoladeFrames.push_back(std::move(fr));
            }
        }
    }
    // ⭐ТИТУЛ-АНИМАЦИЯ: сырые блоки ROM для рендерера title_fx.hpp (лента/лучи/глоу/спрайты).
    // ZTU: те же блоки со сдвигом (графика 100% байт-идентична ZT @0xF5336; таблицы @0x1DDC8, 2542/2560 Б).
    if (a.hasTitle) {
        auto grab = [&](std::vector<uint8_t>& v, size_t a0, size_t a1) {
            v.clear(); if (a1 > rom.size()) a1 = rom.size();
            v.reserve(a1 - a0); for (size_t i = a0; i < a1; ++i) v.push_back(rom.u8(i));
        };
        // ⭐конец 0x125100 РЕЗАЛ последний спрайт-тайл титула (VRAM 0x424 = ROM 0x1250F6..0x125116 —
        // нижние строки буквы S в «OPTIONS» читались нулями; юзер). Правый край → 0x125120 (с запасом).
        grab(gd.titleGfxBlk, (size_t)(0x119B8E + a.titleGfxDelta), (size_t)(0x125120 + a.titleGfxDelta));
        grab(gd.titleTabBlk, (size_t)(0x1D400 + a.titleTabDelta),  (size_t)(0x1DE00 + a.titleTabDelta));
    }
    gd.wall.count = a.wallTiles;
    gd.wall.data.assign(static_cast<size_t>(a.wallTiles) * 512, 0);
    for (size_t i = 0; i < gd.wall.data.size(); ++i) gd.wall.data[i] = rom.u8(a.wallBank + i);

    // Банк объектов/декора (billboard-тайлы 32×32 column-major, тот же формат что стены).
    gd.obj.count = a.objTiles;
    gd.obj.data.assign(static_cast<size_t>(a.objTiles) * 512, 0);
    for (size_t i = 0; i < gd.obj.data.size(); ++i) gd.obj.data[i] = rom.u8(a.objBank + i);

    if (a.hasPanorama) {                                // ZTU: фонов нет (все уровни с потолком) → панорамы пустые
        gd.bgCity  = loadZtBackground(rom, 0);
        gd.bgSpace = loadZtBackground(rom, 1);
    }

    decodeHud(gd, rom, a);              // HUD/кокпит (320×224) + фон меню паузы
    gd.levelNames.clear();             // имена уровней (для меню паузы): 48 × 16 симв, обрезаем пробелы
    if (a.levelNameTable) for (int i = 0; i < 48; ++i) {
        std::string s; for (int c = 0; c < 16; ++c) { char ch = (char)rom.u8(a.levelNameTable + (size_t)i * 16 + c); if (ch >= 32 && ch < 127) s += ch; }
        size_t b = s.find_first_not_of(' '), e = s.find_last_not_of(' ');
        gd.levelNames.push_back(b == std::string::npos ? "" : s.substr(b, e - b + 1));
    }
    if (gd.pauseNames.empty())          // билды без длинного пауза-списка (ZTU): собрать из коротких имён
        for (const std::string& n : gd.levelNames) gd.pauseNames.push_back(n.empty() ? "" : n + " : ");

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

    setActiveCellTable(cellTableForBuild((int)gd.build));   // классификация клеток по билду (ZTU = US-семантика)
    loadZAngLUT(rom, a.angLUTAddr);                         // LUT углов (cos/sin ×256) — для fixed-point рендера стен
    loadZFanLUT(rom, a.angLUTAddr + 0x1000);                // веер лучей $9124 (блок: углы 0x800 + наклоны 0x800 + веер; сигнатура-валидация внутри)
    if (gd.build == Build::ZT || gd.build == Build::ZT_German)
        decodeEnemySprites(rom, gd.wallPal);               // реальные спрайты врагов (дерево 0x1B7B38… пал 0x20F2)
    else if (a.enemyGfx[0])                                // ZTU: свои банки по слотам + свой CLUT, альт-банка FH нет
        decodeEnemySprites(rom, gd.wallPal, a.enemyGfx, a.enemyAltFH, a.spriteClutAddr);

    // ⭐КАРТОЧКИ БОЙЦОВ (экран выбора): 5 × 0x13E ASCII-блоков (рендер ZT sub_0c6934). Формат общий:
    // ZT @0xC6B36, ZTU @0x974D6 (тот же отряд U-RON, стрид/поля идентичны — verified дампом 2026-07-19).
    // Поля: 6 заголовков ×0x12 {0,0x12,0x24,0x36,0x48,0x5A}; 6 строк био ×0x23 {0x6C,0x8F,0xB2,0xD5,0xF8,0x11B}.
    gd.fighters.clear();
    if (a.fighterBase) {
        const size_t FBASE = a.fighterBase, FSTEP = 0x13E;
        auto readField = [&](size_t off, size_t len) {
            std::string s; for (size_t i = 0; i < len; ++i) { char c = (char)rom.u8(off + i); if (c == 0) break; if (c >= 32 && c < 127) s += c; }
            size_t b = s.find_first_not_of(' '), e = s.find_last_not_of(' ');
            return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
        };
        for (int f = 0; f < 5; ++f) {
            size_t base = FBASE + (size_t)f * FSTEP;
            FighterCard fc;
            fc.name = readField(base + 0x00, 0x12); fc.code = readField(base + 0x12, 0x12);
            fc.cls  = readField(base + 0x24, 0x12); fc.height = readField(base + 0x36, 0x12);
            fc.weight = readField(base + 0x48, 0x12); fc.dob = readField(base + 0x5A, 0x12);
            static const size_t bioOff[6] = { 0x6C, 0x8F, 0xB2, 0xD5, 0xF8, 0x11B };
            for (int i = 0; i < 6; ++i) fc.bio[i] = readField(base + bioOff[i], 0x23);
            gd.fighters.push_back(std::move(fc));
        }
    }
    // ⭐СТАРТОВЫЙ ИНВЕНТАРЬ по бойцу (ZT @0xF98, ZTU @0x107E байт-в-байт; level_load: боец×8 байт = 2 пары {id.w, count.w 8.8}).
    if (a.startInvAddr)
        for (int f = 0; f < 5; ++f)
            for (int i = 0; i < 4; ++i) gd.startInv[f][i] = rom.u16(a.startInvAddr + (size_t)f * 8 + i * 2);
    // ⭐ID-КАРТЫ ОТРЯДА U-RON (пауза-экран Tab): таблица (ZT @0x2CC8, ZTU @0x2D8A) = 5 указателей на объекты
    //   блиттера 0x1F72 {u16 ntiles, u16 gfxoff, u16 W, u16 H, nametable W×H BE, тайлы 4bpp @+gfxoff};
    //   палитра HUD (= heldPal: ZT 0x20D2, ZTU 0x2194), idx0 прозрачен (VDP).
    gd.idCards.clear();
    if (a.idCardTable) {
        gd.idCards.assign(5, {});
        for (int fi = 0; fi < 5; ++fi) {
            size_t o = a.idCardTable + (size_t)fi * 4; if (o + 4 > rom.size()) break;
            size_t addr = rom.u32(o);
            if (!addr || addr + 8 >= rom.size()) continue;
            int poff = rom.u16(addr + 2), w = rom.u16(addr + 4), h = rom.u16(addr + 6);
            if (w < 1 || w > 32 || h < 1 || h > 32 || poff != 8 + w * h * 2) continue;   // валидация формата
            gd.idcW = w * 8; gd.idcH = h * 8;
            std::vector<uint32_t>& img = gd.idCards[fi];
            img.assign((size_t)gd.idcW * gd.idcH, 0);
            size_t nt = addr + 8, tiles = addr + (size_t)poff;
            for (int cy = 0; cy < h; ++cy)
                for (int cx = 0; cx < w; ++cx) {
                    int V = rom.u16(nt + (size_t)(cy * w + cx) * 2) & 0x7FF;
                    size_t t = tiles + (size_t)V * 32; if (t + 32 > rom.size()) continue;
                    for (int r = 0; r < 8; ++r)
                        for (int c = 0; c < 4; ++c) {
                            uint8_t b = rom.u8(t + (size_t)r * 4 + c);
                            uint32_t* dst = &img[(size_t)(cy * 8 + r) * gd.idcW + cx * 8 + c * 2];
                            dst[0] = (b >> 4)   ? gd.heldPal.c[b >> 4]   : 0;   // idx0 = прозрачно
                            dst[1] = (b & 0x0F) ? gd.heldPal.c[b & 0x0F] : 0;
                        }
                }
        }
    }
    // Портреты экрана выбора без своей таблицы: ID-карты как портреты (прозрачное → чёрный фон).
    if (!a.portraitTable && !gd.idCards.empty())
        for (int fi = 0; fi < (int)gd.fighters.size() && fi < (int)gd.idCards.size(); ++fi) {
            if (gd.idCards[fi].empty()) continue;
            FighterCard& fc = gd.fighters[fi];
            fc.pw = gd.idcW; fc.ph = gd.idcH;
            fc.portrait.assign(gd.idCards[fi].size(), 0xFF000000u);
            for (size_t i = 0; i < gd.idCards[fi].size(); ++i)
                if (gd.idCards[fi][i]) fc.portrait[i] = gd.idCards[fi][i];
        }
    // ⭐ЭКРАН ВЫБОРА: ZT — прямые адреса; ZTU — тот же блок данных, целиком сдвинутый на selDelta
    // (−0x2F660; DECEASED/шрифт/спрайты/палитры байт-идентичны ZT — verified 2026-07-19).
    if (gd.build == Build::ZT || gd.build == Build::ZTU) {
        const long D = a.selDelta;
        // ПОРТРЕТ-ID-КАРТА (128×96): таблица указателей @0xC634E (5 long) → блоб {u16 tileCount, u16 pixOff,
        //   u16 wTiles=16, u16 hTiles=12, nametable[w*h] BE&0x7FF, тайлы ×32 б @+pixOff}. Палитра @0xC7322 (16 цв, 0BGR).
        uint32_t ppal[16];
        for (int i = 0; i < 16; ++i) { uint16_t c = rom.u16((size_t)(0xC7322 + D) + i * 2);
            int r = ((c >> 1) & 7) * 255 / 7, g = ((c >> 5) & 7) * 255 / 7, b = ((c >> 9) & 7) * 255 / 7;
            ppal[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b; }
        for (int fi = 0; fi < (int)gd.fighters.size(); ++fi) {
            size_t ptr = rom.u32(a.portraitTable + (size_t)fi * 4);
            if (!ptr || ptr + 8 >= rom.size()) continue;
            int cnt = rom.u16(ptr), poff = rom.u16(ptr + 2), w = rom.u16(ptr + 4), h = rom.u16(ptr + 6);
            (void)cnt; if (w < 1 || w > 32 || h < 1 || h > 32) continue;
            size_t nt = ptr + 8, tiles = ptr + (size_t)poff;
            FighterCard& fc = gd.fighters[fi];
            fc.pw = w * 8; fc.ph = h * 8; fc.portrait.assign((size_t)fc.pw * fc.ph, 0xFF000000u);
            for (int cy = 0; cy < h; ++cy)
                for (int cx = 0; cx < w; ++cx) {
                    int V = rom.u16(nt + (size_t)(cy * w + cx) * 2) & 0x7FF;
                    size_t t = tiles + (size_t)V * 32; if (t + 32 > rom.size()) continue;
                    for (int r = 0; r < 8; ++r)
                        for (int c = 0; c < 4; ++c) {
                            uint8_t b = rom.u8(t + (size_t)r * 4 + c);
                            fc.portrait[(size_t)(cy * 8 + r) * fc.pw + cx * 8 + c * 2]     = ppal[b >> 4];
                            fc.portrait[(size_t)(cy * 8 + r) * fc.pw + cx * 8 + c * 2 + 1] = ppal[b & 0x0F];
                        }
                }
        }

        // (ID-карты U-RON декодируются выше — общий per-build блок a.idCardTable.)

        // ⭐DECEASED-графика (ZT панель c6a16, рисуется для мёртвого бойца $FF28EC==0): nametable @0xCA0B2 (40×28),
        //   тайлы @0xC7EF2 база V-1, палитра **2 @0xCA972** (ТЁМНО-КРАСНЫЙ градиент, VERIFIED MAME CRAM; [0]синий но прозрачный).
        {
            uint32_t dpal[16];
            for (int i = 0; i < 16; ++i) { uint16_t c = rom.u16((size_t)(0xCA972 + D) + i * 2);
                int r = ((c >> 1) & 7) * 255 / 7, g = ((c >> 5) & 7) * 255 / 7, b = ((c >> 9) & 7) * 255 / 7;
                dpal[i] = (i == 0) ? 0u : (0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b); }
            const int NW = 40, NH = 28, FW = NW * 8, FH = NH * 8;
            std::vector<uint32_t> full((size_t)FW * FH, 0u);
            int minx = FW, miny = FH, maxx = -1, maxy = -1;
            for (int ty = 0; ty < NH; ++ty)
                for (int tx = 0; tx < NW; ++tx) {
                    int V = rom.u16((size_t)(0xCA0B2 + D) + (size_t)(ty * NW + tx) * 2) & 0x7FF; if (!V) continue;
                    size_t t = (size_t)(0xC7EF2 + D) + (size_t)(V - 1) * 32; if (t + 32 > rom.size()) continue;
                    for (int r = 0; r < 8; ++r)
                        for (int c = 0; c < 4; ++c) {
                            uint8_t b = rom.u8(t + (size_t)r * 4 + c);
                            for (int k = 0; k < 2; ++k) { uint32_t px = dpal[k ? (b & 0x0F) : (b >> 4)];
                                if (!px) continue; int X = tx * 8 + c * 2 + k, Y = ty * 8 + r;
                                full[(size_t)Y * FW + X] = px;
                                if (X < minx) minx = X; if (X > maxx) maxx = X; if (Y < miny) miny = Y; if (Y > maxy) maxy = Y; }
                        }
                }
            if (maxx >= minx && maxy >= miny) {
                gd.deceasedW = maxx - minx + 1; gd.deceasedH = maxy - miny + 1;
                gd.deceasedX = minx; gd.deceasedY = miny;   // native-позиция для точной посадки надписи (как ZT plane A)
                gd.deceasedGfx.assign((size_t)gd.deceasedW * gd.deceasedH, 0u);
                for (int y = 0; y < gd.deceasedH; ++y)
                    for (int x = 0; x < gd.deceasedW; ++x)
                        gd.deceasedGfx[(size_t)y * gd.deceasedW + x] = full[(size_t)(miny + y) * FW + (minx + x)];
            }
        }

        // ⭐ФИРМЕННЫЙ ШРИФТ ЭКРАНА ВЫБОРА (таблица глифов @0xC7196: 4б/символ верх+низ тайл; тайлы 0x125116 VRAM-база 0x4B0;
        //   палитра 3 @0xC7302 сине-белый градиент). Извлекаем ASCII 0x20..0x7F, 8×16 ARGB (индекс 0 прозрачный).
        {
            uint32_t fp[16];
            for (int i = 0; i < 16; ++i) { uint16_t c = rom.u16((size_t)(0xC7302 + D) + i * 2);
                int r = ((c >> 1) & 7) * 255 / 7, g = ((c >> 5) & 7) * 255 / 7, b = ((c >> 9) & 7) * 255 / 7;
                fp[i] = (i == 0) ? 0u : (0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b); }
            const size_t LUT = (size_t)(0xC7196 + D), GFX = a.fontBigBank; const int VBASE = 0x4B0;   // тайлы = банк Font_grph билда
            for (int ch = 0x20; ch < 0x80; ++ch) {
                int e = ch - 0x20;
                int tv[2] = { rom.u16(LUT + (size_t)e * 4) & 0x7FF, rom.u16(LUT + (size_t)e * 4 + 2) & 0x7FF };
                for (int half = 0; half < 2; ++half) {
                    int tile = tv[half]; if (tile < VBASE) continue;
                    size_t data = GFX + (size_t)(tile - VBASE) * 32; if (data + 32 > rom.size()) continue;
                    for (int r = 0; r < 8; ++r)
                        for (int c = 0; c < 4; ++c) {
                            uint8_t b = rom.u8(data + (size_t)r * 4 + c);
                            gd.selFont.glyph[e][(half * 8 + r) * 8 + c * 2]     = fp[b >> 4];
                            gd.selFont.glyph[e][(half * 8 + r) * 8 + c * 2 + 1] = fp[b & 0x0F];
                            gd.selFont.idx[e][(half * 8 + r) * 8 + c * 2]       = b >> 4;   // сырые индексы (перекраска)
                            gd.selFont.idx[e][(half * 8 + r) * 8 + c * 2 + 1]   = b & 0x0F;
                        }
                }
            }
            gd.selFont.have = true;
        }

        // ⭐ФОН + СПРАЙТЫ экрана выбора: backdrop = палитра0[0] @0xC7322 (светло-серый); стрелка @0xC76A2, push-start @0xC76F2
        //   (nametable-порядок → пиксели 0xC7342+idx*32; ZT 1f7da), палитра 1 @0xC7E72 (жёлтая). Стрелка↓ = vflip стрелки.
        {
            uint16_t bg = rom.u16((size_t)(0xC7322 + D));
            gd.selBg = 0xFF000000u | ((uint32_t)(((bg >> 1) & 7) * 255 / 7) << 16)
                     | ((uint32_t)(((bg >> 5) & 7) * 255 / 7) << 8) | (uint32_t)(((bg >> 9) & 7) * 255 / 7);
            uint32_t p1[16];
            for (int i = 0; i < 16; ++i) { uint16_t c = rom.u16((size_t)(0xC7E72 + D) + i * 2);
                int r = ((c >> 1) & 7) * 255 / 7, g = ((c >> 5) & 7) * 255 / 7, b = ((c >> 9) & 7) * 255 / 7;
                p1[i] = (i == 0) ? 0u : (0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b); }
            const size_t PIX = (size_t)(0xC7342 + D);
            auto buildSpr = [&](size_t nt, int w, int h, std::vector<uint32_t>& out) {
                out.assign((size_t)w * 8 * h * 8, 0u);
                for (int cw = 0; cw < w; ++cw)                            // SAT column-major
                    for (int ch = 0; ch < h; ++ch) {
                        int idx = rom.u16(nt + (size_t)(cw * h + ch) * 2);
                        size_t data = PIX + (size_t)idx * 32; if (data + 32 > rom.size()) continue;
                        for (int r = 0; r < 8; ++r)
                            for (int c = 0; c < 4; ++c) {
                                uint8_t b = rom.u8(data + (size_t)r * 4 + c);
                                out[(size_t)(ch * 8 + r) * (w * 8) + cw * 8 + c * 2]     = p1[b >> 4];
                                out[(size_t)(ch * 8 + r) * (w * 8) + cw * 8 + c * 2 + 1] = p1[b & 0x0F];
                            }
                    }
            };
            buildSpr((size_t)(0xC76A2 + D), 3, 4, gd.selArrow);   // стрелка 24×32
            buildSpr((size_t)(0xC76F2 + D), 4, 4, gd.selPush);    // "PUSH START TO SELECT" 32×32
        }

        // ⭐СТРЕЛКА-КУРСОР МЕНЮ ОПЦИЙ (ROM sub_10B4C8: спрайт tile 0x4AE верх / 0x4AF низ, палитра line0 0x10CBDC).
        //   Тайлы лежат линейно @0x10CB7C (2 тайла по 32 Б). Адреса ZT-релизные → декодируем только для ZT.
        if (gd.build == Build::ZT) {
            uint32_t ap[16];
            for (int i = 0; i < 16; ++i) { uint16_t c = rom.u16((size_t)0x10CBDC + i * 2);
                int r = ((c >> 1) & 7) * 255 / 7, g = ((c >> 5) & 7) * 255 / 7, b = ((c >> 9) & 7) * 255 / 7;
                ap[i] = (i == 0) ? 0u : (0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b); }
            // ⭐Спрайт 16×8 = 2 тайла ГОРИЗОНТАЛЬНО (SAT size d1=0x400 → w=2,h=1): 0x4AE=левая половина, 0x4AF=правая.
            //   Стрелка «────►» вправо; хранение 16 шириной × 8 высотой.
            for (int half = 0; half < 2; ++half) {               // half0 = левый тайл, half1 = правый
                size_t data = (size_t)0x10CB7C + (size_t)half * 32; if (data + 32 > rom.size()) continue;
                for (int r = 0; r < 8; ++r)
                    for (int c = 0; c < 4; ++c) {
                        uint8_t b = rom.u8(data + (size_t)r * 4 + c);
                        gd.optArrow[r * 16 + half * 8 + c * 2]     = ap[b >> 4];
                        gd.optArrow[r * 16 + half * 8 + c * 2 + 1] = ap[b & 0x0F];
                    }
            }
            gd.optArrowHave = true;
        }

        // ⭐КУРСОР-СКОБКА ЭКРАНА ПАРОЛЯ (ROM 0x581BA: тайл 0x243 @0x12ea86, 8×8, палитра линии 2 = 0x1CDAE).
        if (gd.build == Build::ZT) {
            uint32_t cp[16];
            for (int i = 0; i < 16; ++i) { uint16_t c = rom.u16((size_t)0x1CDAE + i * 2);
                int r = ((c >> 1) & 7) * 255 / 7, g = ((c >> 5) & 7) * 255 / 7, b = ((c >> 9) & 7) * 255 / 7;
                cp[i] = (i == 0) ? 0u : (0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b); }
            size_t data = 0x12ea86;
            if (data + 32 <= rom.size()) {
                for (int r = 0; r < 8; ++r)
                    for (int c = 0; c < 4; ++c) {
                        uint8_t b = rom.u8(data + (size_t)r * 4 + c);
                        gd.pwCursor[r * 8 + c * 2]     = cp[b >> 4];
                        gd.pwCursor[r * 8 + c * 2 + 1] = cp[b & 0x0F];
                    }
                gd.pwCursorHave = true;
            }
        }

        // (стартовый инвентарь — общий per-build блок a.startInvAddr выше.)

        // ⭐ЗАСТАВКИ/БРИФИНГИ (ZT @0xCB1E4): тексты (записи 0x25 ASCII, пустая=конец) + фоны (тайлмап 40×28 + тайлы + палитра).
        auto briefLines = [&](size_t addr) {
            std::vector<std::string> out; if (!addr) return out; size_t a = addr;
            if (gd.build == Build::ZTU) {
                // ⭐ZTU-рендерер 0x9BC92 (verified дизасмом): строки = ПОСЛЕДОВАТЕЛЬНЫЕ NUL-терминированные
                // C-строки ПЕРЕМЕННОЙ длины (draw до NUL; следующая сразу за ним; `cmpi.b #0,(a0)` =
                // пустая строка → конец блока). Фикс-стрид 0x25 здесь ломал victory-текст мода (рваные
                // длины строк → у каждой съедалась 1-я буква: «iko Interractive», «RTISTS:»). Хвост-доскролл
                // 0x9DED4 (17 пустых строк, скроллер прыгает туда после НЕ-последнего блока) не читаем —
                // порт доскролливает сам (BriefingState::done).
                for (int ln = 0; ln < 200 && rom.u8(a) != 0; ++ln) {
                    std::string s;
                    for (; rom.u8(a) != 0; ++a) { char ch = (char)rom.u8(a); if (ch >= 32 && ch < 127) s += ch; }
                    ++a;                                       // съесть NUL строки
                    while (!s.empty() && s.back() == ' ') s.pop_back();
                    out.push_back(s);
                }
                return out;
            }
            for (int ln = 0; ln < 200; ++ln) {                 // ⭐ZT: фикс-записи 0x25 до 0-терминатора (ROM cb288): intro=31, victory=117 С ТИТРАМИ (CREDITS @0xCC429)
                if (rom.u8(a) == 0) break;                 // пустая запись = конец блока
                std::string s; for (int c = 0; c < 0x25; ++c) { char ch = (char)rom.u8(a + c); if (ch == 0) break; if (ch >= 32 && ch < 127) s += ch; }
                while (!s.empty() && s.back() == ' ') s.pop_back();   // trailing trim (leading пробелы = выравнивание, сохраняем)
                out.push_back(s); a += 0x25;
            }
            return out;
        };
        // Фон = MD nametable 40×28 с 64-ЦВЕТ палитрой (линии 0-1 из screen-pal, линии 2-3 из общей 0xCD47A);
        //   тайл = gfx + idx*32 (idx = e&0x7FF, НЕ V-1!); палитра-линия = (e>>13)&3 → цвет pal[line*16+nib]; h/v-флип. (ztextractor _compose)
        auto cramCol = [&](uint16_t c) { return 0xFF000000u | ((uint32_t)(((c >> 1) & 7) * 255 / 7) << 16)
                       | ((uint32_t)(((c >> 5) & 7) * 255 / 7) << 8) | (uint32_t)(((c >> 9) & 7) * 255 / 7); };
        auto briefBg = [&](size_t tm, size_t gfx, size_t paloff, GameData::Briefing& b) {
            uint32_t pal[64];
            for (int i = 0; i < 48; ++i) pal[i]      = cramCol(rom.u16(paloff + i * 2));   // линии 0-2: screen-палитра (obёртка грузит 1-2 линии)
            for (int i = 0; i < 16; ++i) pal[48 + i] = cramCol(rom.u16(a.fontBigPal + i * 2));  // линия 3: палитра ТЕКСТА (ZT 0xCD47A, ZTU 0x9E2B6 — verified дизасм 0x9B4DE → CRAM ff07d6)
            for (int i = 0; i < 16; ++i) b.textCol[i] = pal[48 + i];                        // палитра текста = линия 3 (0xCD47A[0-15])
            if (!tm || !gfx || !paloff) return;
            b.bgW = 320; b.bgH = 224; b.bg.assign(320 * 224, pal[0]);
            for (int ty = 0; ty < 28; ++ty)
                for (int tx = 0; tx < 40; ++tx) {
                    int e = rom.u16(tm + (size_t)(ty * 40 + tx) * 2), idx = e & 0x7FF, line = (e >> 13) & 3;
                    bool hf = e & 0x800, vf = e & 0x1000;
                    size_t data = gfx + (size_t)idx * 32; if (data + 32 > rom.size()) continue;
                    for (int r = 0; r < 8; ++r) { int sr = vf ? 7 - r : r;
                        for (int c = 0; c < 8; ++c) { int sc = hf ? 7 - c : c;
                            uint8_t bb = rom.u8(data + (size_t)sr * 4 + (sc >> 1));
                            uint8_t nib = (sc & 1) ? (bb & 0x0F) : (bb >> 4);
                            b.bg[(size_t)(ty * 8 + r) * 320 + tx * 8 + c] = pal[line * 16 + nib];
                        } }
                }
        };
        struct BR { size_t text, tm, gfx, pal; };
        static const BR brsZt[10] = {  // {текст, тайлмап, тайлы, палитра} — VERIFIED (адреса SCREEN_BANKS ztextractor)
            {0xCB3A8, 0xD54BA,  0xCD49A,  0xD5D7A},    // 0 intro   — монитор/солдат (screen 0.1)
            {0xCB824, 0xDD63A,  0xD5DFA,  0xDDEFA},    // 1 mission — космос-станция (screen 0.2)
            {0xCBB2F, 0xDD63A,  0xD5DFA,  0xDDEFA},    // 2 decoy   — космос (screen 1.1)
            {0xCBCC7, 0xE679A,  0xDDF7A,  0xE705A},    // 3 central — Central Command (screen 1.2)
            {0xCBECF, 0xEF2FA,  0xE70DA,  0xEFBBA},    // 4 subbase — sub-basement (screen 2)
            {0xCBFD3, 0x10A7DA, 0x10201A, 0x10B09A},   // 5 victory — ПОБЕДА (screen 4, пришелец)
            {0xCB9E1, 0xDD63A,  0xD5DFA,  0xDDEFA},    // 6 badend1 — зона0 (screen 5, космос)
            {0xCBDF0, 0xE679A,  0xDDF7A,  0xE705A},    // 7 badend2 — зона1/2 (screen 5)
            {0,       0xF84DA,  0xEFC3A,  0xF8D9A},    // 8 взрыв   — финал зон1/2 (cb0be, БЕЗ текста, held-экран)
            {0xCBDF0, 0x1016DA, 0xF8E1A,  0x101F9A},   // 9 EXIT    — зона2 badend2 (текст 0xCBDF0 на EXIT-фоне)
        };
        // ⭐ZTU-заставки: тексты = КОПИИ ZT со сдвигом −0x2F664 (байт-в-байт), КРОМЕ двух авторских
        // текстов мода: subway-intro 0x9C96F («Aliens ambushed the subway…») и victory 0x9CABD
        // («You've done it!…Relocate to the main base»). Диспетчер playCutscene @0x9B3EE: индекс со
        // стека → таблица переходов @0x9BD2C (6 функций-последовательностей, verified дизасмом):
        //   [0] intro(0x9BD44)+mission(0x9C1C0) @9B490   [1] decoy(0x9C4CB)+central(0x9C663) @9B5B6
        //   [2] subway(0x9C96F) @9B6DC — ОДИН экран      [3] intro-only(0x9BD44) @9B778
        //   [4] VICTORY(0x9CABD) @9B81E — ОДИН экран      [5] badend 0x9C37D+0x9C78C+0x9C78C+взрыв @9B8BA
        // ⭐Игровой ФЛОУ (verified): СТАРТ игры = playCutscene(2)=subway (0xEB8: move #2,d0), НЕ intro+mission;
        //   ПОБЕДА = playCutscene(4)=victory (0x1B44: move #4,d0) с текстом 0x9CABD; мод одноэпизодный
        //   (0x1B62: ep+1==1 → ребут 0x976). Таблица ниже индексируется НАШИМ BriefId (не индексом диспетчера).
        static const BR brsZtu[10] = {
            {0x9BD44, 0xA62F6, 0x9E2D6, 0xA6BB6},      // 0 intro   — солдат (ZT-текст интро)
            {0x9C1C0, 0xAE476, 0xA6C36, 0xAED36},      // 1 mission — станция метро
            {0x9C4CB, 0xAE476, 0xA6C36, 0xAED36},      // 2 decoy   — станция
            {0x9C663, 0xB75D6, 0xAEDB6, 0xB7E96},      // 3 central — крыша
            {0x9C96F, 0xC0136, 0xB7F16, 0xC09F6},      // 4 subway  — НОВЫЙ текст мода (ambush); СТАРТ игры ZTU
            {0x9CABD, 0xDB616, 0xD2E56, 0xDBED6},      // 5 victory — ⭐текст мода 0x9CABD (F5@0x9B81E, playCutscene(4))
            {0x9C78C, 0xAE476, 0xA6C36, 0xAED36},      // 6 badend1 — станция + ZT badend-текст
            {0x9C78C, 0xB75D6, 0xAEDB6, 0xB7E96},      // 7 badend2 — крыша + ZT badend-текст
            {0,       0xC9316, 0xC0A76, 0xC9BD6},      // 8 взрыв   — БЕЗ текста
            {0x9C78C, 0xB75D6, 0xAEDB6, 0xB7E96},      // 9 = слот 7
        };
        const BR* brs = (gd.build == Build::ZTU) ? brsZtu : brsZt;
        gd.briefings.clear();
        for (int i = 0; i < 10; ++i) { GameData::Briefing b; b.lines = briefLines(brs[i].text);
            briefBg(brs[i].tm, brs[i].gfx, brs[i].pal, b); gd.briefings.push_back(std::move(b)); }
    }

    // ⭐ПОЕЗД МЕТРО ZTU (FSM 0xDE8DA; VERIFIED 2026-07-24, см. train.hpp): скрипты текстур вагонов —
    // ROM 0xDE578..0xDE7E0 (прибытие @0, отъезд @0x138; по 8 байт/тик в texorder face0 клеток 0x67..0x6E).
    if (gd.build == Build::ZTU) {
        gd.trainScript.resize(0x268);
        for (size_t i = 0; i < 0x268; ++i) gd.trainScript[i] = rom.u8(0xDE578 + i);
    }

    gd.valid = !gd.levels.empty() && gd.levels[0].valid();
    return gd.valid;
}
