# Nuked-OPN2 (YM2612) — required, not bundled / нужно скачать отдельно

**English.** This port renders the game's audio through a cycle-accurate **Yamaha YM2612 / YM3438**
emulation. The chip core (Nuked-OPN2 by Alexey "Nuke.YKT" Khokholov, **LGPL-2.1**) is **not** included
in this repository. To build the port you must drop these two files into this folder (`src/opn2/`):

```
ym3438.c
ym3438.h
```

Get them from the upstream project: **https://github.com/nukeykt/Nuked-OPN2**

The audio code (`src/sound.hpp`) is written specifically against this chip's API
(`OPN2_Reset` / `OPN2_SetChipType` / `OPN2_Write` / `OPN2_Clock`) — without these files the build
will not link.

---

**Русский.** Звук в порте воспроизводится точным эмулятором чипа **Yamaha YM2612 / YM3438**.
Ядро чипа (Nuked-OPN2, автор Alexey «Nuke.YKT» Khokholov, **LGPL-2.1**) в репозиторий **не** включено.
Чтобы собрать порт, положи сюда (`src/opn2/`) два файла:

```
ym3438.c
ym3438.h
```

Скачать из оригинального проекта: **https://github.com/nukeykt/Nuked-OPN2**

Звуковой код (`src/sound.hpp`) спроектирован именно под API этого чипа
(`OPN2_Reset` / `OPN2_SetChipType` / `OPN2_Write` / `OPN2_Clock`) — без этих файлов сборка не слинкуется.
