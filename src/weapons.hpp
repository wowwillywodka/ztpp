// ztpp — ИНВЕНТАРЬ / ОРУЖИЕ / ПОДБОР / ОТОБРАЖЕНИЕ В РУКАХ (FPS-вид).
//
// Данные ZT (подтверждено по ROM):
//  • Предметы/оружие = celltype'ы карты (отдельной таблицы позиций НЕТ): celltype 0x19..0x26 →
//    индекс предмета = ct−0x18 (таблица из 15). Оружие = idx{2,7,8,10,11,12,13,14}.
//    Разбросанные клетки одного оружия = пикапы боезапаса (1-й раз = получить ствол, далее патроны).
//  • Held-графика «в руках» — VDP-спрайты 8×8 COLUMN-MAJOR (порт WeaponViewer из mdgfx/gui.py).
//    Таблица @0x11C98 (15 лонгов, id→графика) → 9 блоков по 672 б = 21 тайл (тело 12 + выстрел 9).
//    Палитра 0x20D2. У ракетницы (блок 6) тело 4×3, у остальных 3×4.
//
// Этот заголовок включается в main.cpp ПОСЛЕ struct FB и ui.hpp (использует FB и gd.heldGfx/heldPal).
#pragma once
#include "gamedata.hpp"
#include "level.hpp"
#include "cells.hpp"
#include "framebuffer.hpp"   // FB (draw-функции инвентаря/HUD рисуют в framebuffer)
#include "raycaster.hpp"     // pickupHiddenFn — хук «пикап подобран» для рендера билбордов
#include "actors.hpp"        // система актёров: снаряды/эффекты/враги + урон (damageRay)
#include <vector>
#include <cstdint>
#include <unordered_set>

// ── Пикапы ZT (idx = celltype − 0x18) — ИМЕНА ПО ГРАФИКЕ объект-банка (game-true из дизасма
// диспетча 0x113F4), НЕ по таблице редактора (она известно-неверна: idx13 = аптечка, не «огнемёт»).
// Флаг weapon — по классификации cells.hpp (icon 10=оружие): оружие ZT = idx {2,7,8,10,11,12}
// (ct 0x1A,0x1F,0x20,0x22,0x23,0x24). Предметы (icon11) = {1,3,4,5,6,9,13}. idx0/14 — не пикапы (icon13).
// Тайлы графики (см. itemBillboardForCt): 8=ящик(мина),14=пистолет,13=граната,12=дробовик,18=АПТЕЧКА…
// ammoPickup/ammoCap — ТОЧНО из ZT-таблиц @0x11222 (дефолт подбора) / @0x11240 (кап), ÷0x100 (8.8 fixed):
// предметы (сканер/жилет/огнетуш/костюм/фонарик/ноч.зрение) = заряд 10/кап 10; оружие = кап 99.
struct ItemDef { const char* name; bool weapon; int ammoPickup; int ammoCap; };
static const ItemDef ITEMS[15] = {
    {"FIRE",          false,  1,  1},   // idx0  ct0x18 — не пикап (огонь/триггер)
    {"BIO SCANNER",   false, 10, 10},   // idx1  ct0x19 предмет (зелёный сканер)
    {"MINE",          true,   2, 99},   // idx2  ct0x1A оружие (ящик-мина)
    {"VEST",          false, 10, 10},   // idx3  ct0x1B предмет (бронежилет)
    {"FIRE EXT.",     true,  10, 10},   // idx4  ct0x1C — ОГНЕТУШИТЕЛЬ: fire-хендлер 0x12cfe (урон 8, тушит ct0x18)
    {"FIRE SUIT",     false, 10, 10},   // idx5  ct0x1D предмет (огнезащ. костюм)
    {"FLASHLIGHT",    false, 10, 10},   // idx6  ct0x1E предмет (фонарик)
    {"GRENADE",       true,   4, 99},   // idx7  ct0x1F оружие (граната)
    {"HANDGUN",       true,   6, 99},   // idx8  ct0x20 оружие (пистолет) ✓
    {"NIGHT VISION",  false, 10, 10},   // idx9  ct0x21 предмет (очки)
    {"LASER GUN",     true,  10, 99},   // idx10 ct0x22 = LASER AIMED GUN (ПП с прицелом, tile5, held block1)
    {"ROCKET",        true,   4, 99},   // idx11 ct0x23 оружие (ракетница)
    {"SHOTGUN",       true,   6, 99},   // idx12 ct0x24 оружие (дробовик) ✓
    {"FLAMETHROWER",  true,  10, 10},   // idx13 = ОГНЕМЁТ (held block8, авто+пламя); кап 10=100% (ROM @0x11240 = 0x0a00, было 99 — БАГ)
    {"PULSE LASER",   true,   6, 99},   // idx14 = PULSE LASER (лазерн. ВИНТОВКА, held block7); ПИКАП = ct0x36
};
inline bool itemIsWeapon(int idx) { return idx >= 0 && idx < 15 && ITEMS[idx].weapon; }

// ── ИНВЕНТАРЬ ИГРОКА ──────────────────────────────────────────────────────────────────────────
// Модель ZT: до 5 НЕСОМЫХ предметов (round-robin слотов $FF904A-904E) — карусель, текущий по центру
// верхних 5 панелей кокпита (дисплей FUN_0108d6). Тут — carried[] (упорядоченные слоты, idx 1..14),
// sel = индекс выбранного (центр карусели), current = разрешённый id (carried[sel] или −1 кулаки).
// Ёмкость capacity=5 (как в игре); флаг unlimited снимает лимит (карусель окном из 5 вокруг текущего).
// owned[]/ammo[] — «есть предмет» + боезапас (счётчик на иконке). slide — выезд ствола при смене (1→0).
struct Inventory {
    bool   owned[15] = {false};
    int    ammo[15]  = {0};
    std::vector<int> carried;           // упорядоченные слоты-предметы (idx 1..14), длина ≤ capacity (или ∞)
    int    sel       = 0;               // индекс выбранного слота в carried (центр карусели)
    int    capacity  = 5;               // макс. слотов (ZT = 5); см. unlimited
    bool   unlimited = false;           // снять лимит ёмкости (неогранич. инвентарь)
    int    current   = -1;              // РАЗРЕШЁННЫЙ id выбранного (carried[sel], или −1 = кулаки)
    double slide     = 0.0;             // выезд ствола при смене: 0 на месте … 1 убран вниз (ZT $FF1058 0x20↔0)
    int    switching = 0;               // фаза смены: 0 нет · 1 уходит ВНИЗ (старый) · 2 выезжает ВВЕРХ (новый)
    int    pendingDir= 0;               // направление карусели, применяется в низшей точке слайда (ZT down→swap→up)
    int    fire      = 0;               // счётчик анимации выстрела ($FF105A): 0 покой, 1..4 кадры
    int    punchSide = 0;               // кулаки: 0 правый удар / 1 левый (чередуется)
    int    punchVariant = 0;            // тип удара (ZT -$6fa4): 0 обычный / 1 верхний (UP) / 2 нога-кик (DOWN)
    double bobPhase  = 0.0;             // фаза покачивания ствола при ходьбе (ZT 0x11286: чередование вида -$58e6)
    double bobAmt    = 0.0;             // текущая амплитуда боба (плавно нарастает при движении, гаснет в покое)

