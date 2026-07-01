# Reproducible build of ztpp in a Linux container.
# Воспроизводимая сборка ztpp в Linux-контейнере.
#
# Headless render works out of the box; the interactive window needs X11/Wayland
# forwarding (OS-specific) — for playing, building natively is simpler.
# Безоконный рендер работает сразу; окно требует проброса X11/Wayland (зависит от ОС) —
# для игры проще собрать нативно.
#
#   docker build -t ztpp .
#   # headless one-frame render (mount your own ROM, NOT included):
#   docker run --rm -v "$PWD/roms:/roms" -v "$PWD/out:/out" ztpp "/roms/your.gen" --dump /out/frame.ppm
#
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake libsdl2-dev curl ca-certificates && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /ztpp
COPY . .
# Fetch Nuked-OPN2 (LGPL-2.1) — not bundled in the repo / качаем чип отдельно
RUN curl -fsSL https://raw.githubusercontent.com/nukeykt/Nuked-OPN2/master/ym3438.c -o src/opn2/ym3438.c && \
    curl -fsSL https://raw.githubusercontent.com/nukeykt/Nuked-OPN2/master/ym3438.h -o src/opn2/ym3438.h
RUN cmake -B build && cmake --build build -j
ENTRYPOINT ["./build/ztpp"]
