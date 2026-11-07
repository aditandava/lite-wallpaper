// LiteWall - tiny MP4 live wallpaper for Windows 10/11.
//
// * Media Foundation (MFPlay) does the playback: GPU-accelerated decode,
//   so 4K H.264 video costs very little CPU. No extra DLLs, no runtime.
// * The video is embedded behind the desktop icons.
// * Seamless loop: a second player is prepared behind the first one a few
//   seconds before the end, starts a few milliseconds early, and takes over
//   the moment the first one finishes. The last frame stays on screen the
//   whole time, so there is no black flash and no restart stall.
// * Pauses itself (0% CPU/GPU) while a game / fullscreen app / maximized
//   window is in front, and resumes when you go back to the desktop.
// * Can register itself to start with Windows (HKCU Run key, no admin).
//
// Usage:
//   litewall.exe "C:\Videos\wall.mp4"                play it
//   litewall.exe --autostart "C:\Videos\wall.mp4"    play it + start with Windows
//   litewall.exe --remove-autostart                  stop starting with Windows
//   litewall.exe --stop                              stop the running wallpaper
// Options: --sound  --stretch  --always-play  --lead <ms>

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
#include <mmsystem.h>
#include <shellapi.h>
#include <objbase.h>
#include <propidl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfplay.h>
#include <cstdlib>
#include <cwchar>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "mfuuid.lib")

static const wchar_t kClass[]      = L"LiteWallWindow";   // hidden controller window
static const wchar_t kVideoClass[] = L"LiteWallVideo";    // the video surfaces
static const wchar_t kRunKey[]     = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kRunName[]    = L"LiteWall";
// Task Manager > Startup apps keeps its Enabled/Disabled switch here:
static const wchar_t kApprovedKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
static const wchar_t kAppKey[]     = L"Software\\LiteWall";   // remembers first run

static const UINT WM_APP_FAIL  = WM_APP + 1;   // active video cannot be played
static const UINT WM_APP_ENDED = WM_APP + 2;   // active video reached its end
static const UINT WM_APP_DROP  = WM_APP + 3;   // standby video failed to load

static const UINT_PTR T_PAUSE = 1;             // 2 s: pause when a game is in front
static const UINT_PTR T_LOOP  = 2;             // 250 ms: watch for the end of the video
static const UINT_PTR T_START = 3;             // one-shot: start the standby just before the end

static const LONGLONG kPrepareMs   = 4000;     // prepare the standby this long before the end
static const int      kDefaultLead = 60;       // ms the standby starts before the end

// One player + the window it draws into.
struct Slot {
    int              id       = 0;
    IMFPMediaPlayer* player   = nullptr;
    HWND             hwnd     = nullptr;
    LONGLONG         duration = 0;      // 100 ns units, 0 = unknown
    int              stage    = 0;      // standby cueing: 1 play, 2 pause, 3 seek, 4 ready
    bool             cued     = false;  // standby sits paused on the first frame
    bool             started  = false;  // standby already told to play
};

static HINSTANCE    g_inst           = nullptr;
static HWND         g_ctrl           = nullptr;
static HWND         g_layer          = nullptr;
static HMONITOR     g_monitor        = nullptr;
static Slot*        g_active         = nullptr;
static Slot*        g_standby        = nullptr;
static int          g_nextId         = 0;
static int          g_leadMs         = kDefaultLead;
static bool         g_paused         = false;
static bool         g_alwaysPlay     = false;
static bool         g_sound          = false;
static bool         g_stretch        = false;
static bool         g_startScheduled = false;
static bool         g_noStandby      = false;   // standby failed: fall back to a plain loop
static bool         g_periodRaised   = false;
static std::wstring g_video;
static std::wstring g_failText;

struct Options {
    std::wstring video;
    bool sound = false, stretch = false, alwaysPlay = false;
    bool autostart = false, removeAutostart = false, stop = false;
    int  lead = kDefaultLead;
};

// ---------------------------------------------------------------- helpers

