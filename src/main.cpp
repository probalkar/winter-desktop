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
constexpr int BUCKET_W = 8;
constexpr float WINDOW_IDLE_SEC = 4.0f;       // Idle time before a window gathers snow
constexpr float ACTIVE_CLIP_GRACE_SEC = 4.0f; // Seconds after input before snow crosses active window
constexpr float BLIZZARD_IDLE_SEC = 180.0f;   // 3 min unattended -> full-screen blizzard
constexpr float PLOW_TRIGGER_WIN = 14.0f;     // Avg snow depth (px) to summon window plow
constexpr float PLOW_TRIGGER_TB = 13.0f;      // Avg snow depth (px) to summon taskbar plow
constexpr float MAX_SNOW_H = 24.0f;
constexpr float GRAVITY = 900.0f;
constexpr float BLIZZARD_STRIP_H = 68.0f;
constexpr float PI_F = 3.14159265f;

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT_PTR TIMER_TICK = 1;

constexpr UINT ID_TRAY_PLOW_WIN = 1001;
constexpr UINT ID_TRAY_PLOW_TB = 1002;
constexpr UINT ID_TRAY_BLIZZARD = 1003;
constexpr UINT ID_TRAY_SANTA = 1004;
constexpr UINT ID_TRAY_ECO = 1005;
constexpr UINT ID_TRAY_CLEAR = 1006;
constexpr UINT ID_TRAY_QUIT = 1007;
constexpr UINT ID_TRAY_AUTOSTART = 1008;

constexpr const wchar_t* REG_RUN_PATH = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* REG_APP_NAME = L"WinterDesktop";

const HWND TASKBAR_KEY = reinterpret_cast<HWND>(static_cast<intptr_t>(-999));

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

// --- Windows Startup Registry Helpers ---
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
};

struct Flake {
    float x = 0, y = 0, vx = 0, vy = 0, r = 2.0f;
    bool ambient = true;
    bool canDeposit = true;
    float depositAmt = 2.0f;
    HWND ignoreHwnd = nullptr;
    float age = 0.0f;
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
    bool blizzardMode = false;
    float blizzardRowY = 0.0f;
};

struct Snowball {
    float x = 0, y = 0, vx = 0, vy = 0, r = 15.0f;
    bool grabbed = false;
    bool resting = false;
    int stackLevel = 0;
};

struct SantaSleigh {
    bool active = false;
    float x = 0, y = 0, baseY = 0, vx = 165.0f, phase = 0.0f;
};

