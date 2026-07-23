#pragma once
// ztpp — src/profile.hpp: КОНТУР ДАННЫХ на ROM. Настройки и сейвы каждого билда живут в своём
// каталоге `profiles/<buildKey>/` (zt / ztu / zt_german / …), чтобы прогресс/настройки разных
// игр не перетирали друг друга. Инициализируется в main сразу после детекта билда.
// Однократная миграция: старые файлы из корня (ztpp_settings.ini, ztpp_quick.sav, ztpp_saveN.sav)
// копируются в профиль ZT, если там ещё пусто (не переносим — вдруг рядом старый бинарник).
// ztpp_launcher.ini остаётся ОБЩИМ (выбор ROM — до детекта билда).
#include "gamedata.hpp"   // Build/buildKey
#include <filesystem>
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#ifdef __APPLE__
#include <mach-o/dyld.h>   // _NSGetExecutablePath (детект запуска из .app-бандла)
#endif

// ⭐КАТАЛОГ ДАННЫХ (настройки/сейвы/лаунчер-ini). Портативный режим = cwd (как раньше: бинарь+данные рядом).
// macOS .app-БАНДЛ: внутрь бандла писать нельзя (и cwd у .app = "/") → стандарт опенсорса:
// ~/Library/Application Support/ztpp. Детект — путь исполняемого содержит ".app/Contents/MacOS".
inline std::string dataRoot() {
    static std::string r;
    if (!r.empty()) return r;
    r = ".";
#ifdef __APPLE__
    char exe[4096]; uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n) == 0 && std::strstr(exe, ".app/Contents/MacOS")) {
        if (const char* home = std::getenv("HOME")) {
            std::string d = std::string(home) + "/Library/Application Support/ztpp";
            std::error_code ec; std::filesystem::create_directories(d, ec);
            if (!ec) { r = d; std::printf("data: app bundle -> %s\n", r.c_str()); }
        }
    }
#endif
    return r;
}

inline std::string& profileDirRef() { static std::string d; return d; }   // пусто = профиль не инициализирован

// Путь файла данных в активном профиле; до initProfile — прежнее поведение (корень).
inline std::string profilePath(const std::string& name) {
    const std::string& d = profileDirRef();
    if (!d.empty()) return d + "/" + name;
    const std::string root = dataRoot();
    return root == "." ? name : root + "/" + name;   // до initProfile: dataRoot (бандл) или cwd (портативно)
}

inline void initProfile(Build b) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string dir = (dataRoot() == "." ? std::string("profiles/")
                                          : dataRoot() + "/profiles/") + buildKey(b);
    fs::create_directories(dir, ec);
    if (ec) { std::fprintf(stderr, "profile: cannot create %s (%s) - using cwd\n", dir.c_str(), ec.message().c_str()); return; }
    profileDirRef() = dir;
    if (b == Build::ZT) {                                    // миграция старых файлов релиза из корня
        const char* files[] = { "ztpp_settings.ini", "ztpp_quick.sav",
                                "ztpp_save1.sav", "ztpp_save2.sav", "ztpp_save3.sav",
                                "ztpp_save4.sav", "ztpp_save5.sav" };
        for (const char* f : files) {
            fs::path src = f, dst = fs::path(dir) / f;
            if (fs::exists(src, ec) && !fs::exists(dst, ec)) {
                fs::copy_file(src, dst, ec);
                if (!ec) std::printf("profile: migrated %s -> %s\n", f, dst.string().c_str());
            }
        }
    }
    std::printf("profile: %s\n", dir.c_str());
}
