// LiteWall (mpv edition) - tiny MP4 live wallpaper for Windows 10/11.
//
// * Uses libmpv for flawless, zero-hitch, gapless hardware-accelerated loops.
// * Pauses itself (0% CPU/GPU) while a game / fullscreen app is in front.
// * Can register itself to start with Windows (HKCU Run key, no admin).
//
// Usage:
//   litewall.exe "C:\Videos\wall.mp4"
//   litewall.exe --autostart "C:\Videos\wall.mp4"
//   litewall.exe --remove-autostart
//   litewall.exe --stop

#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <shellapi.h>
#include <string>
#include <cstdlib>
#include <mpv/client.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "mpv.lib")

static const wchar_t kVideoClass[]  = L"LiteWallVideo";
static const wchar_t kRunKey[]      = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kRunName[]     = L"LiteWall";
static const wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
static const wchar_t kAppKey[]      = L"Software\\LiteWall";

static const UINT_PTR T_PAUSE = 1;

static HINSTANCE    g_inst    = nullptr;
static HWND         g_layer   = nullptr;
static HWND         g_hwnd    = nullptr;
static HMONITOR     g_monitor = nullptr;
static mpv_handle*  g_mpv     = nullptr;
static bool         g_paused  = false;
static std::wstring g_video;

struct Options {
    std::wstring video;
    bool autostart = false;
    bool removeAutostart = false;
    bool stop = false;
};

// ---------------------------------------------------------------- helpers

static void ShowError(const std::wstring& text) {
    MessageBoxW(nullptr, text.c_str(), L"LiteWall", MB_OK | MB_ICONERROR);
}

static BOOL CALLBACK EnumProc(HWND top, LPARAM lp) {
    if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) {
        HWND w = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
        if (w) {
            *reinterpret_cast<HWND*>(lp) = w;
            return FALSE;
        }
    }
    return TRUE;
}

static HWND GetDesktopLayer() {
    HWND prog = nullptr;
    for (int i = 0; i < 120 && !prog; ++i) {
        prog = FindWindowW(L"Progman", nullptr);
        if (!prog) Sleep(500);
    }
    if (!prog) return nullptr;

    DWORD_PTR result = 0;
    SendMessageTimeoutW(prog, 0x052C, 0xD, 1, SMTO_NORMAL, 1000, &result);

    HWND layer = nullptr;
    for (int i = 0; i < 20 && !layer; ++i) {
        EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&layer));
        if (!layer) layer = FindWindowExW(prog, nullptr, L"WorkerW", nullptr);
        if (!layer) Sleep(500);
    }
    return layer;
}

static bool StopRunningInstance(HWND layer) {
    HWND old = FindWindowExW(layer, nullptr, kVideoClass, nullptr);
    if (!old) return false;
    PostMessageW(old, WM_CLOSE, 0, 0);
    for (int i = 0; i < 30 && IsWindow(old); ++i) Sleep(100);
    return true;
}

static void PlaceWindow(HWND hwnd) {
    HWND parent = GetParent(hwnd);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!parent || !GetMonitorInfoW(g_monitor, &mi)) return;
    POINT p = { mi.rcMonitor.left, mi.rcMonitor.top };
    ScreenToClient(parent, &p);
    MoveWindow(hwnd, p.x, p.y,
               mi.rcMonitor.right - mi.rcMonitor.left,
               mi.rcMonitor.bottom - mi.rcMonitor.top, TRUE);
}

static std::wstring FullPath(const std::wstring& p) {
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (!n) return p;
    std::wstring out(n, L'\0');
    n = GetFullPathNameW(p.c_str(), n, &out[0], nullptr);
    out.resize(n);
    return out;
}

// ----------------------------------------------------------- pause logic

static bool ShouldPause() {
    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state)) &&
        (state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN ||
         state == QUNS_PRESENTATION_MODE)) {
        return true;
    }

    HWND fg = GetForegroundWindow();
    if (!fg) return false;

    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
        !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, kVideoClass)) {
        return false;
    }
    if (!IsWindowVisible(fg) || IsIconic(fg) || !IsZoomed(fg)) return false;
    return MonitorFromWindow(fg, MONITOR_DEFAULTTONULL) == g_monitor;
}

static void UpdatePauseState() {
    if (!g_mpv) return;
    bool want = ShouldPause();
    if (want == g_paused) return;
    mpv_set_property_string(g_mpv, "pause", want ? "yes" : "no");
    g_paused = want;
}

// ----------------------------------------------------------- window proc

