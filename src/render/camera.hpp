// ztpp — src/render/camera.hpp: состояние камеры/игрока (вынесено из raycaster.hpp).
// POD: позиция/направление/этаж + ZT-инерция (DAB8) + межэтажные переходы + целочисленное
// авторитетное состояние (fixedmove, как МД). Отдельный заголовок — чтобы top-down карты/HUD
// зависели от Camera без тяжёлого рейкастера.
#pragma once

struct Camera {
    double px = 1.5, py = 1.5;
    double dirX = 1.0, dirY = 0.0;
    double planeX = 0.0, planeY = 0.66;
    int    floor = 0;
    // ZT-ИНЕРЦИЯ движения (DAB8): три плавно разгоняемых значения (eased) — угл.скорость / вперёд / стрейф.
    double turnVel = 0.0, fwdVel = 0.0, strafeVel = 0.0;
    // Межэтажные переходы (см. rcUpdateTransit): питч вида + автомат лифта.
    double pitch = 0;       // вертикальный сдвиг вида (ZT-ед., ±64≈одна высота стены); = -cabin
    double cabin = 0;       // счётчик кабины/наклона лестницы (ZT $FF116E, -64..+64)
    int    elevState = 0;   // ROM $FF1170: 0 idle | +1 UP(floor--) | -1 DOWN(floor++) | ±2 приехал-стоп. Вне rcUpdateTransit читается только !=0
    double ang512 = 0;      // НЕПРЕРЫВНЫЙ аккумулятор угла в ROM-единицах (512=окружность); dir берётся ОТ ОКРУГЛЁННОГО
    // ── ЦЕЛОЧИСЛЕННОЕ СОСТОЯНИЕ (fixedmove, как МД DAB8): авторитетно, из него выводятся float px/py/dir ──
    int  angI = -1;         // угол 0..511 (МД -$71fc; a0=−Y). −1 = не синхронизировано с float
    int  pxI = 0, pyI = 0;  // позиция в 8.8 fixed (МД -$7214/-$7212 = X·256/Y·256)
    int  turnV = 0, fwdV = 0, strafeV = 0;  // целые скорости (МД -$7182/-$717e/-$717a)
};