    // КАРУСЕЛЬ-КОЛЬЦО (как ZT round-robin 5 слотов, дизасм 0x1125e): значения слотов, −1 = пусто/кулаки.
    //  • unlimited → все несомые + ВСЕГДА кулаки (−1) в конце (юзер: кулаки всегда выбираемы);
    //  • 5-слотов (ZT) → РОВНО capacity позиций: несомые + пустые(−1) до 5, перебираются даже пустые.
    std::vector<int> ring() const {
        std::vector<int> r;
        if (unlimited) { for (int c : carried) r.push_back(c); r.push_back(-1); }
        else for (int i = 0; i < capacity; ++i) r.push_back(i < (int)carried.size() ? carried[i] : -1);
        return r;
    }
    void syncCurrent() {
        std::vector<int> r = ring(); int n = (int)r.size();
        if (n == 0) { sel = 0; current = -1; return; }
        if (sel < 0) sel = 0; if (sel >= n) sel = n - 1;
        current = r[sel];
    }
    bool has(int idx) const { for (int c : carried) if (c == idx) return true; return false; }
    // Убрать предмет из инвентаря (боезапас кончился — как ZT FUN_10b00: слот очищается, переключение).
    // Освобождает слот; выбор сдвигается на оставшееся (или кулаки). Re-pickup вернёт его (has()→false).
    void dropItem(int idx) {
        int p = -1; for (size_t i = 0; i < carried.size(); ++i) if (carried[i] == idx) { p = (int)i; break; }
        if (p < 0) return;
        carried.erase(carried.begin() + p);
        owned[idx] = false; ammo[idx] = 0;
        if (carried.empty()) { sel = 0; current = -1; }
        else { if (sel > p || sel >= (int)carried.size()) sel = (sel > 0 ? sel - 1 : 0); syncCurrent(); }
        fire = 0; slide = 1.0;                                   // сброс выстрела + анимация смены ствола
    }
    // Подобрать предмет в слот. true — добавлен НОВЫЙ слот; false — уже несём ИЛИ нет места (full).
    bool addItem(int idx) {
        if (idx < 1 || idx >= 15) return false;
        if (has(idx)) return false;                                       // уже в инвентаре
        if (!unlimited && (int)carried.size() >= capacity) return false; // слотов нет
        carried.push_back(idx); owned[idx] = true; return true;
    }
    void reset() { *this = Inventory{}; }
};

// Отдача тела по кадру выстрела (таблица ZT @0x12828, индекс 905a): сдвиг Y вниз. 4-кадровая анимация.
static const int FIRE_RECOIL[6] = {0, 4, 7, 6, 3, 0};
inline int fireRecoil(int fire) { return (fire >= 0 && fire <= 5) ? FIRE_RECOIL[fire] : 0; }
// Дульная вспышка (тайл 0x50c) — ТОЛЬКО у пуле-/энергостволов (дизасм дисплей-хендлеров): пистолет/лазер/
// ракета/дробовик/пульс. Метательные (мина 2, граната 7) — бросок; огнемёт (13) — пламя (не вспышка).
inline bool fireHasFlash(int id) { return id == 8 || id == 10 || id == 11 || id == 12 || id == 14; }
// Авто-огонь (зажатие = непрерывно, дизасм 0x1247a лазер / 0x1282e огнемёт: дисплей сам перестреливает по
// btst #4,802e): laser aimed gun (10) и flamethrower (13). Остальные — по фронту.
inline bool fireAuto(int id) { return id == 10 || id == 13 || id == 4; }   // лазер / огнемёт / огнетушитель (непрерывная струя)
// Огнемёт (13) — непрерывная струя пламени вместо дульной вспышки/пули.
inline bool fireIsFlame(int id) { return id == 13; }
// Метательное (летящий снаряд по дуге/прямой): мина 2, граната 7, ракета 11.
inline bool fireIsProjectile(int id) { return id == 2 || id == 7 || id == 11; }

// Per-weapon экранные офсеты дисплея (Yoff,Xoff от дизасм-хендлеров; база ≈ 0). Хэндган-ссылка (0xd9,0x114).
// Используется для X-смещения (граната правее) — Y держим у низа (в нашем виде без кокпита спрайт целиком).
inline void heldOffset(int id, int& yoff, int& xoff) {
    yoff = 0xd9; xoff = 0x114;                       // дефолт (пистолет/лазер/дробовик/пульс/огнемёт)
    switch (id) {
        case 7:  yoff = 0xe8; xoff = 0x134; break;   // граната — правее
        case 11: yoff = 0xe1; xoff = 0x120; break;   // ракетница — чуть правее
        case 10: xoff = 0x118; break;                // лазер
        default: break;
    }
}

// Выстрел (как FUN_12bac/12eac): нельзя в выезде ствола (slide) или пока идёт анимация (fire); оружие тратит
// патрон. Возвращает true, если выстрел начался. Урон/рейкаст/спавн пули — позже (нужны актёры).
inline bool fireWeapon(Inventory& inv) {
    if (inv.fire != 0 || inv.slide > 0) return false;
    int id = inv.current;
    if (id >= 0 && ITEMS[id].weapon) { if (inv.ammo[id] <= 0) return false; --inv.ammo[id]; }
    if (id < 0) inv.punchSide ^= 1;          // кулаки: чередуем бьющую руку (правая/левая)
    inv.fire = 1;
    return true;
}

