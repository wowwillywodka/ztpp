#pragma once
// ztpp — src/launcher_data.hpp: DATA-слой окна выбора ROM (скан каталогов, детекция билда по
// заголовку, память последнего выбора, состояние списка). ОБЩИЙ для нативного macOS-окна
// (launcher_osx.mm) и SDL-фолбэка (launcher.hpp). Намеренно БЕЗ рендер-слоя (framebuffer/ui):
// иначе ObjC++ TU потянул бы проектный тип Rect (ui.hpp), который конфликтует с легаси-Rect из
// Cocoa/MacTypes.h. UI-константы/отрисовка живут в launcher.hpp.

#include <SDL.h>                // SDL_GetBasePath/SDL_free (каталог бинарника при скане)
#include "gamedata.hpp"
#include "profile.hpp"   // dataRoot() — ini в Application Support при .app-запуске         // Build + detectBuildFromHeader (единая детекция с загрузчиком игры)
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace launcher {

struct Entry {
    std::string path;      // полный путь к файлу
    std::string file;      // имя файла (для списка)
    std::string title;     // overseas-название из заголовка ROM (0x150)
    std::string serial;    // серийник (0x180, 14 символов)
    Build       build = Build::Unknown;
    size_t      size  = 0;
    bool        supported = false;
};

inline const char* buildLabel(Build b) {
    switch (b) {
        case Build::ZT:        return "ZERO TOLERANCE (RELEASE)";
        case Build::ZTU:       return "ZT UNDERGROUND V1.5 (PARTIAL SUPPORT)";   // играбелен, но часть контента/механик в работе
        case Build::ZT_German: return "ZERO TOLERANCE (GERMAN)";
        case Build::BZT_June:  return "BEYOND ZT PROTO 1995-06-23";
        case Build::BZT_July:  return "BEYOND ZT PROTO 1995-07-14";
        default:               return "UNKNOWN MEGA DRIVE ROM";
    }
}
inline bool buildSupported(Build b) { return b == Build::ZT || b == Build::ZTU; }
// Статус для колонки лаунчера: ZTU играбелен, но поддержан ЧАСТИЧНО (юзер 2026-07-22).
inline const char* buildStatus(Build b) {
    if (b == Build::ZT)  return "SUPPORTED";
    if (b == Build::ZTU) return "PARTIALLY SUPPORTED";
    return "NOT SUPPORTED YET";
}

// Прочитать заголовок и собрать Entry. false — не ROM семейства ZT (или нечитаем).
// Детекция — общая с игрой (gamedata detectBuildFromHeader); NotMd отсекаем сами по маркеру SEGA.
inline bool probeRom(const std::string& path, Entry& out, bool keepUnknown) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    size_t sz = (size_t)f.tellg();
    if (sz < 0x20000 || sz > 0x800000) return false;       // MD-ROM = 128КБ..8МБ
    uint8_t hdr[0x3200] = {0};
    size_t n = sz < sizeof(hdr) ? sz : sizeof(hdr);
    f.seekg(0);
    if (!f.read((char*)hdr, (std::streamsize)n)) return false;
    bool isMd = n > 0x110 && std::search(hdr + 0x100, hdr + 0x110, (const uint8_t*)"SEGA", (const uint8_t*)"SEGA" + 4) != hdr + 0x110;
    if (!isMd) return false;
    Build b = detectBuildFromHeader(hdr, n, sz);
    if (b == Build::Unknown && !keepUnknown) return false; // скан не засоряем чужими MD-ROM'ами
    out.path = path;
    out.file = std::filesystem::path(path).filename().string();
    out.size = sz;
    out.build = b;
    out.supported = buildSupported(b);
    auto ascii = [&](size_t off, size_t len) {
        std::string s;
        for (size_t i = off; i < off + len && i < n; ++i) {
            char c = (char)hdr[i];
            s += (c >= 0x20 && c < 0x7F) ? c : ' ';
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();          // хвостовые пробелы
        std::string t; bool sp = false;                              // схлопнуть повторные пробелы
        for (char c : s) { if (c == ' ') { if (!sp) t += c; sp = true; } else { t += c; sp = false; } }
        while (!t.empty() && t.front() == ' ') t.erase(t.begin());
        return t;
    };
    out.title  = ascii(0x150, 0x30);                       // overseas-название
    if (out.title.empty()) out.title = ascii(0x120, 0x30); // fallback: domestic
    out.serial = ascii(0x180, 0x0E);
    return true;
}

// ---- Скан каталогов: cwd, два родителя (= пути findRom), каталог бинарника ----
inline std::vector<Entry> scanRoms() {
    namespace fs = std::filesystem;
    std::vector<fs::path> dirs;
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (!ec) { dirs.push_back(cwd); dirs.push_back(cwd.parent_path()); dirs.push_back(cwd.parent_path().parent_path()); }
    if (char* bp = SDL_GetBasePath()) {
        fs::path base(bp); SDL_free(bp);
        dirs.push_back(base);
        // ⭐.app-бандл: SDL_GetBasePath = …/ztpp.app/Contents/MacOS → ROM ищем ещё РЯДОМ С БАНДЛОМ
        if (base.string().find(".app/Contents/MacOS") != std::string::npos)
            dirs.push_back(base.parent_path().parent_path().parent_path().parent_path());
    }

    std::vector<Entry> out;
    std::vector<std::string> seenDirs, seenFiles;
    for (const fs::path& d : dirs) {
        std::string dkey = fs::weakly_canonical(d, ec).string();
        if (ec || dkey.empty()) continue;
        if (std::find(seenDirs.begin(), seenDirs.end(), dkey) != seenDirs.end()) continue;
        seenDirs.push_back(dkey);
        for (fs::directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::string ext = it->path().extension().string();
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
            if (ext != ".gen" && ext != ".bin" && ext != ".md") continue;   // .smd (чересстрочный) Rom::load не умеет
            std::string fkey = fs::weakly_canonical(it->path(), ec).string();
            if (ec || std::find(seenFiles.begin(), seenFiles.end(), fkey) != seenFiles.end()) continue;
            Entry e;
            if (probeRom(it->path().string(), e, /*keepUnknown=*/false)) {
                seenFiles.push_back(fkey);
                out.push_back(e);
            }
        }
        ec.clear();
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.supported != b.supported) return a.supported;   // поддерживаемые сверху
        return a.file < b.file;
    });
    return out;
}

