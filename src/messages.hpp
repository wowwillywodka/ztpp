// ztpp — HUD-СООБЩЕНИЯ как в оригинале ZT (подбор/этаж/зачистка/мало патронов/HP и пр.) + ОЗВУЧКА.
//
// Дизасм (text.asm): очередь FUN_0001e0fa, enqueue FUN_0001e0c2 (лимит 0x10=16), менеджер 0001e2cc.
// Рендер FUN_0001e0fa@0x1e204: 4 ряда VRAM 0xCA04/0xCA84/0xCB04/0xCB84 → план A база 0xC000, ширина 64
// тайла → тайл (col 2, row 20..23) = ПИКСЕЛЬ (16,160) в экране 320×224 = НИЖНИЙ-ЛЕВЫЙ угол.
// Каждый ряд = 9 символов → СООБЩЕНИЕ = 4 ряда × 9 = 36 символов, шрифт Letters (через таблицу 0x1e04c).
// Очередь: показ по таймеру, по одному, FIFO. Тексты — БАЙТ-В-БАЙТ из ROM (таблица предметов 0x1e326,
// одиночные 0x1e5f0/0x1e61e/0x1e64c/0x1e67a/0x1e6f8/0x1e726/0x1e754/0x1e7b8/0x1e7ea).
//
// ⭐ОЗВУЧКА СООБЩЕНИЙ (2026-07-16, дизасм 1e288/1e2cc + меню опций «Voice:» 10b6ac): за 36 символами текста
// в ROM лежит ХВОСТ = пары слов (длительность_тиков, sound-id), терминатор 0xFFFF. Речь КОМПОЗИТНАЯ из
// слов-сэмплов: «HAND»(0x5a)+«GUN»(0x59)+«COLLECTED»(0x34); цифры 0x47..0x5f, «ENTERING»=0x53 и т.д.
// Все голос-id = SFX type3 (DAC-сэмпл, p1 через своп-ремап dacSampleIdx). Механика ROM: при ПОКАЗЕ строки
// (гейт «Voice: On» = бит1 $FF0014) играется 1-й сэмпл, таймер = 1-я длительность; по истечении — следующая
// пара (менеджер 1e2cc, флаг -58ea). Если предыдущая реплика ЕЩЁ ЗВУЧИТ — голос нового сообщения ПРОПУСКАЕТСЯ
// (1e294 tst -58ea), не прерывает. Дефолт ROM: $FF0014 не инициализируется → Voice OFF (вкл. в Options).
#pragma once
#include "ui.hpp"        // drawText (Letters/g_uiFont)
#include "tuning.hpp"    // voicePace (темп озвучки, меню/ini)
#include "render/sound.hpp"  // snd::playSfx (голос-сэмплы DAC)
#include <deque>
#include <cstring>