// Переключить выбранный слот (dir −1 пред. / +1 след.) по кольцу carried — карусель (центр = текущий).
// ZT-смена ДВУХФАЗНАЯ ($FF1058 0x20↔0): текущий ствол УХОДИТ ВНИЗ → в нижней точке swap → новый ВЫЕЗЖАЕТ ВВЕРХ.
inline void cycleWeapon(Inventory& inv, int dir) {
    int n = (int)inv.ring().size();                          // кольцо: unlim=несомые+кулаки · 5-слот=5 позиций (с пустыми)
    if (n <= 0) { inv.sel = 0; inv.current = -1; return; }
    // ZT (1125e): слот меняется МГНОВЕННО на каждое нажатие (быстрый проклик); перебираются и ПУСТЫЕ слоты (=кулаки).
    inv.sel = ((inv.sel + dir) % n + n) % n;
    inv.syncCurrent();
    inv.slide = 1.0; inv.switching = 2;                      // новый ствол выезжает вверх (не блокирует следующий проклик)
}
// Анимация выезда/боба/выстрела: вызывать раз в игр.кадр. speed = фактическая скорость игрока (кл/кадр) — боб
// масштабируется по ней (быстрее идёшь → чаще/сильнее качается; стоишь → гаснет). 0 = покой.
inline void updateHeld(Inventory& inv, double speed = 0.0) {
    if (inv.switching == 1) {                                // СТАРЫЙ уходит вниз (ZT 0x11c2c: -$6fa8 += 8 до 0x20 = 4 кадра)
        inv.slide += 0.25; if (inv.slide >= 1.0) {           // достиг низа → СМЕНА слота
            inv.slide = 1.0;
            int n = (int)inv.ring().size(); if (n < 1) n = 1;
            inv.sel = ((inv.sel + inv.pendingDir) % n + n) % n; inv.syncCurrent();
            inv.switching = 2;                               // фаза 2: новый выезжает вверх
        }
    } else if (inv.switching == 2 || inv.slide > 0) {        // НОВЫЙ выезжает вверх (ZT: -$6fa8 -= 8 до 0 = 4 кадра)
        inv.slide -= 0.25; if (inv.slide <= 0) { inv.slide = 0; inv.switching = 0; }
    }
    if (inv.fire > 0)  { ++inv.fire; if (inv.fire >= 5) inv.fire = 0; }
    // ПОКАЧИВАНИЕ ствола: скорость фазы И амплитуда ∝ фактической скорости игрока (не статично). reff: 0.156 кл/кадр = бег.
    double sp = speed; if (sp > 0.20) sp = 0.20;             // нормировка к макс. скорости
    double t = sp / 0.156;                                   // доля от обычной ходьбы (1.0 = норма)
    if (sp > 0.004) { inv.bobPhase += 0.42 * t; inv.bobAmt += (t - inv.bobAmt) * 0.18; }  // фаза/амплитуда по скорости
    else            { inv.bobAmt += (0.0 - inv.bobAmt) * 0.12; }
    if (inv.bobAmt > 1.2) inv.bobAmt = 1.2;
    if (inv.bobPhase > 6.28318530718) inv.bobPhase -= 6.28318530718;
}

// ── ПОДБОР ПИКАПОВ (по аналогии с doorMap: множество «погашенных» клеток floor*1024+y*32+x) ──
inline std::unordered_set<int>& pickedSet() { static std::unordered_set<int> s; return s; }
inline void rcResetPickups() { pickedSet().clear(); }
inline int  pickKey(int f, int x, int y) { return ((f * 32 + y) * 32 + x); }
// idx предмета-в-инвентарь по celltype (icon 10 оружие / 11 предмет → idx = ct−0x18). Возвращает idx или −1.
// ⚠ ct0x25 = МЕДПАК (+HP, НЕ инвентарь) → −1 (обрабатывается в rcTryPickup отдельно). ОГНЕМЁТ-оружие
// (weapon-id 13) = пикап ct0x82 (НЕ ct0x25!). Лазерн. ВИНТОВКА = ct0x36 → 14 (НЕ laser-aimed-gun ct0x22=10).
inline int pickupItemIdx(uint8_t ct) {
    if (ct == 0x25) return -1;          // медпак — не инвентарный предмет (см. rcTryPickup)
    if (ct == 0x36) return 14;          // PULSE LASER (лазерн. винтовка, held block7), графика мира tile17
    if (ct == 0x82) return 13;          // FLAME THROWER (weapon-id 13, held block8) — ПИКАП ct0x82
    int ic = cellIcon(ct);
    if (ic != 10 && ic != 11) return -1;
    int idx = (int)ct - 0x18;
    if (idx >= 0 && idx < 15) return idx;
    return -1;
}
// Скрыт ли пикап-билборд (подобран) — для рендера в мире. main() ставит его в pickupHiddenFn (raycaster).
inline bool pickupIsConsumed(int f, int x, int y) { return pickedSet().count(pickKey(f, x, y)) > 0; }

// Спец-код возврата rcTryPickup для медпака (ct0x25): не инвентарный предмет, мгновенный +HP.
static const int PICK_MEDKIT = -2;

// Подобрать пикап под игроком. Возвращает подобранный idx (или −1; PICK_MEDKIT для медпака).
//  • МЕДПАК (ct0x25): +20 HP (кап 100), не в инвентарь; не брать при полном HP (как ZT 0x11b7c).
//  • уже несём → добор боезапаса (consume).
//  • новый + есть слот → занять слот + боезапас + авто-выбор нового ствола (consume).
//  • новый + инвентарь ПОЛОН (5, не unlimited) → НЕ подбирать (оставить в мире, return −1).
int rcTryPickup(Inventory& inv, const Level& lvl, int floor, double px, double py);   // weapons.cpp

// Выдать оружие/предмет в инвентарь (как rcTryPickup, но без celltype/pickedSet) — для дропа с трупов. true = взято.
inline bool grantPickup(Inventory& inv, int idx) {
    if (idx < 1 || idx >= 15) return false;
    bool firstTime = !inv.has(idx);
    if (firstTime && !inv.addItem(idx)) return false;            // инвентарь полон → не берём
    inv.ammo[idx] += ITEMS[idx].ammoPickup;
    if (inv.ammo[idx] > ITEMS[idx].ammoCap) inv.ammo[idx] = ITEMS[idx].ammoCap;
    if (firstTime && ITEMS[idx].weapon) {                        // авто-выбор нового ствола
        for (size_t i = 0; i < inv.carried.size(); ++i) if (inv.carried[i] == idx) { inv.sel = (int)i; break; }
        inv.syncCurrent(); inv.slide = 1.0;
    } else inv.syncCurrent();
    return true;
}
// Подбор оброненного оружия при шаге на труп (ZT: солдаты бросают ствол у трупа). Возвращает id или −1.
inline int rcTryCorpsePickup(Inventory& inv, const Camera& cam) {
    int got = -1;
    corpsePickup(cam, [&](int wid) -> bool { if (grantPickup(inv, wid)) { got = wid; return true; } return false; });
    return got;
}

