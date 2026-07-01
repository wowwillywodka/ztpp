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
    {"FLAMETHROWER",  true,  10, 99},   // idx13 = ОГНЕМЁТ (held block8, авто+пламя, фаер 0x13024)
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
inline int rcTryPickup(Inventory& inv, const Level& lvl, int floor, double px, double py) {
    int x = (int)px, y = (int)py;
    if (x < 0 || y < 0 || x >= Level::W || y >= Level::H) return -1;
    uint8_t ct = lvl.cellType(floor, x, y);
    int k = pickKey(floor, x, y);
    if (pickedSet().count(k)) return -1;             // уже подобрано
    if (ct == 0x25) {                                // МЕДПАК (+HP, не инвентарь)
        if (player().hp >= player().maxHp) return -1;   // полное HP → не подбирать (ZT 0x11b7c)
        pickedSet().insert(k); healPlayer(20); return PICK_MEDKIT;
    }
    int idx = pickupItemIdx(ct);
    if (idx < 1 || idx >= 15) return -1;
    bool firstTime = !inv.has(idx);
    if (firstTime && !inv.addItem(idx)) return -1;   // инвентарь полон → не берём (остаётся в мире)
    pickedSet().insert(k);                           // занять слот удалось (или добор) → гасим клетку
    inv.ammo[idx] += ITEMS[idx].ammoPickup;          // добор боезапаса с КАПОМ (таблица @0x11240)
    if (inv.ammo[idx] > ITEMS[idx].ammoCap) inv.ammo[idx] = ITEMS[idx].ammoCap;
    if (firstTime && ITEMS[idx].weapon) {            // авто-выбор нового ствола (встать на его слот)
        for (size_t i = 0; i < inv.carried.size(); ++i) if (inv.carried[i] == idx) { inv.sel = (int)i; break; }
        inv.syncCurrent(); inv.slide = 1.0;
    } else inv.syncCurrent();
    return idx;
}

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

