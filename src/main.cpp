#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>

namespace Gdiplus {
    using std::min;
    using std::max;
}
#include <gdiplus.h>

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif
#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

using namespace Gdiplus;

namespace {

// --- Tuning Constants ---
constexpr int BUCKET_W = 4;                   // Smooth contour resolution for curved window corners
constexpr DWORD DEFAULT_IDLE_SEC = 120;       // Default: 2 minutes idle before snowfall starts
constexpr DWORD MAX_IDLE_SEC = 86400;         // Max configurable idle delay (24 hours)
constexpr float WINDOW_IDLE_SEC = 4.0f;       // Idle time before a window gathers snow
constexpr float ACTIVE_CLIP_GRACE_SEC = 4.0f; // Seconds after input before snow crosses active window
constexpr float PLOW_TRIGGER_WIN = 14.0f;     // Avg snow depth (px) to summon window plow
constexpr float PLOW_TRIGGER_TB = 13.0f;      // Avg snow depth (px) to summon taskbar plow
constexpr float MAX_SNOW_H = 24.0f;
constexpr float GRAVITY = 900.0f;
constexpr float PI_F = 3.14159265f;
constexpr float STACK_MAX_ANGLE_RAD = 30.0f * PI_F / 180.0f; // +/- 30 deg from vertical Y-axis

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT_PTR TIMER_TICK = 1;

constexpr UINT ID_TRAY_PLOW_WIN = 1001;
constexpr UINT ID_TRAY_PLOW_TB = 1002;
constexpr UINT ID_TRAY_PENGUINS = 1003;
constexpr UINT ID_TRAY_SANTA = 1004;
constexpr UINT ID_TRAY_ECO = 1005;
constexpr UINT ID_TRAY_CLEAR = 1006;
constexpr UINT ID_TRAY_QUIT = 1007;
constexpr UINT ID_TRAY_AUTOSTART = 1008;
constexpr UINT ID_TRAY_FORCE_SNOW = 1009;

// Idle Timer Submenu IDs
constexpr UINT ID_IDLE_0 = 1100;
constexpr UINT ID_IDLE_30 = 1101;
constexpr UINT ID_IDLE_60 = 1102;
constexpr UINT ID_IDLE_120 = 1103;
constexpr UINT ID_IDLE_300 = 1104;
constexpr UINT ID_IDLE_600 = 1105;
constexpr UINT ID_IDLE_CUSTOM = 1106;

// Blizzard Intensity Submenu IDs
constexpr UINT ID_INTENSITY_LIGHT = 1200;
constexpr UINT ID_INTENSITY_MODERATE = 1201;
constexpr UINT ID_INTENSITY_SEVERE = 1202;

// Custom Idle Dialog Control IDs
constexpr int IDC_EDIT_MIN = 2001;
constexpr int IDC_EDIT_SEC = 2002;

constexpr const wchar_t* REG_RUN_PATH = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* REG_APP_NAME = L"WinterDesktop";
constexpr const wchar_t* REG_SETTINGS_PATH = L"Software\\WinterDesktop";
constexpr const wchar_t* REG_IDLE_VALUE = L"IdleDelaySec";
constexpr const wchar_t* REG_INTENSITY_VALUE = L"BlizzardIntensity";

const HWND TASKBAR_KEY = reinterpret_cast<HWND>(static_cast<intptr_t>(-999));

enum class BlizzardIntensity : DWORD {
    Light = 0,
    Moderate = 1,
    Severe = 2
};

const wchar_t* IntensityLabel(BlizzardIntensity lvl) {
    switch (lvl) {
    case BlizzardIntensity::Moderate: return L"Moderate";
    case BlizzardIntensity::Severe:   return L"Severe";
    case BlizzardIntensity::Light:
    default:                          return L"Light";
    }
}

// --- Fast RNG ---
std::mt19937& Rng() {
    static std::mt19937 gen(std::random_device{}());
    return gen;
}
float RandRange(float lo, float hi) {
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(Rng());
}
int RandInt(int lo, int hi) {
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(Rng());
}

void EnableDpiAwareness() {
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        using Fn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(u32, "SetProcessDpiAwarenessContext"))) {
            fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            return;
        }
    }
    SetProcessDPIAware();
}

float GetGlobalIdleSeconds() {
    LASTINPUTINFO lii{};
    lii.cbSize = sizeof(lii);
    if (GetLastInputInfo(&lii)) {
        DWORD elapsed = GetTickCount() - lii.dwTime;
        return static_cast<float>(elapsed) / 1000.0f;
    }
    return 0.0f;
}

bool IsIgnoredClass(HWND hwnd) {
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    static const wchar_t* kIgnored[] = {
        L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"Windows.UI.Core.CoreWindow", L"NotifyIconOverflowWindow"
    };
    for (const wchar_t* name : kIgnored) {
        if (wcscmp(cls, name) == 0) return true;
    }
    return false;
}

RECT GetWindowBounds(HWND hwnd) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) {
        GetWindowRect(hwnd, &r);
    }
    return r;
}

std::wstring FormatIdleDuration(DWORD totalSec) {
    if (totalSec == 0) return L"Immediate";
    DWORD mins = totalSec / 60;
    DWORD secs = totalSec % 60;
    if (mins > 0 && secs > 0) return std::to_wstring(mins) + L"m " + std::to_wstring(secs) + L"s";
    if (mins > 0) return std::to_wstring(mins) + L"m";
    return std::to_wstring(secs) + L"s";
}

// --- Windows Registry Helpers (Startup & Persistent Settings) ---
std::wstring GetQuotedExePath() {
    std::wstring buf(MAX_PATH, L'\0');
    while (true) {
        DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) return L"";
        if (len < buf.size() - 1) {
            buf.resize(len);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return L"\"" + buf + L"\"";
}

bool IsAutoStartEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_PATH, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD byteSize = 0;
    bool enabled = false;
    if (RegQueryValueExW(hKey, REG_APP_NAME, nullptr, &type, nullptr, &byteSize) == ERROR_SUCCESS &&
        type == REG_SZ && byteSize >= sizeof(wchar_t)) {
        std::wstring value(byteSize / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(
                hKey, REG_APP_NAME, nullptr, &type,
                reinterpret_cast<LPBYTE>(value.data()), &byteSize) == ERROR_SUCCESS) {
            while (!value.empty() && value.back() == L'\0') value.pop_back();
            std::wstring expected = GetQuotedExePath();
            enabled = (!expected.empty() &&
                       CompareStringOrdinal(value.c_str(), -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL);
        }
    }

    RegCloseKey(hKey);
    return enabled;
}

bool SetAutoStart(bool enable) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_PATH, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    LONG status = ERROR_SUCCESS;
    if (enable) {
        std::wstring cmd = GetQuotedExePath();
        if (cmd.empty()) {
            RegCloseKey(hKey);
            return false;
        }
        DWORD bytes = static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t));
        status = RegSetValueExW(
            hKey, REG_APP_NAME, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(cmd.c_str()), bytes
        );
    } else {
        status = RegDeleteValueW(hKey, REG_APP_NAME);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }

    RegCloseKey(hKey);
    return status == ERROR_SUCCESS;
}

DWORD LoadRegDwordSetting(const wchar_t* valueName, DWORD defaultVal, DWORD maxVal) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_SETTINGS_PATH, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return defaultVal;
    }
    DWORD val = defaultVal;
    DWORD type = 0;
    DWORD size = sizeof(val);
    if (RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(&val), &size) != ERROR_SUCCESS ||
        type != REG_DWORD) {
        val = defaultVal;
    }
    RegCloseKey(hKey);
    return std::min(val, maxVal);
}

void SaveRegDwordSetting(const wchar_t* valueName, DWORD value) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_SETTINGS_PATH, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, valueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(hKey);
    }
}

DWORD LoadIdleDelaySeconds() {
    return LoadRegDwordSetting(REG_IDLE_VALUE, DEFAULT_IDLE_SEC, MAX_IDLE_SEC);
}

void SaveIdleDelaySeconds(DWORD seconds) {
    SaveRegDwordSetting(REG_IDLE_VALUE, std::min(seconds, MAX_IDLE_SEC));
}

BlizzardIntensity LoadBlizzardIntensity() {
    DWORD raw = LoadRegDwordSetting(REG_INTENSITY_VALUE, 0, 2);
    return static_cast<BlizzardIntensity>(raw);
}

void SaveBlizzardIntensity(BlizzardIntensity lvl) {
    SaveRegDwordSetting(REG_INTENSITY_VALUE, static_cast<DWORD>(lvl));
}

// --- Native Custom Idle Timer Dialog ---
struct IdleDialogState {
    bool done = false;
    bool accepted = false;
    DWORD resultSeconds = DEFAULT_IDLE_SEC;
};

