// ztpp — HUD-СООБЩЕНИЯ как в оригинале ZT (подбор/этаж/зачистка/мало патронов/HP и пр.).
//
// Дизасм (text.asm): очередь FUN_0001e0fa, enqueue FUN_0001e0c2 (лимит 0x10=16), менеджер 0001e2cc.
// Рендер FUN_0001e0fa@0x1e204: 4 ряда VRAM 0xCA04/0xCA84/0xCB04/0xCB84 → план A база 0xC000, ширина 64
// тайла → тайл (col 2, row 20..23) = ПИКСЕЛЬ (16,160) в экране 320×224 = НИЖНИЙ-ЛЕВЫЙ угол.
// Каждый ряд = 9 символов → СООБЩЕНИЕ = 4 ряда × 9 = 36 символов, шрифт Letters (через таблицу 0x1e04c).
// Очередь: показ по таймеру, по одному, FIFO. Тексты — БАЙТ-В-БАЙТ из ROM (таблица предметов 0x1e326,
// одиночные 0x1e5f0/0x1e61e/0x1e64c/0x1e67a/0x1e6f8/0x1e726/0x1e754/0x1e7b8/0x1e7ea).
#pragma once
#include "ui.hpp"        // drawText (Letters/g_uiFont)
#include <deque>
#include <cstring>

// Строки 36 символов = 4 ряда × 9 (как раскладка в ROM). Пустые места — пробелы.
namespace ztmsg {
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
}

struct HudMessages {
    struct Msg { char row[4][10]; };     // 4 ряда по ≤9 символов + '\0'
    std::deque<Msg> queue;
    int  timer = 0;                      // кадров до смены текущего сообщения
    int  showFrames = 70;               // длительность показа (ставится из main по frameLimit)

    // Разобрать 36-символьную строку (4×9) в сообщение и поставить в очередь (лимит 16, как ZT 0x10).
    void push(const char* s36) {
        if (!s36 || queue.size() >= 16) return;
        size_t len = std::strlen(s36);
        Msg m{};
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
    void clear() { queue.clear(); timer = 0; }

    // Тик раз в кадр: дотикать текущее, при истечении снять и показать следующее (FIFO).
    void update() {
        if (timer > 0 && --timer == 0 && !queue.empty()) queue.pop_front();
        if (timer == 0 && !queue.empty()) timer = showFrames;
    }

    // Отрисовка текущего сообщения в НИЖНЕМ-ЛЕВОМ углу (как ROM: x≈16,y≈160 в 320×224 → масштаб FB).
    // sc — масштаб шрифта (Letters 8×8). x,y — левый-верх блока сообщения.
    void draw(FB& fb, int x, int y, int sc) const {
        if (queue.empty() || timer <= 0) return;
        const Msg& m = queue.front();
        for (int r = 0; r < 4; ++r)
            if (m.row[r][0]) drawText(fb, x, y + r * 8 * sc, m.row[r], 0xFFFFFFFFu, sc);
    }
};