// ---- Память последнего выбора (свой файл: см. шапку launcher.hpp) ----
inline const char* LAUNCHER_INI = "ztpp_launcher.ini";
inline std::string launcherIniPath() { return profilePath(LAUNCHER_INI); }   // бандл → Application Support
inline std::string loadLastRom() {
    std::ifstream f(launcherIniPath());
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("last_rom=", 0) == 0) return line.substr(9);
    return "";
}
inline void saveLastRom(const std::string& p) {
    std::ofstream f(launcherIniPath(), std::ios::trunc);
    if (f) f << "last_rom=" << p << "\n";
}

// ---- Состояние списка ----
struct State {
    std::vector<Entry> entries;
    int sel = 0, scroll = 0;
    std::string msg;           // мигающее сообщение SDL-фолбэка (ENTER на неподдерживаемом билде)
    int msgTimer = 0;
};

// Число видимых строк в SDL-списке (нужно и makeState/clampScroll для инициализации scroll).
inline constexpr int VIS_ROWS = 9;

// ---- Общая инициализация состояния (нативное окно, SDL-фолбэк, headless-дамп) ----
inline State makeState(const std::string& preselect) {
    State st;
    st.entries = scanRoms();
    std::printf("launcher: found %d ROM(s)\n", (int)st.entries.size());
    for (const Entry& e : st.entries)
        std::printf("  [%s] %s - %s\n", e.supported ? "OK" : "--", e.file.c_str(), buildLabel(e.build));
    // Приоритет предвыбора: last_rom из ini -> preselect (дефолт findRom) -> первый поддерживаемый
    namespace fs = std::filesystem;
    std::error_code ec;
    auto indexOf = [&](const std::string& p) -> int {
        if (p.empty()) return -1;
        std::string key = fs::weakly_canonical(p, ec).string();
        for (int i = 0; i < (int)st.entries.size(); ++i)
            if (fs::weakly_canonical(st.entries[i].path, ec).string() == key) return i;
        return -1;
    };
    int pre = indexOf(loadLastRom());
    if (pre < 0) pre = indexOf(preselect);
    if (pre < 0)
        for (int i = 0; i < (int)st.entries.size(); ++i)
            if (st.entries[i].supported) { pre = i; break; }
    st.sel = pre < 0 ? 0 : pre;
    if (st.sel >= VIS_ROWS) st.scroll = st.sel - VIS_ROWS + 1;
    return st;
}

