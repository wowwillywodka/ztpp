// ztpp — src/launcher_win.cpp: НАТИВНОЕ окно выбора ROM на Windows (Win32), по образцу
// стартап-окна eduke32 (source/duke3d/src/startwin.game.c). Реальные виджеты ОС: окно с
// SysListView32 (список ROM, report-режим), кнопки Play/Quit/Browse (GetOpenFileName —
// нативный файловый диалог), drag&drop (WM_DROPFILES), MessageBox на неподдерж. билд.
//
// ⚠⚠ НЕ ПРОВЕРЕНО СБОРКОЙ: разработка ztpp идёт на macOS, здесь нет MSVC/MinGW. Код написан
// best-effort по Win32 API — при первой сборке на Windows возможны правки. Data-слой
// (scanRoms/probeRom/makeState/saveLastRom) — общий с macOS/Linux (launcher_data.hpp).
//
// Модель eduke32: собственный цикл сообщений GetMessage/IsDialogMessage/DispatchMessage с
// сентинелом result (1=Play/0=Quit), окно ДО инициализации SDL-окна игры. CMake добавляет файл
// только на WIN32 (линкует comctl32/comdlg32/shell32).

#ifdef _WIN32

#include "launcher_data.hpp"   // launcher::Entry / scanRoms / makeState / probeRom / buildLabel / saveLastRom
#include "version.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <cstdio>
#include <cwchar>       // std::swprintf
#include <string>
#include <vector>

