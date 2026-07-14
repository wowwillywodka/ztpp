#pragma once
// ── ПЕРЕНАЗНАЧАЕМЫЕ КЛАВИШИ + 4 ПРЕСЕТА (SDL-free: скан-коды как int, имена-строки заполняет main.cpp через SDL) ──
// Только ИГРОВЫЕ действия (движение/огонь/прыжок/присед/оружие/карта). Дебаг-хоткеи (noclip/god/тюнинг) не биндятся.
#include <string>

enum GameAction {
    GA_FORWARD, GA_BACK, GA_STRAFE_L, GA_STRAFE_R, GA_TURN_L, GA_TURN_R,
    GA_FIRE, GA_JUMP, GA_CROUCH, GA_WEAP_PREV, GA_WEAP_NEXT, GA_MAP, GA_COUNT
};
const int NUM_PRESETS = 4;                                                     // 4 конфигурируемых пресета управления

inline int (*keyBindsAll())[GA_COUNT] { static int b[NUM_PRESETS][GA_COUNT] = {}; return b; }  // [пресет][действие] = scancode
inline int&  curPreset()    { static int p = 0; return p; }                    // активный пресет 0..3
inline int&  keyBindP(int p, int a) { return keyBindsAll()[p][a]; }            // биндинг конкретного пресета
inline int&  keyBind(int a) { return keyBindsAll()[curPreset()][a]; }          // биндинг АКТИВНОГО пресета (весь ввод игры через это)
inline std::string* keyBindNames() { static std::string n[GA_COUNT]; return n; } // экранные имена клавиш АКТИВНОГО пресета (main.cpp через SDL)
inline int&  rebindAction() { static int a = -1; return a; }                   // индекс действия в режиме переназначения (−1 = нет)

inline const char* gaLabel(int a) {
    static const char* L[GA_COUNT] = {
        "Move forward", "Move back", "Strafe left", "Strafe right", "Turn left", "Turn right",
        "Fire", "Jump", "Crouch", "Weapon prev", "Weapon next", "Map / Pause"
    };
    return (a >= 0 && a < GA_COUNT) ? L[a] : "?";
}
inline const char* gaIniKey(int a) {                                           // базовое имя ключа ini (+ "_p<preset>")
    static const char* K[GA_COUNT] = {
        "key_forward", "key_back", "key_strafe_l", "key_strafe_r", "key_turn_l", "key_turn_r",
        "key_fire", "key_jump", "key_crouch", "key_weap_prev", "key_weap_next", "key_map"
    };
    return (a >= 0 && a < GA_COUNT) ? K[a] : "";
}
// Разбор строки настроек биндинга: "<key>_p<preset>" (новое) / "<key>" (легаси → пресет 0) / "control_preset".
inline bool parseKeyBind(const std::string& k, int v) {
    if (k == "control_preset") { curPreset() = (v < 0 ? 0 : (v >= NUM_PRESETS ? NUM_PRESETS - 1 : v)); return true; }
    for (int a = 0; a < GA_COUNT; ++a) {
        std::string base = gaIniKey(a);
        if (k == base) { keyBindP(0, a) = v; return true; }                    // легаси (без суффикса) → пресет 0
        for (int p = 0; p < NUM_PRESETS; ++p)
            if (k == base + "_p" + std::to_string(p)) { keyBindP(p, a) = v; return true; }
    }
    return false;
}