// --- Main Application ---
class WinterApp {
public:
    bool Init(HINSTANCE hInst) {
        hInst_ = hInst;
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = StaticWndProc;
        wc.hInstance = hInst_;
        wc.lpszClassName = L"WinterDesktopOverlayClass";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&wc)) return false;

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
        SeedAmbientFlakes();

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
    NOTIFYICONDATAW nid_{};
    HICON trayIcon_ = nullptr;

    RECT workArea_{};
    int fullW_ = 1920, fullH_ = 1080;
    int width_ = 1920, height_ = 1040;
    bool hasBottomTaskbar_ = true;

    // Reusable 32-bit DIB for zero-allocation UpdateLayeredWindow
    HDC memDC_ = nullptr;
    HBITMAP dibBmp_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    void* dibBits_ = nullptr;

    // Quarter-scale offscreen blizzard blanket
    static constexpr int BLIZ_SCALE = 4;
    std::unique_ptr<Bitmap> blizBmp_;
    bool blizzardActive_ = false;
    bool blizzardClearing_ = false;
    float blizzardFillY_ = 0.0f;
    bool wasGloballyIdle_ = false;

    // State
    bool clickThrough_ = true;
    bool paused_ = false;
    bool ecoMode_ = false;
    bool hasActiveWindowClip_ = false;
    RectFSim activeWindowClip_{};

    std::unordered_map<HWND, WindowSurface> surfaces_;
    std::vector<HWND> zOrder_;
    std::vector<Flake> flakes_;
    std::vector<Sparkle> sparkles_;
    std::vector<SnowPlow> plows_;
    std::vector<Snowball> snowballs_;
    SantaSleigh santa_;
    float santaCooldown_ = 28.0f;

    int draggedIdx_ = -1;
    POINT lastMouse_{};
    float mouseVx_ = 0.0f, mouseVy_ = 0.0f;

    LARGE_INTEGER qpcFreq_{}, lastTick_{};
    float pollAccum_ = 0.4f;
    float hoverAccum_ = 0.0f;
    float elapsedSec_ = 0.0f;

    std::vector<PointF> polyBuf_;

    UINT TargetFrameMs() const { return ecoMode_ ? 50 : 33; } // 20 FPS eco / 30 FPS normal
    size_t TargetAmbientCount() const { return ecoMode_ ? 36 : 58; }

    void RefreshMetrics() {
        fullW_ = GetSystemMetrics(SM_CXSCREEN);
        fullH_ = GetSystemMetrics(SM_CYSCREEN);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea_, 0);
        width_ = std::max(1, static_cast<int>(workArea_.right - workArea_.left));
        height_ = std::max(1, static_cast<int>(workArea_.bottom - workArea_.top));
        hasBottomTaskbar_ = (workArea_.bottom < fullH_ - 2);
    }

    void InitBuffers() {
        DestroyBuffers();
        HDC screenDC = GetDC(nullptr);
        memDC_ = CreateCompatibleDC(screenDC);
        ReleaseDC(nullptr, screenDC);

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width_;
        bmi.bmiHeader.biHeight = -height_; // top-down DIB
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        dibBmp_ = CreateDIBSection(memDC_, &bmi, DIB_RGB_COLORS, &dibBits_, nullptr, 0);
        oldBmp_ = SelectObject(memDC_, dibBmp_);

        int bw = std::max(1, width_ / BLIZ_SCALE);
        int bh = std::max(1, height_ / BLIZ_SCALE);
        blizBmp_ = std::make_unique<Bitmap>(bw, bh, PixelFormat32bppPARGB);
        ClearBlizzardBitmap();
    }

    void DestroyBuffers() {
        blizBmp_.reset();
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
            draggedIdx_ = -1;
            ReleaseCapture();
            SetClickThrough(true);
            ShowWindow(hwnd_, SW_HIDE);
            SetTimer(hwnd_, TIMER_TICK, 400, nullptr); // ~0% CPU sleep
        } else {
            QueryPerformanceCounter(&lastTick_);
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
            SetTimer(hwnd_, TIMER_TICK, TargetFrameMs(), nullptr);
        }
    }

    Flake MakeAmbientFlake(bool randomY) {
        Flake f;
        f.x = RandRange(0.0f, static_cast<float>(width_));
        f.y = randomY ? RandRange(0.0f, static_cast<float>(height_)) : RandRange(-20.0f, 0.0f);
        f.vx = RandRange(-18.0f, 18.0f);
        f.vy = RandRange(45.0f, 95.0f);
        f.r = RandRange(1.7f, 3.6f);
        f.ambient = true;
        f.canDeposit = true;
        f.depositAmt = 2.0f;
        return f;
    }

    void SeedAmbientFlakes() {
        flakes_.clear();
        size_t n = TargetAmbientCount();
        flakes_.reserve(n + 120);
        for (size_t i = 0; i < n; ++i) flakes_.push_back(MakeAmbientFlake(true));
    }

    void ClearBlizzardBitmap() {
        if (!blizBmp_) return;
        Graphics g(blizBmp_.get());
        g.Clear(Color(0, 0, 0, 0));
    }

    void PaintFullBlizzard() {
        if (!blizBmp_) return;
        Graphics g(blizBmp_.get());
        g.Clear(Color(238, 242, 248, 255));
    }

    void CarveBlizzardTunnel(float xCenter, float yBottom, float w) {
        if (!blizBmp_) return;
        Graphics g(blizBmp_.get());
        g.SetCompositingMode(CompositingModeSourceCopy);
        SolidBrush erase(Color(0, 0, 0, 0));
        g.FillRectangle(
            &erase,
            (xCenter - w * 0.5f) / BLIZ_SCALE,
            (yBottom - BLIZZARD_STRIP_H) / BLIZ_SCALE,
            w / BLIZ_SCALE,
            (BLIZZARD_STRIP_H + 4.0f) / BLIZ_SCALE
        );
    }

    void StartBlizzardClear() {
        if (blizzardClearing_) return;
        blizzardClearing_ = true;
        SnowPlow p;
        p.x = -40.0f;
        p.y = static_cast<float>(height_);
        p.dir = 1;
        p.speed = 540.0f;
        p.blizzardMode = true;
        p.blizzardRowY = static_cast<float>(height_);
        plows_.push_back(p);
    }

    // --- Window Enumeration & Topmost Z-Order Precedence ---
    struct EnumCtx {
        HWND ownHwnd;
        RECT workArea;
        std::vector<std::pair<HWND, RectFSim>> list;
    };

    static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
        auto* ctx = reinterpret_cast<EnumCtx*>(lParam);
        if (hwnd == ctx->ownHwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
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
        if (!fg || fg == hwnd_ || IsIconic(fg) || IsIgnoredClass(fg)) return;

        RECT r = GetWindowBounds(fg);
        // True borderless fullscreen (F11 / video / game)
        if (r.left <= 4 && r.top <= 4 && r.right >= fullW_ - 4 && r.bottom >= fullH_ - 4) {
            outShouldPause = true;
            return;
        }
        // Maximized window or >= 94% of work area -> pause completely
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

        // Windowed focused app: exclude snowfall through it while user is active
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
        for (size_t i = 0; i < surf.heights.size(); ++i) {
            float h = surf.heights[i];
            if (h > 1.5f) {
                hadSnow = true;
                Flake f;
                f.x = surf.rect.Left() + static_cast<float>(i * BUCKET_W) + RandRange(-2.0f, 2.0f);
                f.y = surf.rect.Top() - RandRange(0.0f, h);
                f.vx = std::clamp(dx * 3.0f, -130.0f, 130.0f) + RandRange(-30.0f, 30.0f);
                f.vy = std::clamp(dy * 2.0f, -60.0f, 90.0f) + RandRange(-20.0f, 25.0f);
                f.r = RandRange(2.4f, 4.4f);
                f.ambient = false;
                f.canDeposit = true;
                f.depositAmt = std::min(5.0f, h * 0.65f);
                f.ignoreHwnd = surf.hwnd;
                flakes_.push_back(f);
            }
        }
        if (hadSnow) std::fill(surf.heights.begin(), surf.heights.end(), 0.0f);
    }

    void PollWindows(float dtPoll) {
        float globalIdle = GetGlobalIdleSeconds();
        bool shouldPause = false;
        CheckFullscreenAndActiveClip(globalIdle, shouldPause);
        if (shouldPause && !blizzardClearing_) {
            SetPaused(true);
            return;
        }
        SetPaused(false);

        // Full-screen unattended blizzard trigger & wake-clear
        if (globalIdle >= BLIZZARD_IDLE_SEC && !blizzardActive_) {
            blizzardActive_ = true;
            blizzardClearing_ = false;
            blizzardFillY_ = static_cast<float>(height_);
            wasGloballyIdle_ = true;
        } else if (wasGloballyIdle_ && globalIdle < 1.5f && blizzardActive_ && !blizzardClearing_) {
            StartBlizzardClear();
            wasGloballyIdle_ = false;
        }

        HWND fg = GetForegroundWindow();
        EnumCtx ctx{ hwnd_, workArea_, {} };
        EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));

        // 1. Taskbar is always active and independent.
        // 2. Strict Z-order precedence: skip windows whose top edge is occluded by a higher-Z window.
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

        // Closed, minimized, or newly-occluded windows spill snow downward
        for (auto it = surfaces_.begin(); it != surfaces_.end(); ) {
            if (activeSet.find(it->first) == activeSet.end()) {
                SpillWindowSnow(it->second, 0.0f, 15.0f);
                it = surfaces_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // --- Plow Spawning ---
    void SpawnPlowForSurface(const WindowSurface& surf) {
        for (const auto& p : plows_) {
            if (!p.blizzardMode && p.attachedHwnd == surf.hwnd) return;
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

    void ClearAllSnow() {
        for (auto& [_, s] : surfaces_) std::fill(s.heights.begin(), s.heights.end(), 0.0f);
        snowballs_.clear();
        plows_.clear();
        blizzardActive_ = false;
        blizzardClearing_ = false;
        ClearBlizzardBitmap();
    }

    // --- Dynamic Click-Through Hit Test ---
    void UpdateClickThrough() {
        if (draggedIdx_ >= 0) {
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

    // --- Physics & Simulation ---
    void SplatSnowball(const Snowball& b) {
        for (int i = 0; i < 12; ++i) {
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
        if (draggedIdx_ >= 0 || hoverAccum_ >= 0.08f) {
            UpdateClickThrough();
            hoverAccum_ = 0.0f;
        }

        UpdateBlizzardRise(dt);
        UpdateFlakes(dt);
        UpdatePlows(dt);
        UpdateSnowballs(dt);
        UpdateSanta(dt);
        UpdateSparkles(dt);
        RenderFrame();
    }

    void UpdateBlizzardRise(float dt) {
        if (blizzardActive_ && !blizzardClearing_ && blizzardFillY_ > 0.0f && blizBmp_) {
            float oldY = blizzardFillY_;
            blizzardFillY_ = std::max(0.0f, blizzardFillY_ - 150.0f * dt);
            Graphics g(blizBmp_.get());
            g.SetCompositingMode(CompositingModeSourceCopy);
            SolidBrush br(Color(238, 242, 248, 255));
            g.FillRectangle(
                &br, 0.0f, blizzardFillY_ / BLIZ_SCALE,
                static_cast<float>(blizBmp_->GetWidth()),
                (oldY - blizzardFillY_ + 4.0f) / BLIZ_SCALE
            );
        }
    }

    void UpdateFlakes(float dt) {
        std::vector<Flake> next;
        next.reserve(flakes_.size());

        for (auto& f : flakes_) {
            f.age += dt;
            if (f.ambient) {
                f.vx = std::clamp(f.vx + std::sin(elapsedSec_ * 1.6f + f.y * 0.02f) * 9.0f * dt, -30.0f, 30.0f);
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
                        int idx = static_cast<int>((f.x - s.rect.Left()) / BUCKET_W);
                        if (idx >= 0 && idx < static_cast<int>(s.heights.size())) {
                            float snowTop = s.rect.Top() - s.heights[idx];
                            if (f.y >= snowTop - 6.0f && f.y <= s.rect.Top() + 10.0f) {
                                const int offs[3] = { -1, 0, 1 };
                                const float wts[3] = { 0.45f, 1.0f, 0.45f };
                                for (int k = 0; k < 3; ++k) {
                                    int bi = idx + offs[k];
                                    if (bi >= 0 && bi < static_cast<int>(s.heights.size())) {
                                        s.heights[bi] = std::min(MAX_SNOW_H, s.heights[bi] + f.depositAmt * wts[k]);
                                    }
                                }
                                landed = true;
                                break;
                            }
                        }
                    }
                }
            }

            if (landed || f.y > height_ + 20 || f.x < -40 || f.x > width_ + 40) {
                if (f.ambient) next.push_back(MakeAmbientFlake(false));
            } else if (next.size() < 240) {
                next.push_back(f);
            }
        }
        flakes_.swap(next);

        if (blizzardActive_) return;

        // 1. Independent auto-plow for the TASKBAR
        bool hasTbPlow = false;
        bool hasWinPlow = false;
        for (const auto& p : plows_) {
            if (!p.blizzardMode) {
                if (p.isTaskbarPlow) hasTbPlow = true;
                else hasWinPlow = true;
            }
        }

        if (!hasTbPlow) {
            auto tbIt = surfaces_.find(TASKBAR_KEY);
            if (tbIt != surfaces_.end() && tbIt->second.AverageHeight() >= PLOW_TRIGGER_TB) {
                SpawnPlowForSurface(tbIt->second);
            }
        }

        // 2. Auto-plow for the topmost snow-eligible window
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

    void TossSnowmenNearPlow(const SnowPlow& pl) {
        for (auto& b : snowballs_) {
            if (b.grabbed) continue;
            float dx = b.x - pl.x;
            bool inBladeX = (pl.dir == 1) ? (dx > -18.0f && dx < 46.0f) : (dx < 18.0f && dx > -46.0f);
            bool inHeightY = (b.y + b.r >= pl.y - 82.0f && b.y - b.r <= pl.y + 8.0f);
            if (inBladeX && inHeightY) {
                b.vx = pl.dir * RandRange(240.0f, 440.0f);
                b.vy = -RandRange(280.0f, 480.0f);
                b.resting = false;
                b.stackLevel = 0;
            }
        }
    }

    void UpdatePlows(float dt) {
        std::vector<SnowPlow> alive;
        for (auto& pl : plows_) {
            if (pl.blizzardMode) {
                pl.x += pl.dir * pl.speed * dt;
                pl.y = pl.blizzardRowY;
                float bladeX = pl.x + pl.dir * 28.0f;
                CarveBlizzardTunnel(bladeX, pl.y, 52.0f);
                if (RandRange(0.0f, 1.0f) < 0.55f && flakes_.size() < 240) {
                    Flake f;
                    f.x = bladeX;
                    f.y = pl.y - RandRange(6.0f, 36.0f);
                    f.vx = pl.dir * RandRange(80.0f, 210.0f);
                    f.vy = RandRange(-160.0f, -40.0f);
                    f.r = RandRange(2.2f, 4.2f);
                    f.ambient = false;
                    f.canDeposit = false;
                    flakes_.push_back(f);
                }
                if ((pl.dir == 1 && pl.x > width_ + 35) || (pl.dir == -1 && pl.x < -35)) {
                    pl.blizzardRowY -= (BLIZZARD_STRIP_H - 6.0f);
                    pl.dir = -pl.dir;
                    if (pl.blizzardRowY < -BLIZZARD_STRIP_H) {
                        blizzardActive_ = false;
                        blizzardClearing_ = false;
                        ClearBlizzardBitmap();
                        continue;
                    }
                }
                alive.push_back(pl);
                continue;
            }

            // Window or independent Taskbar plow
            auto it = pl.attachedHwnd ? surfaces_.find(pl.attachedHwnd) : surfaces_.end();
            if (it != surfaces_.end()) {
                WindowSurface& s = it->second;
                pl.x += pl.dir * pl.speed * dt;
                pl.y = s.rect.Top();

                TossSnowmenNearPlow(pl);

                float bladeX = pl.x + pl.dir * 26.0f;
                int bIdx = static_cast<int>((bladeX - s.rect.Left()) / BUCKET_W);
                for (int k = bIdx - 1; k <= bIdx + 1; ++k) {
                    if (k >= 0 && k < static_cast<int>(s.heights.size()) && s.heights[k] > 0.3f) {
                        float cleared = s.heights[k];
                        s.heights[k] = 0.0f;
                        pl.moundMass = std::min(28.0f, pl.moundMass + cleared * 0.22f);
                        if (RandRange(0.0f, 1.0f) < 0.45f && flakes_.size() < 240) {
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
                    for (int c = 0; c < clumps && flakes_.size() < 240; ++c) {
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
        std::vector<Snowball> survivors;
        survivors.reserve(snowballs_.size());

        for (size_t i = 0; i < snowballs_.size(); ++i) {
            Snowball b = snowballs_[i];
            if (b.grabbed) {
                survivors.push_back(b);
                continue;
            }
            b.vy += GRAVITY * dt;
            b.x += b.vx * dt;
            b.y += b.vy * dt;
            b.resting = false;
            b.stackLevel = 0;
            bool splat = false;

            if (b.x - b.r < 0.0f || b.x + b.r > static_cast<float>(width_)) {
                if (std::abs(b.vx) > 350.0f) {
                    SplatSnowball(b);
                    splat = true;
                } else {
                    b.x = std::clamp(b.x, b.r, static_cast<float>(width_) - b.r);
                    b.vx *= -0.45f;
                }
            }

            if (!splat) {
                for (HWND h : zOrder_) {
                    auto it = surfaces_.find(h);
                    if (it == surfaces_.end()) continue;
                    const auto& s = it->second;
                    if (b.x >= s.rect.Left() && b.x <= s.rect.Right()) {
                        float sy = s.rect.Top();
                        if (b.vy >= 0.0f && b.y + b.r >= sy - 6.0f && b.y + b.r <= sy + 24.0f) {
                            if (std::hypot(b.vx, b.vy) > 540.0f) {
                                SplatSnowball(b);
                                splat = true;
                            } else {
                                b.y = sy - b.r;
                                b.vy = (std::abs(b.vy) > 90.0f) ? -b.vy * 0.22f : 0.0f;
                                b.resting = (b.vy == 0.0f);
                                b.vx *= 0.80f;
                            }
                            break;
                        }
                    }
                }
            }

            if (!splat && b.y + b.r > static_cast<float>(height_)) {
                b.y = static_cast<float>(height_) - b.r;
                b.vy = 0.0f;
                b.vx *= 0.80f;
                b.resting = true;
            }

            if (!splat) survivors.push_back(b);
        }
        snowballs_.swap(survivors);

        std::vector<bool> dead(snowballs_.size(), false);
        for (size_t i = 0; i < snowballs_.size(); ++i) {
            if (snowballs_[i].grabbed || dead[i]) continue;
            for (size_t j = 0; j < snowballs_.size(); ++j) {
                if (i == j || snowballs_[j].grabbed || dead[j]) continue;
                auto& a = snowballs_[i];
                auto& b = snowballs_[j];
                float dx = a.x - b.x;
                float dy = a.y - b.y;
                float dist = std::hypot(dx, dy);
                float minDist = a.r + b.r - 2.0f;
                if (dist > 0.001f && dist < minDist) {
                    if (std::hypot(a.vx - b.vx, a.vy - b.vy) > 350.0f) {
                        SplatSnowball(a);
                        SplatSnowball(b);
                        dead[i] = dead[j] = true;
                    } else if (a.y < b.y && std::abs(dx) < b.r * 0.75f && std::abs(a.vx) < 130.0f) {
                        a.y = b.y - minDist;
                        a.x += (b.x - a.x) * 0.16f;
                        a.vy = 0.0f;
                        a.vx *= 0.6f;
                        a.resting = true;
                        a.stackLevel = b.stackLevel + 1;
                    } else {
                        float overlap = (minDist - dist) * 0.5f;
                        float nx = dx / dist, ny = dy / dist;
                        a.x += nx * overlap; a.y += ny * overlap;
                        b.x -= nx * overlap; b.y -= ny * overlap;
                    }
                }
            }
        }
        if (std::any_of(dead.begin(), dead.end(), [](bool d) { return d; })) {
            std::vector<Snowball> kept;
            for (size_t i = 0; i < snowballs_.size(); ++i) {
                if (!dead[i]) kept.push_back(snowballs_[i]);
            }
            snowballs_.swap(kept);
            draggedIdx_ = -1;
        }
    }

    void UpdateSanta(float dt) {
        if (!santa_.active) {
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

    void RenderFrame() {
        if (!dibBits_ || !memDC_) return;
        ZeroMemory(dibBits_, static_cast<size_t>(width_) * height_ * 4);

        Graphics g(memDC_);
        g.SetCompositingMode(CompositingModeSourceOver);

        // 1. Full-screen idle blizzard blanket
        if (blizzardActive_ && blizBmp_) {
            g.SetInterpolationMode(InterpolationModeLowQuality);
            g.DrawImage(blizBmp_.get(), 0, 0, width_, height_);
        }

        // 2. Window & taskbar snowbanks
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
                if (s.heights[i] > 0.5f) {
                    polyBuf_.clear();
                    polyBuf_.push_back({ x0 + static_cast<float>(i * BUCKET_W), yBase });
                    while (i < n && s.heights[i] > 0.5f) {
                        polyBuf_.push_back({ x0 + (static_cast<float>(i) + 0.5f) * BUCKET_W, yBase - s.heights[i] });
                        ++i;
                    }
                    polyBuf_.push_back({ x0 + static_cast<float>(i * BUCKET_W), yBase });
                    g.FillPolygon(&snowBr, polyBuf_.data(), static_cast<INT>(polyBuf_.size()));
                } else {
                    ++i;
                }
            }
        }

        // 3. Red snowplows
        for (const auto& pl : plows_) DrawRedPlow(g, pl);

        // 4. Snowballs & stacked snowmen
        if (!snowballs_.empty()) {
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            Pen ballRim(Color(225, 180, 212, 238), 1.4f);
            SolidBrush ballFill(Color(250, 249, 253, 255));
            SolidBrush coal(Color(255, 32, 34, 42));
            SolidBrush carrot(Color(255, 245, 118, 28));
            for (const auto& b : snowballs_) {
                g.FillEllipse(&ballFill, b.x - b.r, b.y - b.r, b.r * 2.0f, b.r * 2.0f);
                g.DrawEllipse(&ballRim, b.x - b.r, b.y - b.r, b.r * 2.0f, b.r * 2.0f);
                if (b.stackLevel >= 1 && b.resting) {
                    g.FillEllipse(&coal, b.x - b.r * 0.3f - 2.2f, b.y - b.r * 0.2f - 2.2f, 4.4f, 4.4f);
                    g.FillEllipse(&coal, b.x + b.r * 0.3f - 2.2f, b.y - b.r * 0.2f - 2.2f, 4.4f, 4.4f);
                    PointF nose[3] = { {b.x, b.y - 1.0f}, {b.x + b.r * 0.7f, b.y + 1.5f}, {b.x, b.y + 3.5f} };
                    g.FillPolygon(&carrot, nose, 3);
                    if (b.stackLevel >= 2) {
                        g.FillRectangle(&coal, b.x - b.r * 0.75f, b.y - b.r - 2.0f, b.r * 1.5f, 3.5f);
                        g.FillRectangle(&coal, b.x - b.r * 0.45f, b.y - b.r - 13.0f, b.r * 0.9f, 11.0f);
                    }
                }
            }
            g.SetSmoothingMode(SmoothingModeNone);
        }

        // 5. Clip snowfall & Santa out of actively focused windowed apps
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
        for (const auto& f : flakes_) {
            g.FillEllipse(&flakeBr, f.x - f.r, f.y - f.r, f.r * 2.0f, f.r * 2.0f);
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
            if (std::hypot(mx - b.x, my - b.y) <= b.r + 7.0f) {
                b.grabbed = true;
                b.resting = false;
                b.stackLevel = 0;
                draggedIdx_ = i;
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
                    for (int k = std::max(0, idx - 3); k <= std::min(static_cast<int>(s.heights.size()) - 1, idx + 3); ++k) {
                        float take = std::min(s.heights[k], 12.0f);
                        s.heights[k] -= take;
                        scooped += take;
                    }
                    Snowball b;
                    b.x = mx; b.y = my;
                    b.r = std::clamp(10.0f + scooped * 0.16f, 11.0f, 22.0f);
                    b.grabbed = true;
                    snowballs_.push_back(b);
                    draggedIdx_ = static_cast<int>(snowballs_.size()) - 1;
                    SetCapture(hwnd_);
                    return;
                }
            }
        }

        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
            Snowball b;
            b.x = mx; b.y = my; b.r = 14.0f; b.grabbed = true;
            snowballs_.push_back(b);
            draggedIdx_ = static_cast<int>(snowballs_.size()) - 1;
            SetCapture(hwnd_);
        }
    }

    void OnMouseMove(int x, int y) {
        if (draggedIdx_ < 0 || draggedIdx_ >= static_cast<int>(snowballs_.size())) return;
        float dx = static_cast<float>(x - lastMouse_.x);
        float dy = static_cast<float>(y - lastMouse_.y);
        mouseVx_ = mouseVx_ * 0.45f + dx * 28.0f;
        mouseVy_ = mouseVy_ * 0.45f + dy * 28.0f;
        snowballs_[draggedIdx_].x = static_cast<float>(x);
        snowballs_[draggedIdx_].y = static_cast<float>(y);
        lastMouse_ = { x, y };
    }

    void OnLButtonUp() {
        if (draggedIdx_ >= 0 && draggedIdx_ < static_cast<int>(snowballs_.size())) {
            auto& b = snowballs_[draggedIdx_];
            b.grabbed = false;
            b.vx = std::clamp(mouseVx_, -1050.0f, 1050.0f);
            b.vy = std::clamp(mouseVy_, -1050.0f, 1050.0f);
        }
        draggedIdx_ = -1;
        ReleaseCapture();
    }

    void ShowTrayMenu() {
        bool autoStart = IsAutoStartEnabled();

        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, ID_TRAY_PLOW_WIN, L"Plow Top Window");
        AppendMenuW(menu, MF_STRING, ID_TRAY_PLOW_TB, L"Plow Taskbar Now");
        AppendMenuW(menu, MF_STRING, ID_TRAY_BLIZZARD, L"Test Blizzard + Clear");
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
        DestroyMenu(menu);

        switch (cmd) {
        case ID_TRAY_PLOW_WIN: SummonTopWindowPlow(); break;
        case ID_TRAY_PLOW_TB: SummonTaskbarPlow(); break;
        case ID_TRAY_BLIZZARD:
            SetPaused(false);
            blizzardActive_ = true;
            blizzardClearing_ = false;
            blizzardFillY_ = 0.0f;
            PaintFullBlizzard();
            StartBlizzardClear();
            break;
        case ID_TRAY_SANTA: SummonSanta(); break;
        case ID_TRAY_ECO:
            ecoMode_ = !ecoMode_;
            if (!paused_) SetTimer(hwnd_, TIMER_TICK, TargetFrameMs(), nullptr);
            SeedAmbientFlakes();
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