namespace {

enum { LR_RUNNING = -1, LR_QUIT = 0, LR_PLAY = 1 };
enum { ID_LIST = 1001, ID_PLAY = 1002, ID_QUIT = 1003, ID_BROWSE = 1004, ID_DETAIL1 = 1005, ID_DETAIL2 = 1006 };

struct WinCtx {
    std::vector<launcher::Entry>* entries = nullptr;
    HWND list = nullptr, play = nullptr, detail1 = nullptr, detail2 = nullptr;
    int result = LR_RUNNING;
    std::string selectedPath;
};

// UTF-8 <-> wide. ⚠ Data-слой строит пути через std::filesystem::path::string(): на MinGW это
// UTF-8, на MSVC — ACP. Берём UTF-8 (как macOS/Linux); для не-ASCII путей на MSVC может понадобиться
// CP_ACP — известное ограничение best-effort.
std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

void setFont(HWND w, HFONT f) { SendMessageW(w, WM_SETFONT, (WPARAM)f, TRUE); }

void addColumn(HWND list, int idx, const wchar_t* title, int width, int fmt) {
    LVCOLUMNW c = {0};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    c.fmt = fmt;
    c.cx = width;
    c.pszText = (LPWSTR)title;
    ListView_InsertColumn(list, idx, &c);
}

void populateList(WinCtx* ctx) {
    ListView_DeleteAllItems(ctx->list);
    for (int i = 0; i < (int)ctx->entries->size(); ++i) {
        const launcher::Entry& e = (*ctx->entries)[(size_t)i];
        std::wstring wfile = widen(e.file);
        LVITEMW it = {0};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = (LPWSTR)wfile.c_str();
        ListView_InsertItem(ctx->list, &it);
        std::wstring wbuild = widen(launcher::buildLabel(e.build));
        wchar_t wsize[32]; std::swprintf(wsize, 32, L"%.1f MB", (double)e.size / (1024.0 * 1024.0));
        const wchar_t* wstat = (e.build == Build::ZTU) ? L"Partially supported" : (e.supported ? L"Supported" : L"Not supported yet");
        ListView_SetItemText(ctx->list, i, 1, (LPWSTR)wbuild.c_str());
        ListView_SetItemText(ctx->list, i, 2, (LPWSTR)wsize);
        ListView_SetItemText(ctx->list, i, 3, (LPWSTR)wstat);
    }
}

int selectedIndex(WinCtx* ctx) { return ListView_GetNextItem(ctx->list, -1, LVNI_SELECTED); }

void updateDetails(WinCtx* ctx) {
    int i = selectedIndex(ctx);
    if (i < 0 || i >= (int)ctx->entries->size()) {
        SetWindowTextW(ctx->detail1, ctx->entries->empty()
            ? L"No Zero Tolerance ROMs found — use Browse… or drag & drop a .gen/.bin/.md file" : L"");
        SetWindowTextW(ctx->detail2, L"");
        EnableWindow(ctx->play, FALSE);
        return;
    }
    const launcher::Entry& e = (*ctx->entries)[(size_t)i];
    std::wstring d1 = L"Title: " + widen(e.title) + L"    Serial: " + widen(e.serial);
    std::wstring d2 = L"Path: " + widen(e.path);
    SetWindowTextW(ctx->detail1, d1.c_str());
    SetWindowTextW(ctx->detail2, d2.c_str());
    EnableWindow(ctx->play, TRUE);
}

void doPlay(HWND hwnd, WinCtx* ctx) {
    int i = selectedIndex(ctx);
    if (i < 0 || i >= (int)ctx->entries->size()) return;
    const launcher::Entry& e = (*ctx->entries)[(size_t)i];
    if (!e.supported) {
        MessageBoxW(hwnd,
            L"Only Zero Tolerance (release) and ZT Underground (partial support) can be launched right now.",
            L"This build is not supported yet", MB_OK | MB_ICONWARNING);
        return;
    }
    ctx->selectedPath = e.path;
    ctx->result = LR_PLAY;
}

void addRom(HWND hwnd, WinCtx* ctx, const std::wstring& wpath) {
    launcher::Entry e;
    if (!launcher::probeRom(narrow(wpath), e, /*keepUnknown=*/true)) {
        MessageBoxW(hwnd, L"The file does not look like a Sega Mega Drive / Genesis ROM.",
                    L"Not a Mega Drive ROM", MB_OK | MB_ICONWARNING);
        return;
    }
    for (int i = 0; i < (int)ctx->entries->size(); ++i)
        if ((*ctx->entries)[(size_t)i].path == e.path) {
            ListView_SetItemState(ctx->list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(ctx->list, i, FALSE);
            return;
        }
    ctx->entries->insert(ctx->entries->begin(), e);
    populateList(ctx);
    ListView_SetItemState(ctx->list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(ctx->list, 0, FALSE);
}

void doBrowse(HWND hwnd, WinCtx* ctx) {
    wchar_t file[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Mega Drive ROM (*.gen;*.bin;*.md)\0*.gen;*.bin;*.md\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Choose a Mega Drive ROM";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn))
        addRom(hwnd, ctx, std::wstring(file));
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WinCtx* ctx = (WinCtx*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_PLAY:   doPlay(hwnd, ctx); return 0;
                case ID_QUIT:
                case IDCANCEL:  ctx->result = LR_QUIT; return 0;   // IDCANCEL = Esc через IsDialogMessage
                case ID_BROWSE: doBrowse(hwnd, ctx); return 0;
            }
            return 0;
        case WM_NOTIFY: {
            NMHDR* nh = (NMHDR*)lp;
            if (nh->idFrom == ID_LIST) {
                if (nh->code == LVN_ITEMCHANGED) { updateDetails(ctx); return 0; }
                if (nh->code == NM_DBLCLK)       { doPlay(hwnd, ctx); return 0; }
            }
            return 0;
        }
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wp;
            wchar_t path[MAX_PATH];
            if (DragQueryFileW(drop, 0, path, MAX_PATH)) addRom(hwnd, ctx, std::wstring(path));
            DragFinish(drop);
            return 0;
        }
        case WM_CLOSE:
            if (ctx) ctx->result = LR_QUIT;
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // anonymous namespace

namespace launcher {

std::string runNative(const std::string& preselect) {
    State st = makeState(preselect);   // общий скан + предвыбор (печатает список найденных ROM)

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    const wchar_t* CLASS = L"ZtppLauncherWin";
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = CLASS;
    RegisterClassExW(&wc);

    // Клиент 640×440 → размер окна с рамкой.
    const int CW = 640, CH = 440;
    RECT wr = { 0, 0, CW, CH };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;

    WinCtx ctx;
    ctx.entries = &st.entries;

    wchar_t wt[64]; _snwprintf(wt, 64, L"ZTPP v%hs — Select a ROM", ztppVersion());
    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, CLASS, wt, style,
        (sw - ww) / 2, (sh - wh) / 2, ww, wh, nullptr, nullptr, hInst, &ctx);
    if (!hwnd) { std::fprintf(stderr, "launcher: CreateWindow failed, SDL fallback\n"); return preselect; }

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    // Список ROM (report-режим, полная строка)
    ctx.list = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        12, 12, 616, 300, hwnd, (HMENU)ID_LIST, hInst, nullptr);
    ListView_SetExtendedListViewStyle(ctx.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    setFont(ctx.list, font);
    addColumn(ctx.list, 0, L"File",   250, LVCFMT_LEFT);
    addColumn(ctx.list, 1, L"Build",  190, LVCFMT_LEFT);
    addColumn(ctx.list, 2, L"Size",   70,  LVCFMT_RIGHT);
    addColumn(ctx.list, 3, L"Status", 100, LVCFMT_LEFT);

    ctx.detail1 = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        12, 320, 616, 18, hwnd, (HMENU)ID_DETAIL1, hInst, nullptr);
    ctx.detail2 = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS,
        12, 340, 616, 18, hwnd, (HMENU)ID_DETAIL2, hInst, nullptr);
    setFont(ctx.detail1, font); setFont(ctx.detail2, font);

    HWND browse = CreateWindowExW(0, L"BUTTON", L"Browse…",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        12, 372, 100, 30, hwnd, (HMENU)ID_BROWSE, hInst, nullptr);
    HWND quit = CreateWindowExW(0, L"BUTTON", L"Quit",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        420, 372, 100, 30, hwnd, (HMENU)ID_QUIT, hInst, nullptr);
    ctx.play = CreateWindowExW(0, L"BUTTON", L"Play",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        528, 372, 100, 30, hwnd, (HMENU)ID_PLAY, hInst, nullptr);
    setFont(browse, font); setFont(quit, font); setFont(ctx.play, font);

    populateList(&ctx);
    if (st.sel >= 0 && st.sel < (int)st.entries.size()) {
        ListView_SetItemState(ctx.list, st.sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(ctx.list, st.sel, FALSE);
    }
    updateDetails(&ctx);

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    // Цикл сообщений с сентинелом (как startwin_run в eduke32).
    MSG msg;
    while (ctx.result == LR_RUNNING && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {   // Tab/Enter/Esc-навигация между контролами
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    DestroyWindow(hwnd);
    UnregisterClassW(CLASS, hInst);

    std::string out = (ctx.result == LR_PLAY) ? ctx.selectedPath : std::string();
    if (!out.empty()) { saveLastRom(out); std::printf("launcher: selected %s\n", out.c_str()); }
    else                std::printf("launcher: no ROM selected, exit\n");
    return out;
}

} // namespace launcher

#endif // _WIN32