// Тип ОТОБРАЖЕНИЯ оружия в руках (как jump-таблица дисплея 0x11d0c): 0=кулаки(два) / 1=ничего(мина) /
// 2=граната(бросок) / 3=тело(ствол/огнетушитель). Пассивные предметы (карта/сканер/жилет/костюм/фонарик/
// ноч.зрение) используют КУЛАКИ (их дисплей-хендлер = 0x11f3c).
inline int heldDisplayKind(int id) {
    if (id < 0) return 0;
    switch (id) {
        case 0: case 1: case 3: case 5: case 6: case 9: return 0;   // пассивные предметы → кулаки
        case 2: return 1;                                           // мина → ничего (display 0x11d48 rts)
        case 7: return 2;                                           // граната → бросок (display 0x11d4a)
        default: return 3;                                          // оружие + огнетушитель → тело
    }
}

// Урон hit-scan-оружия — D1 из фаер-хендлеров ZT (12eac/12f62/1303c/130de/12cfe): пистолет 1, лазер 3,
// дробовик 8, пульс 8, огнетушитель 8 (в ZT ×больше при заряде -0x6fd0 — пока не моделируем). Остальное 1.
inline int weaponDamage(int id) {
    switch (id) { case 8: return 1; case 10: return 3; case 12: return 8; case 14: return 8; case 4: return 8; default: return 1; }
}

// Полуширина КОНУСА автонаведения d1 (ZT fire-хендлеры): handgun 1 (боец0/2 = 4), laser 3, shotgun 8 (боец0 = 12),
// pulse 8 (боец0 = 12), foam 8. Больше d1 = шире авто-захват цели.
inline int coneWidth(int id) {
    int fg = playerFighter();
    switch (id) {
        case 8:  return (fg == 0 || fg == 2) ? 4 : 1;   // HANDGUN
        case 10: return 3;                               // LASER
        case 12: return (fg == 0) ? 12 : 8;             // SHOTGUN
        case 14: return (fg == 0) ? 12 : 8;             // PULSE
        case 4:  return 8;                               // FIRE EXT (пена)
        default: return 8;
    }
}
// Промах hit-scan (ZT 12ad4): луч ОТ ГЛАЗ до 1-й СОЛИДНОЙ клетки (макс 31 клетка) → спрайт импакта У ГРАНИ стены.
// ⚠ ROM 12ad4 актёров НЕ проверяет — луч ПРОХОДИТ СКВОЗЬ ТРУПЫ (и живых) прямо к стене; трупы выстрел НЕ
// задевают/не стопают (фидбэк юзера: «выстрелы задевают трупы — неверно» + «нет импакта по стене за трупом»).
// Нет солида в 31 клетке → импакта НЕТ (ROM rts). Диагональ учитывается полуплоскостью (cellBlockedAt).
inline void traceMiss(const Level& lvl, int floor, double px, double py, double dx, double dy) {
    double x = px, y = py;
    for (int i = 0; i < 600; ++i) {                               // 31 кл / 0.06 ≈ 517 шагов (+запас)
        double nx = x + dx * 0.06, ny = y + dy * 0.06; int cx = (int)nx, cy = (int)ny;
        if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H) return;
        if (cellBlockedAt(lvl.cellType(floor, cx, cy), nx - cx, ny - cy)) {   // 1-я солидная клетка = СТЕНА
            if (wallIsSecret(lvl.cellType(floor, cx, cy))) requestDestruct(floor, cx, cy);  // ТОЛЬКО секрет-стены 0x79/7B/7D/7F (a6bc: смена текстуры). 0x83/84 пуля НЕ ломает — разрушает тревога-камера
            spawnSparkA(nx, ny, floor); return;                   // ИМПАКТ у грани стены (ZT актёр 15ad6, банк 0x1167be, 3 кадра)
        }
        if ((nx - px) * (nx - px) + (ny - py) * (ny - py) > 31.0 * 31.0) return;  // ROM: дальность 31 клетка (0x1f)
        x = nx; y = ny;
    }
}

// ── ВЫСТРЕЛ (спавн актёров) ─────────────────────────────────────────────────────
// По типу оружия: hit-scan (пистолет 8 / ПП-лазер 10 / дробовик 12 / пульс 14 / огнетушитель 4) — МГНОВЕННО
// рейкаст-урон D1 (damageRay) → искра у стены ТОЛЬКО при промахе (как ZT: 0x12ad4 спавнит трассер лишь когда
// враг не задет; на попадании урон применяется напрямую). Ракета/граната — летящий снаряд → взрыв. Огнемёт —
// пламя. Мина — ставится ВПЕРЁД по трассе (до 8 клеток, ZT 12bfe). Пассивные предметы/кулаки — удар (melee).
void fireSpawn(const Inventory& inv, const Level& lvl, const Camera& cam);   // weapons.cpp

// ── ДЕКОД held-графики: 8×8 4bpp тайлы в COLUMN-MAJOR блоке (порт _colmajor_block) ──
// tile(col,row) = startTile + col*H + row; каждый тайл — стандартный MD 8×8 (4 б/строка, hi-ниббл=левый).
inline void decode8(const uint8_t* s, uint8_t out[64]) {
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 4; ++c) { uint8_t b = s[r * 4 + c]; out[r * 8 + c * 2] = b >> 4; out[r * 8 + c * 2 + 1] = b & 0x0F; }
}
inline std::vector<uint8_t> decodeColMajor(const uint8_t* blockBase, int startTile, int ntiles, int W,
                                           int& outW, int& outH) {
    int H = (ntiles + W - 1) / W;
    outW = W * 8; outH = H * 8;
    std::vector<uint8_t> buf((size_t)outW * outH, 0);
    uint8_t t[64];
    for (int col = 0; col < W; ++col)
        for (int row = 0; row < H; ++row) {
            int k = col * H + row;
            if (k >= ntiles) continue;
            decode8(blockBase + (size_t)(startTile + k) * 32, t);
            for (int yy = 0; yy < 8; ++yy)
                for (int xx = 0; xx < 8; ++xx)
                    buf[(size_t)(row * 8 + yy) * outW + col * 8 + xx] = t[yy * 8 + xx];
        }
    return buf;
}

