// LiteWall - tiny MP4 live wallpaper for Windows 10/11.
//
// * Media Foundation (MFPlay) does the playback: GPU-accelerated decode,
//   so 4K H.264 video costs very little CPU. No extra DLLs, no runtime.
// * The video window is embedded behind the desktop icons.
// * Pauses itself (0% CPU/GPU) while a game / fullscreen app / maximized
//   window is in front, and resumes when you go back to the desktop.
// * Can register itself to start with Windows (HKCU Run key, no admin).
//
// Usage:
//   litewall.exe "C:\Videos\wall.mp4"                play it
//   litewall.exe --autostart "C:\Videos\wall.mp4"    play it + start with Windows
//   litewall.exe --remove-autostart                  stop starting with Windows
//   litewall.exe --stop                              stop the running wallpaper
// Options: --sound  --stretch  --always-play

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
#include <objbase.h>
#include <propidl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfplay.h>
#include <cwchar>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "mfuuid.lib")

static const wchar_t kClass[]   = L"LiteWallWindow";
static const wchar_t kRunKey[]  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kRunName[] = L"LiteWall";
static const UINT    WM_APP_FAIL = WM_APP + 1;

static HWND               g_hwnd       = nullptr;
static HMONITOR           g_monitor    = nullptr;
static IMFPMediaPlayer*   g_player     = nullptr;
static bool               g_paused     = false;
static bool               g_alwaysPlay = false;
static std::wstring       g_failText;

struct Options {
    std::wstring video;
    bool sound = false, stretch = false, alwaysPlay = false;
    bool autostart = false, removeAutostart = false, stop = false;
};

// ---------------------------------------------------------------- helpers

static void ShowError(const std::wstring& text) {
    MessageBoxW(nullptr, text.c_str(), L"LiteWall", MB_OK | MB_ICONERROR);
}

static BOOL CALLBACK EnumProc(HWND top, LPARAM lp) {
    // Classic layout: the wallpaper layer is the WorkerW that comes right
    // after the WorkerW which holds the desktop icons (SHELLDLL_DefView).
    if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) {
        HWND w = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
        if (w) {
            *reinterpret_cast<HWND*>(lp) = w;
            return FALSE;
        }
    }
    return TRUE;
}

// Finds the window that sits behind the desktop icons. Waits for Explorer,
// which matters when we are started at login.
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
        // Windows 11 24H2+: the wallpaper WorkerW is a child of Progman.
        if (!layer) layer = FindWindowExW(prog, nullptr, L"WorkerW", nullptr);
        if (!layer) Sleep(500);
    }
    return layer;
}

static bool StopRunningInstance(HWND layer) {
    HWND old = FindWindowExW(layer, nullptr, kClass, nullptr);
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

static bool ShouldPause() {
    if (g_alwaysPlay) return false;

    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state)) &&
        (state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN ||
         state == QUNS_PRESENTATION_MODE)) {
        return true;                       // game / fullscreen app
    }

    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_hwnd) return false;

    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
        !wcscmp(cls, L"Shell_TrayWnd")) {
        return false;                      // you are on the desktop
    }
    if (!IsWindowVisible(fg) || IsIconic(fg) || !IsZoomed(fg)) return false;
    return MonitorFromWindow(fg, MONITOR_DEFAULTTONULL) == g_monitor;
}

static void UpdatePauseState() {
    if (!g_player) return;
    bool want = ShouldPause();
    if (want == g_paused) return;
    HRESULT hr = want ? g_player->Pause() : g_player->Play();
    if (SUCCEEDED(hr)) g_paused = want;
}

// ------------------------------------------------------- player callback

class PlayerCallback : public IMFPMediaPlayerCallback {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFPMediaPlayerCallback)) {
            *ppv = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&m_ref));
    }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = static_cast<ULONG>(InterlockedDecrement(&m_ref));
        if (r == 0) delete this;
        return r;
    }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* h) override {
        if (!h) return;

        if (FAILED(h->hrEvent)) {
            if (h->eEventType == MFP_EVENT_TYPE_MEDIAITEM_CREATED ||
                h->eEventType == MFP_EVENT_TYPE_MEDIAITEM_SET ||
                h->eEventType == MFP_EVENT_TYPE_ERROR) {
                g_failText =
                    L"This video could not be played.\n\n"
                    L"Use an MP4 with H.264 video (works everywhere). H.265/HEVC "
                    L"needs Microsoft's free \"HEVC Video Extensions\" from the "
                    L"Store, and AV1 needs the \"AV1 Video Extension\".";
                PostMessageW(g_hwnd, WM_APP_FAIL, 0, 0);
            }
            return;
        }

        if (h->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED && g_player) {
            // Loop: jump back to the start and keep playing.
            PROPVARIANT pos = {};
            pos.vt = VT_I8;
            pos.hVal.QuadPart = 0;
            g_player->SetPosition(MFP_POSITIONTYPE_100NS, &pos);
            g_player->Play();
        }
    }