// Строки 36 символов = 4 ряда × 9 (как раскладка в ROM). Пустые места — пробелы.
namespace ztmsg {
// ── ОЗВУЧКА: пары хвоста (длительность в родных тиках ZT ~15fps, sound-id DAC-сэмпла). Терминатор {0,0}.
// Данные БАЙТ-В-БАЙТ из ROM (слова за 36-символьным текстом). Словарь: 0x53=ENTERING 0x56=FLOOR 0x3d=LEVEL
// 0x34=COLLECTED 0x47..0x4a=one..four 0x4b=five 0x4e=six 0x4f=seven 0x5b=eight 0x5f=nine 0x4c=fifty
// 0x91=sixty 0x92=one(hundred) 0x5a=HAND 0x59=GUN 0x57=GRENADE и т.д.
struct VP { uint8_t dur, id; };
// Озвучка (ROM: пункт Options «Voice:», бит1 $FF0014; дефолт ROM = OFF, порт = ON как QoL): вкл/выкл и темп
// через ползунок voicePace() (tuning.hpp, меню стр.4 «Voice pace», 0 = выкл).
inline bool voiceEnabled() { return voicePace() > 0.01; }
//                        "ряд0----.ряд1----.ряд2----.ряд3----"  (по 9 символов)
inline const char* FLOOR_UP        = "STEPPING ONE FLOORUP                ";
inline const char* FLOOR_DOWN      = "STEPPING ONE FLOORDOWN              ";
inline const char* FLOOR_SECURED   = "FLOOR    SECURED                    ";
inline const char* FLOOR_NOT_SEC   = "FLOOR NOTSECURED                    ";
inline const char* ZERO_ENEMIES    = "ZERO     ENEMIES  REMAINING         ";
inline const char* PROCEED_NEXT    = "PROCEED  TO NEXT  LEVEL             ";
inline const char* AMMO_LOW        = "AMMU-    NITION ISLOW               ";
inline const char* HEALTH_LOW      = "HEALTH   CONDITIONLOW               ";
inline const char* HEALTH_CRITICAL = "HEALTH   CONDITIONCRITICAL          ";
inline const char* CONNECTION_LOST = "2 PLAYER CONNEC-  TION LOST         ";
inline const char* MEDIPACK        = "MEDIPACK COLLECTED                  ";  // ct0x25 (+HP, не оружие)

// Сообщение «<предмет> COLLECTED» по item-id (= celltype−0x18, как ITEMS[] в weapons.hpp).
// Тексты из ROM-таблицы 0x1e326 (раскладка 4×9). id0 (FIRE) — не пикап.
inline const char* itemCollected(int id) {
    switch (id) {
        case 1:  return "BIO      SCANNER  COLLECTED         "; // ct0x19
        case 2:  return "MINE     COLLECTED                  "; // ct0x1A
        case 3:  return "BULLET   PROOF    VEST     COLLECTED"; // ct0x1B
        case 4:  return "FIRE     EXTIN-   GUISHER  COLLECTED"; // ct0x1C
        case 5:  return "FIRE     PROOF    SUIT     COLLECTED"; // ct0x1D
        case 6:  return "FLASH    LIGHT    COLLECTED         "; // ct0x1E
        case 7:  return "HAND     GRENADE  COLLECTED         "; // ct0x1F
        case 8:  return "HANDGUN  COLLECTED                  "; // ct0x20
        case 9:  return "NIGHT    VISION   COLLECTED         "; // ct0x21
        case 10: return "LASER    AIMED GUNCOLLECTED         "; // ct0x22
        case 11: return "ROCKET   LAUNCHER COLLECTED         "; // ct0x23
        case 12: return "SHOTGUN  COLLECTED                  "; // ct0x24
        case 13: return "FLAME    THROWER  COLLECTED         "; // weapon-id 13 = огнемёт (ПИКАП ct0x82, НЕ ct0x25!)
        case 14: return "PULSE    LASER    COLLECTED         "; // ct0x36 (ztpp id14)
        default: return nullptr;
    }
}

// ── ХВОСТЫ ОЗВУЧКИ (ROM: слова за текстом, см. шапку). Именованные сообщения: ──
inline const VP V_MEDIPACK[] = {{40,0x3f},{30,0x34},{0,0}};
inline const VP V_FLOOR_UP[] = {{60,0x43},{30,0x44},{0,0}};
inline const VP V_FLOOR_DN[] = {{60,0x43},{30,0x37},{0,0}};
inline const VP V_SECURED[]  = {{17,0x56},{30,0xa0},{0,0}};
inline const VP V_NOT_SEC[]  = {{17,0x56},{20,0x9d},{30,0xa0},{0,0}};
inline const VP V_ZERO_EN[]  = {{25,0xa1},{25,0x9c},{30,0x9f},{0,0}};
inline const VP V_PROCEED[]  = {{65,0x9e},{30,0x3d},{0,0}};
inline const VP V_AMMO[]     = {{45,0x60},{55,0x45},{30,0x3e},{0,0}};
inline const VP V_HP_LOW[]   = {{45,0x3c},{30,0x3e},{0,0}};
inline const VP V_HP_CRIT[]  = {{45,0x3c},{30,0x35},{0,0}};
// Предметы id1..14 (таблица подбора 0x1ef14):
inline const VP V_ITEM[15][4] = {
    /*0 */ {{0,0}},
    /*1 */ {{20,0x32},{30,0x42},{30,0x34},{0,0}},   // BIO SCANNER
    /*2 */ {{20,0x40},{30,0x34},{0,0}},             // MINE
    /*3 */ {{60,0x33},{30,0x34},{0,0}},             // BULLET PROOF VEST
    /*4 */ {{55,0x38},{30,0x34},{0,0}},             // FIRE EXTINGUISHER
    /*5 */ {{70,0x3a},{30,0x34},{0,0}},             // FIRE PROOF SUIT
    /*6 */ {{30,0x39},{30,0x34},{0,0}},             // FLASHLIGHT
    /*7 */ {{15,0x5a},{30,0x57},{30,0x34},{0,0}},   // HAND GRENADE
    /*8 */ {{15,0x5a},{17,0x59},{30,0x34},{0,0}},   // HANDGUN
    /*9 */ {{40,0x41},{30,0x34},{0,0}},             // NIGHT VISION
    /*10*/ {{35,0x5d},{17,0x59},{30,0x34},{0,0}},   // LASER AIMED GUN
    /*11*/ {{20,0x63},{30,0x5e},{30,0x34},{0,0}},   // ROCKET LAUNCHER
    /*12*/ {{40,0x64},{30,0x34},{0,0}},             // SHOTGUN
    /*13*/ {{45,0x55},{30,0x34},{0,0}},             // FLAME THROWER
    /*14*/ {{15,0x61},{25,0x5c},{30,0x34},{0,0}},   // PULSE LASER
};
// Этажи (таблица 0x1ee94): ep0 корабль, ep1 небоскрёб (165→151).
inline const VP V_EP0[16][5] = {
    {{30,0x53},{35,0x51},{20,0x3d},{30,0x47},{0,0}}, {{30,0x53},{35,0x51},{20,0x3d},{30,0x48},{0,0}},
    {{30,0x53},{40,0x50},{30,0x47},{0,0}},           {{30,0x53},{40,0x52},{25,0x3d},{30,0x47},{0,0}},
    {{30,0x53},{40,0x52},{25,0x3d},{30,0x48},{0,0}}, {{30,0x53},{40,0x52},{25,0x3d},{30,0x49},{0,0}},
    {{30,0x53},{40,0x52},{25,0x3d},{30,0x4a},{0,0}}, {{30,0x53},{35,0x58},{25,0x3d},{30,0x47},{0,0}},
    {{30,0x53},{35,0x58},{25,0x3d},{30,0x48},{0,0}}, {{30,0x53},{35,0x58},{25,0x3d},{30,0x49},{0,0}},
    {{30,0x53},{40,0x50},{30,0x48},{0,0}},           {{63,0x54},{25,0x3d},{30,0x47},{0,0}},
    {{63,0x54},{25,0x3d},{30,0x48},{0,0}},           {{63,0x54},{25,0x3d},{30,0x48},{0,0}},
    {{63,0x54},{25,0x3d},{30,0x48},{0,0}},           {{63,0x54},{25,0x3d},{30,0x48},{0,0}},
};
inline const VP V_EP1[16][6] = {
    {{30,0x53},{20,0x56},{40,0x92},{20,0x91},{30,0x4b},{0,0}}, {{30,0x53},{20,0x56},{40,0x92},{25,0x91},{30,0x4a},{0,0}},
    {{30,0x53},{20,0x56},{40,0x92},{25,0x91},{30,0x49},{0,0}}, {{30,0x53},{20,0x56},{40,0x92},{25,0x91},{30,0x48},{0,0}},
    {{30,0x53},{20,0x56},{40,0x92},{20,0x91},{30,0x47},{0,0}}, {{30,0x53},{20,0x56},{40,0x92},{20,0x91},{0,0}},
    {{30,0x53},{20,0x56},{40,0x92},{30,0x4c},{30,0x5f},{0,0}}, {{30,0x53},{20,0x56},{40,0x92},{30,0x4c},{30,0x5b},{0,0}},
    {{30,0x53},{20,0x56},{40,0x92},{30,0x4c},{30,0x4f},{0,0}}, {{30,0x53},{20,0x56},{40,0x92},{30,0x4c},{30,0x4e},{0,0}},
    {{30,0x53},{20,0x56},{40,0x92},{30,0x4c},{30,0x4b},{0,0}}, {{30,0x53},{20,0x56},{40,0x92},{30,0x4c},{30,0x4a},{0,0}},
    {{30,0x53},{20,0x56},{40,0x92},{30,0x4c},{30,0x49},{0,0}}, {{30,0x53},{20,0x56},{40,0x92},{30,0x4c},{30,0x48},{0,0}},
    {{30,0x53},{20,0x56},{40,0x92},{27,0x4c},{30,0x47},{0,0}}, {{30,0x53},{20,0x56},{40,0x92},{27,0x4c},{30,0x47},{0,0}},
};

// «ENTERING ‹название› LEVEL n» при смене этажа — ТОЛЬКО эпизоды 0/1 (ROM level_load: если -0x58e4≠2 →
// таблица @0x1ee94[ep*16+floor]). Эпизод 2 (третий) → generic «STEPPING ONE FLOOR UP/DOWN» (см. main).
// Тексты байт-в-байт из ROM (36 симв 4×9). ep 0-based, floor = индекс этажа 0..15.
inline const char* enteringFloor(int ep, int floor) {
    if (floor < 0 || floor > 15) return nullptr;
    static const char* EP0[16] = {   // эпизод 1: корабль/платформа U.S.S.
        "ENTERING DOCKING  BAY LEVEL1        ", "ENTERING DOCKING  BAY LEVEL2        ",
        "ENTERING BRIDGE   LEVEL 1           ", "ENTERING ENGINE   LEVEL 1           ",
        "ENTERING ENGINE   LEVEL 2           ", "ENTERING ENGINE   LEVEL 3           ",
        "ENTERING ENGINE   LEVEL 4           ", "ENTERING GREEN    HOUSE    LEVEL 1  ",
        "ENTERING GREEN    HOUSE    LEVEL 2  ", "ENTERING GREEN    HOUSE    LEVEL 3  ",
        "ENTERING BRIDGE   LEVEL 2           ", "ENTERING REACTOR  LEVEL 1           ",
        "ENTERING REACTOR  LEVEL 2           ", "ENTERING REACTOR  LEVEL 2           ",
        "ENTERING REACTOR  LEVEL 2           ", "ENTERING REACTOR  LEVEL 2           ",
    };
    static const char* EP1[16] = {   // эпизод 2: небоскрёб, отсчёт этажей ВНИЗ 165→151
        "ENTERING FLOOR 165                  ", "ENTERING FLOOR 164                  ",
        "ENTERING FLOOR 163                  ", "ENTERING FLOOR 162                  ",
        "ENTERING FLOOR 161                  ", "ENTERING FLOOR 160                  ",
        "ENTERING FLOOR 159                  ", "ENTERING FLOOR 158                  ",
        "ENTERING FLOOR 157                  ", "ENTERING FLOOR 156                  ",
        "ENTERING FLOOR 155                  ", "ENTERING FLOOR 154                  ",
        "ENTERING FLOOR 153                  ", "ENTERING FLOOR 152                  ",
        "ENTERING FLOOR 151                  ", "ENTERING FLOOR 151                  ",
    };
    if (ep == 0) return EP0[floor];
    if (ep == 1) return EP1[floor];
    return nullptr;                  // эпизод 2 → generic (обрабатывается снаружи)
}
inline const VP* enteringFloorVoice(int ep, int floor) {
    if (floor < 0 || floor > 15) return nullptr;
    if (ep == 0) return V_EP0[floor];
    if (ep == 1) return V_EP1[floor];
    return nullptr;
}
// Озвучка по указателю ТЕКСТА (все тексты — inline-синглтоны, сравнение указателей корректно).
// Так push() сам находит реплику — вызывающим не надо тянуть второй аргумент.
inline const VP* voiceFor(const char* s) {
    if (s == FLOOR_UP)        return V_FLOOR_UP;
    if (s == FLOOR_DOWN)      return V_FLOOR_DN;
    if (s == FLOOR_SECURED)   return V_SECURED;
    if (s == FLOOR_NOT_SEC)   return V_NOT_SEC;
    if (s == ZERO_ENEMIES)    return V_ZERO_EN;
    if (s == PROCEED_NEXT)    return V_PROCEED;
    if (s == AMMO_LOW)        return V_AMMO;
    if (s == HEALTH_LOW)      return V_HP_LOW;
    if (s == HEALTH_CRITICAL) return V_HP_CRIT;
    if (s == MEDIPACK)        return V_MEDIPACK;
    for (int i = 1; i <= 14; ++i) if (s == itemCollected(i)) return V_ITEM[i];
    for (int f = 0; f < 16; ++f) { if (s == enteringFloor(0, f)) return V_EP0[f];
                                   if (s == enteringFloor(1, f)) return V_EP1[f]; }
    return nullptr;                 // CONNECTION_LOST и прочие — без озвучки (как ROM)
}
}