static void ShowError(const std::wstring& text) {
    MessageBoxW(nullptr, text.c_str(), L"LiteWall", MB_OK | MB_ICONERROR);
}

static LONGLONG VariantToLL(const PROPVARIANT& v) {
    return (v.vt == VT_I8 || v.vt == VT_UI8) ? static_cast<LONGLONG>(v.hVal.QuadPart) : 0;
}

static void RaisePeriod() { if (!g_periodRaised) { timeBeginPeriod(1); g_periodRaised = true; } }
static void DropPeriod()  { if (g_periodRaised)  { timeEndPeriod(1);   g_periodRaised = false; } }

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

static Slot* FindSlot(int id) {
    if (g_active  && g_active->id  == id) return g_active;
    if (g_standby && g_standby->id == id) return g_standby;
    return nullptr;                      // slot was already retired: ignore late events
}

// ------------------------------------------------------- player callback

static void HandlePlayerEvent(int id, MFP_EVENT_HEADER* h);

class PlayerCallback : public IMFPMediaPlayerCallback {
public:
    explicit PlayerCallback(int id) : m_id(id) {}

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
        HandlePlayerEvent(m_id, h);
    }

private:
    volatile LONG m_ref = 1;
    int           m_id;
};

// ------------------------------------------------------------ slot life

static void DestroySlot(Slot* s) {
    if (!s) return;
    if (s->hwnd) SetWindowLongPtrW(s->hwnd, GWLP_USERDATA, 0);
    if (s->player) {
        s->player->Shutdown();
        s->player->Release();
        s->player = nullptr;
    }
    if (s->hwnd) {
        DestroyWindow(s->hwnd);
        s->hwnd = nullptr;
    }
    delete s;
}

// Creates a video window + player. 'below' puts the new window directly
// underneath another one so it can render unseen. *dest is set before the
// player exists so no event can arrive for an unknown slot.
static Slot* MakeSlot(bool startPlayback, HWND below, Slot** dest) {
    Slot* s = new Slot();
    s->id = ++g_nextId;
    *dest = s;

    s->hwnd = CreateWindowExW(0, kVideoClass, L"",
                              WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                              0, 0, 100, 100, g_layer, nullptr, g_inst, nullptr);
    if (!s->hwnd) {
        *dest = nullptr;
        delete s;
        return nullptr;
    }
    SetWindowLongPtrW(s->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    PlaceWindow(s->hwnd);
    if (below) {
        SetWindowPos(s->hwnd, below, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    PlayerCallback* cb = new PlayerCallback(s->id);
    HRESULT hr = MFPCreateMediaPlayer(g_video.c_str(), startPlayback ? TRUE : FALSE,
                                      MFP_OPTION_NONE, cb, s->hwnd, &s->player);
    cb->Release();                       // the player keeps its own reference
    if (FAILED(hr) || !s->player) {
        *dest = nullptr;
        DestroySlot(s);
        return nullptr;
    }

    // The standby stays silent until it takes over.
    s->player->SetMute((startPlayback && g_sound) ? FALSE : TRUE);
    // 0 = MFVideoARMode_None (stretch to fill), 1 = MFVideoARMode_PreservePicture
    s->player->SetAspectRatioMode(g_stretch ? 0 : 1);
    return s;
}

static void CreateStandby() {
    if (g_standby || g_noStandby || !g_active) return;
    RaisePeriod();                       // finer timer for the handoff
    if (!MakeSlot(false, g_active->hwnd, &g_standby)) {
        g_noStandby = true;
        DropPeriod();
    }
}

static void CancelStandbyStart() {
    KillTimer(g_ctrl, T_START);
    g_startScheduled = false;
    if (g_standby && g_standby->started && g_standby->player) {
        // Went to sleep after the standby began: park it on frame 0 again.
        g_standby->player->Pause();
        PROPVARIANT z = {};
        z.vt = VT_I8;
        z.hVal.QuadPart = 0;
        g_standby->player->SetPosition(MFP_POSITIONTYPE_100NS, &z);
        g_standby->started = false;
    }
}

// ----------------------------------------------------------- pause logic

static bool ShouldPause() {
    if (g_alwaysPlay) return false;

    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state)) &&
        (state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN ||
         state == QUNS_PRESENTATION_MODE)) {
        return true;                       // game / fullscreen app
    }

    HWND fg = GetForegroundWindow();
    if (!fg) return false;

    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
        !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, kClass) ||
        !wcscmp(cls, kVideoClass)) {
        return false;                      // you are on the desktop
    }
    if (!IsWindowVisible(fg) || IsIconic(fg) || !IsZoomed(fg)) return false;
    return MonitorFromWindow(fg, MONITOR_DEFAULTTONULL) == g_monitor;
}