// ── ВЫСТРЕЛ (спавн актёров) ─────────────────────────────────────────────────────
// По типу оружия: hit-scan (пистолет 8 / ПП-лазер 10 / дробовик 12 / пульс 14 / огнетушитель 4) — МГНОВЕННО
// рейкаст-урон D1 (damageRay) → искра у стены ТОЛЬКО при промахе (как ZT: 0x12ad4 спавнит трассер лишь когда
// враг не задет; на попадании урон применяется напрямую). Ракета/граната — летящий снаряд → взрыв. Огнемёт —
// пламя. Мина — ставится ВПЕРЁД по трассе (до 8 клеток, ZT 12bfe). Пассивные предметы/кулаки — удар (melee).
inline void fireSpawn(const Inventory& inv, const Level& lvl, const Camera& cam) {
    int id = inv.current;
    int kind = heldDisplayKind(id);
    if (kind == 0) {                                            // КУЛАКИ/пассивные предметы — УДАР в упор (короткий мили-конус)
        for (Actor& a : actors()) {                            // (в ZT кулаки урона не наносят; здесь — рабочий удар по просьбе юзера)
            if (!a.active || a.think != AT_ENEMY || a.floor != cam.floor) continue;
            double rx = a.x - cam.px, ry = a.y - cam.py, d = std::hypot(rx, ry);
            if (d < 0.01 || d > 1.3) continue;                 // только вплотную (~1.3 клетки)
            if ((rx * cam.dirX + ry * cam.dirY) / d > 0.6) { hitEnemy(a, 2, cam.px, cam.py); break; }  // в конусе взгляда → удар (урон 2 + отлёт)
        }
        return;
    }
    double x = cam.px + cam.dirX * 0.4, y = cam.py + cam.dirY * 0.4;
    if (id == 2)  {                                             // МИНА — ПРЯМО ПЕРЕД игроком (ZT 0x12bfe: forward/16 ×8 ≈ ½ кл)
        double mx = cam.px, my = cam.py;
        for (int i = 0; i < 8; ++i) {                           // мелкий шаг (½/16 ≈ 0.03 кл) до стены — итог ~0.5 кл
            double nx = mx + cam.dirX * 0.0625, ny = my + cam.dirY * 0.0625;
            if (cellBlocks(lvl.cellType(cam.floor, (int)nx, (int)ny))) break;
            mx = nx; my = ny;
        }
        spawnMine(mx, my, cam.floor); return;
    }
    if (id == 13) { spawnFlameP(x, y, cam.floor, cam.dirX, cam.dirY); return; }                       // огнемёт
    if (id == 4)  {                                            // ОГНЕТУШИТЕЛЬ (ZT 0x12cfe): ОДИН спрайт пены (БЕЗ разброса), падает вниз, тушит огонь
        double mx = cam.px + cam.dirX * 0.35, my = cam.py + cam.dirY * 0.35;
        spawnFoam(mx, my, cam.floor, cam.dirX, cam.dirY);
        return;
    }
    if (id == 7)  { spawnGrenade(cam.px + cam.dirX * 0.3, cam.py + cam.dirY * 0.3, cam.floor, cam.dirX, cam.dirY); return; } // граната
    if (id == 11) { spawnBullet(x, y, cam.floor, cam.dirX, cam.dirY, 0.35, A_EXPL_TILE, 200); return; } // ракета
    // hit-scan стрелковое: ДИСТ-урон врагу по кривой оружия (playerWeaponDamage); промах → искра у стены.
    double hx, hy;
    if (!damageRay(lvl, cam.floor, cam.px, cam.py, cam.dirX, cam.dirY, id, hx, hy))
        spawnSparkA(hx, hy, cam.floor);
}

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
    auto blit = [&](int block, int startTile, int ntiles, int W, int sx, int sy, bool hflip) {
        if (block < 0 || block >= gd.heldBlocks) return;
        const uint8_t* bp = gd.heldGfx.data() + (size_t)block * 672;
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
        } else {                                           // удар: рука+предплечье (2 спрайта), одна сторона
            int fr = f - 1; if (fr > 2) fr = 2;            // кадры 0..2 (table 0x1219a)
            static const int PR[3][4] = {{96,180, 84,177}, {88,168, 64,162}, {96,180, 84,177}};  // правый (hflip)
            static const int PL[3][4] = {{96,116, 84,119}, {88,128, 64,134}, {96,116, 84,119}};  // левый
            const int* p = inv.punchSide ? PL[fr] : PR[fr];
            bool hf = (inv.punchSide == 0);
            blit(0, 0, 12, 3, p[1], p[0], hf);             // нижний (предплечье)
            blit(0, 0, 12, 3, p[3], p[2], hf);             // верхний (кулак)
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
inline void drawInventoryHud(uint32_t* frame, int FW, int FH, const GameData& gd, const Inventory& inv) {
    std::vector<int> r = inv.ring();                      // кольцо: значения слотов (−1 = пусто/кулаки)
    int n = (int)r.size();
    if (gd.hudIcons.empty() || n == 0) return;
    static const int SLOT_X[5] = {40, 88, 144, 200, 248};
    const int SLOT_Y = 3;
    auto px = [&](int x, int y, uint32_t c) { if (x >= 0 && x < FW && y >= 0 && y < FH) frame[(size_t)y * FW + x] = c; };
    uint8_t ip[32 * 32];
    for (int k = 0; k < 5; ++k) {
        int j = inv.sel + (k - 2);                       // центр (k=2) = текущий
        if (n >= 5) j = ((j % n) + n) % n;               // карусель-обмотка (окно 5 вокруг текущего)
        else if (j < 0 || j >= n) continue;              // <5 слотов: пустые края
        int id = r[j];
        if (id < 0) continue;                            // пустой слот / кулаки — иконки нет
        int icon = gd.iconForId(id);
        if (icon < 0) continue;
        gd.decodeIcon(icon, ip);
        int bx = SLOT_X[k];
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) { uint8_t i = ip[y * 32 + x]; if (i) px(bx + x, SLOT_Y + y, gd.heldPal.c[i]); }
        // боезапас (только оружие) — маленький номер ZT-шрифтом в НИЗ-ЛЕВО углу (как ZT тайл 3 / FUN_1DF54).
        if (id >= 1 && id < 15 && ITEMS[id].weapon) {
            int a = inv.ammo[id]; if (a > 99) a = 99; if (a < 0) a = 0;
            char b[4]; int len = std::snprintf(b, sizeof b, "%d", a);
            int dx = bx + 1, dy = SLOT_Y + 25;                         // низ-лев. угол иконки
            for (int yy = -1; yy < 7; ++yy) for (int xx = -1; xx < len * 4; ++xx) px(dx + xx, dy + yy, 0xFF000000u);  // плашка-тайл
            for (int c = 0; c < len; ++c) {
                int d = b[c] - '0';
                for (int ry = 0; ry < 6; ++ry) for (int rx = 0; rx < 4; ++rx) {
                    uint8_t pi = gd.digitPx(d, rx, ry);
                    if (pi) px(dx + c * 4 + rx, dy + ry, gd.heldPal.c[pi]);
                }
            }
        }
    }
    // подсветка ТЕКУЩЕГО (центральная панель): тонкая рамка
    int rx = SLOT_X[2] - 1, ry = SLOT_Y - 1, rw = 34, rh = 34;
    for (int x = 0; x < rw; ++x) { px(rx + x, ry, 0xFF80FF80u); px(rx + x, ry + rh - 1, 0xFF80FF80u); }
    for (int y = 0; y < rh; ++y) { px(rx, ry + y, 0xFF80FF80u); px(rx + rw - 1, ry + y, 0xFF80FF80u); }
}