// ── РЕНДЕР ОРУЖИЯ В РУКАХ (низ-центр заданного РЕГИОНА вида) ──
// КАК В ИГРЕ (дизасм хендлеров 0x11f3c кулаки / 0x12404 пистолет и т.п.): held — аппаратный
// VDP-спрайт фикс. размера, пишется ПРЯМО в SAT: размер-байт 0x0B = 3 тайла×4 = 24×32 px (ракетница
// 4×3 = 32×24), тайл 0x500 = VRAM 0xA000, рисуется 1:1 БЕЗ масштабирования на экране 320×224, по
// центру-низу 3D-вида (Y=base+slide; slide 0..0x20=32px вниз при смене). Поэтому экранный масштаб =
// «нативные пиксели» (scale задаёт вызывающий = ширина_кадра/320). Спрайты пошире (ракетница) выходят
// шире при ТОМ ЖЕ scale — как в игре. Низом у низа региона, по центру; slide → вниз. Прозрачный idx 0.
inline double& weaponScale() { static double v = 1.0; return v; }   // тюнинг размера ствола (дефолт 1.0 = как в игре)

// Одно размещение спрайта: левый-верх (ox,oy) + зеркало.
template<class Put>
inline void blitHeld(Put put, const uint8_t* body, int sw, int sh, int scale,
                     int ox, int oy, bool hflip, int rx, int ry, int rw, int rh, const Palette& pal) {
    int dstW = sw * scale, dstH = sh * scale;
    for (int y = 0; y < dstH; ++y) {
        int fy = oy + y; if (fy < ry || fy >= ry + rh) continue;
        int sy = y / scale;
        for (int x = 0; x < dstW; ++x) {
            int fx = ox + x; if (fx < rx || fx >= rx + rw) continue;
            int sx = x / scale; if (hflip) sx = sw - 1 - sx;
            uint8_t i = body[(size_t)sy * sw + sx];
            if (i == 0) continue;                       // прозрачный
            put(fx, fy, pal.c[i]);
        }
    }
}

template<class Put>
inline void drawHeldRegion(Put put, int rx, int ry, int rw, int rh,
                           const GameData& gd, const Inventory& inv, int scale) {
    if (gd.heldGfx.empty() || gd.heldBlocks <= 0) return;
    const size_t HELD_BLOCK = 672;
    int id = inv.current;                               // −1 → кулаки
    int block = (id < 0) ? gd.heldBlockForId[0] : gd.heldBlockForId[id];
    if (block < 0 || block >= gd.heldBlocks) block = 0;
    const uint8_t* base = gd.heldGfx.data() + (size_t)block * HELD_BLOCK;
    int bw = (block == 6) ? 4 : 3;                      // ракетница: тело 4×3
    int sw = 0, sh = 0;
    std::vector<uint8_t> body = decodeColMajor(base, 0, 12, bw, sw, sh);

    if (scale < 1) scale = 1;
    int dstW = sw * scale, dstH = sh * scale;
    int rec = fireRecoil(inv.fire);                     // профиль отдачи (таблица 0x12828: {0,4,7,6,3})
    // ПОКАЧИВАНИЕ при ходьбе: вертикаль ТОЛЬКО ВНИЗ (|sin| — ствол не «выскакивает» выше покоя, ZT держит фикс. высоту),
    // гориз. восьмёрка. Амплитуда мала и ∝ bobAmt (нарастает в движении). Ствол стоит низом у низа региона (покой = -$71d6+slide).
    int bobY = (int)(std::fabs(std::sin(inv.bobPhase)) * inv.bobAmt * 3.0 * scale);
    int bobX = (int)(std::sin(inv.bobPhase * 0.5) * inv.bobAmt * 3.0 * scale);
    int oy0 = ry + rh - dstH + (int)(inv.slide * dstH) + bobY; // низом у низа региона; slide → вниз; +боб (вниз)
    const Palette& pal = gd.heldPal;
    if (id < 0) {
        // КУЛАКИ (хендлер 0x11f3c): статика — ДВА кулака у низа (сдвиг 0x58 нат.px). УДАР (дизасм 0x1219a):
        // бьёт ОДНА рука (правая/левая по punchSide — чередуется), тянется вверх-внутрь (пик кадр 2), второй
        // кулак на месте. Полный спрайт (клип к региону). Хит — на кадре 3 (рейкаст 16388, melee).
        int off = (int)(0x58 * scale);
        int x1 = rx + (rw - (off + dstW)) / 2 + bobX;      // левый кулак (+боб по X)
        int x2 = x1 + off;                                 // правый кулак
        int up = rec * 4 * scale, conv = rec * 2 * scale;  // джеб бьющей руки (вверх+к центру)
        bool punchRight = (inv.punchSide == 0);
        // левый кулак (без флипа)
        if (punchRight) blitHeld(put, body.data(), sw, sh, scale, x1, oy0, false, rx, ry, rw, rh, pal);
        else            blitHeld(put, body.data(), sw, sh, scale, x1 + conv, oy0 - up, false, rx, ry, rw, rh, pal);
        // правый кулак (hflip)
        if (punchRight) blitHeld(put, body.data(), sw, sh, scale, x2 - conv, oy0 - up, true, rx, ry, rw, rh, pal);
        else            blitHeld(put, body.data(), sw, sh, scale, x2, oy0, true, rx, ry, rw, rh, pal);
    } else {
        int yoff, xoff; heldOffset(id, yoff, xoff);
        int dx = (xoff - 0x114) * scale;                // per-weapon гориз. смещение (граната/ракета правее)
        int raise = (yoff - 0xd9) * scale;              // нативно низкие (граната/ракета) ПОДНИМАЕМ: в нашем
                                                        // виде нет кокпита, режущего низ → показываем целиком
        bool gun = fireHasFlash(id), flame = fireIsFlame(id), thrown = (id == 2 || id == 7);
        int oy = oy0 - raise + (thrown ? -rec * 2 * scale : rec * scale);  // ствол/огнемёт — отдача вниз; бросок — вверх
        int ox = rx + (rw - dstW) / 2 + dx + bobX;      // один спрайт, со смещением (+боб по X)
        blitHeld(put, body.data(), sw, sh, scale, ox, oy, false, rx, ry, rw, rh, pal);
        // ВСПЫШКА/ПЛАМЯ (тайлы 12-20 = 3×3): у стволов — дульная вспышка на кадре 2; у ОГНЕМЁТА — непрерывное
        // пламя пока стреляем (fire>0), как в игре (0x1282e рисует струю всё время удержания). Выше дула.
        if ((gun && inv.fire == 2) || (flame && inv.fire > 0)) {
            int fw = 0, fh = 0;
            std::vector<uint8_t> flash = decodeColMajor(base, 12, 9, 3, fw, fh);
            int fox = ox + (dstW - fw * scale) / 2;      // по центру тела
            int foy = oy - 9 * scale;                    // выше дула
            blitHeld(put, flash.data(), fw, fh, scale, fox, foy, false, rx, ry, rw, rh, pal);
        }
    }
}