LRESULT CALLBACK CustomIdleDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<IdleDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_COMMAND: {
        UINT id = LOWORD(wp);
        if (id == IDOK && st) {
            UINT mins = GetDlgItemInt(hwnd, IDC_EDIT_MIN, nullptr, FALSE);
            UINT secs = GetDlgItemInt(hwnd, IDC_EDIT_SEC, nullptr, FALSE);
            uint64_t total = static_cast<uint64_t>(mins) * 60ULL + static_cast<uint64_t>(secs);
            st->resultSeconds = static_cast<DWORD>(std::min<uint64_t>(total, MAX_IDLE_SEC));
            st->accepted = true;
            st->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDCANCEL && st) {
            st->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (st) st->done = true;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --- Simulation Entities ---
struct RectFSim {
    float x = 0, y = 0, w = 0, h = 0;
    float Left() const { return x; }
    float Top() const { return y; }
    float Right() const { return x + w; }
    float Bottom() const { return y + h; }
};

struct WindowSurface {
    HWND hwnd = nullptr;
    RectFSim rect{};
    bool isTaskbar = false;
    float idleTime = 0.0f;
    std::vector<float> heights;

    void EnsureBuckets() {
        size_t count = static_cast<size_t>(std::max(1, static_cast<int>(rect.w / BUCKET_W)));
        if (heights.size() != count) {
            heights.assign(count, 0.0f);
        }
    }
    float AverageHeight() const {
        if (heights.empty()) return 0.0f;
        float sum = 0.0f;
        for (float h : heights) sum += h;
        return sum / static_cast<float>(heights.size());
    }
    bool HasSnow() const {
        for (float h : heights) {
            if (h > 0.5f) return true;
        }
        return false;
    }
};

struct Flake {
    float x = 0, y = 0, vx = 0, vy = 0, r = 2.0f;
    bool ambient = true;
    bool canDeposit = true;
    float depositAmt = 2.0f;
    HWND ignoreHwnd = nullptr;
    float age = 0.0f;
};

struct DustParticle {
    float x = 0, y = 0, vx = 0, vy = 0, r = 2.0f;
    float life = 0.8f, maxLife = 0.8f;
    bool icyTint = false;
};

struct Sparkle {
    float x = 0, y = 0, vx = 0, vy = 0;
    float life = 0.5f, maxLife = 0.5f;
    bool gold = true;
};

struct SnowPlow {
    float x = 0, y = 0, vx = 0, vy = 0;
    int dir = 1;
    float speed = 150.0f;
    HWND attachedHwnd = nullptr;
    bool isTaskbarPlow = false;
    float moundMass = 0.0f;
};

struct Snowball {
    int id = 0;
    float x = 0, y = 0, vx = 0, vy = 0, r = 15.0f;
    bool grabbed = false;
    bool resting = false;
    HWND restingHwnd = nullptr;  // Last surface this snowball rested on
    bool fellFromWindow = false; // True when detached from an app window -> collapses on taskbar impact
    int stackLevel = 0;          // 0 = standalone / snowman body, 1 = snowman head
    int stackedOnId = -1;        // ID of lower body snowball when stacked
    bool aligning = false;
    float alignAngle = 0.0f;     // Signed angle (rad) from vertical Y-axis
    float snowmanReveal = 0.0f;  // 0..1 smooth fade-in of hat/scarf/face/arms
    bool collapsing = false;     // True while toppling into snowdust
    float collapseTimer = 0.0f;
};

struct SantaSleigh {
    bool active = false;
    float x = 0, y = 0, baseY = 0, vx = 165.0f, phase = 0.0f;
};

struct Penguin {
    float x = 0, y = 0;
    int dir = 1;           // 1 = left-to-right, -1 = right-to-left
    float speed = 34.0f;
    float scale = 1.0f;
    float waddlePhase = 0.0f;
    float hopY = 0.0f;
    float hopVy = 0.0f;
    float dustTimer = 0.0f;
};

// --- Main Application ---
class WinterApp {
public:
    bool Init(HINSTANCE hInst) {
        hInst_ = hInst;
        ownPid_ = GetCurrentProcessId();
        idleDelaySec_ = LoadIdleDelaySeconds();
        intensity_ = LoadBlizzardIntensity();
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = StaticWndProc;
        wc.hInstance = hInst_;
        wc.lpszClassName = L"WinterDesktopOverlayClass";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&wc)) return false;

        WNDCLASSEXW dlgWc{};
        dlgWc.cbSize = sizeof(dlgWc);
        dlgWc.lpfnWndProc = CustomIdleDlgProc;
        dlgWc.hInstance = hInst_;
        dlgWc.lpszClassName = L"WinterDesktopIdleDialogClass";
        dlgWc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        dlgWc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        RegisterClassExW(&dlgWc);

        RefreshMetrics();

        hwnd_ = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
            wc.lpszClassName, L"Winter Desktop",
            WS_POPUP,
            workArea_.left, workArea_.top, width_, height_,
            nullptr, nullptr, hInst_, this
        );
        if (!hwnd_) return false;

        InitBuffers();
        CreateTrayIcon();
        RenderFrame(); // Present initial clean transparent frame

        QueryPerformanceFrequency(&qpcFreq_);
        QueryPerformanceCounter(&lastTick_);

        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetTimer(hwnd_, TIMER_TICK, TargetFrameMs(), nullptr);
        return true;
    }

    ~WinterApp() {
        DestroyTrayIcon();
        DestroyBuffers();
    }

    int Run() {
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

private:
    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND activeDlg_ = nullptr;
    DWORD ownPid_ = 0;
    NOTIFYICONDATAW nid_{};
    HICON trayIcon_ = nullptr;

    RECT workArea_{};
    int fullW_ = 1920, fullH_ = 1080;
    int width_ = 1920, height_ = 1040;
    bool hasBottomTaskbar_ = true;
    float cornerRadius_ = 10.0f;

    // Reusable 32-bit DIB for zero-allocation UpdateLayeredWindow
    HDC memDC_ = nullptr;
    HBITMAP dibBmp_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    void* dibBits_ = nullptr;

    // State
    bool clickThrough_ = true;
    bool paused_ = false;
    bool ecoMode_ = false;
    DWORD idleDelaySec_ = DEFAULT_IDLE_SEC;
    BlizzardIntensity intensity_ = BlizzardIntensity::Light;
    bool forceSnowfall_ = false;
    bool snowfallActive_ = false;
    bool lastFrameHadContent_ = false;
    bool hasActiveWindowClip_ = false;
    RectFSim activeWindowClip_{};

    std::unordered_map<HWND, WindowSurface> surfaces_;
    std::vector<HWND> zOrder_;
    std::vector<Flake> flakes_;
    std::vector<DustParticle> dust_;
    std::vector<Sparkle> sparkles_;
    std::vector<SnowPlow> plows_;
    std::vector<Snowball> snowballs_;
    std::vector<Penguin> penguins_;
    int nextBallId_ = 1;
    SantaSleigh santa_;
    float santaCooldown_ = 30.0f;
    float penguinCooldown_ = 22.0f;

    int draggedBallId_ = -1;
    POINT lastMouse_{};
    float mouseVx_ = 0.0f, mouseVy_ = 0.0f;

    LARGE_INTEGER qpcFreq_{}, lastTick_{};
    float pollAccum_ = 0.4f;
    float hoverAccum_ = 0.0f;
    float relaxAccum_ = 0.0f;
    float elapsedSec_ = 0.0f;
    float ambientSpawnAccum_ = 0.0f;

    std::vector<PointF> polyBuf_;

    UINT TargetFrameMs() const { return ecoMode_ ? 50 : 33; } // 20 FPS eco / 30 FPS normal

    size_t TargetAmbientCount() const {
        switch (intensity_) {
        case BlizzardIntensity::Moderate: return ecoMode_ ? 68 : 115;
        case BlizzardIntensity::Severe:   return ecoMode_ ? 110 : 195;
        case BlizzardIntensity::Light:
        default:                          return ecoMode_ ? 36 : 58;
        }
    }

    float AmbientSpawnRate() const {
        switch (intensity_) {
        case BlizzardIntensity::Moderate: return 34.0f;
        case BlizzardIntensity::Severe:   return 70.0f;
        case BlizzardIntensity::Light:
        default:                          return 14.0f;
        }
    }

    size_t MaxFlakeCount() const {
        switch (intensity_) {
        case BlizzardIntensity::Moderate: return 340;
        case BlizzardIntensity::Severe:   return 460;
        case BlizzardIntensity::Light:
        default:                          return 240;
        }
    }

    void RefreshSnowfallActive(float globalIdle) {
        snowfallActive_ = forceSnowfall_ || (idleDelaySec_ == 0) ||
                          (globalIdle >= static_cast<float>(idleDelaySec_));
    }

    void ApplyIdleDelay(DWORD newSeconds) {
        idleDelaySec_ = std::min(newSeconds, MAX_IDLE_SEC);
        SaveIdleDelaySeconds(idleDelaySec_);
        RefreshSnowfallActive(GetGlobalIdleSeconds());
    }

    void ApplyBlizzardIntensity(BlizzardIntensity newLvl) {
        intensity_ = newLvl;
        SaveBlizzardIntensity(intensity_);

        // Immediately adapt existing ambient flakes to the new weather intensity
        size_t target = TargetAmbientCount();
        size_t keptAmbient = 0;
        std::vector<Flake> adjusted;
        adjusted.reserve(flakes_.size());
        for (auto f : flakes_) {
            if (f.ambient) {
                if (keptAmbient >= target) continue;
                ++keptAmbient;
                if (intensity_ == BlizzardIntensity::Light) {
                    f.vx = std::clamp(f.vx, -24.0f, 24.0f);
                    f.vy = std::clamp(f.vy, 45.0f, 95.0f);
                    f.depositAmt = 2.0f;
                } else if (intensity_ == BlizzardIntensity::Moderate) {
                    f.vx = RandRange(40.0f, 95.0f);
                    f.vy = RandRange(75.0f, 145.0f);
                    f.depositAmt = 2.6f;
                } else {
                    f.vx = RandRange(130.0f, 240.0f);
                    f.vy = RandRange(120.0f, 210.0f);
                    f.depositAmt = 3.4f;
                }
            }
            adjusted.push_back(f);
        }
        flakes_.swap(adjusted);
    }

    void ShowCustomIdleDialog() {
        if (activeDlg_ && IsWindow(activeDlg_)) {
            SetForegroundWindow(activeDlg_);
            return;
        }

        HDC screenDC = GetDC(nullptr);
        int dpi = GetDeviceCaps(screenDC, LOGPIXELSX);
        ReleaseDC(nullptr, screenDC);
        auto sc = [dpi](int px) { return MulDiv(px, dpi, 96); };

        int dlgW = sc(320);
        int dlgH = sc(178);
        int cx = workArea_.left + (width_ - dlgW) / 2;
        int cy = workArea_.top + (height_ - dlgH) / 2;

        IdleDialogState state{};
        state.resultSeconds = idleDelaySec_;

        HWND dlg = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_DLGMODALFRAME,
            L"WinterDesktopIdleDialogClass",
            L"Custom Idle Timer - Winter Desktop",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            cx, cy, dlgW, dlgH,
            nullptr, nullptr, hInst_, nullptr
        );
        if (!dlg) return;

        activeDlg_ = dlg;
        SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));

        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        HFONT hFont = nullptr;
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            hFont = CreateFontIndirectW(&ncm.lfMessageFont);
        }
        HFONT useFont = hFont ? hFont : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        auto addCtrl = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                           int x, int y, int w, int h, int id) -> HWND {
            HWND c = CreateWindowExW(
                0, cls, text, WS_CHILD | WS_VISIBLE | style,
                sc(x), sc(y), sc(w), sc(h),
                dlg, reinterpret_cast<HMENU>(static_cast<intptr_t>(id)), hInst_, nullptr
            );
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(useFont), TRUE);
            return c;
        };

        addCtrl(L"STATIC", L"Start snowfall after system is idle for:", 0, 16, 14, 275, 20, 0);

        std::wstring initMin = std::to_wstring(idleDelaySec_ / 60);
        std::wstring initSec = std::to_wstring(idleDelaySec_ % 60);

        HWND editMin = addCtrl(L"EDIT", initMin.c_str(),
            WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 16, 40, 64, 24, IDC_EDIT_MIN);
        addCtrl(L"STATIC", L"min", 0, 86, 44, 34, 20, 0);

        addCtrl(L"EDIT", initSec.c_str(),
            WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 132, 40, 64, 24, IDC_EDIT_SEC);
        addCtrl(L"STATIC", L"sec", 0, 202, 44, 34, 20, 0);

        addCtrl(L"STATIC", L"Tip: Set 0 min 0 sec for immediate snowfall.", 0, 16, 74, 275, 18, 0);

        addCtrl(L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, 130, 104, 76, 28, IDOK);
        addCtrl(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 214, 104, 76, 28, IDCANCEL);

        ShowWindow(dlg, SW_SHOW);
        SetForegroundWindow(dlg);
        SetFocus(editMin);
        SendMessageW(editMin, EM_SETSEL, 0, -1);

        MSG msg{};
        while (!state.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(dlg, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
        }

        activeDlg_ = nullptr;
        if (hFont) DeleteObject(hFont);

        if (state.accepted) {
            ApplyIdleDelay(state.resultSeconds);
        }
    }

    void RefreshMetrics() {
        fullW_ = GetSystemMetrics(SM_CXSCREEN);
        fullH_ = GetSystemMetrics(SM_CYSCREEN);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea_, 0);
        width_ = std::max(1, static_cast<int>(workArea_.right - workArea_.left));
        height_ = std::max(1, static_cast<int>(workArea_.bottom - workArea_.top));
        hasBottomTaskbar_ = (workArea_.bottom < fullH_ - 2);

        HDC screenDC = GetDC(nullptr);
        int dpiX = GetDeviceCaps(screenDC, LOGPIXELSX);
        ReleaseDC(nullptr, screenDC);
        cornerRadius_ = std::max(8.0f, 8.5f * (static_cast<float>(dpiX) / 96.0f));
    }

    void InitBuffers() {
        DestroyBuffers();
        HDC screenDC = GetDC(nullptr);
        memDC_ = CreateCompatibleDC(screenDC);
        ReleaseDC(nullptr, screenDC);

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width_;
        bmi.bmiHeader.biHeight = -height_;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        dibBmp_ = CreateDIBSection(memDC_, &bmi, DIB_RGB_COLORS, &dibBits_, nullptr, 0);
        oldBmp_ = SelectObject(memDC_, dibBmp_);
    }

    void DestroyBuffers() {
        if (memDC_) {
            if (oldBmp_) SelectObject(memDC_, oldBmp_);
            if (dibBmp_) DeleteObject(dibBmp_);
            DeleteDC(memDC_);
        }
        memDC_ = nullptr;
        dibBmp_ = nullptr;
        oldBmp_ = nullptr;
        dibBits_ = nullptr;
    }

    void CreateTrayIcon() {
        Bitmap iconBmp(32, 32, PixelFormat32bppPARGB);
        Graphics g(&iconBmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        Pen pen(Color(255, 175, 232, 255), 2.5f);
        for (int a = 0; a < 180; a += 45) {
            float rad = a * PI_F / 180.0f;
            float dx = std::cos(rad) * 11.0f;
            float dy = std::sin(rad) * 11.0f;
            g.DrawLine(&pen, 16.0f - dx, 16.0f - dy, 16.0f + dx, 16.0f + dy);
        }
        iconBmp.GetHICON(&trayIcon_);

        nid_.cbSize = sizeof(nid_);
        nid_.hWnd = hwnd_;
        nid_.uID = 1;
        nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid_.uCallbackMessage = WM_TRAYICON;
        nid_.hIcon = trayIcon_;
        lstrcpynW(nid_.szTip, L"Winter Desktop", static_cast<int>(std::size(nid_.szTip)));
        Shell_NotifyIconW(NIM_ADD, &nid_);
    }

    void DestroyTrayIcon() {
        if (nid_.hWnd) Shell_NotifyIconW(NIM_DELETE, &nid_);
        if (trayIcon_) DestroyIcon(trayIcon_);
        trayIcon_ = nullptr;
    }

    void SetClickThrough(bool enable) {
        if (enable == clickThrough_) return;
        LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        if (enable) ex |= WS_EX_TRANSPARENT;
        else ex &= ~WS_EX_TRANSPARENT;
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
        clickThrough_ = enable;
    }

    void SetPaused(bool shouldPause) {
        if (shouldPause == paused_) return;
        paused_ = shouldPause;
        if (paused_) {
            draggedBallId_ = -1;
            ReleaseCapture();
            SetClickThrough(true);
            ShowWindow(hwnd_, SW_HIDE);
            SetTimer(hwnd_, TIMER_TICK, 400, nullptr);
        } else {
            QueryPerformanceCounter(&lastTick_);
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
            SetTimer(hwnd_, TIMER_TICK, TargetFrameMs(), nullptr);
        }
    }

    // --- Curved Window Corner Profile Helpers ---
    float CornerDropAt(const WindowSurface& s, float localX) const {
        if (s.isTaskbar) return 0.0f;
        float r = std::min(cornerRadius_, s.rect.w * 0.25f);
        float edgeDist = std::min(localX, s.rect.w - localX);
        if (edgeDist >= r) return 0.0f;
        float dx = r - std::max(0.0f, edgeDist);
        return r - std::sqrt(std::max(0.0f, r * r - dx * dx));
    }

    float MaxSnowHeightAt(const WindowSurface& s, float localX) const {
        if (s.isTaskbar) return MAX_SNOW_H;
        float r = std::min(cornerRadius_, s.rect.w * 0.25f);
        float edgeDist = std::min(localX, s.rect.w - localX);
        float slipZone = r * 0.60f;
        float taperEnd = r * 2.40f;
        if (edgeDist <= slipZone) return 0.0f;
        if (edgeDist >= taperEnd) return MAX_SNOW_H;
        float t = (edgeDist - slipZone) / (taperEnd - slipZone);
        float smooth = t * t * (3.0f - 2.0f * t);
        return MAX_SNOW_H * smooth;
    }

    void SpawnCornerSlideDust(float x, float y, int sideDir, int count) {
        for (int i = 0; i < count && dust_.size() < 260; ++i) {
            float life = RandRange(0.45f, 1.05f);
            dust_.push_back({
                x + RandRange(-1.5f, 1.5f),
                y + RandRange(-1.0f, 2.0f),
                sideDir * RandRange(16.0f, 48.0f),
                RandRange(18.0f, 65.0f),
                RandRange(1.2f, 2.6f),
                life,
                life,
                RandInt(0, 2) == 0
            });
        }
    }

    // --- Snowball Lookup & Fall Tracking Helpers ---
    int FindBallIndexById(int id) const {
        if (id < 0) return -1;
        for (size_t i = 0; i < snowballs_.size(); ++i) {
            if (snowballs_[i].id == id) return static_cast<int>(i);
        }
        return -1;
    }

    int FindStackedChildIndex(int parentId) const {
        if (parentId < 0) return -1;
        for (size_t i = 0; i < snowballs_.size(); ++i) {
            if (snowballs_[i].stackedOnId == parentId && !snowballs_[i].collapsing) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    bool IsPartOfSnowman(int idx) const {
        if (idx < 0 || idx >= static_cast<int>(snowballs_.size())) return false;
        const auto& b = snowballs_[idx];
        if (b.collapsing) return false;
        return (b.stackedOnId >= 0) || (FindStackedChildIndex(b.id) >= 0);
    }

    void MarkBallsOnWindowAsFalling(HWND windowHwnd) {
        if (!windowHwnd || windowHwnd == TASKBAR_KEY) return;
        for (auto& b : snowballs_) {
            if (b.grabbed || b.collapsing) continue;
            if (b.restingHwnd == windowHwnd) {
                b.fellFromWindow = true;
                b.restingHwnd = nullptr;
                b.resting = false;
                int childIdx = FindStackedChildIndex(b.id);
                if (childIdx >= 0) {
                    snowballs_[childIdx].fellFromWindow = true;
                }
            }
        }
    }

    Flake MakeAmbientFlake(bool randomY) {
        Flake f;
        f.ambient = true;
        f.canDeposit = true;

        if (intensity_ == BlizzardIntensity::Light) {
            f.x = RandRange(0.0f, static_cast<float>(width_));
            f.y = randomY ? RandRange(0.0f, static_cast<float>(height_)) : RandRange(-20.0f, -2.0f);
            f.vx = RandRange(-18.0f, 18.0f);
            f.vy = RandRange(45.0f, 95.0f);
            f.r = RandRange(1.7f, 3.6f);
            f.depositAmt = 2.0f;
        } else if (intensity_ == BlizzardIntensity::Moderate) {
            bool spawnLeft = !randomY && (RandRange(0.0f, 1.0f) < 0.35f);
            if (spawnLeft) {
                f.x = RandRange(-20.0f, -2.0f);
                f.y = RandRange(0.0f, static_cast<float>(height_) * 0.85f);
            } else {
                f.x = RandRange(0.0f, static_cast<float>(width_));
                f.y = randomY ? RandRange(0.0f, static_cast<float>(height_)) : RandRange(-20.0f, -2.0f);
            }
            f.vx = RandRange(40.0f, 95.0f);
            f.vy = RandRange(75.0f, 145.0f);
            f.r = RandRange(1.8f, 4.1f);
            f.depositAmt = 2.6f;
        } else {
            // Severe blizzard: strong blowing crosswind + dense flakes from top & upwind edge
            bool spawnLeft = !randomY && (RandRange(0.0f, 1.0f) < 0.50f);
            if (spawnLeft) {
                f.x = RandRange(-24.0f, -2.0f);
                f.y = RandRange(0.0f, static_cast<float>(height_) * 0.90f);
            } else {
                f.x = RandRange(0.0f, static_cast<float>(width_));
                f.y = randomY ? RandRange(0.0f, static_cast<float>(height_)) : RandRange(-24.0f, -2.0f);
            }
            f.vx = RandRange(130.0f, 245.0f);
            f.vy = RandRange(120.0f, 215.0f);
            f.r = RandRange(1.6f, 4.6f);
            f.depositAmt = 3.4f;
        }
        return f;
    }

    // --- Window Enumeration & Topmost Z-Order Precedence ---
    struct EnumCtx {
        DWORD ownPid;
        RECT workArea;
        std::vector<std::pair<HWND, RectFSim>> list;
    };

    static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
        auto* ctx = reinterpret_cast<EnumCtx*>(lParam);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == ctx->ownPid || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
        if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;

        int cloaked = 0;
        DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked != 0) return TRUE;
        if (IsIgnoredClass(hwnd)) return TRUE;

        RECT r = GetWindowBounds(hwnd);
        int w = r.right - r.left;
        int h = r.bottom - r.top;
        if (w <= 120 || h <= 60) return TRUE;

        RectFSim local{
            static_cast<float>(r.left - ctx->workArea.left),
            static_cast<float>(r.top - ctx->workArea.top),
            static_cast<float>(w),
            static_cast<float>(h)
        };
        float ovW = static_cast<float>(ctx->workArea.right - ctx->workArea.left);
        float ovH = static_cast<float>(ctx->workArea.bottom - ctx->workArea.top);
        if (local.Right() > 0 && local.Left() < ovW && local.Bottom() > 0 && local.Top() < ovH) {
            ctx->list.push_back({ hwnd, local });
        }
        return TRUE;
    }

    void CheckFullscreenAndActiveClip(float globalIdle, bool& outShouldPause) {
        outShouldPause = false;
        hasActiveWindowClip_ = false;

        QUERY_USER_NOTIFICATION_STATE quns{};
        if (SUCCEEDED(SHQueryUserNotificationState(&quns))) {
            if (quns == QUNS_BUSY || quns == QUNS_RUNNING_D3D_FULL_SCREEN || quns == QUNS_PRESENTATION_MODE) {
                outShouldPause = true;
                return;
            }
        }

        HWND fg = GetForegroundWindow();
        if (!fg || IsIconic(fg) || IsIgnoredClass(fg)) return;
        DWORD fgPid = 0;
        GetWindowThreadProcessId(fg, &fgPid);
        if (fgPid == ownPid_) return;

        RECT r = GetWindowBounds(fg);
        if (r.left <= 4 && r.top <= 4 && r.right >= fullW_ - 4 && r.bottom >= fullH_ - 4) {
            outShouldPause = true;
            return;
        }
        if (IsZoomed(fg)) {
            outShouldPause = true;
            return;
        }
        LONG iw = std::max(static_cast<LONG>(0), std::min(r.right, workArea_.right) - std::max(r.left, workArea_.left));
        LONG ih = std::max(static_cast<LONG>(0), std::min(r.bottom, workArea_.bottom) - std::max(r.top, workArea_.top));
        float areaRatio = static_cast<float>(iw * ih) / std::max(1.0f, static_cast<float>(width_ * height_));
        if (areaRatio >= 0.94f) {
            outShouldPause = true;
            return;
        }

        if (globalIdle < ACTIVE_CLIP_GRACE_SEC) {
            hasActiveWindowClip_ = true;
            activeWindowClip_ = {
                static_cast<float>(r.left - workArea_.left),
                static_cast<float>(r.top - workArea_.top),
                static_cast<float>(r.right - r.left),
                static_cast<float>(r.bottom - r.top)
            };
        }
    }

    void SpillWindowSnow(WindowSurface& surf, float dx, float dy) {
        bool hadSnow = false;
        size_t maxFlakes = MaxFlakeCount();
        for (size_t i = 0; i < surf.heights.size(); ++i) {
            float h = surf.heights[i];
            if (h > 1.5f) {
                hadSnow = true;
                if (i % 2 == 0 && flakes_.size() < maxFlakes) {
                    Flake f;
                    f.x = surf.rect.Left() + static_cast<float>(i * BUCKET_W) + RandRange(-2.0f, 2.0f);
                    f.y = surf.rect.Top() - RandRange(0.0f, h);
                    f.vx = std::clamp(dx * 3.0f, -130.0f, 130.0f) + RandRange(-30.0f, 30.0f);
                    f.vy = std::clamp(dy * 2.0f, -60.0f, 90.0f) + RandRange(-20.0f, 25.0f);
                    f.r = RandRange(2.4f, 4.4f);
                    f.ambient = false;
                    f.canDeposit = true;
                    f.depositAmt = std::min(5.0f, h * 0.75f);
                    f.ignoreHwnd = surf.hwnd;
                    flakes_.push_back(f);
                }
            }
        }
        if (hadSnow) std::fill(surf.heights.begin(), surf.heights.end(), 0.0f);
    }

    void PollWindows(float dtPoll) {
        float globalIdle = GetGlobalIdleSeconds();
        bool shouldPause = false;
        CheckFullscreenAndActiveClip(globalIdle, shouldPause);
        if (shouldPause) {
            SetPaused(true);
            return;
        }
        SetPaused(false);

        RefreshSnowfallActive(globalIdle);

        HWND fg = GetForegroundWindow();
        EnumCtx ctx{ ownPid_, workArea_, {} };
        EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));

        std::vector<std::pair<HWND, RectFSim>> eligible;
        if (hasBottomTaskbar_) {
            eligible.push_back({ TASKBAR_KEY, RectFSim{ 0.0f, static_cast<float>(height_), static_cast<float>(width_), 40.0f } });
        }

        std::vector<RectFSim> higherRects;
        for (const auto& [h, r] : ctx.list) {
            bool topOccluded = false;
            for (const auto& hr : higherRects) {
                bool vertHit = (r.Top() >= hr.Top() - 18.0f && r.Top() <= hr.Bottom() + 4.0f);
                bool horizOverlap = (std::min(r.Right(), hr.Right()) - std::max(r.Left(), hr.Left())) > 16.0f;
                if (vertHit && horizOverlap) {
                    topOccluded = true;
                    break;
                }
            }
            if (!topOccluded) {
                eligible.push_back({ h, r });
            }
            higherRects.push_back(r);
        }

        std::unordered_set<HWND> activeSet;
        zOrder_.clear();

        for (const auto& [h, r] : eligible) {
            bool isTB = (h == TASKBAR_KEY);
            activeSet.insert(h);
            zOrder_.push_back(h);

            auto it = surfaces_.find(h);
            if (it == surfaces_.end()) {
                WindowSurface s;
                s.hwnd = h;
                s.rect = r;
                s.isTaskbar = isTB;
                s.EnsureBuckets();
                surfaces_.emplace(h, std::move(s));
            } else {
                WindowSurface& s = it->second;
                float dx = r.x - s.rect.x;
                float dy = r.y - s.rect.y;
                if (std::abs(dx) > 2.0f || std::abs(dy) > 2.0f || std::abs(r.w - s.rect.w) > 2.0f) {
                    SpillWindowSnow(s, dx, dy);
                    if (!isTB) MarkBallsOnWindowAsFalling(s.hwnd);
                    s.rect = r;
                    s.idleTime = 0.0f;
                    s.EnsureBuckets();
                } else {
                    if (isTB || h != fg || globalIdle >= WINDOW_IDLE_SEC) {
                        s.idleTime += dtPoll;
                    } else {
                        s.idleTime = 0.0f;
                    }
                }
            }
        }

        // Closed, minimized, or occluded windows spill snow & drop snowballs/snowmen
        for (auto it = surfaces_.begin(); it != surfaces_.end(); ) {
            if (activeSet.find(it->first) == activeSet.end()) {
                SpillWindowSnow(it->second, 0.0f, 15.0f);
                if (!it->second.isTaskbar) MarkBallsOnWindowAsFalling(it->first);
                it = surfaces_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // --- Plow, Santa & Penguin Spawning ---
    void SpawnPlowForSurface(const WindowSurface& surf) {
        for (const auto& p : plows_) {
            if (p.attachedHwnd == surf.hwnd) return;
        }
        SnowPlow pl;
        pl.x = surf.rect.Left() - 10.0f;
        pl.y = surf.rect.Top();
        pl.dir = 1;
        pl.speed = surf.isTaskbar ? 175.0f : 145.0f;
        pl.attachedHwnd = surf.hwnd;
        pl.isTaskbarPlow = surf.isTaskbar;
        plows_.push_back(pl);
    }

    void SummonTopWindowPlow() {
        WindowSurface* best = nullptr;
        float bestAvg = 0.5f;
        for (HWND h : zOrder_) {
            if (h == TASKBAR_KEY) continue;
            auto it = surfaces_.find(h);
            if (it == surfaces_.end()) continue;
            float avg = it->second.AverageHeight();
            if (avg > bestAvg) {
                bestAvg = avg;
                best = &it->second;
            }
        }
        if (best) SpawnPlowForSurface(*best);
    }

    void SummonTaskbarPlow() {
        auto it = surfaces_.find(TASKBAR_KEY);
        if (it != surfaces_.end()) SpawnPlowForSurface(it->second);
    }

    void SummonSanta() {
        if (paused_) return;
        santa_.active = true;
        santa_.x = -240.0f;
        santa_.baseY = RandRange(height_ * 0.10f, height_ * 0.24f);
        santa_.y = santa_.baseY;
        santa_.vx = 165.0f;
        santa_.phase = 0.0f;
    }

    void SummonPenguinGroup() {
        if (paused_ || !hasBottomTaskbar_) return;
        int count = RandInt(4, 5);
        int dir = (RandInt(0, 1) == 0) ? 1 : -1;
        float baseSpeed = RandRange(28.0f, 38.0f);
        float startX = (dir == 1) ? -22.0f : static_cast<float>(width_) + 22.0f;

        float cursor = startX;
        for (int i = 0; i < count; ++i) {
            Penguin pg;
            pg.x = cursor;
            pg.y = static_cast<float>(height_);
            pg.dir = dir;
            pg.speed = baseSpeed;
            pg.scale = (i == 0) ? RandRange(0.96f, 1.05f) : RandRange(0.76f, 1.02f);
            pg.waddlePhase = RandRange(0.0f, 2.0f * PI_F);
            pg.dustTimer = RandRange(0.15f, 0.45f);
            penguins_.push_back(pg);
            cursor -= dir * RandRange(27.0f, 35.0f);
        }
    }

    void ClearAllSnow() {
        for (auto& [_, s] : surfaces_) std::fill(s.heights.begin(), s.heights.end(), 0.0f);
        snowballs_.clear();
        plows_.clear();
        penguins_.clear();
        dust_.clear();
        flakes_.clear();
    }

    // --- Dynamic Click-Through Hit Test ---
    void UpdateClickThrough() {
        if (draggedBallId_ >= 0) {
            SetClickThrough(false);
            return;
        }
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
            SetClickThrough(false);
            return;
        }

        POINT pt{};
        GetCursorPos(&pt);
        float mx = static_cast<float>(pt.x - workArea_.left);
        float my = static_cast<float>(pt.y - workArea_.top);

        for (const auto& b : snowballs_) {
            if (b.collapsing) continue;
            if (std::hypot(mx - b.x, my - b.y) <= b.r + 8.0f) {
                SetClickThrough(false);
                return;
            }
        }
        for (const auto& [_, s] : surfaces_) {
            if (mx >= s.rect.Left() && mx <= s.rect.Right() &&
                my >= s.rect.Top() - MAX_SNOW_H - 6.0f && my <= s.rect.Top() + 8.0f) {
                int idx = static_cast<int>((mx - s.rect.Left()) / BUCKET_W);
                if (idx >= 0 && idx < static_cast<int>(s.heights.size()) && s.heights[idx] >= 4.0f) {
                    SetClickThrough(false);
                    return;
                }
            }
        }
        SetClickThrough(true);
    }

    // --- Snowball Splat, Snowdust & Collapse ---
    void SplatSnowball(const Snowball& b) {
        size_t maxFlakes = MaxFlakeCount();
        for (int i = 0; i < 12 && flakes_.size() < maxFlakes; ++i) {
            float ang = RandRange(0.0f, 2.0f * PI_F);
            float spd = RandRange(70.0f, 250.0f);
            Flake f;
            f.x = b.x;
            f.y = b.y;
            f.vx = std::cos(ang) * spd;
            f.vy = std::sin(ang) * spd - 60.0f;
            f.r = RandRange(2.2f, 4.2f);
            f.ambient = false;
            f.canDeposit = true;
            f.depositAmt = 2.0f;
            flakes_.push_back(f);
        }
    }

    void BurstIntoSnowdust(const Snowball& b) {
        for (int i = 0; i < 24 && dust_.size() < 260; ++i) {
            float ang = RandRange(0.0f, 2.0f * PI_F);
            float spd = RandRange(35.0f, 195.0f);
            float life = RandRange(0.55f, 1.25f);
            dust_.push_back({
                b.x + RandRange(-b.r * 0.45f, b.r * 0.45f),
                b.y + RandRange(-b.r * 0.45f, b.r * 0.45f),
                std::cos(ang) * spd,
                std::sin(ang) * spd - RandRange(25.0f, 80.0f),
                RandRange(1.4f, 3.6f),
                life,
                life,
                RandInt(0, 2) == 0
            });
        }
        size_t maxFlakes = MaxFlakeCount();
        for (int i = 0; i < 6 && flakes_.size() < maxFlakes; ++i) {
            float ang = RandRange(0.0f, 2.0f * PI_F);
            float spd = RandRange(45.0f, 140.0f);
            Flake f;
            f.x = b.x;
            f.y = b.y;
            f.vx = std::cos(ang) * spd;
            f.vy = std::sin(ang) * spd - 40.0f;
            f.r = RandRange(1.8f, 3.2f);
            f.ambient = false;
            f.canDeposit = true;
            f.depositAmt = 1.4f;
            flakes_.push_back(f);
        }
    }

    void DestroyBallOrSnowmanIntoDust(size_t idx, std::vector<bool>& dead) {
        if (idx >= snowballs_.size() || dead[idx]) return;
        const auto& b = snowballs_[idx];
        BurstIntoSnowdust(b);
        dead[idx] = true;

        int childIdx = FindStackedChildIndex(b.id);
        if (childIdx >= 0 && !dead[childIdx]) {
            auto& head = snowballs_[childIdx];
            head.x = b.x;
            head.y = b.y - (b.r + head.r - 3.0f);
            BurstIntoSnowdust(head);
            dead[childIdx] = true;
        }
    }

    void TriggerSnowmanCollapse(int incomingIdx, int snowmanMemberIdx) {
        int bodyIdx = -1;
        int headIdx = -1;
        const auto& member = snowballs_[snowmanMemberIdx];
        if (member.stackedOnId >= 0) {
            headIdx = snowmanMemberIdx;
            bodyIdx = FindBallIndexById(member.stackedOnId);
        } else {
            bodyIdx = snowmanMemberIdx;
            headIdx = FindStackedChildIndex(member.id);
        }

        auto startCollapse = [&](int idx, float pushDir, float upKick, float timer) {
            if (idx < 0 || idx >= static_cast<int>(snowballs_.size())) return;
            auto& b = snowballs_[idx];
            if (b.collapsing) return;
            b.collapsing = true;
            b.collapseTimer = timer;
            b.grabbed = false;
            b.resting = false;
            b.restingHwnd = nullptr;
            b.fellFromWindow = false;
            b.stackedOnId = -1;
            b.stackLevel = 0;
            b.aligning = false;
            b.snowmanReveal = 0.0f;
            b.vx = pushDir * RandRange(70.0f, 155.0f);
            b.vy = -upKick;
        };

        float dir = (snowballs_[incomingIdx].x >= snowballs_[snowmanMemberIdx].x) ? 1.0f : -1.0f;
        startCollapse(incomingIdx, dir, RandRange(40.0f, 110.0f), 0.34f);
        startCollapse(headIdx, -dir, RandRange(70.0f, 130.0f), 0.38f);
        startCollapse(bodyIdx, dir * 0.5f, RandRange(90.0f, 145.0f), 0.32f);
    }

    bool HasVisualContent() const {
        if (santa_.active || !flakes_.empty() || !dust_.empty() ||
            !sparkles_.empty() || !plows_.empty() || !snowballs_.empty() || !penguins_.empty()) {
            return true;
        }
        for (const auto& [_, s] : surfaces_) {
            if (s.HasSnow()) return true;
        }
        return false;
    }

    void RelaxSnowbanks() {
        for (auto& [_, s] : surfaces_) {
            size_t n = s.heights.size();
            if (n < 3 || !s.HasSnow()) continue;

            for (size_t i = 0; i + 1 < n; ++i) {
                float diff = s.heights[i] - s.heights[i + 1];
                if (std::abs(diff) > 2.2f) {
                    float flow = diff * 0.22f;
                    s.heights[i] -= flow;
                    s.heights[i + 1] += flow;
                }
            }

            if (s.isTaskbar) continue;

            for (size_t i = 0; i < n; ++i) {
                float localX = (static_cast<float>(i) + 0.5f) * BUCKET_W;
                if (localX > cornerRadius_ * 2.5f && (s.rect.w - localX) > cornerRadius_ * 2.5f) {
                    continue;
                }
                float maxH = MaxSnowHeightAt(s, localX);
                if (s.heights[i] > maxH) {
                    float overflow = s.heights[i] - maxH;
                    s.heights[i] = maxH;
                    if (overflow > 0.15f) {
                        int sideDir = (localX < s.rect.w * 0.5f) ? -1 : 1;
                        float worldX = s.rect.Left() + localX;
                        float worldY = s.rect.Top() + CornerDropAt(s, localX) - maxH;
                        SpawnCornerSlideDust(worldX, worldY, sideDir, (overflow > 0.8f) ? 2 : 1);
                    }
                } else if (s.heights[i] > 1.8f && RandRange(0.0f, 1.0f) < 0.04f) {
                    int sideDir = (localX < s.rect.w * 0.5f) ? -1 : 1;
                    float worldX = s.rect.Left() + localX;
                    float worldY = s.rect.Top() + CornerDropAt(s, localX) - s.heights[i] * 0.5f;
                    s.heights[i] = std::max(0.0f, s.heights[i] - 0.12f);
                    SpawnCornerSlideDust(worldX, worldY, sideDir, 1);
                }
            }
        }
    }

    void UpdateTick() {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - lastTick_.QuadPart) / static_cast<float>(qpcFreq_.QuadPart);
        lastTick_ = now;
        dt = std::min(dt, 0.06f);
        elapsedSec_ += dt;

        if (paused_) {
            PollWindows(0.4f);
            return;
        }

        pollAccum_ += dt;
        if (pollAccum_ >= 0.30f) {
            PollWindows(pollAccum_);
            pollAccum_ = 0.0f;
            if (paused_) return;
        }

        hoverAccum_ += dt;
        if (draggedBallId_ >= 0 || hoverAccum_ >= 0.08f) {
            UpdateClickThrough();
            hoverAccum_ = 0.0f;
        }

        UpdateFlakes(dt);

        relaxAccum_ += dt;
        if (relaxAccum_ >= 0.10f) {
            RelaxSnowbanks();
            relaxAccum_ = 0.0f;
        }

        UpdateDust(dt);
        UpdatePlows(dt);
        UpdatePenguins(dt);
        UpdateSnowballs(dt);
        UpdateSanta(dt);
        UpdateSparkles(dt);

        bool hasContent = HasVisualContent();
        if (hasContent || lastFrameHadContent_) {
            RenderFrame();
            lastFrameHadContent_ = hasContent;
        }
    }

    void UpdateFlakes(float dt) {
        size_t maxFlakes = MaxFlakeCount();
        if (snowfallActive_) {
            size_t ambientCount = 0;
            for (const auto& f : flakes_) {
                if (f.ambient) ++ambientCount;
            }
            size_t target = TargetAmbientCount();
            if (ambientCount < target) {
                ambientSpawnAccum_ += dt * AmbientSpawnRate();
                while (ambientSpawnAccum_ >= 1.0f && ambientCount < target && flakes_.size() < maxFlakes) {
                    flakes_.push_back(MakeAmbientFlake(false));
                    ambientSpawnAccum_ -= 1.0f;
                    ++ambientCount;
                }
            }
        } else {
            ambientSpawnAccum_ = 0.0f;
        }

        // Dynamic gusts for Moderate & Severe blizzards
        float gustModerate = 68.0f + std::sin(elapsedSec_ * 0.9f) * 24.0f;
        float gustSevere = 185.0f + std::sin(elapsedSec_ * 0.8f) * 55.0f + std::sin(elapsedSec_ * 2.1f) * 20.0f;

        std::vector<Flake> next;
        next.reserve(flakes_.size());

        for (auto& f : flakes_) {
            f.age += dt;
            if (f.ambient) {
                if (intensity_ == BlizzardIntensity::Light) {
                    f.vx = std::clamp(f.vx + std::sin(elapsedSec_ * 1.6f + f.y * 0.02f) * 9.0f * dt, -30.0f, 30.0f);
                } else if (intensity_ == BlizzardIntensity::Moderate) {
                    float targetVx = gustModerate + std::sin(elapsedSec_ * 2.0f + f.y * 0.02f) * 16.0f;
                    f.vx += (targetVx - f.vx) * std::min(1.0f, dt * 2.2f);
                } else {
                    float targetVx = gustSevere + std::sin(elapsedSec_ * 3.1f + f.y * 0.025f) * 32.0f;
                    f.vx += (targetVx - f.vx) * std::min(1.0f, dt * 3.0f);
                }
            } else {
                f.vy += GRAVITY * 0.55f * dt;
                f.vx *= 0.99f;
            }
            f.x += f.vx * dt;
            f.y += f.vy * dt;

            bool landed = false;
            if (f.canDeposit && f.vy > 0.0f) {
                for (HWND h : zOrder_) {
                    if (h == f.ignoreHwnd && f.age < 0.25f) continue;
                    auto it = surfaces_.find(h);
                    if (it == surfaces_.end()) continue;
                    WindowSurface& s = it->second;
                    if (f.ambient && !s.isTaskbar && s.idleTime < WINDOW_IDLE_SEC) continue;

                    if (f.x >= s.rect.Left() && f.x <= s.rect.Right()) {
                        float localX = f.x - s.rect.Left();
                        int idx = static_cast<int>(localX / BUCKET_W);
                        if (idx >= 0 && idx < static_cast<int>(s.heights.size())) {
                            float edgeY = s.rect.Top() + CornerDropAt(s, localX);
                            float snowTop = edgeY - s.heights[idx];
                            if (f.y >= snowTop - 6.0f && f.y <= edgeY + 12.0f) {
                                float maxHere = MaxSnowHeightAt(s, localX);
                                if (!s.isTaskbar && maxHere < 1.2f) {
                                    int sideDir = (localX < s.rect.w * 0.5f) ? -1 : 1;
                                    SpawnCornerSlideDust(f.x, edgeY, sideDir, 2);
                                    landed = true;
                                    break;
                                }

                                const int offs[5] = { -2, -1, 0, 1, 2 };
                                const float wts[5] = { 0.22f, 0.55f, 0.90f, 0.55f, 0.22f };
                                for (int k = 0; k < 5; ++k) {
                                    int bi = idx + offs[k];
                                    if (bi >= 0 && bi < static_cast<int>(s.heights.size())) {
                                        float bx = (static_cast<float>(bi) + 0.5f) * BUCKET_W;
                                        float cap = MaxSnowHeightAt(s, bx);
                                        float added = s.heights[bi] + f.depositAmt * wts[k];
                                        if (added > cap && !s.isTaskbar && cap < MAX_SNOW_H * 0.9f) {
                                            int sideDir = (bx < s.rect.w * 0.5f) ? -1 : 1;
                                            SpawnCornerSlideDust(s.rect.Left() + bx, s.rect.Top() + CornerDropAt(s, bx) - cap, sideDir, 1);
                                        }
                                        s.heights[bi] = std::min(cap, added);
                                    }
                                }
                                landed = true;
                                break;
                            }
                        }
                    }
                }
            }

            if (landed || f.y > height_ + 20 || f.x < -50 || f.x > width_ + 50) {
                if (f.ambient && snowfallActive_ && next.size() < maxFlakes) {
                    next.push_back(MakeAmbientFlake(false));
                }
            } else if (next.size() < maxFlakes) {
                next.push_back(f);
            }
        }
        flakes_.swap(next);

        bool hasTbPlow = false;
        bool hasWinPlow = false;
        for (const auto& p : plows_) {
            if (p.isTaskbarPlow) hasTbPlow = true;
            else hasWinPlow = true;
        }

        if (!hasTbPlow) {
            auto tbIt = surfaces_.find(TASKBAR_KEY);
            if (tbIt != surfaces_.end() && tbIt->second.AverageHeight() >= PLOW_TRIGGER_TB) {
                SpawnPlowForSurface(tbIt->second);
            }
        }

        if (!hasWinPlow) {
            for (HWND h : zOrder_) {
                if (h == TASKBAR_KEY) continue;
                auto it = surfaces_.find(h);
                if (it != surfaces_.end() && it->second.AverageHeight() >= PLOW_TRIGGER_WIN) {
                    SpawnPlowForSurface(it->second);
                    break;
                }
            }
        }
    }

    void UpdateDust(float dt) {
        if (dust_.empty()) return;
        std::vector<DustParticle> alive;
        alive.reserve(dust_.size());
        for (auto d : dust_) {
            d.life -= dt;
            if (d.life > 0.0f) {
                d.vx *= 0.95f;
                d.vy = d.vy * 0.96f + 135.0f * dt;
                d.x += d.vx * dt;
                d.y += d.vy * dt;
                alive.push_back(d);
            }
        }
        dust_.swap(alive);
    }

    void UpdatePenguins(float dt) {
        if (penguins_.empty()) {
            if (hasBottomTaskbar_ && snowfallActive_) {
                penguinCooldown_ -= dt;
                if (penguinCooldown_ <= 0.0f) {
                    SummonPenguinGroup();
                    penguinCooldown_ = RandRange(55.0f, 115.0f);
                }
            }
            return;
        }

        auto tbIt = surfaces_.find(TASKBAR_KEY);
        std::vector<Penguin> alive;
        alive.reserve(penguins_.size());

        for (auto pg : penguins_) {
            pg.waddlePhase += dt * (6.2f / pg.scale);
            pg.x += pg.dir * pg.speed * dt;
            pg.y = static_cast<float>(height_);

            if (tbIt != surfaces_.end()) {
                auto& tb = tbIt->second;
                int idx = static_cast<int>((pg.x - tb.rect.Left()) / BUCKET_W);
                if (idx >= 0 && idx < static_cast<int>(tb.heights.size())) {
                    float sh = tb.heights[idx];
                    if (sh > 2.0f) {
                        pg.y = static_cast<float>(height_) - std::min(5.0f, sh * 0.30f);
                        pg.dustTimer -= dt;
                        if (pg.dustTimer <= 0.0f && dust_.size() < 260) {
                            pg.dustTimer = RandRange(0.28f, 0.55f);
                            float life = RandRange(0.35f, 0.65f);
                            dust_.push_back({
                                pg.x - pg.dir * 4.0f,
                                pg.y - 2.0f,
                                -pg.dir * RandRange(12.0f, 30.0f),
                                -RandRange(22.0f, 48.0f),
                                RandRange(1.2f, 2.2f),
                                life, life, false
                            });
                        }
                    }
                }
            }

            if (pg.hopY == 0.0f && pg.hopVy == 0.0f) {
                for (const auto& pl : plows_) {
                    if (pl.isTaskbarPlow && std::abs(pl.x - pg.x) < 42.0f) {
                        pg.hopVy = -270.0f;
                        break;
                    }
                }
            }
            if (pg.hopY < 0.0f || pg.hopVy != 0.0f) {
                pg.hopVy += GRAVITY * 0.85f * dt;
                pg.hopY += pg.hopVy * dt;
                if (pg.hopY >= 0.0f) {
                    pg.hopY = 0.0f;
                    pg.hopVy = 0.0f;
                }
            }

            bool finished = (pg.dir == 1 && pg.x > width_ + 48.0f) || (pg.dir == -1 && pg.x < -48.0f);
            if (!finished) alive.push_back(pg);
        }

        penguins_.swap(alive);
    }

    void TossSnowmenNearPlow(const SnowPlow& pl) {
        for (auto& b : snowballs_) {
            if (b.grabbed || b.collapsing) continue;
            float dx = b.x - pl.x;
            bool inBladeX = (pl.dir == 1) ? (dx > -18.0f && dx < 46.0f) : (dx < 18.0f && dx > -46.0f);
            bool inHeightY = (b.y + b.r >= pl.y - 82.0f && b.y - b.r <= pl.y + 8.0f);
            if (inBladeX && inHeightY) {
                b.vx = pl.dir * RandRange(240.0f, 440.0f);
                b.vy = -RandRange(280.0f, 480.0f);
                b.resting = false;
                b.restingHwnd = nullptr;
                b.fellFromWindow = !pl.isTaskbarPlow;
                b.stackLevel = 0;
                b.stackedOnId = -1;
                b.aligning = false;
                b.snowmanReveal = 0.0f;
            }
        }
    }

    void UpdatePlows(float dt) {
        size_t maxFlakes = MaxFlakeCount();
        std::vector<SnowPlow> alive;
        for (auto& pl : plows_) {
            auto it = pl.attachedHwnd ? surfaces_.find(pl.attachedHwnd) : surfaces_.end();
            if (it != surfaces_.end()) {
                WindowSurface& s = it->second;
                pl.x += pl.dir * pl.speed * dt;
                pl.y = s.rect.Top();

                TossSnowmenNearPlow(pl);

                float bladeX = pl.x + pl.dir * 26.0f;
                int bIdx = static_cast<int>((bladeX - s.rect.Left()) / BUCKET_W);
                for (int k = bIdx - 2; k <= bIdx + 2; ++k) {
                    if (k >= 0 && k < static_cast<int>(s.heights.size()) && s.heights[k] > 0.3f) {
                        float cleared = s.heights[k];
                        s.heights[k] = 0.0f;
                        pl.moundMass = std::min(28.0f, pl.moundMass + cleared * 0.14f);
                        if (RandRange(0.0f, 1.0f) < 0.30f && flakes_.size() < maxFlakes) {
                            Flake f;
                            f.x = s.rect.Left() + static_cast<float>(k * BUCKET_W);
                            f.y = s.rect.Top() - RandRange(2.0f, std::max(4.0f, cleared));
                            f.vx = pl.dir * RandRange(55.0f, 140.0f);
                            f.vy = RandRange(-130.0f, -45.0f);
                            f.r = RandRange(2.0f, 3.8f);
                            f.ambient = false;
                            f.canDeposit = !s.isTaskbar;
                            f.depositAmt = 1.4f;
                            f.ignoreHwnd = s.hwnd;
                            flakes_.push_back(f);
                        }
                    }
                }

                bool offRight = (pl.dir == 1 && pl.x > s.rect.Right() + 14.0f);
                bool offLeft = (pl.dir == -1 && pl.x < s.rect.Left() - 14.0f);
                if (offRight || offLeft) {
                    int clumps = static_cast<int>(pl.moundMass);
                    for (int c = 0; c < clumps && flakes_.size() < maxFlakes; ++c) {
                        Flake f;
                        f.x = bladeX + RandRange(-6.0f, 6.0f);
                        f.y = pl.y - RandRange(2.0f, std::max(6.0f, pl.moundMass));
                        f.vx = pl.dir * RandRange(45.0f, 145.0f);
                        f.vy = RandRange(-85.0f, 20.0f);
                        f.r = RandRange(2.5f, 4.4f);
                        f.ambient = false;
                        f.canDeposit = !s.isTaskbar;
                        f.depositAmt = 2.4f;
                        f.ignoreHwnd = s.hwnd;
                        flakes_.push_back(f);
                    }
                    pl.moundMass = 0.0f;
                    if (s.isTaskbar) continue;
                    pl.attachedHwnd = nullptr;
                    pl.vx = pl.dir * 95.0f;
                    pl.vy = -55.0f;
                }
            } else {
                pl.vy += GRAVITY * dt;
                pl.x += pl.vx * dt;
                pl.y += pl.vy * dt;

                if (pl.vy > 0.0f) {
                    for (HWND h : zOrder_) {
                        auto cit = surfaces_.find(h);
                        if (cit == surfaces_.end()) continue;
                        const auto& cand = cit->second;
                        if (pl.x >= cand.rect.Left() && pl.x <= cand.rect.Right() &&
                            pl.y >= cand.rect.Top() - 8.0f && pl.y <= cand.rect.Top() + 26.0f) {
                            pl.y = cand.rect.Top();
                            pl.vx = 0.0f;
                            pl.vy = 0.0f;
                            pl.attachedHwnd = h;
                            pl.isTaskbarPlow = cand.isTaskbar;
                            pl.dir = ((cand.rect.Right() - pl.x) >= (pl.x - cand.rect.Left())) ? 1 : -1;
                            break;
                        }
                    }
                }
                if (pl.y > height_ + 60 || pl.x < -80 || pl.x > width_ + 80) continue;
            }
            alive.push_back(pl);
        }
        plows_.swap(alive);
    }

    void UpdateSnowballs(float dt) {
        std::vector<bool> dead(snowballs_.size(), false);

        for (size_t i = 0; i < snowballs_.size(); ++i) {
            auto& b = snowballs_[i];
            if (dead[i] || b.grabbed) continue;
            if (b.stackedOnId >= 0 && !b.collapsing) continue;

            // Detect if a snowball rolled off or lost the app window underneath it
            if (b.restingHwnd != nullptr && b.restingHwnd != TASKBAR_KEY) {
                auto rit = surfaces_.find(b.restingHwnd);
                bool stillOnWin = (rit != surfaces_.end() &&
                                   b.x >= rit->second.rect.Left() &&
                                   b.x <= rit->second.rect.Right() &&
                                   std::abs((b.y + b.r) - rit->second.rect.Top()) <= 14.0f);
                if (!stillOnWin) {
                    b.fellFromWindow = true;
                    b.restingHwnd = nullptr;
                    int childIdx = FindStackedChildIndex(b.id);
                    if (childIdx >= 0) snowballs_[childIdx].fellFromWindow = true;
                }
            }

            float prevBottom = b.y + b.r;
            b.vy += GRAVITY * dt;
            b.x += b.vx * dt;
            b.y += b.vy * dt;
            b.resting = false;

            if (b.collapsing) {
                b.collapseTimer -= dt;
                bool hitFloor = false;
                if (b.y + b.r >= static_cast<float>(height_)) {
                    b.y = static_cast<float>(height_) - b.r;
                    hitFloor = true;
                } else if (b.vy > 0.0f) {
                    for (HWND h : zOrder_) {
                        auto it = surfaces_.find(h);
                        if (it == surfaces_.end()) continue;
                        const auto& s = it->second;
                        if (b.x >= s.rect.Left() && b.x <= s.rect.Right()) {
                            float sy = s.rect.Top();
                            if (prevBottom <= sy + 12.0f && (b.y + b.r) >= sy - 4.0f) {
                                b.y = sy - b.r;
                                hitFloor = true;
                                break;
                            }
                        }
                    }
                }
                if (b.collapseTimer <= 0.0f || (hitFloor && b.collapseTimer < 0.22f)) {
                    BurstIntoSnowdust(b);
                    dead[i] = true;
                }
                continue;
            }

            if (b.x - b.r < 0.0f || b.x + b.r > static_cast<float>(width_)) {
                if (std::abs(b.vx) > 350.0f) {
                    SplatSnowball(b);
                    dead[i] = true;
                    continue;
                } else {
                    b.x = std::clamp(b.x, b.r, static_cast<float>(width_) - b.r);
                    b.vx *= -0.45f;
                }
            }

            // Swept surface collision so fast falls never tunnel past lower windows or the taskbar
            for (HWND h : zOrder_) {
                auto it = surfaces_.find(h);
                if (it == surfaces_.end()) continue;
                const auto& s = it->second;
                if (b.x >= s.rect.Left() && b.x <= s.rect.Right()) {
                    float sy = s.rect.Top();
                    if (b.vy >= 0.0f && prevBottom <= sy + 12.0f && (b.y + b.r) >= sy - 6.0f) {
                        if (s.isTaskbar && b.fellFromWindow) {
                            // Fell from an app window onto the taskbar -> shatter into snowdust!
                            b.y = sy - b.r;
                            DestroyBallOrSnowmanIntoDust(i, dead);
                        } else if (!b.fellFromWindow && std::hypot(b.vx, b.vy) > 540.0f) {
                            SplatSnowball(b);
                            dead[i] = true;
                        } else {
                            // Landed intact (including window-to-window drops!)
                            b.y = sy - b.r;
                            b.vy = (std::abs(b.vy) > 90.0f) ? -b.vy * 0.22f : 0.0f;
                            b.resting = (b.vy == 0.0f);
                            b.vx *= 0.80f;
                            b.restingHwnd = s.hwnd;
                            b.fellFromWindow = false;
                            int childIdx = FindStackedChildIndex(b.id);
                            if (childIdx >= 0) snowballs_[childIdx].fellFromWindow = false;
                        }
                        break;
                    }
                }
            }

            if (!dead[i] && b.y + b.r > static_cast<float>(height_)) {
                b.y = static_cast<float>(height_) - b.r;
                if (b.fellFromWindow) {
                    DestroyBallOrSnowmanIntoDust(i, dead);
                } else {
                    b.vy = 0.0f;
                    b.vx *= 0.80f;
                    b.resting = true;
                    b.restingHwnd = hasBottomTaskbar_ ? TASKBAR_KEY : nullptr;
                }
            }
        }

        // Smooth arc realignment for stacked snowman heads
        for (size_t i = 0; i < snowballs_.size(); ++i) {
            auto& head = snowballs_[i];
            if (dead[i] || head.grabbed || head.collapsing || head.stackedOnId < 0) continue;

            int pIdx = FindBallIndexById(head.stackedOnId);
            if (pIdx < 0 || dead[pIdx] || snowballs_[pIdx].grabbed || snowballs_[pIdx].collapsing) {
                head.stackedOnId = -1;
                head.stackLevel = 0;
                head.aligning = false;
                head.snowmanReveal = 0.0f;
                continue;
            }

            const auto& body = snowballs_[pIdx];
            float nestDist = body.r + head.r - 3.0f;

            if (head.aligning) {
                head.alignAngle *= std::exp(-9.5f * dt);
                if (std::abs(head.alignAngle) < 0.006f) {
                    head.alignAngle = 0.0f;
                    head.aligning = false;
                }
            }

            head.x = body.x + std::sin(head.alignAngle) * nestDist;
            head.y = body.y - std::cos(head.alignAngle) * nestDist;
            head.vx = 0.0f;
            head.vy = 0.0f;
            head.resting = body.resting;
            head.restingHwnd = body.restingHwnd;
            head.fellFromWindow = body.fellFromWindow;
            head.stackLevel = 1;

            if (std::abs(head.alignAngle) < 0.25f) {
                head.snowmanReveal = std::min(1.0f, head.snowmanReveal + dt * 4.5f);
            }
        }

        // Snowball-to-snowball stacking (+/- 30 deg), 3rd-ball collapse, and splats
        for (size_t i = 0; i < snowballs_.size(); ++i) {
            if (dead[i] || snowballs_[i].grabbed || snowballs_[i].collapsing) continue;
            if (snowballs_[i].stackedOnId >= 0) continue;

            for (size_t j = 0; j < snowballs_.size(); ++j) {
                if (i == j || dead[j] || snowballs_[j].grabbed || snowballs_[j].collapsing) continue;

                auto& a = snowballs_[i];
                auto& b = snowballs_[j];
                if (b.stackedOnId == a.id) continue;

                float dx = a.x - b.x;
                float dy = b.y - a.y;
                float dist = std::hypot(dx, dy);
                float minDist = a.r + b.r - 2.0f;

                bool bIsSnowmanHead = (b.stackedOnId >= 0);
                bool bIsSnowman = IsPartOfSnowman(static_cast<int>(j));
                bool aIsSnowman = IsPartOfSnowman(static_cast<int>(i));

                bool hitSnowmanHatZone = bIsSnowmanHead && (dy > 0.0f) &&
                                         (std::abs(dx) < b.r * 1.15f) &&
                                         (dy < a.r + b.r + 14.0f);

                if ((dist > 0.001f && dist < minDist + 3.0f) || hitSnowmanHatZone) {
                    if (bIsSnowman && !aIsSnowman) {
                        if (dy > -b.r * 0.35f || hitSnowmanHatZone) {
                            TriggerSnowmanCollapse(static_cast<int>(i), static_cast<int>(j));
                            break;
                        }
                    }

                    float relSpeed = std::hypot(a.vx - b.vx, a.vy - b.vy);
                    if (relSpeed > 480.0f || std::abs(a.vx - b.vx) > 290.0f) {
                        SplatSnowball(a);
                        SplatSnowball(b);
                        dead[i] = dead[j] = true;
                        break;
                    }

                    if (!aIsSnowman && !bIsSnowman && dy > 0.0f) {
                        float angleFromY = std::atan2(std::abs(dx), dy);
                        if (angleFromY <= STACK_MAX_ANGLE_RAD) {
                            a.stackedOnId = b.id;
                            a.stackLevel = 1;
                            a.aligning = true;
                            a.alignAngle = std::atan2(dx, dy);
                            a.vx = 0.0f;
                            a.vy = 0.0f;
                            a.resting = true;
                            a.restingHwnd = b.restingHwnd;
                            a.fellFromWindow = b.fellFromWindow;
                            a.snowmanReveal = 0.0f;
                            b.vx *= 0.15f;
                            break;
                        }
                    }

                    if (dist > 0.001f && dist < minDist) {
                        float overlap = (minDist - dist) * 0.5f;
                        float nx = dx / dist, ny = (a.y - b.y) / dist;
                        a.x += nx * overlap; a.y += ny * overlap;
                        if (!bIsSnowman) {
                            b.x -= nx * overlap; b.y -= ny * overlap;
                        }
                    }
                }
            }
        }

        if (std::any_of(dead.begin(), dead.end(), [](bool d) { return d; })) {
            std::vector<Snowball> kept;
            kept.reserve(snowballs_.size());
            for (size_t i = 0; i < snowballs_.size(); ++i) {
                if (!dead[i]) kept.push_back(snowballs_[i]);
            }
            snowballs_.swap(kept);
            if (FindBallIndexById(draggedBallId_) < 0) draggedBallId_ = -1;
        }
    }

    void UpdateSanta(float dt) {
        if (!santa_.active) {
            if (!snowfallActive_) return;
            santaCooldown_ -= dt;
            if (santaCooldown_ <= 0.0f) {
                SummonSanta();
                santaCooldown_ = RandRange(65.0f, 125.0f);
            }
            return;
        }
        santa_.phase += dt * 3.5f;
        santa_.x += santa_.vx * dt;
        santa_.y = santa_.baseY + std::sin(santa_.phase) * 12.0f;

        if (RandRange(0.0f, 1.0f) < 0.5f) {
            sparkles_.push_back({
                santa_.x - 10.0f + RandRange(-6.0f, 6.0f),
                santa_.y + RandRange(-5.0f, 8.0f),
                RandRange(-30.0f, -10.0f),
                RandRange(-10.0f, 15.0f),
                RandRange(0.4f, 0.9f), 0.9f,
                RandInt(0, 1) == 0
            });
        }
        if (santa_.x > width_ + 260) santa_.active = false;
    }

    void UpdateSparkles(float dt) {
        std::vector<Sparkle> alive;
        for (auto sp : sparkles_) {
            sp.life -= dt;
            if (sp.life > 0.0f) {
                sp.x += sp.vx * dt;
                sp.y += sp.vy * dt;
                alive.push_back(sp);
            }
        }
        sparkles_.swap(alive);
    }

    // --- Rendering ---
    void DrawPenguin(Graphics& g, const Penguin& pg) {
        GraphicsState st = g.Save();
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.TranslateTransform(pg.x, pg.y + pg.hopY);
        g.ScaleTransform(pg.dir * pg.scale, pg.scale);

        float waddleSin = std::sin(pg.waddlePhase);
        float bounce = std::abs(waddleSin) * 1.8f;
        float tiltDeg = waddleSin * 7.0f;

        g.TranslateTransform(0.0f, -bounce);
        g.RotateTransform(tiltDeg);

        SolidBrush orange(Color(255, 245, 146, 26));
        SolidBrush darkFeather(Color(255, 28, 32, 42));
        SolidBrush flipperColor(Color(255, 20, 24, 33));
        SolidBrush bellyWhite(Color(255, 248, 251, 255));
        SolidBrush pupil(Color(255, 15, 18, 24));

        float step = waddleSin * 3.0f;
        g.FillEllipse(&orange, -5.5f - step, -3.2f, 6.8f, 3.2f);
        g.FillEllipse(&orange, -0.5f + step, -3.2f, 6.8f, 3.2f);

        g.FillEllipse(&darkFeather, -8.2f, -21.5f, 16.4f, 19.8f);
        g.FillEllipse(&bellyWhite, -3.0f, -16.8f, 10.2f, 14.0f);

        g.FillEllipse(&darkFeather, -6.5f, -29.2f, 13.0f, 11.8f);
        g.FillEllipse(&bellyWhite, 0.2f, -25.2f, 5.2f, 5.2f);

        g.FillEllipse(&bellyWhite, 1.4f, -26.2f, 3.6f, 3.6f);
        g.FillEllipse(&pupil, 2.6f, -25.4f, 2.0f, 2.0f);

        PointF beak[3] = { {5.4f, -24.2f}, {11.6f, -22.2f}, {5.4f, -20.5f} };
        g.FillPolygon(&orange, beak, 3);

        GraphicsState wingSt = g.Save();
        g.TranslateTransform(-1.5f, -16.0f);
        g.RotateTransform(14.0f + waddleSin * 12.0f);
        g.FillEllipse(&flipperColor, -3.0f, 0.0f, 5.2f, 11.0f);
        g.Restore(wingSt);

        g.Restore(st);
    }

    void DrawRedPlow(Graphics& g, const SnowPlow& pl) {
        GraphicsState st = g.Save();
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.TranslateTransform(pl.x, pl.y);
        if (pl.dir == -1) g.ScaleTransform(-1.0f, 1.0f);

        GraphicsPath beam;
        PointF bPts[4] = { {16, -13}, {86, -28}, {86, 2}, {16, -9} };
        beam.AddPolygon(bPts, 4);
        LinearGradientBrush beamBr(PointF(16, -12), PointF(86, -12), Color(140, 255, 240, 175), Color(0, 255, 240, 175));
        g.FillPath(&beamBr, &beam);

        if (pl.moundMass > 1.5f) {
            float mh = pl.moundMass;
            float mw = 12.0f + mh * 1.15f;
            GraphicsPath mound;
            mound.AddBezier(22.0f, 0.0f, 22.0f + mw * 0.35f, -mh * 1.35f, 22.0f + mw * 0.75f, -mh * 1.1f, 22.0f + mw, 0.0f);
            mound.CloseFigure();
            SolidBrush mBr(Color(246, 246, 251, 255));
            g.FillPath(&mBr, &mound);
        }

        SolidBrush tire(Color(255, 32, 34, 40)), hub(Color(255, 195, 200, 210));
        g.FillEllipse(&tire, -18.5f, -10.5f, 11.0f, 11.0f);
        g.FillEllipse(&tire, 2.5f, -10.5f, 11.0f, 11.0f);
        g.FillEllipse(&hub, -15.2f, -7.2f, 4.4f, 4.4f);
        g.FillEllipse(&hub, 5.8f, -7.2f, 4.4f, 4.4f);

        SolidBrush red(Color(255, 215, 38, 42));
        g.FillRectangle(&red, -22.0f, -16.0f, 38.0f, 10.0f);
        g.FillRectangle(&red, -10.0f, -26.0f, 18.0f, 12.0f);

        bool on = (static_cast<int>(elapsedSec_ * 6.0f) % 2) == 0;
        SolidBrush beacon(on ? Color(255, 255, 215, 50) : Color(255, 195, 130, 20));
        g.FillRectangle(&beacon, -3.0f, -29.5f, 6.0f, 3.5f);

        SolidBrush winBr(Color(255, 165, 222, 250)), hlBr(Color(255, 255, 248, 190));
        g.FillRectangle(&winBr, -2.0f, -23.5f, 8.5f, 6.5f);
        g.FillRectangle(&hlBr, 14.0f, -14.0f, 3.0f, 3.5f);

        Pen armPen(Color(255, 75, 80, 90), 2.2f);
        g.DrawLine(&armPen, 15.0f, -8.0f, 22.0f, -5.0f);
        PointF bladePts[4] = { {20, -17}, {26, -8}, {23, 0}, {18, 0} };
        SolidBrush bladeBr(Color(255, 225, 175, 35));
        g.FillPolygon(&bladeBr, bladePts, 4);

        g.Restore(st);
    }

    void DrawSanta(Graphics& g) {
        GraphicsState st = g.Save();
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.TranslateTransform(santa_.x, santa_.y);

        Pen reins(Color(190, 235, 200, 120), 1.2f);
        g.DrawLine(&reins, 26.0f, -6.0f, 142.0f, -10.0f);

        SolidBrush deerBr(Color(255, 150, 95, 55));
        SolidBrush rudolph(Color(255, 255, 50, 50));
        for (int i = 0; i < 4; ++i) {
            float rx = 52.0f + i * 28.0f;
            float bob = std::sin(santa_.phase * 2.0f + i * 0.9f) * 3.0f;
            g.FillRectangle(&deerBr, rx, -10.0f + bob, 14.0f, 7.0f);
            g.FillEllipse(&deerBr, rx + 11.5f, -16.0f + bob, 7.0f, 6.0f);
            if (i == 3) g.FillEllipse(&rudolph, rx + 17.0f, -15.0f + bob, 4.4f, 4.4f);
        }

        Pen runner(Color(255, 245, 205, 85), 2.0f);
        g.DrawLine(&runner, -24.0f, 2.0f, 26.0f, 2.0f);
        g.DrawLine(&runner, 26.0f, 2.0f, 31.0f, -4.0f);

        SolidBrush sleighBr(Color(255, 205, 28, 38));
        g.FillRectangle(&sleighBr, -22.0f, -13.0f, 44.0f, 13.0f);

        SolidBrush skin(Color(255, 255, 224, 196)), beard(Color(255, 250, 250, 255));
        g.FillEllipse(&skin, -2.5f, -23.5f, 9.0f, 9.0f);
        g.FillEllipse(&beard, 0.5f, -18.5f, 7.0f, 5.0f);
        g.FillEllipse(&sleighBr, -5.0f, -26.0f, 10.0f, 6.0f);

        g.Restore(st);
    }

    void DrawSnowmanStickArms(Graphics& g, const Snowball& body, BYTE alpha) {
        Pen mainStick(Color(alpha, 104, 62, 34), 2.6f);
        mainStick.SetStartCap(LineCapRound);
        mainStick.SetEndCap(LineCapRound);
        Pen twigPen(Color(alpha, 104, 62, 34), 1.7f);
        twigPen.SetEndCap(LineCapRound);

        float lx0 = body.x - body.r * 0.78f;
        float ly0 = body.y - body.r * 0.12f;
        float lx1 = body.x - body.r * 1.95f;
        float ly1 = body.y - body.r * 0.78f;
        g.DrawLine(&mainStick, lx0, ly0, lx1, ly1);
        float lmx = lx0 + (lx1 - lx0) * 0.72f;
        float lmy = ly0 + (ly1 - ly0) * 0.72f;
        g.DrawLine(&twigPen, lmx, lmy, lmx - body.r * 0.32f, lmy - body.r * 0.36f);
        g.DrawLine(&twigPen, lmx, lmy, lmx - body.r * 0.36f, lmy + body.r * 0.14f);

        float rx0 = body.x + body.r * 0.78f;
        float ry0 = body.y - body.r * 0.12f;
        float rx1 = body.x + body.r * 1.95f;
        float ry1 = body.y - body.r * 0.78f;
        g.DrawLine(&mainStick, rx0, ry0, rx1, ry1);
        float rmx = rx0 + (rx1 - rx0) * 0.72f;
        float rmy = ry0 + (ry1 - ry0) * 0.72f;
        g.DrawLine(&twigPen, rmx, rmy, rmx + body.r * 0.32f, rmy - body.r * 0.36f);
        g.DrawLine(&twigPen, rmx, rmy, rmx + body.r * 0.36f, rmy + body.r * 0.14f);
    }

    void DrawSnowmanAccessories(Graphics& g, const Snowball& head, const Snowball& body, BYTE alpha) {
        float neckY = head.y + head.r - 1.5f;
        float neckW = std::min(head.r, body.r) * 1.85f;
        SolidBrush scarfRed(Color(alpha, 215, 28, 38));
        SolidBrush scarfStripe(Color(alpha, 245, 215, 110));
        Pen scarfOutline(Color(alpha, 155, 18, 26), 1.0f);

        PointF tail[4] = {
            { head.x + neckW * 0.18f, neckY + 1.0f },
            { head.x + neckW * 0.44f, neckY + 1.0f },
            { head.x + neckW * 0.56f, neckY + body.r * 0.92f },
            { head.x + neckW * 0.24f, neckY + body.r * 0.86f }
        };
        g.FillPolygon(&scarfRed, tail, 4);
        g.DrawPolygon(&scarfOutline, tail, 4);

        RectF wrapRect(head.x - neckW * 0.5f, neckY - 3.0f, neckW, 6.2f);
        g.FillRectangle(&scarfRed, wrapRect);
        g.FillRectangle(&scarfStripe, head.x - neckW * 0.5f + 2.0f, neckY - 0.5f, neckW - 4.0f, 1.4f);
        g.DrawRectangle(&scarfOutline, wrapRect);

        SolidBrush coal(Color(alpha, 26, 28, 34));
        float eyeR = std::max(1.9f, head.r * 0.14f);
        g.FillEllipse(&coal, head.x - head.r * 0.32f - eyeR, head.y - head.r * 0.26f - eyeR, eyeR * 2.0f, eyeR * 2.0f);
        g.FillEllipse(&coal, head.x + head.r * 0.32f - eyeR, head.y - head.r * 0.26f - eyeR, eyeR * 2.0f, eyeR * 2.0f);

        float dotR = std::max(1.2f, head.r * 0.085f);
        for (int k = 0; k < 5; ++k) {
            float t = static_cast<float>(k) / 4.0f;
            float ang = (28.0f + t * 124.0f) * PI_F / 180.0f;
            float sx = head.x - std::cos(ang) * (head.r * 0.46f);
            float sy = head.y + head.r * 0.10f + std::sin(ang) * (head.r * 0.42f);
            g.FillEllipse(&coal, sx - dotR, sy - dotR, dotR * 2.0f, dotR * 2.0f);
        }

        SolidBrush carrot(Color(alpha, 248, 122, 24));
        Pen carrotRim(Color(alpha, 195, 78, 12), 0.9f);
        PointF nose[3] = {
            { head.x, head.y - 2.0f },
            { head.x + head.r * 0.95f, head.y + 1.5f },
            { head.x, head.y + 3.2f }
        };
        g.FillPolygon(&carrot, nose, 3);
        g.DrawPolygon(&carrotRim, nose, 3);

        float brimW = head.r * 1.75f;
        float brimH = 4.0f;
        float crownW = head.r * 1.08f;
        float crownH = std::max(10.0f, head.r * 0.85f);
        float brimY = head.y - head.r - 2.2f;

        SolidBrush hatBlack(Color(alpha, 28, 30, 38));
        g.FillRectangle(&hatBlack, head.x - crownW * 0.5f, brimY - crownH, crownW, crownH + 1.0f);
        g.FillRectangle(&scarfRed, head.x - crownW * 0.5f, brimY - 3.2f, crownW, 3.0f);
        g.FillRectangle(&hatBlack, head.x - brimW * 0.5f, brimY, brimW, brimH);
    }

    void RenderFrame() {
        if (!dibBits_ || !memDC_) return;
        ZeroMemory(dibBits_, static_cast<size_t>(width_) * height_ * 4);

        Graphics g(memDC_);
        g.SetCompositingMode(CompositingModeSourceOver);

        // 1. Window & taskbar snowbanks (curved to hug Windows 11 rounded corners)
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        SolidBrush snowBr(Color(244, 246, 251, 255));
        for (auto it = zOrder_.rbegin(); it != zOrder_.rend(); ++it) {
            auto sit = surfaces_.find(*it);
            if (sit == surfaces_.end()) continue;
            const auto& s = sit->second;
            float yBase = s.rect.Top();
            float x0 = s.rect.Left();
            size_t n = s.heights.size();
            size_t i = 0;
            while (i < n) {
                if (s.heights[i] > 0.35f) {
                    size_t iStart = i;
                    polyBuf_.clear();
                    float startLX = static_cast<float>(iStart * BUCKET_W);
                    polyBuf_.push_back({ x0 + startLX, yBase + CornerDropAt(s, startLX) });

                    while (i < n && s.heights[i] > 0.35f) {
                        float midLX = (static_cast<float>(i) + 0.5f) * BUCKET_W;
                        polyBuf_.push_back({ x0 + midLX, yBase + CornerDropAt(s, midLX) - s.heights[i] });
                        ++i;
                    }

                    size_t iEnd = i;
                    float endLX = std::min(s.rect.w, static_cast<float>(iEnd * BUCKET_W));
                    polyBuf_.push_back({ x0 + endLX, yBase + CornerDropAt(s, endLX) });

                    if (!s.isTaskbar) {
                        for (size_t k = iEnd; k > iStart; --k) {
                            float lx = (static_cast<float>(k - 1) + 0.5f) * BUCKET_W;
                            if (lx < cornerRadius_ + BUCKET_W || (s.rect.w - lx) < cornerRadius_ + BUCKET_W) {
                                polyBuf_.push_back({ x0 + lx, yBase + CornerDropAt(s, lx) });
                            }
                        }
                    }

                    g.FillPolygon(&snowBr, polyBuf_.data(), static_cast<INT>(polyBuf_.size()));
                } else {
                    ++i;
                }
            }
        }
        g.SetSmoothingMode(SmoothingModeNone);

        // 2. Strolling taskbar penguins
        for (const auto& pg : penguins_) DrawPenguin(g, pg);

        // 3. Red snowplows
        for (const auto& pl : plows_) DrawRedPlow(g, pl);

        // 4. Snowballs & complete 2-ball snowmen
        if (!snowballs_.empty()) {
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            Pen ballRim(Color(225, 180, 212, 238), 1.4f);
            SolidBrush ballFill(Color(250, 249, 253, 255));

            for (const auto& b : snowballs_) {
                if (b.collapsing || b.stackedOnId >= 0) continue;
                int childIdx = FindStackedChildIndex(b.id);
                if (childIdx >= 0 && snowballs_[childIdx].snowmanReveal > 0.02f) {
                    BYTE alpha = static_cast<BYTE>(255.0f * snowballs_[childIdx].snowmanReveal);
                    DrawSnowmanStickArms(g, b, alpha);
                }
            }

            for (int pass = 0; pass < 2; ++pass) {
                for (const auto& b : snowballs_) {
                    bool isHead = (b.stackedOnId >= 0);
                    if ((pass == 0 && isHead) || (pass == 1 && !isHead)) continue;
                    g.FillEllipse(&ballFill, b.x - b.r, b.y - b.r, b.r * 2.0f, b.r * 2.0f);
                    g.DrawEllipse(&ballRim, b.x - b.r, b.y - b.r, b.r * 2.0f, b.r * 2.0f);
                }
            }

            for (const auto& head : snowballs_) {
                if (head.collapsing || head.stackedOnId < 0 || head.snowmanReveal <= 0.02f) continue;
                int bodyIdx = FindBallIndexById(head.stackedOnId);
                if (bodyIdx < 0) continue;
                BYTE alpha = static_cast<BYTE>(255.0f * head.snowmanReveal);
                DrawSnowmanAccessories(g, head, snowballs_[bodyIdx], alpha);
            }

            g.SetSmoothingMode(SmoothingModeNone);
        }

        // 5. Snowdust particles
        for (const auto& d : dust_) {
            BYTE alpha = static_cast<BYTE>(235.0f * std::max(0.0f, d.life / d.maxLife));
            SolidBrush dbr(d.icyTint ? Color(alpha, 208, 232, 252) : Color(alpha, 252, 254, 255));
            g.FillEllipse(&dbr, d.x - d.r, d.y - d.r, d.r * 2.0f, d.r * 2.0f);
        }

        // 6. Clip snowfall & Santa out of actively focused windowed apps
        if (hasActiveWindowClip_) {
            RectF excl(activeWindowClip_.x, activeWindowClip_.y, activeWindowClip_.w, activeWindowClip_.h);
            g.SetClip(excl, CombineModeExclude);
        }

        for (const auto& sp : sparkles_) {
            BYTE alpha = static_cast<BYTE>(255.0f * std::max(0.0f, sp.life / sp.maxLife));
            SolidBrush sbr(sp.gold ? Color(alpha, 255, 245, 175) : Color(alpha, 255, 255, 255));
            g.FillRectangle(&sbr, sp.x - 1.5f, sp.y - 1.5f, 3.0f, 3.0f);
        }
        if (santa_.active) DrawSanta(g);

        SolidBrush flakeBr(Color(225, 255, 255, 255));
        bool stretchWind = (intensity_ == BlizzardIntensity::Severe);
        for (const auto& f : flakes_) {
            if (stretchWind && f.ambient) {
                g.FillEllipse(&flakeBr, f.x - f.r * 1.35f, f.y - f.r * 0.85f, f.r * 2.7f, f.r * 1.7f);
            } else {
                g.FillEllipse(&flakeBr, f.x - f.r, f.y - f.r, f.r * 2.0f, f.r * 2.0f);
            }
        }
        g.ResetClip();

        // Present layered frame
        HDC screenDC = GetDC(nullptr);
        POINT ptSrc{ 0, 0 };
        POINT ptDst{ workArea_.left, workArea_.top };
        SIZE szWnd{ width_, height_ };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(hwnd_, screenDC, &ptDst, &szWnd, memDC_, &ptSrc, 0, &bf, ULW_ALPHA);
        ReleaseDC(nullptr, screenDC);
    }

    // --- Mouse Interaction ---
    void OnLButtonDown(int x, int y) {
        float mx = static_cast<float>(x), my = static_cast<float>(y);
        lastMouse_ = { x, y };
        mouseVx_ = mouseVy_ = 0.0f;

        for (int i = static_cast<int>(snowballs_.size()) - 1; i >= 0; --i) {
            auto& b = snowballs_[i];
            if (b.collapsing) continue;
            if (std::hypot(mx - b.x, my - b.y) <= b.r + 7.0f) {
                b.grabbed = true;
                b.resting = false;
                b.restingHwnd = nullptr;
                b.fellFromWindow = false;
                b.stackLevel = 0;
                b.stackedOnId = -1;
                b.aligning = false;
                b.snowmanReveal = 0.0f;
                int childIdx = FindStackedChildIndex(b.id);
                if (childIdx >= 0) {
                    auto& c = snowballs_[childIdx];
                    c.stackedOnId = -1;
                    c.stackLevel = 0;
                    c.aligning = false;
                    c.snowmanReveal = 0.0f;
                    c.resting = false;
                    c.restingHwnd = nullptr;
                    c.fellFromWindow = false;
                }
                draggedBallId_ = b.id;
                SetCapture(hwnd_);
                return;
            }
        }

        for (auto& [_, s] : surfaces_) {
            if (mx >= s.rect.Left() && mx <= s.rect.Right() &&
                my >= s.rect.Top() - MAX_SNOW_H - 8.0f && my <= s.rect.Top() + 10.0f) {
                int idx = static_cast<int>((mx - s.rect.Left()) / BUCKET_W);
                if (idx >= 0 && idx < static_cast<int>(s.heights.size()) && s.heights[idx] >= 4.0f) {
                    float scooped = 0.0f;
                    for (int k = std::max(0, idx - 5); k <= std::min(static_cast<int>(s.heights.size()) - 1, idx + 5); ++k) {
                        float take = std::min(s.heights[k], 12.0f);
                        s.heights[k] -= take;
                        scooped += take;
                    }
                    Snowball b;
                    b.id = nextBallId_++;
                    b.x = mx; b.y = my;
                    b.r = std::clamp(10.0f + scooped * 0.11f, 11.0f, 22.0f);
                    b.grabbed = true;
                    snowballs_.push_back(b);
                    draggedBallId_ = b.id;
                    SetCapture(hwnd_);
                    return;
                }
            }
        }

        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
            Snowball b;
            b.id = nextBallId_++;
            b.x = mx; b.y = my; b.r = 15.0f; b.grabbed = true;
            snowballs_.push_back(b);
            draggedBallId_ = b.id;
            SetCapture(hwnd_);
        }
    }

    void OnMouseMove(int x, int y) {
        int idx = FindBallIndexById(draggedBallId_);
        if (idx < 0) return;
        float dx = static_cast<float>(x - lastMouse_.x);
        float dy = static_cast<float>(y - lastMouse_.y);
        mouseVx_ = mouseVx_ * 0.45f + dx * 28.0f;
        mouseVy_ = mouseVy_ * 0.45f + dy * 28.0f;
        snowballs_[idx].x = static_cast<float>(x);
        snowballs_[idx].y = static_cast<float>(y);
        lastMouse_ = { x, y };
    }

    void OnLButtonUp() {
        int idx = FindBallIndexById(draggedBallId_);
        if (idx >= 0) {
            auto& b = snowballs_[idx];
            b.grabbed = false;
            b.vx = std::clamp(mouseVx_, -1050.0f, 1050.0f);
            b.vy = std::clamp(mouseVy_, -1050.0f, 1050.0f);
        }
        draggedBallId_ = -1;
        ReleaseCapture();
    }

    void ShowTrayMenu() {
        bool autoStart = IsAutoStartEnabled();
        bool isPreset = (idleDelaySec_ == 0 || idleDelaySec_ == 30 || idleDelaySec_ == 60 ||
                         idleDelaySec_ == 120 || idleDelaySec_ == 300 || idleDelaySec_ == 600);

        HMENU idleSub = CreatePopupMenu();
        AppendMenuW(idleSub, MF_STRING | (idleDelaySec_ == 0 ? MF_CHECKED : 0), ID_IDLE_0, L"Immediate (0s)");
        AppendMenuW(idleSub, MF_STRING | (idleDelaySec_ == 30 ? MF_CHECKED : 0), ID_IDLE_30, L"30 Seconds");
        AppendMenuW(idleSub, MF_STRING | (idleDelaySec_ == 60 ? MF_CHECKED : 0), ID_IDLE_60, L"1 Minute");
        AppendMenuW(idleSub, MF_STRING | (idleDelaySec_ == 120 ? MF_CHECKED : 0), ID_IDLE_120, L"2 Minutes (Default)");
        AppendMenuW(idleSub, MF_STRING | (idleDelaySec_ == 300 ? MF_CHECKED : 0), ID_IDLE_300, L"5 Minutes");
        AppendMenuW(idleSub, MF_STRING | (idleDelaySec_ == 600 ? MF_CHECKED : 0), ID_IDLE_600, L"10 Minutes");
        AppendMenuW(idleSub, MF_SEPARATOR, 0, nullptr);
        std::wstring customLabel = isPreset
            ? L"Custom..."
            : (L"Custom (" + FormatIdleDuration(idleDelaySec_) + L")...");
        AppendMenuW(idleSub, MF_STRING | (!isPreset ? MF_CHECKED : 0), ID_IDLE_CUSTOM, customLabel.c_str());

        HMENU intensitySub = CreatePopupMenu();
        AppendMenuW(intensitySub, MF_STRING | (intensity_ == BlizzardIntensity::Light ? MF_CHECKED : 0),
                    ID_INTENSITY_LIGHT, L"Light (Default)");
        AppendMenuW(intensitySub, MF_STRING | (intensity_ == BlizzardIntensity::Moderate ? MF_CHECKED : 0),
                    ID_INTENSITY_MODERATE, L"Moderate");
        AppendMenuW(intensitySub, MF_STRING | (intensity_ == BlizzardIntensity::Severe ? MF_CHECKED : 0),
                    ID_INTENSITY_SEVERE, L"Severe");

        HMENU menu = CreatePopupMenu();
        std::wstring idleTitle = L"Idle Timer (" + FormatIdleDuration(idleDelaySec_) + L")";
        std::wstring intensityTitle = std::wstring(L"Blizzard Intensity (") + IntensityLabel(intensity_) + L")";
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(idleSub), idleTitle.c_str());
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(intensitySub), intensityTitle.c_str());
        AppendMenuW(menu, MF_STRING | (forceSnowfall_ ? MF_CHECKED : 0), ID_TRAY_FORCE_SNOW, L"Force Snowfall (Skip Idle)");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_TRAY_PLOW_WIN, L"Plow Top Window");
        AppendMenuW(menu, MF_STRING, ID_TRAY_PLOW_TB, L"Plow Taskbar Now");
        AppendMenuW(menu, MF_STRING, ID_TRAY_PENGUINS, L"Summon Penguins");
        AppendMenuW(menu, MF_STRING, ID_TRAY_SANTA, L"Summon Santa Sleigh");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (ecoMode_ ? MF_CHECKED : 0), ID_TRAY_ECO, L"Eco Mode (20 FPS)");
        AppendMenuW(menu, MF_STRING | (autoStart ? MF_CHECKED : 0), ID_TRAY_AUTOSTART, L"Start with Windows");
        AppendMenuW(menu, MF_STRING, ID_TRAY_CLEAR, L"Clear All Snow");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quit");

        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);
        UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        DestroyMenu(menu); // Also destroys submenus

        switch (cmd) {
        case ID_IDLE_0: ApplyIdleDelay(0); break;
        case ID_IDLE_30: ApplyIdleDelay(30); break;
        case ID_IDLE_60: ApplyIdleDelay(60); break;
        case ID_IDLE_120: ApplyIdleDelay(120); break;
        case ID_IDLE_300: ApplyIdleDelay(300); break;
        case ID_IDLE_600: ApplyIdleDelay(600); break;
        case ID_IDLE_CUSTOM: ShowCustomIdleDialog(); break;
        case ID_INTENSITY_LIGHT: ApplyBlizzardIntensity(BlizzardIntensity::Light); break;
        case ID_INTENSITY_MODERATE: ApplyBlizzardIntensity(BlizzardIntensity::Moderate); break;
        case ID_INTENSITY_SEVERE: ApplyBlizzardIntensity(BlizzardIntensity::Severe); break;
        case ID_TRAY_FORCE_SNOW:
            forceSnowfall_ = !forceSnowfall_;
            RefreshSnowfallActive(GetGlobalIdleSeconds());
            break;
        case ID_TRAY_PLOW_WIN: SummonTopWindowPlow(); break;
        case ID_TRAY_PLOW_TB: SummonTaskbarPlow(); break;
        case ID_TRAY_PENGUINS: SummonPenguinGroup(); break;
        case ID_TRAY_SANTA: SummonSanta(); break;
        case ID_TRAY_ECO:
            ecoMode_ = !ecoMode_;
            if (!paused_) SetTimer(hwnd_, TIMER_TICK, TargetFrameMs(), nullptr);
            break;
        case ID_TRAY_AUTOSTART:
            SetAutoStart(!autoStart);
            break;
        case ID_TRAY_CLEAR: ClearAllSnow(); break;
        case ID_TRAY_QUIT: PostQuitMessage(0); break;
        }
    }

    // --- Window Procedure ---
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        WinterApp* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = reinterpret_cast<WinterApp*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<WinterApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self ? self->WndProc(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_TIMER:
            if (wp == TIMER_TICK) UpdateTick();
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_LBUTTONDOWN:
            OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSEMOVE:
            OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_LBUTTONUP:
            OnLButtonUp();
            return 0;
        case WM_TRAYICON:
            if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) ShowTrayMenu();
            return 0;
        case WM_DISPLAYCHANGE:
            RefreshMetrics();
            SetWindowPos(hwnd_, HWND_TOPMOST, workArea_.left, workArea_.top, width_, height_, SWP_NOACTIVATE);
            InitBuffers();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, TIMER_TICK);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    EnableDpiAwareness();

    GdiplusStartupInput gsi;
    ULONG_PTR gdiToken = 0;
    if (GdiplusStartup(&gdiToken, &gsi, nullptr) != Ok) return 1;

    int exitCode = 0;
    {
        WinterApp app;
        if (app.Init(hInst)) {
            exitCode = app.Run();
        }
    }

    GdiplusShutdown(gdiToken);
    return exitCode;
}