// Цвет HP-числа по ZT (d98e: спрайт-палитра 0x20D2 индексы 7/6/5 по HP) — ЖЁЛТ→ОРАНЖ→КРАСН (НЕ зелёный!).
// Пороги масок: HP>80 idx7, >40 idx6, иначе idx5 (предупр. текст.asm d952<50/d970<15).
inline uint32_t hpColor(int hp) { return (hp > 60) ? 0xFFFCFC24u : (hp > 20 ? 0xFFD86C24u : 0xFFFC2424u); }

// HP-ИНДИКАТОР (REFERENCE): ЧИСЛО (2 цифры) ШРИФТОМ ЧИСЕЛ ZT (fontNum 0x16E618, как d98e), цвет по HP
// (спрайт-палитра 0x20D2 idx 7/6/5 = жёлт/оранж/красн, 2-тон ДИЗЕР как маски 0x77→0x55). Не зелёный, не ammo-шрифт.
inline void drawHpHud(uint32_t* frame, int FW, int FH, const GameData& gd) {
    const PlayerState& p = player();
    int hp = p.hp; if (hp < 0) hp = 0; if (hp > 99) hp = 99;            // 2 цифры (ZT d98e: tens+units)
    int hi, lo;                                                         // дизер-индексы спрайт-палитры по HP (маски d98e)
    if      (hp > 80) { hi = lo = 7; }                                  // 0x77 жёлтый
    else if (hp > 60) { hi = 7; lo = 6; }                              // 0x76
    else if (hp > 40) { hi = lo = 6; }                                  // 0x66 оранжевый
    else if (hp > 20) { hi = 6; lo = 5; }                              // 0x65
    else              { hi = lo = 5; }                                  // 0x55 красный
    uint32_t cHi = gd.heldPal.c[hi], cLo = gd.heldPal.c[lo];
    const ZtFont& f = gd.fontNum;
    if (!f.have) return;
    int x = 254, y = 104, sc = 2;                                       // правый-нижний угол 3D-вида (справа на экране)
    int digs[2] = { (hp / 10) % 10, hp % 10 };
    for (int di = 0; di < 2; ++di) {
        int a = '0' + digs[di]; if (a < 0 || a >= 128 || !f.supported[a]) continue;
        for (int ry = 0; ry < 8; ++ry) { uint8_t row = f.glyph[a][ry];
            for (int rx = 0; rx < 8; ++rx) if (row & (1 << rx)) {
                uint32_t c = ((rx + ry) & 1) ? cHi : cLo;              // 2-тон дизер
                for (int yy = 0; yy < sc; ++yy) for (int xx = 0; xx < sc; ++xx) {
                    int fx = x + (di * 8 + rx) * sc + xx, fy = y + ry * sc + yy;
                    if (fx >= 0 && fx < FW && fy >= 0 && fy < FH) frame[(size_t)fy * FW + fx] = c;
                }
            } }
    }
}

// ВСПЫШКА УРОНА (палитра d800/d98e): тинт кадра — КРАСНЕЕТ + БЕЛЕЕТ с интенсивностью ~ УРОНУ (не только
// длительность). Большой урон → ярко-белая вспышка, малый → красноватая; затухает (p.flash убывает в updateActors).
inline void applyDamageFlash(uint32_t* buf, int n) {
    const PlayerState& p = player();
    if (p.flash <= 0) return;
    double a = p.flash / 15.0; if (a > 1.0) a = 1.0;                     // интенсивность ~ урон (затухает с flash)
    double amt = a * 0.6;                                                // сила тинта
    int wht = (int)(a * 210);                                           // белизна по интенсивности (больш.урон → белее)
    int tr = 255, tg = 30 + wht, tb = 30 + wht;                         // база красная + подмешать белый
    for (int i = 0; i < n; ++i) {
        uint32_t c = buf[i]; int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        r = r + (int)((tr - r) * amt); g = g + (int)((tg - g) * amt); b = b + (int)((tb - b) * amt);
        buf[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}