// Экранный масштаб ствола: нативный спрайт рисуется 1:1 на экране 320px → scale = ширина_кадра/320.
inline int heldScaleFor(int frameW) {
    int s = (int)((double)frameW / 320.0 * weaponScale() + 0.5);
    return s < 1 ? 1 : s;
}

// Главный вид: ствол на весь FB (низ-центр).
inline void drawHeldWeapon(FB& fb, int vw, int vh, const GameData& gd, const Inventory& inv) {
    drawHeldRegion([&](int x, int y, uint32_t c) { fb.put(x, y, c); }, 0, 0, vw, vh, gd, inv,
                   heldScaleFor(vw));
}

// ── ЛАЗЕРНЫЙ ПРИЦЕЛ laser aimed gun (id 10), дизасм-дисплей 0x1247a ───────────────────────────────
// Красная лазерная ТОЧКА над стволом: размер ∝ 1/дистанция (ближе препятствие → крупнее), покачивается
// при ходьбе вместе со стволом. ВСЕГДА включён, пока несём лазерную пушку. fwdDist = глубина центр.колонны (zbuf).
template<class Put>
inline void drawLaserSight(Put put, int vx, int vy, int vw, int vh, const Inventory& inv, double fwdDist) {
    if (inv.current != 10) return;                       // только laser aimed gun
    double d = fwdDist; if (d < 0.3) d = 0.3;
    int r = (int)(2.4 / d + 0.5); if (r < 1) r = 1; if (r > 7) r = 7;   // радиус по дистанции
    int bx = (int)(std::sin(inv.bobPhase) * inv.bobAmt * 3.0);          // покачивание как у ствола
    int by = (int)(std::fabs(std::sin(inv.bobPhase)) * inv.bobAmt * 2.0);
    int cx = vx + vw / 2 + bx, cy = vy + vh / 2 + by;     // центр вида (куда смотрит ствол) + боб
    for (int dy = -r; dy <= r; ++dy) for (int dx = -r; dx <= r; ++dx) {
        if (dx * dx + dy * dy > r * r) continue;
        bool core = (dx * dx + dy * dy) <= (r > 2 ? (r - 1) * (r - 1) : 0);
        put(cx + dx, cy + dy, core ? 0xFFFF3030u : 0xFFFF9090u);        // ядро ярко-красное, кайма светлее
    }
}

// ⭐ПОЗЫ КУЛАКОВ (АВТОГЕН из ROM таблицы 0x1219a): [вариант 0=обычный/1=верхний/2=нога][рука 0=L/1=R][фаза 0-2] →
// 1-2 VDP-спрайта. ТОЧНЫЕ размеры/тайлы/позиции (SAT из фрейм-таблицы). Обычный=кулак 3×4; верхний=2 кулака
// (3×4 + предплечье 3×3 tile+12); нога=замах/возврат 4×1 (tile+16 боот) + удар 4×4 (tile+0, выпад). Экран=raw−0x80.
struct FistSpr { int tileOff, w, h, x, y; bool hflip; };
struct FistPose { FistSpr s[2]; int n; };
static const FistPose FIST_POSE[3][2][3] = {
  { // NORMAL (idle-блок 0x16c2b8, кулак 3×4)
    { {{{0,3,4,116,96,0},{0,0,0,0,0,0}},1}, {{{0,3,4,128,88,0},{0,0,0,0,0,0}},1}, {{{0,3,4,116,96,0},{0,0,0,0,0,0}},1} },
    { {{{0,3,4,180,96,1},{0,0,0,0,0,0}},1}, {{{0,3,4,168,88,1},{0,0,0,0,0,0}},1}, {{{0,3,4,180,96,1},{0,0,0,0,0,0}},1} },
  },
  { // UPPER (idle-блок, кулак 3×4 + предплечье 3×3 tile+12 НИЖЕ, 2-й спрайт @rs+0x30)
    { {{{0,3,4,119,84,0},{12,3,3,119,116,0}},2}, {{{0,3,4,134,64,0},{12,3,3,134,96,0}},2}, {{{0,3,4,119,84,0},{12,3,3,119,116,0}},2} },
    { {{{0,3,4,177,84,1},{12,3,3,177,116,1}},2}, {{{0,3,4,162,64,1},{12,3,3,162,96,1}},2}, {{{0,3,4,177,84,1},{12,3,3,177,116,1}},2} },
  },
  { // KICK (action-блок 0x16c038: замах/возврат нога 4×1 tile+16, удар выпад 4×4 tile+0)
    { {{{16,4,1,112,112,1},{0,0,0,0,0,0}},1}, {{{0,4,4,128,88,1},{0,0,0,0,0,0}},1}, {{{16,4,1,148,112,1},{0,0,0,0,0,0}},1} },
    { {{{16,4,1,176,112,0},{0,0,0,0,0,0}},1}, {{{0,4,4,160,88,0},{0,0,0,0,0,0}},1}, {{{16,4,1,140,112,0},{0,0,0,0,0,0}},1} },
  },
};