static void UpdatePauseState() {
    if (!g_active || !g_active->player) return;
    bool want = ShouldPause();
    if (want == g_paused) return;
    HRESULT hr = want ? g_active->player->Pause() : g_active->player->Play();
    if (SUCCEEDED(hr)) {
        g_paused = want;
        if (want) CancelStandbyStart();
    }
}

// ---------------------------------------------------------- loop handoff

// Runs every 250 ms. Prepares the standby shortly before the end of the
// video and schedules its start for 'lead' ms before the end.
static void LoopTick() {
    Slot* a = g_active;
    if (!a || !a->player || g_paused) return;

    if (a->duration <= 0) {
        PROPVARIANT d = {};
        if (SUCCEEDED(a->player->GetDuration(MFP_POSITIONTYPE_100NS, &d))) {
            a->duration = VariantToLL(d);
        }
    }
    if (a->duration <= 0) return;

    PROPVARIANT p = {};
    if (FAILED(a->player->GetPosition(MFP_POSITIONTYPE_100NS, &p))) return;
    LONGLONG remainMs = (a->duration - VariantToLL(p)) / 10000;
    if (remainMs < 0) remainMs = 0;

    if (!g_standby) {
        if (remainMs < kPrepareMs) CreateStandby();
        return;
    }
    if (g_standby->cued && !g_standby->started && !g_startScheduled) {
        LONGLONG wait = remainMs - g_leadMs;
        if (wait < 400) {                  // inside the window: arm the one-shot timer
            if (wait < 1) wait = 1;
            SetTimer(g_ctrl, T_START, static_cast<UINT>(wait), nullptr);
            g_startScheduled = true;
        }
    }
}

static void StartStandby() {
    if (g_standby && g_standby->cued && !g_standby->started && !g_paused) {
        if (SUCCEEDED(g_standby->player->Play())) g_standby->started = true;
    }
}