private:
    volatile LONG m_ref = 1;
};

// ----------------------------------------------------------- window proc

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_player) g_player->UpdateVideo();
        else FillRect(dc, &ps.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (g_player) g_player->UpdateVideo();
        return 0;
    case WM_DISPLAYCHANGE:
        g_monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        PlaceWindow(hwnd);
        return 0;
    case WM_TIMER:
        UpdatePauseState();
        return 0;
    case WM_APP_FAIL:
        ShowError(g_failText);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (g_player) {
            g_player->Shutdown();
            g_player->Release();
            g_player = nullptr;
        }
        PostQuitMessage(0);   // also runs if Explorer restarts and kills our parent
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

static void RemoveAutostart() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, kRunName);
        RegCloseKey(key);
    }
}

static std::wstring FullPath(const std::wstring& p) {
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (!n) return p;
    std::wstring out(n, L'\0');
    n = GetFullPathNameW(p.c_str(), n, &out[0], nullptr);
    out.resize(n);
    return out;
}

// ------------------------------------------------------------------ main

static bool ParseArgs(Options& o) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if      (a == L"--sound")            o.sound = true;
        else if (a == L"--stretch")          o.stretch = true;
        else if (a == L"--always-play")      o.alwaysPlay = true;
        else if (a == L"--autostart")        o.autostart = true;
        else if (a == L"--remove-autostart") o.removeAutostart = true;
        else if (a == L"--stop")             o.stop = true;
        else if (a.rfind(L"--", 0) != 0)     o.video = a;
    }
    LocalFree(argv);
    return true;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    Options opt;
    if (!ParseArgs(opt)) return 1;
    g_alwaysPlay = opt.alwaysPlay;

    if (opt.removeAutostart) {
        RemoveAutostart();
        HWND layer = FindWindowW(L"Progman", nullptr) ? GetDesktopLayer() : nullptr;
        if (layer) StopRunningInstance(layer);
        return 0;
    }
    if (opt.stop) {
        HWND layer = FindWindowW(L"Progman", nullptr) ? GetDesktopLayer() : nullptr;
        if (layer) StopRunningInstance(layer);
        return 0;
    }

    if (opt.video.empty()) {
        ShowError(L"Drag an .mp4 file onto litewall.exe, or run:\n\n"
                  L"litewall.exe \"C:\\path\\video.mp4\"\n"
                  L"litewall.exe --autostart \"C:\\path\\video.mp4\"\n"
                  L"litewall.exe --remove-autostart\n"
                  L"litewall.exe --stop\n\n"
                  L"Options: --sound  --stretch  --always-play");
        return 1;
    }

    opt.video = FullPath(opt.video);
    if (GetFileAttributesW(opt.video.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ShowError(L"File not found:\n" + opt.video);
        return 1;
    }

    if (opt.autostart) {
        wchar_t exe[MAX_PATH * 4] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH * 4);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
        if (opt.sound)      cmd += L" --sound";
        if (opt.stretch)    cmd += L" --stretch";
        if (opt.alwaysPlay) cmd += L" --always-play";
        cmd += L" \"" + opt.video + L"\"";
        if (!SetAutostart(cmd)) ShowError(L"Could not set up start with Windows.");
    }

    HWND layer = GetDesktopLayer();
    if (!layer) {
        ShowError(L"Could not find the desktop layer. Is Explorer running?");
        return 1;
    }
    StopRunningInstance(layer);            // replace an already-running wallpaper

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.lpszClassName = kClass;
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    g_monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    g_hwnd = CreateWindowExW(0, kClass, L"LiteWall",
                             WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                             0, 0, 100, 100, layer, nullptr, inst, nullptr);
    if (!g_hwnd) {
        ShowError(L"Could not create the wallpaper window.");
        return 1;
    }
    PlaceWindow(g_hwnd);

    PlayerCallback* cb = new PlayerCallback();
    HRESULT hr = MFPCreateMediaPlayer(opt.video.c_str(), TRUE, MFP_OPTION_NONE,
                                      cb, g_hwnd, &g_player);
    cb->Release();                         // the player keeps its own reference
    if (FAILED(hr) || !g_player) {
        ShowError(L"Could not start Media Foundation playback.");
        return 1;
    }

    g_player->SetMute(opt.sound ? FALSE : TRUE);
    // 0 = MFVideoARMode_None (stretch to fill), 1 = MFVideoARMode_PreservePicture
    g_player->SetAspectRatioMode(opt.stretch ? 0 : 1);
    SetTimer(g_hwnd, 1, 2000, nullptr);    // check every 2 s whether to pause

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return 0;
}