// ── ПИКСЕЛЬ-В-ПИКСЕЛЬ оружие в руках для REFERENCE (нативный экран 320×224, scale 1) ──────────────
// Координаты — ТОЧНО из дизасм-хендлеров (SAT), put(x,y,c) пишет в кадр 320×224, клип к 3D-окну
// [vx..vx+vw]×[vy..vy+vh] (как кокпит режет низ оружия в оригинале). Анимации: отдача (0x12828), вспышка
// (0x50c), бросок гранаты (table 0x11f14), удар кулаков (table 0x1219a — рука+предплечье, одна сторона).
template<class Put>
inline void drawHeldNative(Put put, int vx, int vy, int vw, int vh, const GameData& gd, const Inventory& inv) {
    if (gd.heldGfx.empty() || gd.heldBlocks <= 0) return;
    const Palette& pal = gd.heldPal;
    // ПОКАЧИВАНИЕ при ходьбе (ТОЛЬКО вниз — ствол не выскакивает выше покоя) + выезд при смене (slide·40px вниз, ZT $FF1058 0x20→0).
    int bobY = (int)(std::fabs(std::sin(inv.bobPhase)) * inv.bobAmt * 2.5) + (int)(inv.slide * 40.0);
    int bobX = (int)(std::sin(inv.bobPhase * 0.5) * inv.bobAmt * 3.0);
    // блит блока (startTile..+ntiles, ширина W) в нативные экранные (sx,sy), клип к окну, прозрачный idx0.
    auto blit = [&](int block, int startTile, int ntiles, int W, int sx, int sy, bool hflip, bool action = false) {
        const uint8_t* bp;
        if (action) { if (gd.fistAction.empty()) return; bp = gd.fistAction.data(); }   // блок ДЕЙСТВИЯ кулаков (0x16c038)
        else { if (block < 0 || block >= gd.heldBlocks) return; bp = gd.heldGfx.data() + (size_t)block * 672; }
        int ow = 0, oh = 0;
        std::vector<uint8_t> buf = decodeColMajor(bp, startTile, ntiles, W, ow, oh);
        for (int y = 0; y < oh; ++y) {
            int fy = sy + y + bobY; if (fy < vy || fy >= vy + vh) continue;
            for (int x = 0; x < ow; ++x) {
                int fx = sx + x + bobX; if (fx < vx || fx >= vx + vw) continue;
                int xx = hflip ? ow - 1 - x : x;
                uint8_t i = buf[(size_t)y * ow + xx]; if (!i) continue;
                put(fx, fy, pal.c[i]);
            }
        }
    };
    int id = inv.current, f = inv.fire;
    int kind = heldDisplayKind(id);
    if (kind == 1) return;                                 // МИНА — в руках ничего (просто ставится)
    if (kind == 0) {                                       // КУЛАКИ (и пассивные предметы: сканер/жилет/костюм…)
        if (f == 0) {                                      // статика: два кулака (block0 3×4) X104/X192 Y107
            blit(0, 0, 12, 3, 104, 107, false);
            blit(0, 0, 12, 3, 192, 107, true);
        } else {                                           // ⭐УДАР (ZT table 0x1219a): 5 вариантов, ТОЧНЫЕ спрайты FIST_POSE
            int fr = f - 1; if (fr < 0) fr = 0; if (fr > 2) fr = 2;   // фаза 0 замах → 1 удар(контакт) → 2 возврат
            int side = (inv.punchSide != 0) ? 1 : 0;                  // рука (0=L / 1=R)
            int var  = (inv.punchVariant >= 0 && inv.punchVariant <= 2) ? inv.punchVariant : 0;
            const FistPose& po = FIST_POSE[var][side][fr];
            // ⭐БЛОК ГРАФИКИ: ТОЛЬКО КИК (-$6fa4<2) DMA-свапит action-блок 0x16c038 (ZT 11f3c: cmpi #2,-$6fa4;bcc);
            // обычный/верхний БЕЗ свапа → idle-блок 0x16c2b8 (heldBlock0). Оттого кик читался из idle = мусор.
            bool useAction = (var == 2);
            for (int i = 0; i < po.n; ++i) {                          // 1-2 спрайта (верхний = 2), точный размер w×h/тайл
                const FistSpr& s = po.s[i];
                blit(0, s.tileOff, s.w * s.h, s.w, s.x, s.y, s.hflip, useAction);
            }
        }
        return;
    }
    int block = (id >= 0 && id < 15) ? gd.heldBlockForId[id] : 0;
    int bw = (block == 6) ? 4 : 3;
    int yoff, xoff; heldOffset(id, yoff, xoff);
    int sx = xoff - 0x80, sy = yoff - 0x80;                // нативная экранная позиция тела
    if (kind == 2) {                                       // ГРАНАТА: статика или бросок (table 0x11f14)
        if (f == 0) { blit(block, 0, 12, 3, sx, sy, false); return; }
        int fr = f - 1; if (fr > 4) fr = 4;
        static const int GT[5][3] = {{104,200,0}, {93,180,0}, {88,160,0}, {96,140,12}, {107,120,12}};  // Y,X,tilebase
        const int* g = GT[fr];
        if (g[2] == 0) blit(block, 0, 12, 3, g[1], g[0], false);   // тело (рука)
        else           blit(block, 12, 9, 3, g[1], g[0], false);   // блок-вспышка (отпускание)
        return;
    }
    // СТВОЛЫ: тело + отдача (0x12828) + вспышка (стволы, кадр 2) / пламя (огнемёт, всё время)
    int rec = fireRecoil(f);
    sy += rec; if (id == 11) sx += rec;                    // ракета — отдача и по X
    blit(block, 0, 12, bw, sx, sy, false);
    bool gun = fireHasFlash(id), flame = fireIsFlame(id);
    if ((gun && f == 2) || (flame && f > 0))
        blit(block, 12, 9, 3, sx - 1, sy - 9, false);     // вспышка/пламя: нативный офсет X−1, Y−9 (дизасм 0x1244c)
}

// ── HUD ИНВЕНТАРЯ (REFERENCE): карусель иконок в 5 верхних панелях кокпита + счётчик боезапаса ────
// Дисплей FUN_0108d6: 5 слотов на экр. X = 40/88/144/200/248 (= sprite 0xa8/0xd8/0x110/0x148/0x178),
// Y≈3 (верх). Центр (X=144) = ТЕКУЩИЙ (карусель: carried[sel] по центру, соседи вокруг, обмотка при ≥5).
// Иконка 32×32 (имя+картинка+ammo запечены) рисуется поверх панели (прозрачный idx0 → видна сетка).
// Боезапас оружия — МАЛЕНЬКИЙ номер ZT-шрифтом (4×6) в НИЗ-ЛЕВО иконки (как FUN_1DF54: тайл 3 = низ-лев.
// угол, VRAM 0x9600+slot·0x200+0x68), НЕ широкая полоса (та перекрывала ствол). Неогранич.: окно 5 вокруг sel.
void drawInventoryHud(uint32_t* frame, int FW, int FH, const GameData& gd, const Inventory& inv);   // weapons.cpp

// Цвет HP-числа по ZT (d98e: спрайт-палитра 0x20D2 индексы 7/6/5 по HP) — ЖЁЛТ→ОРАНЖ→КРАСН (НЕ зелёный!).
// Пороги масок: HP>80 idx7, >40 idx6, иначе idx5 (предупр. текст.asm d952<50/d970<15).
inline uint32_t hpColor(int hp) { return (hp > 60) ? 0xFFFCFC24u : (hp > 20 ? 0xFFD86C24u : 0xFFFC2424u); }

