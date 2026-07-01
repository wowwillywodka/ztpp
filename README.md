# ztpp — Zero Tolerance port (C++ / SDL2)

A from-scratch reimplementation of **Zero Tolerance** (Sega Mega Drive / Genesis, 1994) in
C++/SDL2 — a first-person engine rebuilt from behaviour, not a decompilation. Work in progress.

![ztpp running in reference mode — the original Zero Tolerance cockpit HUD](screenshot.png)

*Reference mode — the original ZT cockpit HUD with the first-person view, radar and inventory (macOS build).*

> Created with heavy use of **Claude (Anthropic's AI)** — the reverse-engineering,
> analysis, and most of the implementation were done together with it.

> ⚠ Only the original **Zero Tolerance** ROM (US/EU release) is supported right now. The German build,
> Zero Tolerance Underground and Beyond Zero Tolerance are planned — see [Planned](#planned).

> Development is intentionally **very slow-paced** — this is a hobby project.

> ⚠ Developed and tested **only on macOS**. Linux/Windows should work (portable CMake + SDL2) but are
> untested — patches welcome.

## Goal
Build a reasonably faithful, original-accurate port that can be **extended and modded**. Right now it
looks more like a developer demo than a game — so there is **no release** yet.

## What works
- Rendering as close to the original as possible, reconstructed from the reverse-engineered draw algorithm
- Original physics (from reverse engineering)
- Wall animations and destructible walls
- Enemies — partially implemented
- Enemy AI ~80% matching the reverse
- Weapons, behaviour ~70% matching the reverse
- All maps from all episodes
- Weapon pickups from corpses
- GZDoom-style console
- Mouse control
- 5-slot inventory, with an option to remove the limit
- Blood physics ~50% matching the original

## Not done yet
- Resolution settings
- Character selection
- Cutscenes / intros
- Remaining-enemy display and the episode-completion mechanic
- Stairs and elevator graphics
- Original-accurate blood physics
- Proper HUD readouts — alive enemies on the level and health (currently looks cursed)
- Item readouts and their limit
- Boss behaviour
- Sound
- Melee / fist-fight mechanic
- Fire-extinguisher graphics
- Properly working enemy animations
- Passwords
- Pause map
- Snipers
- Original radar

## Planned
- Support for **Zero Tolerance** (original & German builds), **Zero Tolerance Underground**, and
  **Beyond Zero Tolerance** (June & July prototypes)
- Both the original graphics mode and a configurable full-screen mode
- Save games
- Tuning of clock-dependent mechanics
- Modding
- Audio through a **YM2612 / YM3438** chip emulator
- Playback of audio files
- Gamepad support with rumble
- Mechanics that push past the engine's and the Mega Drive's limits, e.g.:
  - A fully working train in ZTU and BZT
  - Floor / ceiling texturing
  - Particles
  - A "what if the game had shipped on Sega CD / 32X" mode

## Requirements
- A C++17 compiler and **CMake ≥ 3.16**
- **SDL2**:
  - macOS: `brew install sdl2`
  - Linux (Debian/Ubuntu): `sudo apt install libsdl2-dev`
  - Windows: vcpkg (`vcpkg install sdl2`) or MSYS2 (`pacman -S mingw-w64-x86_64-SDL2`)
- **Nuked-OPN2** chip core — **not bundled**. Download `ym3438.c` and `ym3438.h` from
  <https://github.com/nukeykt/Nuked-OPN2> into [`src/opn2/`](src/opn2/) (see the note there).
- **Your own copy of the ROM.** It is **not** included — you supply the file yourself. No game data
  ships in this repo.

## Build
> ⚠ Built and tested **only on macOS** (Apple Silicon & Intel). Linux/Windows should work (portable
> CMake + SDL2) but are **untested** — the steps below are the verified macOS path.

1. **Install SDL2** — `brew install sdl2` (Linux/Windows: see [Requirements](#requirements)).
2. **Add the Nuked-OPN2 chip core** (LGPL-2.1, *not* bundled): download `ym3438.c` and `ym3438.h`
   from <https://github.com/nukeykt/Nuked-OPN2> into [`src/opn2/`](src/opn2/).
3. **Configure & build:**
   ```bash
   cmake -B build
   cmake --build build
   ```
   On macOS you can instead just run `./build.sh` — it does both steps.

The resulting binary is `build/ztpp` (see [Run](#run) below).
On Windows pass your toolchain, e.g. `cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`.

### Docker (optional)
A `Dockerfile` builds it reproducibly (Linux container, fetches Nuked-OPN2 for you):
```bash
docker build -t ztpp .
docker run --rm -v "$PWD/roms:/roms" -v "$PWD/out:/out" ztpp "/roms/your.gen" --dump /out/frame.ppm
```
(The interactive window needs OS-specific X11/Wayland forwarding; headless `--dump` works as-is.)

## Run
```bash
./build/ztpp "path/to/Zero Tolerance (ROM).gen"
```
Headless render of one frame (no window): `./build/ztpp <rom> --dump out.ppm`.
Controls and the in-game console are documented in [`CONSOLE.md`](CONSOLE.md).

## License
- Port source code: **LGPL-2.1** — © **Willy Wodka**.
- `src/opn2/ym3438.*` (Nuked-OPN2): **LGPL-2.1**, fetched separately, © Alexey Khokholov.
- *Zero Tolerance* and all its data/assets are © their respective rights holders and are **not** part
  of this repository.

## Related repositories
- **ztextractor** — data inspection/extraction tool for the ROM: <https://github.com/wowwillywodka/ztextractor>
- **Reverse-engineering notes** — addresses, formats, subsystem behaviour: <https://github.com/wowwillywodka/ztresearchdocs>

## Acknowledgements
- **Smoke** — the **BZTEdit** editor; the map-cell icons used in the port come from it.
- **alex-west** — **zmap-tools** (ZMAP level format and ZT texture-bank references).
- **Firewing** & **Lurler** — **ZTEdit** / its modified version (ZMAP format).
- **Dr MeFiSto** (<https://github.com/lab313ru>) — the Ghidra plugin for reverse-engineering Mega Drive games.
- **realmonster** — GEMS tools (<https://github.com/realmonster/GEMS>); the port's understanding of GEMS sound is based on them.
- **ValleyBell** — GEMSPlay (GEMS PCM sample-bank format).
- **Nuked-OPN2** by **nukeykt** — the YM2612 / YM3438 emulator used for sound playback.
- **Claude (Anthropic)** — reverse-engineering, analysis, and most of the implementation.

---
---

# ztpp — порт Zero Tolerance (C++ / SDL2)

Реимплементация игры **Zero Tolerance** (Sega Mega Drive / Genesis, 1994) на C++/SDL2 с нуля —
движок от первого лица, воссозданный по поведению (это **не** декомпиляция). В разработке.

![ztpp в reference-режиме — оригинальный кокпит-HUD Zero Tolerance](screenshot.png)

*Reference-режим — оригинальный кокпит-HUD ZT: вид от первого лица, радар и инвентарь (сборка на macOS).*

> Сделано при активном участии **Claude (ИИ от Anthropic)** — реверс-инжиниринг,
> анализ и бо́льшая часть реализации выполнены вместе с ним.

> ⚠ Сейчас поддерживается только оригинальный ROM **Zero Tolerance** (релиз US/EU). Немецкий билд,
> Zero Tolerance Underground и Beyond Zero Tolerance — в планах (см. [Планируется](#планируется)).

> Разработка сознательно ведётся **очень неспешно** — это хобби-проект.

> ⚠ Разрабатывалось и тестировалось **только на macOS**. Linux/Windows должны работать (портативные
> CMake + SDL2), но не проверялись — патчи приветствуются.

## Цель
Сделать достаточно близкий к оригиналу порт с возможностью **расширения и моддинга**. Сейчас это
выглядит скорее как демка для разработчиков, чем как игра, — поэтому **релиза нет**.

## Что готово
- Максимально близкий к оригиналу рендер, воссозданный на основе реверса оригинального алгоритма отрисовки
- Оригинальная физика на основе реверса
- Анимации стен, разрушаемость стен
- Враги — частично реализованы
- ИИ врагов, на ~80% совпадающий с реверсом
- Оружие, поведение на ~70% совпадающее с реверсом
- Все карты из всех эпизодов
- Пикапы оружия с трупов
- Консоль в стиле GZDoom
- Управление мышью
- 5-слотовый инвентарь с возможностью снять ограничение
- Физика крови, на ~50% совпадающая с оригиналом

## Что не сделано
- Настройки разрешения
- Выбор персонажа
- Заставки
- Отображение неубитых врагов и механика завершения эпизодов
- Графика лестниц и лифтов
- Физика крови как в оригинале
- Нормальное отображение показателей — живых врагов на уровне и здоровья (выглядит проклято)
- Показатели предметов и их лимит
- Поведение боссов
- Звук
- Механика кулачного боя
- Графика огнетушителя
- Нормально работающие анимации врагов
- Пароли
- Карта с паузы
- Снайперы
- Оригинальный радар

## Планируется
- Поддержка **Zero Tolerance** (оригинальный и немецкий билды), **Zero Tolerance Underground** и
  **Beyond Zero Tolerance** (июньский и июльский прототипы)
- Оригинальный режим отображения графики и полноэкранный настраиваемый
- Сохранения
- Настройка клок-зависимых механик
- Моддинг
- Воспроизведение звука через эмулятор чипа **YM2612 / YM3438**
- Воспроизведение аудиофайлов
- Поддержка геймпадов и вибрации
- Механики, позволяющие выйти за ограничения движка и Mega Drive, например:
  - Полноценный поезд в ZTU и BZT
  - Текстурирование пола / потолка
  - Частицы
  - Режим «если бы игра вышла на Sega CD / 32X»

## Требования
- Компилятор C++17 и **CMake ≥ 3.16**
- **SDL2**:
  - macOS: `brew install sdl2`
  - Linux (Debian/Ubuntu): `sudo apt install libsdl2-dev`
  - Windows: vcpkg (`vcpkg install sdl2`) или MSYS2 (`pacman -S mingw-w64-x86_64-SDL2`)
- Ядро чипа **Nuked-OPN2** — **в репозиторий не входит**. Скачай `ym3438.c` и `ym3438.h` с
  <https://github.com/nukeykt/Nuked-OPN2> в [`src/opn2/`](src/opn2/) (см. заметку там).
- **Свой ROM.** В репозитории его **нет** — файл предоставляешь сам. Никаких игровых данных в
  репозитории нет.

## Сборка
> ⚠ Собиралось и тестировалось **только на macOS** (Apple Silicon и Intel). Linux/Windows должны
> работать (портативные CMake + SDL2), но **не проверялись** — шаги ниже это выверенный путь для macOS.

1. **Установи SDL2** — `brew install sdl2` (Linux/Windows — см. [Требования](#требования)).
2. **Добавь ядро чипа Nuked-OPN2** (LGPL-2.1, в репозиторий *не* входит): скачай `ym3438.c` и `ym3438.h`
   с <https://github.com/nukeykt/Nuked-OPN2> в [`src/opn2/`](src/opn2/).
3. **Конфигурация и сборка:**
   ```bash
   cmake -B build
   cmake --build build
   ```
   На macOS можно вместо этого просто запустить `./build.sh` — он делает оба шага.

Готовый бинарник — `build/ztpp` (см. [Запуск](#запуск) ниже).
На Windows укажи тулчейн, напр. `cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`.

### Docker (опционально)
`Dockerfile` собирает воспроизводимо (Linux-контейнер, сам качает Nuked-OPN2):
```bash
docker build -t ztpp .
docker run --rm -v "$PWD/roms:/roms" -v "$PWD/out:/out" ztpp "/roms/your.gen" --dump /out/frame.ppm
```
(Интерактивное окно требует проброса X11/Wayland под конкретную ОС; безоконный `--dump` работает сразу.)

## Запуск
```bash
./build/ztpp "путь/к/Zero Tolerance (ROM).gen"
```
Безоконный рендер одного кадра: `./build/ztpp <rom> --dump out.ppm`.
Управление и встроенная консоль — в [`CONSOLE.md`](CONSOLE.md).

## Лицензия
- Код порта: **LGPL-2.1** — © **Willy Wodka**.
- `src/opn2/ym3438.*` (Nuked-OPN2): **LGPL-2.1**, качается отдельно, © Alexey Khokholov.
- *Zero Tolerance* и все её данные/ассеты © правообладателям и **не** входят в этот репозиторий.

## Связанные репозитории
- **ztextractor** — инструмент инспекции/извлечения данных из ROM: <https://github.com/wowwillywodka/ztextractor>
- **Заметки по реверс-инжинирингу** — адреса, форматы, поведение подсистем: <https://github.com/wowwillywodka/ztresearchdocs>

## Благодарности
- **Smoke** — редактор **BZTEdit**; иконки клеток карты в порте взяты из него.
- **alex-west** — **zmap-tools** (формат уровней ZMAP и банки текстур ZT).
- **Firewing** и **Lurler** — **ZTEdit** / его модифицированная версия (формат ZMAP).
- **Dr MeFiSto** (<https://github.com/lab313ru>) — плагин для Ghidra под реверс игр Mega Drive.
- **realmonster** — инструменты GEMS (<https://github.com/realmonster/GEMS>); понимание звука GEMS в порте сделано на их основе.
- **ValleyBell** — GEMSPlay (формат банка PCM-сэмплов GEMS).
- **Nuked-OPN2** от **nukeykt** — эмулятор YM2612 / YM3438 для воспроизведения звука.
- **Claude (Anthropic)** — реверс-инжиниринг, анализ и бо́льшая часть реализации.