struct HudMessages {
    struct Msg { char row[4][10]; const ztmsg::VP* voice = nullptr; };  // 4 ряда по ≤9 симв + реплика
    std::deque<Msg> queue;
    int  timer = 0;                      // кадров до смены текущего сообщения
    int  showFrames = 70;               // длительность показа (ставится из main по frameLimit)
    int  fps = 15;                       // игровой frameLimit (для масштаба родных тиков озвучки, ZT=15)
    // Активная РЕПЛИКА (ROM буфер -$7774 + флаг -$58ea): идёт независимо от показа/смены сообщений.
    const ztmsg::VP* vSeq = nullptr;    // последовательность пар; nullptr = тишина
    int vIdx = 0, vTimer = 0;

    // Разобрать 36-символьную строку (4×9) в сообщение и поставить в очередь (лимит 16, как ZT 0x10).
    // ⭐Произвольный текст (сервисные сообщения порта: QUICKSAVE и т.п.): перенос по словам в ряды ≤9 симв.
    void pushRaw(const char* txt) {
        Msg m{}; std::memset(m.row, 0, sizeof m.row);
        int r = 0, c = 0;
        for (const char* p = txt; *p && r < 4; ) {
            const char* w = p; int wl = 0; while (w[wl] && w[wl] != ' ') ++wl;
            if (c > 0 && c + 1 + wl > 9) { ++r; c = 0; if (r >= 4) break; }
            if (c > 0) m.row[r][c++] = ' ';
            for (int i = 0; i < wl && c < 9; ++i) m.row[r][c++] = w[i];
            m.row[r][c] = 0;
            p = w + wl; while (*p == ' ') ++p;
        }
        if (queue.size() < 16) queue.push_back(m);
        if (queue.size() == 1) timer = showFrames;
    }
    void push(const char* s36) {
        if (!s36 || queue.size() >= 16) return;
        size_t len = std::strlen(s36);
        Msg m{};
        m.voice = ztmsg::voiceFor(s36);              // реплика по указателю текста (может быть nullptr)
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 9; ++c) {
                size_t i = (size_t)r * 9 + c;
                m.row[r][c] = (i < len && s36[i]) ? s36[i] : ' ';
            }
            m.row[r][9] = 0;
            for (int c = 8; c >= 0 && m.row[r][c] == ' '; --c) m.row[r][c] = 0;  // обрезать хвостовые пробелы
        }
        queue.push_back(m);
    }
    void pushItem(int id) { push(ztmsg::itemCollected(id)); }
    void clear() { queue.clear(); timer = 0; vSeq = nullptr; vIdx = 0; vTimer = 0; }

    // ⭐Длительности реплик — в 1/60 с: таймер -$7734 декрементирует VBLANK-обработчик @0xAA2
    // (`subq #1,$ff08cc` каждую развёртку, 60 Гц), а НЕ игровой цикл. (Дисплей-таймер -$76f0 — наоборот,
    // игровые тики: его убавляет скроллер 1e12e в главном лупе.) Было ×fps/15 → паузы вчетверо длиннее.
    // voicePace() — пользовательский темп (меню «Voice pace»): 1.0 = ROM, меньше = плотнее, 0 = выкл.
    int vScale(int dur60) const { int f = (int)(dur60 * fps * voicePace() / 60.0 + 0.5); return f < 1 ? 1 : f; }

    // Тик раз в кадр: дотикать текущее, при истечении снять и показать следующее (FIFO).
    // ⭐ROM-точно (скроллер sub_01e0fa @1e12e): ДИСПЛЕЙ-таймер убывает на (1 + число ЖДУЩИХ в очереди) —
    // при забитой очереди сообщения мелькают быстрее, разгребая её (d0 -= -0x76f2 каждый кадр). База
    // -0x76f0=0x14 (20 родных кадров ZT ≈ showFrames). Хвост-таймеры сообщения — НЕ для дисплея, а для
    // ОЗВУЧКИ (см. шапку): пары (длительность, сэмпл), гейт «Voice:» = бит1 $FF0014 (меню Options; я сперва
    // счёл его мёртвым — грепал байт -7feb=$FF0015, а пишется он СЛОВОМ через $FF0014, юзер поправил).
    void update() {
        // РЕПЛИКА (менеджер ROM 1e2cc): тикает независимо от показа; по истечении пары — следующий сэмпл.
        if (vSeq) {
            if (--vTimer <= 0) {
                ++vIdx;
                const ztmsg::VP& p = vSeq[vIdx];
                if (p.dur == 0 && p.id == 0) { vSeq = nullptr; vIdx = 0; }         // конец последовательности
                else { snd::playSfx(p.id); vTimer = vScale(p.dur); }
            }
        }
        bool wasShowing = (timer > 0);
        if (timer > 0) {
            int waiting = (int)queue.size() > 1 ? (int)queue.size() - 1 : 0;   // ждущие за текущим
            timer -= (1 + waiting);
            if (timer <= 0) { timer = 0; if (!queue.empty()) queue.pop_front(); }
        }
        if (timer == 0 && !queue.empty()) {
            timer = showFrames;                                                // ПОКАЗ нового сообщения
            (void)wasShowing;
            // ОЗВУЧКА при показе (ROM 1e288): гейт «Voice: On»; если предыдущая реплика ЕЩЁ ЗВУЧИТ —
            // голос этого сообщения ПРОПУСКАЕТСЯ (1e294 tst -58ea), не прерывает и не ставится в очередь.
            const ztmsg::VP* v = queue.front().voice;
            if (ztmsg::voiceEnabled() && !vSeq && v && !(v[0].dur == 0 && v[0].id == 0)) {
                vSeq = v; vIdx = 0;
                snd::playSfx(v[0].id); vTimer = vScale(v[0].dur);              // 1-й сэмпл сразу (1e2bc→d796)
            }
        }
    }

    // Отрисовка текущего сообщения в НИЖНЕМ-ЛЕВОМ углу (как ROM: x≈16,y≈160 в 320×224 → масштаб FB).
    // sc — масштаб шрифта (Letters 8×8). x,y — левый-верх блока. color — ЖЁЛТЫЙ штрих как HP/счётчик врагов
    // (ZT: текст статус-строки той же палитры, idx7 #FCFC24 = gd.heldPal.c[7]).
    // ⭐font = НАСТОЯЩИЙ ZT-шрифт Letters (0x16E758): встроенный FONT8X8 имел жирные глифы («B засвеченная»).
    void draw(FB& fb, int x, int y, int sc, uint32_t color = 0xFFFCFC24u, const ZtFont* font = nullptr) const {
        if (queue.empty() || timer <= 0) return;
        const Msg& m = queue.front();
        for (int r = 0; r < 4; ++r)
            if (m.row[r][0]) { int cx = x;
                for (const char* s = m.row[r]; *s; ++s) { drawCharFont(fb, cx, y + r * 8 * sc, *s, color, sc, font); cx += 8 * sc; } }
    }
};