// The active video reached its end: swap in the standby, or (if it is not
// ready) fall back to restarting the same player.
static void OnEnded() {
    Slot* a = g_active;
    Slot* b = g_standby;
    if (!a) return;

    if (b && b->cued && b->player) {
        if (!b->started) {                 // started late: it shows frame 0 until it runs
            b->player->Play();
            b->started = true;
        }
        if (g_sound) b->player->SetMute(FALSE);
        SetWindowPos(b->hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        g_active  = b;
        g_standby = nullptr;
        KillTimer(g_ctrl, T_START);
        g_startScheduled = false;
        DropPeriod();
        DestroySlot(a);                    // hidden behind b by now
        return;
    }

    PROPVARIANT zero = {};
    zero.vt = VT_I8;
    zero.hVal.QuadPart = 0;
    a->player->SetPosition(MFP_POSITIONTYPE_100NS, &zero);
    a->player->Play();
}

static void HandlePlayerEvent(int id, MFP_EVENT_HEADER* h) {
    if (!h) return;
    Slot* s = FindSlot(id);
    if (!s || !s->player) return;
    const bool standby = (s == g_standby);

    if (FAILED(h->hrEvent)) {
        const MFP_EVENT_TYPE t = h->eEventType;
        if (t == MFP_EVENT_TYPE_MEDIAITEM_CREATED || t == MFP_EVENT_TYPE_MEDIAITEM_SET ||
            t == MFP_EVENT_TYPE_ERROR) {
            if (standby) {
                PostMessageW(g_ctrl, WM_APP_DROP, 0, 0);
            } else {
                g_failText =
                    L"This video could not be played.\n\n"
                    L"Use an MP4 with H.264 video (works everywhere). H.265/HEVC "
                    L"needs Microsoft's free \"HEVC Video Extensions\" from the "
                    L"Store, and AV1 needs the \"AV1 Video Extension\".";
                PostMessageW(g_ctrl, WM_APP_FAIL, 0, 0);
            }
        }
        return;
    }

    switch (h->eEventType) {
    case MFP_EVENT_TYPE_MEDIAITEM_SET: {
        PROPVARIANT d = {};
        if (SUCCEEDED(s->player->GetDuration(MFP_POSITIONTYPE_100NS, &d))) {
            s->duration = VariantToLL(d);
        }
        if (standby) {                     // cue: play -> pause -> back to frame 0
            s->stage = 1;
            s->player->Play();
        }
        break;
    }
    case MFP_EVENT_TYPE_PLAY:
        if (standby && s->stage == 1) {
            s->stage = 2;
            s->player->Pause();
        }
        break;
    case MFP_EVENT_TYPE_PAUSE:
        if (standby && s->stage == 2) {
            s->stage = 3;
            PROPVARIANT z = {};
            z.vt = VT_I8;
            z.hVal.QuadPart = 0;
            s->player->SetPosition(MFP_POSITIONTYPE_100NS, &z);
        }
        break;
    case MFP_EVENT_TYPE_POSITION_SET:
        if (standby && s->stage == 3) {
            s->stage = 4;
            s->cued = true;                // ready: parked on frame 0
        }
        break;
    case MFP_EVENT_TYPE_PLAYBACK_ENDED:
        if (s == g_active) PostMessageW(g_ctrl, WM_APP_ENDED, 0, 0);
        break;
    default:
        break;
    }
}

// ----------------------------------------------------------- window procs

static LRESULT CALLBACK VideoProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Slot* s = reinterpret_cast<Slot*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (s && s->player) s->player->UpdateVideo();
        else FillRect(dc, &ps.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (s && s->player) s->player->UpdateVideo();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK CtrlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == T_PAUSE) {
            UpdatePauseState();
        } else if (wp == T_LOOP) {
            LoopTick();
        } else if (wp == T_START) {
            KillTimer(hwnd, T_START);
            g_startScheduled = false;
            StartStandby();
        }
        return 0;
    case WM_APP_ENDED:
        OnEnded();
        return 0;
    case WM_APP_DROP:
        CancelStandbyStart();
        if (g_standby) {
            DestroySlot(g_standby);
            g_standby = nullptr;
        }
        g_noStandby = true;                // plain loop from now on
        DropPeriod();
        return 0;
    case WM_APP_FAIL:
        ShowError(g_failText);
        DestroyWindow(hwnd);
        return 0;
    case WM_DISPLAYCHANGE:
        g_monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        if (g_active  && g_active->hwnd)  PlaceWindow(g_active->hwnd);
        if (g_standby && g_standby->hwnd) PlaceWindow(g_standby->hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, T_PAUSE);
        KillTimer(hwnd, T_LOOP);
        KillTimer(hwnd, T_START);
        DestroySlot(g_standby);
        g_standby = nullptr;
        DestroySlot(g_active);
        g_active = nullptr;
        DropPeriod();
        PostQuitMessage(0);                // also runs if Explorer restarts and kills our parent
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
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    LONG r = RegQueryValueExW(key, name, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

static void RegDeleteValue(const wchar_t* subkey, const wchar_t* name) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, name);
        RegCloseKey(key);
    }
}