// Общий вывод N цифр значения val шрифтом Numbers ZT (fontNum 0x16E618, как d98e/1e00e) в кадр кокпита
// 320×224; 2-тон дизер (cHi/cLo по чётности rx+ry). x/y — левый-верхний угол первой цифры, sc — масштаб (ZT=1).
// bgHi/bgLo != 0 → ЗАЛИВАТЬ плитку (ZT d98e: font|mask — фон плитки в цвет, штрих цифры контрастный);
// 0 → прозрачный фон (штрих в cHi/cLo, как счётчик врагов 1e00e). Дизер по чётности rx+ry.
inline void drawHudDigits(uint32_t* frame, int FW, int FH, const ZtFont& f, int x, int y, int sc,
                          int val, int ndig, uint32_t cHi, uint32_t cLo, uint32_t bgHi = 0, uint32_t bgLo = 0) {
    if (!f.have) return;
    bool fill = (bgHi != 0 || bgLo != 0);
    for (int di = 0; di < ndig; ++di) {
        int place = 1; for (int k = 0; k < ndig - 1 - di; ++k) place *= 10;
        int a = '0' + (val / place) % 10; if (a < 0 || a >= 128 || !f.supported[a]) continue;
        for (int ry = 0; ry < 8; ++ry) { uint8_t row = f.glyph[a][ry];
            for (int rx = 0; rx < 8; ++rx) {
                bool on = row & (1 << rx);
                if (!on && !fill) continue;                          // прозрачный фон
                uint32_t c = on ? (((rx + ry) & 1) ? cHi  : cLo)     // штрих цифры
                                : (((rx + ry) & 1) ? bgHi : bgLo);   // фон плитки (health-тинт)
                for (int yy = 0; yy < sc; ++yy) for (int xx = 0; xx < sc; ++xx) {
                    int fx = x + (di * 8 + rx) * sc + xx, fy = y + ry * sc + yy;
                    if (fx >= 0 && fx < FW && fy >= 0 && fy < FH) frame[(size_t)fy * FW + fx] = c;
                }
            } }
    }
}
// HP-ИНДИКАТОР (REFERENCE, ZT d98e): 2 цифры Plane A @VRAM 0xC4CA = кол 37, ряд 9 = ЭКРАН (296,72), 8×8 тайлы —
// в ЖЁЛТОМ LCD-слоте справа сверху кокпита. Цвет по HP (спрайт-палитра 0x20D2 idx 7/6/5, дизер 0x77→0x55).
// (Прежняя позиция (254,104) sc=2 была неверна — не совпадала со слотом кокпита.)
inline void drawHpHud(uint32_t* frame, int FW, int FH, const GameData& gd) {
    const PlayerState& p = player();
    int hp = p.hp; if (hp < 0) hp = 0; if (hp > 99) hp = 99;            // ZT d98e: (HP*100-1)/100 → макс 99 (2 цифры)
    int hi, lo;                                                         // дизер-индексы спрайт-палитры по HP (маски d98e)
    if      (hp > 80) { hi = lo = 7; }                                  // 0x77 жёлтый
    else if (hp > 60) { hi = 7; lo = 6; }                              // 0x76
    else if (hp > 40) { hi = lo = 6; }                                  // 0x66 оранжевый
    else if (hp > 20) { hi = 6; lo = 5; }                              // 0x65
    else              { hi = lo = 5; }                                  // 0x55 красный
    // ZT d98e (font|mask): ШТРИХ цифры = health-цвет idx 7/6/5 (ЖЁЛТ→ОРАНЖ→КРАСН по HP), ФОН плитки = idx 0xF (#000000).
    uint32_t black = gd.heldPal.c[0xF];
    drawHudDigits(frame, FW, FH, gd.fontNum, 296, 72, 1, hp, 2, gd.heldPal.c[hi], gd.heldPal.c[lo], black, black);
}
// СЧЁТЧИК НЕУБИТЫХ ВРАГОВ (REFERENCE, ZT 1e00e): 2 цифры Plane A @VRAM 0xC482 = кол 1, ряд 9 = ЭКРАН (8,72),
// 8×8 тайлы — в БЕЛОМ readout-слоте слева сверху кокпита (палитра 3). Значение = enemyCountRemaining (кламп 99).
inline void drawEnemyCountHud(uint32_t* frame, int FW, int FH, const GameData& gd, int floor) {
    int n = enemyCountRemaining(floor);
    uint32_t yellow = gd.heldPal.c[7], black = gd.heldPal.c[0xF];      // ЖЁЛТЫЕ штрихи на чёрной плитке (как HP полн., idx 7 #FCFC24)
    drawHudDigits(frame, FW, FH, gd.fontNum, 8, 72, 1, n, 2, yellow, yellow, black, black);
}

// ВСПЫШКА УРОНА (палитра d800/d98e): тинт кадра — КРАСНЕЕТ + БЕЛЕЕТ с интенсивностью ~ УРОНУ (не только
// длительность). Большой урон → ярко-белая вспышка, малый → красноватая; затухает (p.flash убывает в updateActors).
// ⭐ FAITHFUL (d98e крутит CRAM, не пиксели): вспышка в оригинале — модификация ПАЛИТРЫ (3-битные уровни),
// значит цвета ВСЕГДА на нелинейной DAC-лестнице. Тинтим RGB, но СНАПИМ каждый канал к ближайшему MD DAC-уровню
// {0,52,87,116,144,172,206,255} — вспышка «ступенчатая», как CRAM оригинала (а не гладкий off-ladder градиент).
inline void applyDamageFlash(uint32_t* buf, int n) {
    const PlayerState& p = player();
    if (p.flash <= 0) return;
    static const int DAC[8] = { 0, 52, 87, 116, 144, 172, 206, 255 };   // нелинейный DAC МД (как cramToArgb)
    auto snap = [](int v) { int best = 0, bd = 1 << 30;                 // ближайший DAC-уровень (CRAM всегда на них)
        for (int k = 0; k < 8; ++k) { int d = v - DAC[k]; if (d < 0) d = -d; if (d < bd) { bd = d; best = DAC[k]; } }
        return best; };
    double a = p.flash / 15.0; if (a > 1.0) a = 1.0;                     // интенсивность ~ урон (затухает с flash)
    double amt = a * 0.6;                                                // сила тинта
    int wht = (int)(a * 210);                                           // белизна по интенсивности (больш.урон → белее)
    int tr = 255, tg = 30 + wht, tb = 30 + wht;                         // база красная + подмешать белый
    for (int i = 0; i < n; ++i) {
        uint32_t c = buf[i]; int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        r = snap(r + (int)((tr - r) * amt)); g = snap(g + (int)((tg - g) * amt)); b = snap(b + (int)((tb - b) * amt));
        buf[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}