static LRESULT CALLBACK VideoProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == T_PAUSE) UpdatePauseState();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DISPLAYCHANGE:
        g_monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        PlaceWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, T_PAUSE);
        if (g_mpv) {
            mpv_terminate_destroy(g_mpv);
            g_mpv = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------- autostart

static bool SetAutostart(const std::wstring& command) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    LONG r = RegSetValueExW(key, kRunName, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(command.c_str()),
                            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

static bool RegValueExists(const wchar_t* subkey, const wchar_t* name) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    LONG r = RegQueryValueExW(key, name, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

static void DeleteRegValue(const wchar_t* subkey, const wchar_t* name) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, name);
        RegCloseKey(key);
    }
}

static void MarkEnabledInTaskManager() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kApprovedKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    BYTE data[12] = { 0x02 };
    RegSetValueExW(key, kRunName, 0, REG_BINARY, data, sizeof(data));
    RegCloseKey(key);
}

static void MarkConfigured() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kAppKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    DWORD one = 1;
    RegSetValueExW(key, L"Configured", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegCloseKey(key);
}

static void RemoveAutostart() {
    DeleteRegValue(kRunKey, kRunName);
    DeleteRegValue(kApprovedKey, kRunName);
    MarkConfigured();
}

static bool ParseArgs(Options& o) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if      (a == L"--autostart")        o.autostart = true;
        else if (a == L"--remove-autostart") o.removeAutostart = true;
        else if (a == L"--stop")             o.stop = true;
        else if (a.rfind(L"--", 0) != 0)     o.video = a;
    }
    LocalFree(argv);
    return true;
}

// ------------------------------------------------------------------ main

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    g_inst = inst;

    Options opt;
    if (!ParseArgs(opt)) return 1;

    if (opt.removeAutostart || opt.stop) {
        if (opt.removeAutostart) RemoveAutostart();
        HWND layer = FindWindowW(L"Progman", nullptr) ? GetDesktopLayer() : nullptr;
        if (layer) StopRunningInstance(layer);
        return 0;
    }

    if (opt.video.empty()) {
        ShowError(L"Usage:\nlitewall.exe \"C:\\path\\video.mp4\"\nlitewall.exe --autostart \"C:\\path\\video.mp4\"\nlitewall.exe --remove-autostart\nlitewall.exe --stop");
        return 1;
    }

    g_video = FullPath(opt.video);
    if (GetFileAttributesW(g_video.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ShowError(L"File not found:\n" + g_video);
        return 1;
    }

    // Task Manager Startup Registration
    {
        wchar_t exe[MAX_PATH * 4] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH * 4);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" \"" + g_video + L"\"";

        const bool firstRun = !RegValueExists(kAppKey, L"Configured");
        if (opt.autostart || firstRun) {
            if (SetAutostart(cmd)) MarkEnabledInTaskManager();
            MarkConfigured();
        } else if (RegValueExists(kRunKey, kRunName)) {
            SetAutostart(cmd);
        }
    }

    g_layer = GetDesktopLayer();
    if (!g_layer) {
        ShowError(L"Could not find the desktop layer.");
        return 1;
    }

    StopRunningInstance(g_layer); // Replace existing instance

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpfnWndProc   = VideoProc;
    wc.lpszClassName = kVideoClass;
    RegisterClassExW(&wc);

    g_monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);

    g_hwnd = CreateWindowExW(0, kVideoClass, L"", WS_CHILD | WS_VISIBLE,
                             0, 0, 100, 100, g_layer, nullptr, inst, nullptr);
    PlaceWindow(g_hwnd);

    g_mpv = mpv_create();
    if (!g_mpv) return 1;

    int64_t wid = reinterpret_cast<int64_t>(g_hwnd);
    mpv_set_property(g_mpv, "wid", MPV_FORMAT_INT64, &wid);

    mpv_set_option_string(g_mpv, "loop-file", "inf");
    mpv_set_option_string(g_mpv, "hwdec", "auto");
    mpv_set_option_string(g_mpv, "audio", "no");
    mpv_set_option_string(g_mpv, "osc", "no");
    mpv_set_option_string(g_mpv, "osd-level", "0");
    mpv_set_option_string(g_mpv, "keep-open", "yes");

    if (mpv_initialize(g_mpv) < 0) return 1;

    char utf8Path[MAX_PATH * 4];
    WideCharToMultiByte(CP_UTF8, 0, g_video.c_str(), -1, utf8Path, sizeof(utf8Path), nullptr, nullptr);

    const char* cmd[] = {"loadfile", utf8Path, nullptr};
    mpv_command(g_mpv, cmd);

    SetTimer(g_hwnd, T_PAUSE, 2000, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