// Makes Task Manager show LiteWall as "Enabled" (first byte 02 = enabled,
// 03 = disabled; the rest is a timestamp Windows fills in when you disable it).
static void MarkEnabledInTaskManager() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kApprovedKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    BYTE data[12] = { 0x02 };
    RegSetValueExW(key, kRunName, 0, REG_BINARY, data, sizeof(data));
    RegCloseKey(key);
}

// Remembers that first-run setup already happened, so "remove" stays removed.
static void MarkConfigured() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kAppKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    DWORD one = 1;
    RegSetValueExW(key, L"Configured", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegCloseKey(key);
}

static void RemoveAutostart() {
    RegDeleteValue(kRunKey, kRunName);
    RegDeleteValue(kApprovedKey, kRunName);
    MarkConfigured();                      // do not re-add it automatically next time
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
        else if (a == L"--lead" && i + 1 < argc) {
            o.lead = _wtoi(argv[++i]);
            if (o.lead < 0)   o.lead = 0;
            if (o.lead > 500) o.lead = 500;
        }
        else if (a.rfind(L"--", 0) != 0)     o.video = a;
    }
    LocalFree(argv);
    return true;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    g_inst = inst;

    Options opt;
    if (!ParseArgs(opt)) return 1;
    g_alwaysPlay = opt.alwaysPlay;
    g_sound      = opt.sound;
    g_stretch    = opt.stretch;
    g_leadMs     = opt.lead;

    if (opt.removeAutostart || opt.stop) {
        if (opt.removeAutostart) RemoveAutostart();
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
                  L"Options: --sound  --stretch  --always-play  --lead <ms>");
        return 1;
    }

    g_video = FullPath(opt.video);
    if (GetFileAttributesW(g_video.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ShowError(L"File not found:\n" + g_video);
        return 1;
    }

    // Start with Windows. Shows up in Task Manager > Startup apps as "LiteWall".
    //  * very first run          -> registered and switched to Enabled
    //  * --autostart             -> registered and switched to Enabled again
    //  * any later run           -> only refreshes which video starts at login;
    //                               your Enabled/Disabled choice is never touched
    {
        wchar_t exe[MAX_PATH * 4] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH * 4);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
        if (opt.sound)      cmd += L" --sound";
        if (opt.stretch)    cmd += L" --stretch";
        if (opt.alwaysPlay) cmd += L" --always-play";
        if (opt.lead != kDefaultLead) cmd += L" --lead " + std::to_wstring(opt.lead);
        cmd += L" \"" + g_video + L"\"";

        const bool firstRun = !RegValueExists(kAppKey, L"Configured");
        if (opt.autostart || firstRun) {
            if (SetAutostart(cmd)) MarkEnabledInTaskManager();
            else ShowError(L"Could not set up start with Windows.");
            MarkConfigured();
        } else if (RegValueExists(kRunKey, kRunName)) {
            SetAutostart(cmd);
        }
    }

    g_layer = GetDesktopLayer();
    if (!g_layer) {
        ShowError(L"Could not find the desktop layer. Is Explorer running?");
        return 1;
    }
    StopRunningInstance(g_layer);          // replace an already-running wallpaper

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));

    wc.lpfnWndProc   = CtrlProc;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    wc.lpfnWndProc   = VideoProc;
    wc.lpszClassName = kVideoClass;
    RegisterClassExW(&wc);

    g_monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);

    // Hidden window that owns the timers and receives our messages.
    g_ctrl = CreateWindowExW(0, kClass, L"LiteWall", WS_CHILD,
                             0, 0, 1, 1, g_layer, nullptr, inst, nullptr);
    if (!g_ctrl) {
        ShowError(L"Could not create the wallpaper window.");
        return 1;
    }

    if (!MakeSlot(true, nullptr, &g_active)) {
        ShowError(L"Could not start Media Foundation playback.");
        DestroyWindow(g_ctrl);
        return 1;
    }

    SetTimer(g_ctrl, T_PAUSE, 2000, nullptr);
    SetTimer(g_ctrl, T_LOOP, 250, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return 0;
}