inline void clampScroll(State& st) {
    int n = (int)st.entries.size();
    if (st.sel < 0) st.sel = 0;
    if (st.sel >= n) st.sel = n ? n - 1 : 0;
    if (st.sel < st.scroll) st.scroll = st.sel;
    if (st.sel >= st.scroll + VIS_ROWS) st.scroll = st.sel - VIS_ROWS + 1;
    if (st.scroll < 0) st.scroll = 0;
}

// Попытка запуска выбранного (SDL-фолбэк): true = выбран поддерживаемый ROM, иначе мигаем причиной.
inline bool tryLaunch(State& st, std::string& outPath) {
    if (st.sel < 0 || st.sel >= (int)st.entries.size()) return false;
    const Entry& e = st.entries[st.sel];
    if (!e.supported) {
        st.msg = "THIS BUILD IS NOT SUPPORTED YET - ZT RELEASE / ZT UNDERGROUND ONLY";
        st.msgTimer = 180;
        return false;
    }
    outPath = e.path;
    saveLastRom(e.path);
    return true;
}

// Drag&drop (SDL-фолбэк): детектим брошенный файл (Unknown тоже показываем — ответ на действие).
inline void addDropped(State& st, const char* path) {
    Entry e;
    if (!probeRom(path, e, /*keepUnknown=*/true)) {
        st.msg = "NOT A MEGA DRIVE ROM";
        st.msgTimer = 180;
        return;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string key = fs::weakly_canonical(path, ec).string();
    for (int i = 0; i < (int)st.entries.size(); ++i)
        if (fs::weakly_canonical(st.entries[i].path, ec).string() == key) { st.sel = i; clampScroll(st); return; }
    st.entries.insert(st.entries.begin(), e);
    st.sel = 0; st.scroll = 0;
}

// ---- Точка входа: выбор реализации окна выбора ROM по платформе ----
// (реализации: runSdl + dispatcher run в launcher.hpp; runNative в launcher_{osx.mm/win.cpp/gtk.cpp})
//   macOS  → launcher_osx.mm (Cocoa)        — __APPLE__
//   Windows→ launcher_win.cpp (Win32)       — _WIN32
//   Linux  → launcher_gtk.cpp (GTK3)        — ZTPP_GTK_LAUNCHER (CMake ставит, если найден gtk+-3.0)
//   иначе  → SDL-фолбэк runSdl
#if !defined(ZTPP_NO_SDL) && (defined(__APPLE__) || defined(_WIN32) || defined(ZTPP_GTK_LAUNCHER))
#  define ZTPP_HAS_NATIVE_LAUNCHER 1
#endif

#ifdef ZTPP_HAS_NATIVE_LAUNCHER
std::string runNative(const std::string& preselect);   // определяется в платформенном файле
#endif

} // namespace launcher
