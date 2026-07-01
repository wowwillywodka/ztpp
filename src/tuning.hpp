#pragma once
#include <cmath>
// ── НАСТРАИВАЕМЫЕ РАСХОЖДЕНИЯ ПОРТ↔ROM (ZT_PHYSICS §11), КРОМЕ КРОВИ ──
// Лёгкий общий заголовок: тумблеры geymplay-точности доступны и движку (actors/weapons → raycaster),
// и презентации (меню в ui.hpp). По умолчанию ВСЁ в reference-режиме (как оригинал).

// Физика игрока: true = ZT-инерция (разгон/выбег поворота, раздельные вперёд/назад, стрейф) [DAB8];
//                false = «движение без физики» — мгновенная скорость (свободный облёт уровня для навигации/отладки).
inline bool& playerPhysics()     { static bool v = true; return v; }

// Метрика дистанции для геймплея (урон-фолофф / радиусы взрыва / дальности): true = октагональная d7c0 (как ROM),
// false = Евклид (hypot — прежнее поведение порта). Расхождение ~5-10% по диагонали.
inline bool& gameDistOctagonal() { static bool v = true; return v; }

// Снять лимит инвентаря (5 несомых, как в ZT). true = безлимит (любое число слотов; пикапы всегда берутся).
inline bool& inventoryUnlimited() { static bool v = true; return v; }

// Замедление анимации стен (множитель периода). ZT-аниматор шёл per-frame, но рейкастер ~30fps; на 60fps порт вдвое
// быстрее → дефолт 2.0. Больше = медленнее. Настраивается в меню (1.0..4.0).
inline double& wallAnimSlow() { static double v = 1.25; return v; }

// МЫШЬ: поворот мышью (без инерции) + чувствительность. 0 = выкл (только клавиши). Угол = xrel·sens·0.004 рад/px.
inline double& mouseSensitivity() { static double v = 0.7; return v; }

// ЗВУК: вкл/выкл звуковых эффектов + громкость (0..1). Музыка пока не реализована.
inline bool&   soundOn()     { static bool b = false; return b; }   // ДЕФОЛТ: звук ВЫКЛ
inline double& soundVolume() { static double v = 0.7; return v; }

// КРОВЬ: брызги при попадании во врага (ZT 0x157ca). true = показывать. Консоль: `blood on/off`.
inline bool&   faBlood()     { static bool b = true; return b; }

// ЛЕСТНИЦА (переходы): тюнинги вида/поведения (доки требуют live-MAME валидации — слайдеры в меню).
inline double& faStairK()   { static double k = 1.0; return k; }   // ТЕКСТУРНЫЙ СКОС лестницы (texVShift=(d1−d0)·t·k); 0=плоско
inline double& faStairUni() { static double v = 0.35; return v; }  // СИЛА СПУСКА камеры (per-column D0·pitch·v/64); 0=без спуска

// Октагональная дистанция ZT (d7c0): (|dx|+|dy|+max(|dx|,|dy|))/2. Переключается gameDistOctagonal().
inline double gameDist(double dx, double dy) {
    if (!gameDistOctagonal()) return std::sqrt(dx * dx + dy * dy);
    double ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    double mx = ax > ay ? ax : ay;
    return (ax + ay + mx) * 0.5;
}
