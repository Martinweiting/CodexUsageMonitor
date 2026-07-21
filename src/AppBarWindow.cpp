#include "AppBarWindow.h"
#include "AppVersion.h"

#include <ShlObj.h>
#include <winreg.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kWindowClassName[] = L"CodexUsageMonitorWindow";
constexpr const wchar_t* kCurrentVersion = APP_VERSION_W;
// Keep the existing settings schema so saved panel coordinates migrate intact;
// new minimum geometry is clamped when it is loaded or displayed.
constexpr int kLayoutVersion = 10;
constexpr UINT kCommandRefresh = 1;
constexpr UINT kCommandExit = 2;
constexpr UINT kCommandResetPosition = 3;
constexpr UINT kCommandLaunchAtStartup = 4;
constexpr UINT kCommandAlwaysOnTop = 5;
constexpr UINT kCommandLockPosition = 6;
constexpr UINT kCommandSimpleMode = 7;
constexpr UINT kCommandLanguageEnglish = 8;
constexpr UINT kCommandLanguageChinese = 9;
constexpr UINT kCommandRefreshInterval1Minute = 10;
constexpr UINT kCommandRefreshInterval3Minutes = 11;
constexpr UINT kCommandRefreshInterval5Minutes = 12;
constexpr UINT kCommandRefreshInterval10Minutes = 13;
constexpr UINT kCommandRefreshInterval30Minutes = 14;
constexpr UINT kCommandCheckVersion = 15;
constexpr UINT kCommandFullMode = 16;
constexpr UINT kCommandTaskbarMode = 17;
constexpr UINT kCommandSettings = 18;
constexpr int kDefaultWidgetWidth = 440;
constexpr int kMinimumWidgetWidth = 420;
constexpr int kSimpleDefaultWidgetWidth = 240;
constexpr int kSimpleMinimumWidgetWidth = 220;
constexpr int kTaskbarDefaultWidgetWidth = codex_widget::kTaskbarWidgetLogicalWidth;
constexpr int kTaskbarMinimumWidgetWidth = codex_widget::kTaskbarWidgetLogicalWidth;
constexpr int kTaskbarWidgetHeight = codex_widget::kTaskbarWidgetLogicalHeight;
constexpr int kBubbleWidgetSize = 64;
constexpr int kPanelBubbleGap = 14;
constexpr int kBubbleHoverSize = 74;
constexpr UINT kHoverExitGuardMilliseconds = 90;
constexpr UINT kHoverPollMilliseconds = 40;
constexpr int kDesktopMargin = 18;
constexpr int kHorizontalPadding = 14;
constexpr int kVerticalPadding = 12;
constexpr int kResizeGrip = 12;
constexpr long long kDaySeconds = 24LL * 60 * 60;
constexpr long long kWeekSeconds = 7LL * kDaySeconds;
constexpr int kReleaseCheckIntervalSeconds = 6 * 60 * 60;

enum WINDOWCOMPOSITIONATTRIB {
    WCA_ACCENT_POLICY = 19,
};

enum ACCENT_STATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
};

struct ACCENT_POLICY {
    DWORD accentState;
    DWORD accentFlags;
    DWORD gradientColor;
    DWORD animationId;
};

struct WINDOWCOMPOSITIONATTRIBDATA {
    WINDOWCOMPOSITIONATTRIB attribute;
    PVOID data;
    SIZE_T dataSize;
};

using SetWindowCompositionAttributeFunction = BOOL(WINAPI*)(WINDOWCOMPOSITIONATTRIBDATA*);

int SanitizeRefreshIntervalSeconds(int seconds) {
    switch (seconds) {
        case 60:
        case 180:
        case 300:
        case 600:
        case 1800:
            return seconds;
        default:
            return 60;
    }
}

std::vector<int> ParseVersionParts(const std::wstring& version) {
    std::vector<int> parts;
    int value = 0;
    bool inNumber = false;

    for (wchar_t ch : version) {
        if (ch >= L'0' && ch <= L'9') {
            value = value * 10 + (ch - L'0');
            inNumber = true;
        } else if (inNumber) {
            parts.push_back(value);
            value = 0;
            inNumber = false;
        }
    }
    if (inNumber) {
        parts.push_back(value);
    }

    return parts;
}

int CompareVersions(const std::wstring& left, const std::wstring& right) {
    const std::vector<int> leftParts = ParseVersionParts(left);
    const std::vector<int> rightParts = ParseVersionParts(right);
    const size_t count = std::max(leftParts.size(), rightParts.size());

    for (size_t i = 0; i < count; ++i) {
        const int leftValue = i < leftParts.size() ? leftParts[i] : 0;
        const int rightValue = i < rightParts.size() ? rightParts[i] : 0;
        if (leftValue < rightValue) {
            return -1;
        }
        if (leftValue > rightValue) {
            return 1;
        }
    }

    return 0;
}

int ScaleForDpi(HWND hwnd, int value) {
    const UINT dpi = GetDpiForWindow(hwnd != nullptr ? hwnd : GetDesktopWindow());
    return MulDiv(value, static_cast<int>(dpi), 96);
}

int RectWidth(const RECT& rect) {
    return rect.right - rect.left;
}

int RectHeight(const RECT& rect) {
    return rect.bottom - rect.top;
}

int CalculateDetailedMinimumWidgetHeight(HWND hwnd, int width) {
    (void)width;
    // Spacious full-mode glass layout with summary, reset and detail cards.
    return ScaleForDpi(hwnd, 650);
}

int CalculateSimpleMinimumWidgetHeight(HWND hwnd) {
    return ScaleForDpi(hwnd, 132);
}

int CalculateTaskbarWidgetHeight(HWND hwnd) {
    return ScaleForDpi(hwnd, kTaskbarWidgetHeight);
}

RECT ShrinkRect(const RECT& rect, int dx, int dy) {
    RECT output = rect;
    output.left += dx;
    output.right -= dx;
    output.top += dy;
    output.bottom -= dy;
    return output;
}

struct PaceInfo {
    bool valid = false;
    double dailyBudgetPercent = 0.0;
    double expectedUsedPercent = 0.0;
    double actualUsedPercent = 0.0;
    double fiveHourExpectedUsedPercent = 0.0;
    double fiveHourActualUsedPercent = 0.0;
    double weeklyRemainingPercent = 0.0;
    double deltaPercent = 0.0;
    int cycleDay = 0;
    int elapsedSeconds = 0;
    int remainingSeconds = 0;
    long long weekStartUnixSeconds = 0;
    bool isOver = false;
};

int ClampInt(int value, int minValue, int maxValue) {
    return std::min(maxValue, std::max(minValue, value));
}

double ClampDouble(double value, double minValue, double maxValue) {
    return std::min(maxValue, std::max(minValue, value));
}

bool HasAvailableUsageWindow(const UsageSnapshot& snapshot) {
    return snapshot.fiveHour.available || snapshot.weekly.available;
}

int LowestAvailableRemainingPercent(const UsageSnapshot& snapshot) {
    int lowest = 100;
    bool found = false;
    if (snapshot.fiveHour.available) {
        lowest = std::min(lowest, ClampInt(snapshot.fiveHour.remainingPercent, 0, 100));
        found = true;
    }
    if (snapshot.weekly.available) {
        lowest = std::min(lowest, ClampInt(snapshot.weekly.remainingPercent, 0, 100));
        found = true;
    }
    return found ? lowest : 100;
}

bool IsAnyUsageWindowExhausted(const UsageSnapshot& snapshot) {
    return (snapshot.fiveHour.available && snapshot.fiveHour.remainingPercent <= 0)
        || (snapshot.weekly.available && snapshot.weekly.remainingPercent <= 0);
}

bool IsAnyUsageWindowTight(const UsageSnapshot& snapshot) {
    return (snapshot.fiveHour.available && snapshot.fiveHour.remainingPercent <= 15)
        || (snapshot.weekly.available && snapshot.weekly.remainingPercent <= 15);
}

RECT MakeRect(int left, int top, int right, int bottom) {
    RECT rect = { left, top, right, bottom };
    return rect;
}

codex_widget::WidgetRect ToWidgetRect(const RECT& rect) {
    return codex_widget::WidgetRect{ rect.left, rect.top, rect.right, rect.bottom };
}

RECT ToWin32Rect(const codex_widget::WidgetRect& rect) {
    return MakeRect(rect.left, rect.top, rect.right, rect.bottom);
}

D2D1_RECT_F ToRectF(const RECT& rect) {
    return D2D1::RectF(
        static_cast<float>(rect.left),
        static_cast<float>(rect.top),
        static_cast<float>(rect.right),
        static_cast<float>(rect.bottom));
}

D2D1_COLOR_F ToColorF(COLORREF color, float alpha = 1.0f) {
    return D2D1::ColorF(
        static_cast<float>(GetRValue(color)) / 255.0f,
        static_cast<float>(GetGValue(color)) / 255.0f,
        static_cast<float>(GetBValue(color)) / 255.0f,
        alpha);
}

void FillSolidRect(HDC hdc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);
}

void StrokeRect(HDC hdc, const RECT& rect, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

std::wstring FormatNumber(double value) {
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"%.1f", value);
    return buffer;
}

std::wstring FormatNumberNoUnit(double value) {
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"%.1f", value);
    return buffer;
}

PaceInfo BuildPaceInfo(const UsageSnapshot& snapshot) {
    PaceInfo info;
    if (!snapshot.success || !snapshot.weekly.available || snapshot.weekly.windowSeconds <= 0) {
        return info;
    }

    info.dailyBudgetPercent = 100.0 / 7.0;
    info.actualUsedPercent = static_cast<double>(snapshot.weekly.usedPercent);
    info.weeklyRemainingPercent = static_cast<double>(snapshot.weekly.remainingPercent);
    info.remainingSeconds = std::max(0, snapshot.weekly.resetAfterSeconds);
    info.elapsedSeconds = ClampInt(snapshot.weekly.windowSeconds - info.remainingSeconds, 0, snapshot.weekly.windowSeconds);
    info.weekStartUnixSeconds = snapshot.weekly.resetAtUnixSeconds - snapshot.weekly.windowSeconds;

    const int elapsedDays = info.elapsedSeconds <= 0 ? 0 : (info.elapsedSeconds / static_cast<int>(kDaySeconds));
    info.cycleDay = ClampInt(elapsedDays + 1, 1, 7);
    info.expectedUsedPercent = ClampDouble(info.cycleDay * info.dailyBudgetPercent, 0.0, 100.0);
    info.deltaPercent = info.actualUsedPercent - info.expectedUsedPercent;
    info.isOver = info.deltaPercent > 0.001;

    // 5-hour budget line: proportional to elapsed time in the primary window.
    if (snapshot.fiveHour.available) {
        info.fiveHourActualUsedPercent = static_cast<double>(snapshot.fiveHour.usedPercent);
    }
    if (snapshot.fiveHour.available && snapshot.fiveHour.windowSeconds > 0) {
        const int fiveElapsed = ClampInt(
            snapshot.fiveHour.windowSeconds - snapshot.fiveHour.resetAfterSeconds,
            0,
            snapshot.fiveHour.windowSeconds);
        info.fiveHourExpectedUsedPercent = ClampDouble(
            100.0 * static_cast<double>(fiveElapsed) / static_cast<double>(snapshot.fiveHour.windowSeconds),
            0.0,
            100.0);
    }

    info.valid = true;
    return info;
}

}  // namespace

AppBarWindow::AppBarWindow(HINSTANCE instance) : instance_(instance) {}

AppBarWindow::~AppBarWindow() {
    DiscardTextFormats();
    DiscardDeviceResources();
    DiscardLayeredSurface();
    UnregisterPrivateFonts();
}

bool AppBarWindow::Create() {
    RegisterWindowClass();
    RegisterPrivateFonts();
    LoadSettings();
    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        kWindowClassName,
        LocalizeText(L"Codex Usage Widget", L"Codex 用量小工具"),
        WS_POPUP | WS_VISIBLE,
        0,
        0,
        100,
        100,
        nullptr,
        nullptr,
        instance_,
        this);

    if (hwnd_ == nullptr) {
        return false;
    }

    if (FAILED(CreateDeviceIndependentResources())) {
        return false;
    }

    RefreshTheme();
    EnableNativeGlassBackdrop();
    UpdateWindowBounds(true);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    RenderLayeredSurface();

    refreshCountdownSeconds_ = refreshIntervalSeconds_;
    releaseCheckCountdownSeconds_ = kReleaseCheckIntervalSeconds;
    SetTimer(hwnd_, kCountdownTimerId, 1000, nullptr);
    SetTimer(hwnd_, kHoverPollTimerId, kHoverPollMilliseconds, nullptr);
    RestartRefreshTimer();
    RequestRefresh(true);
    RequestLatestReleaseCheck(true);
    return true;
}

int AppBarWindow::Run() {
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK AppBarWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<AppBarWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }

    auto* self = reinterpret_cast<AppBarWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
        return self->HandleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT AppBarWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_TIMER:
            if (wParam == kHoverExitTimerId || wParam == kHoverPollTimerId) {
                StopHoverExitGuard();
                UpdateHoverStateFromCursor();
            } else if (wParam == kCountdownTimerId) {
                if (snapshot_.success) {
                    if (snapshot_.fiveHour.available) {
                        snapshot_.fiveHour.resetAfterSeconds = std::max(0, snapshot_.fiveHour.resetAfterSeconds - 1);
                    }
                    if (snapshot_.weekly.available) {
                        snapshot_.weekly.resetAfterSeconds = std::max(0, snapshot_.weekly.resetAfterSeconds - 1);
                    }
                }
                refreshCountdownSeconds_ = std::max(0, refreshCountdownSeconds_ - 1);
                releaseCheckCountdownSeconds_ = std::max(0, releaseCheckCountdownSeconds_ - 1);
                if (releaseCheckCountdownSeconds_ == 0) {
                    RequestLatestReleaseCheck(false);
                }
                RenderLayeredSurface();
            } else if (wParam == kRefreshTimerId) {
                refreshCountdownSeconds_ = refreshIntervalSeconds_;
                RequestRefresh(false);
            }
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd_, &ps);
            EndPaint(hwnd_, &ps);
            RenderLayeredSurface();
            return 0;
        }

        case WM_THEMECHANGED:
            RefreshTheme();
            EnableNativeGlassBackdrop();
            RenderLayeredSurface();
            return 0;

        case WM_SETTINGCHANGE:
            RefreshTheme();
            EnableNativeGlassBackdrop();
            if (taskbarMode_) {
                UpdateWindowBounds(false);
            }
            RenderLayeredSurface();
            return 0;

        case WM_DISPLAYCHANGE:
            UpdateWindowBounds(true);
            RenderLayeredSurface();
            return 0;

        case WM_DPICHANGED:
            DiscardTextFormats();
            UpdateWindowBounds(true);
            RenderLayeredSurface();
            return 0;

        case WM_NCHITTEST: {
            return HTCLIENT;
        }

        case WM_SETCURSOR: {
            POINT screenPoint = {};
            GetCursorPos(&screenPoint);
            POINT clientPoint = screenPoint;
            ScreenToClient(hwnd_, &clientPoint);
            switch (HitTestDragMode(clientPoint)) {
                case DragMode::ResizeRight:
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                case DragMode::ResizeBottom:
                    SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                    return TRUE;
                case DragMode::ResizeCorner:
                    SetCursor(LoadCursorW(nullptr, IDC_SIZENWSE));
                    return TRUE;
                case DragMode::Move:
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                    return TRUE;
                case DragMode::None:
                    break;
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            StopHoverExitGuard();
            POINT screenPoint = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ClientToScreen(hwnd_, &screenPoint);
            POINT clientPoint = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (TryHandleControlClick(clientPoint)) {
                return 0;
            }

            const bool insideBubbleButton = !settingsOpen_
                && bubbleButtonRect_.right > bubbleButtonRect_.left
                && PtInRect(&bubbleButtonRect_, clientPoint);
            if (insideBubbleButton) {
                if (!lockPosition_) {
                    bubbleClickPending_ = true;
                    dragMoved_ = false;
                    BeginDrag(DragMode::Move, screenPoint);
                } else {
                    ActivateBubbleClick();
                }
                return 0;
            }

            const DragMode mode = HitTestDragMode(clientPoint);
            if (!taskbarMode_
                && (presentationState_ == codex_widget::PresentationState::Bubble
                || presentationState_ == codex_widget::PresentationState::HoverExpanded)) {
                SetPresentationState(codex_widget::PresentationState::PinnedExpanded);
            }
            BeginDrag(mode, screenPoint);
            return 0;
        }

        case WM_MOUSEMOVE:
            if (settingsDragging_) {
                const POINT clientPoint = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                UpdateTransparencyFromPoint(clientPoint);
                return 0;
            }
            if (dragMode_ != DragMode::None) {
                POINT screenPoint = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ClientToScreen(hwnd_, &screenPoint);
                if (bubbleClickPending_ && !dragMoved_) {
                    const int dragThreshold = ScaleForDpi(hwnd_, 4);
                    if (!codex_widget::IsDragGesture(
                            screenPoint.x - dragStartPoint_.x,
                            screenPoint.y - dragStartPoint_.y,
                            dragThreshold)) {
                        return 0;
                    }
                    dragMoved_ = true;
                }
                UpdateDrag(screenPoint);
                return 0;
            }
            ArmMouseLeaveTracking();
            UpdateHoverStateFromCursor();
            return 0;

        case WM_MOUSELEAVE:
            mouseTracking_ = false;
            UpdateHoverStateFromCursor();
            return 0;

        case WM_RBUTTONUP:
        case WM_NCRBUTTONUP: {
            POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (message == WM_RBUTTONUP) {
                ClientToScreen(hwnd_, &point);
            }
            SetForegroundWindow(hwnd_);
            ShowContextMenu(point);
            return 0;
        }

        case WM_LBUTTONUP:
            if (settingsDragging_) {
                settingsDragging_ = false;
                ReleaseCapture();
                return 0;
            }
            if (dragMode_ != DragMode::None) {
                const bool activateBubble = bubbleClickPending_ && !dragMoved_;
                EndDrag(!bubbleClickPending_ || dragMoved_);
                bubbleClickPending_ = false;
                dragMoved_ = false;
                if (activateBubble) {
                    ActivateBubbleClick();
                }
            }
            return 0;

        case WM_CAPTURECHANGED:
            if (settingsDragging_) {
                settingsDragging_ = false;
                return 0;
            }
            if (dragMode_ != DragMode::None) {
                const bool activateBubble = bubbleClickPending_ && !dragMoved_;
                EndDrag(!bubbleClickPending_ || dragMoved_);
                bubbleClickPending_ = false;
                dragMoved_ = false;
                if (activateBubble) {
                    ActivateBubbleClick();
                }
            }
            return 0;

        case WM_CONTEXTMENU: {
            POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (point.x == -1 && point.y == -1) {
                if (!GetCursorPos(&point)) {
                    RECT windowRect = {};
                    GetWindowRect(hwnd_, &windowRect);
                    point.x = windowRect.left + ScaleForDpi(hwnd_, 18);
                    point.y = windowRect.top + ScaleForDpi(hwnd_, 18);
                }
            }
            SetForegroundWindow(hwnd_);
            ShowContextMenu(point);
            return 0;
        }

        case kUsageUpdatedMessage:
            OnUsageUpdated(reinterpret_cast<UsageSnapshot*>(lParam));
            return 0;

        case kReleaseVersionUpdatedMessage:
            OnLatestReleaseChecked(reinterpret_cast<ReleaseVersionInfo*>(lParam));
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd_, kCountdownTimerId);
            KillTimer(hwnd_, kRefreshTimerId);
            KillTimer(hwnd_, kHoverPollTimerId);
            StopHoverExitGuard();
            CancelMouseLeaveTracking();
            DisableNativeGlassBackdrop();
            SaveSettings();
            DiscardTextFormats();
            DiscardDeviceResources();
            DiscardLayeredSurface();
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void AppBarWindow::RegisterWindowClass() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClassName;
    RegisterClassExW(&wc);
}

RECT AppBarWindow::GetDesktopClientRect() const {
    RECT rect = {};
    rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    rect.right = rect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    rect.bottom = rect.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return rect;
}

bool AppBarWindow::GetCurrentMonitorInfo(MONITORINFO& monitorInfo) const {
    monitorInfo = {};
    monitorInfo.cbSize = sizeof(MONITORINFO);

    HMONITOR monitor = nullptr;
    if (hwnd_ != nullptr) {
        monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    }
    if (monitor == nullptr && hasSavedRect_) {
        POINT center = {
            savedRect_.left + RectWidth(savedRect_) / 2,
            savedRect_.top + RectHeight(savedRect_) / 2,
        };
        monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    }
    if (monitor == nullptr) {
        POINT origin = { 0, 0 };
        monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTONEAREST);
    }

    return monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo) != FALSE;
}

RECT AppBarWindow::GetCurrentMonitorWorkRect() const {
    MONITORINFO monitorInfo = {};
    if (GetCurrentMonitorInfo(monitorInfo)) {
        return monitorInfo.rcWork;
    }
    return GetDesktopClientRect();
}

int AppBarWindow::GetMinimumWidgetWidth() const {
    if (taskbarMode_) {
        return ScaleForDpi(hwnd_, kTaskbarMinimumWidgetWidth);
    }
    return ScaleForDpi(hwnd_, simpleMode_ ? kSimpleMinimumWidgetWidth : kMinimumWidgetWidth);
}

int AppBarWindow::GetMinimumWidgetHeight(int width) const {
    if (taskbarMode_) {
        return CalculateTaskbarWidgetHeight(hwnd_);
    }
    if (simpleMode_) {
        return CalculateSimpleMinimumWidgetHeight(hwnd_);
    }
    int height = CalculateDetailedMinimumWidgetHeight(hwnd_, width);
    // Grow only for additional reset-credit rows (one row already in base height).
    if (snapshot_.success && snapshot_.resetCredits.fetched) {
        const int extraRows = std::max(0, static_cast<int>(snapshot_.resetCredits.availableCredits.size()) - 1);
        height += extraRows * ScaleForDpi(hwnd_, 18);
    }
    return height;
}

void AppBarWindow::SetLanguage(Language language) {
    if (language_ == language) {
        return;
    }

    language_ = language;
    if (hwnd_ != nullptr) {
        SetWindowTextW(hwnd_, LocalizeText(L"Codex Usage Widget", L"Codex 用量小工具"));
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
    SaveSettings();
}

void AppBarWindow::SetRefreshIntervalSeconds(int seconds) {
    const int sanitized = SanitizeRefreshIntervalSeconds(seconds);
    if (refreshIntervalSeconds_ == sanitized) {
        return;
    }

    refreshIntervalSeconds_ = sanitized;
    refreshCountdownSeconds_ = refreshIntervalSeconds_;
    if (hwnd_ != nullptr) {
        RestartRefreshTimer();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    SaveSettings();
}

void AppBarWindow::RestartRefreshTimer() {
    if (hwnd_ == nullptr) {
        return;
    }

    KillTimer(hwnd_, kRefreshTimerId);
    SetTimer(hwnd_, kRefreshTimerId, static_cast<UINT>(refreshIntervalSeconds_ * 1000), nullptr);
}

const wchar_t* AppBarWindow::LocalizeText(const wchar_t* english, const wchar_t* chinese) const {
    return language_ == Language::Chinese ? chinese : english;
}

std::wstring AppBarWindow::GetVersionStatusText(bool compact) const {
    if (updateAvailable_ && !latestReleaseTag_.empty()) {
        if (compact) {
            return language_ == Language::Chinese
                ? (std::wstring(kCurrentVersion) + L" -> " + latestReleaseTag_)
                : (std::wstring(kCurrentVersion) + L" -> " + latestReleaseTag_);
        }
        return language_ == Language::Chinese
            ? (L"版本: " + std::wstring(kCurrentVersion) + L"，可更新到 " + latestReleaseTag_)
            : (L"Version: " + std::wstring(kCurrentVersion) + L", update available: " + latestReleaseTag_);
    }

    if (compact) {
        return kCurrentVersion;
    }
    return language_ == Language::Chinese
        ? (L"版本: " + std::wstring(kCurrentVersion))
        : (L"Version: " + std::wstring(kCurrentVersion));
}

std::wstring AppBarWindow::GetRefreshStatusText() const {
    if (refreshInFlight_) {
        return LocalizeText(L"Refreshing", L"重新整理中");
    }
    if (lastRefreshCompletedUnixSeconds_ <= 0) {
        return LocalizeText(L"Waiting for update", L"等待更新");
    }
    if (!lastRefreshSucceeded_) {
        return LocalizeText(L"Update failed", L"更新失敗");
    }
    return std::wstring(LocalizeText(L"Updated ", L"已更新 "))
        + FormatClockTime(lastRefreshCompletedUnixSeconds_);
}

RECT AppBarWindow::BuildDefaultRect(const RECT& desktopRect) const {
    if (taskbarMode_) {
        return BuildTaskbarDockRect();
    }

    // Default / reset position: top-right of the current desktop work area.
    const int margin = ScaleForDpi(hwnd_, kDesktopMargin);
    const int width = ScaleForDpi(hwnd_, simpleMode_ ? kSimpleDefaultWidgetWidth : kDefaultWidgetWidth);
    const int height = GetMinimumWidgetHeight(width);
    RECT rect = {};
    rect.right = std::max(width + margin, static_cast<int>(desktopRect.right) - margin);
    rect.left = std::max(static_cast<int>(desktopRect.left) + margin, static_cast<int>(rect.right) - width);
    rect.top = static_cast<int>(desktopRect.top) + margin;
    if (rect.top + height > static_cast<int>(desktopRect.bottom) - margin) {
        rect.top = std::max(static_cast<int>(desktopRect.top) + margin,
            static_cast<int>(desktopRect.bottom) - margin - height);
    }
    rect.bottom = rect.top + height;
    return rect;
}

RECT AppBarWindow::BuildTaskbarDockRect() const {
    MONITORINFO monitorInfo = {};
    RECT monitorRect = GetDesktopClientRect();
    RECT workRect = monitorRect;
    if (GetCurrentMonitorInfo(monitorInfo)) {
        monitorRect = monitorInfo.rcMonitor;
        workRect = monitorInfo.rcWork;
    }

    const int margin = ScaleForDpi(hwnd_, 4);
    const int width = ScaleForDpi(hwnd_, kTaskbarDefaultWidgetWidth);
    const int height = CalculateTaskbarWidgetHeight(hwnd_);
    const int leftGap = std::max(0, static_cast<int>(workRect.left - monitorRect.left));
    const int topGap = std::max(0, static_cast<int>(workRect.top - monitorRect.top));
    const int rightGap = std::max(0, static_cast<int>(monitorRect.right - workRect.right));
    const int bottomGap = std::max(0, static_cast<int>(monitorRect.bottom - workRect.bottom));
    const int largestGap = std::max({ leftGap, topGap, rightGap, bottomGap });

    RECT rect = {};
    if (leftGap == largestGap && leftGap > 0) {
        rect.left = workRect.left + margin;
        rect.top = workRect.bottom - height - margin;
    } else if (rightGap == largestGap && rightGap > 0) {
        rect.left = workRect.right - width - margin;
        rect.top = workRect.bottom - height - margin;
    } else if (topGap == largestGap && topGap > 0) {
        rect.left = workRect.right - width - margin;
        rect.top = workRect.top + margin;
    } else {
        rect.left = workRect.right - width - margin;
        rect.top = workRect.bottom - height - margin;
    }

    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
    return rect;
}

RECT AppBarWindow::ClampRectToDesktop(RECT rect) const {
    const RECT desktopRect = taskbarMode_ ? GetCurrentMonitorWorkRect() : GetDesktopClientRect();
    const int minWidth = GetMinimumWidgetWidth();
    const int minHeight = GetMinimumWidgetHeight(std::max(RectWidth(rect), minWidth));

    if (RectWidth(rect) < minWidth) {
        rect.right = rect.left + minWidth;
    }
    if (RectHeight(rect) < minHeight) {
        rect.bottom = rect.top + minHeight;
    }

    if (rect.left < desktopRect.left) {
        const int width = RectWidth(rect);
        rect.left = desktopRect.left;
        rect.right = rect.left + width;
    }
    if (rect.top < desktopRect.top) {
        const int height = RectHeight(rect);
        rect.top = desktopRect.top;
        rect.bottom = rect.top + height;
    }
    if (rect.right > desktopRect.right) {
        const int width = RectWidth(rect);
        rect.right = desktopRect.right;
        rect.left = rect.right - width;
    }
    if (rect.bottom > desktopRect.bottom) {
        const int height = RectHeight(rect);
        rect.bottom = desktopRect.bottom;
        rect.top = rect.bottom - height;
    }

    rect.left = std::max(rect.left, desktopRect.left);
    rect.top = std::max(rect.top, desktopRect.top);
    rect.right = std::min(rect.right, desktopRect.right);
    rect.bottom = std::min(rect.bottom, desktopRect.bottom);
    return rect;
}

RECT AppBarWindow::ClampPanelGroupRect(RECT rect) const {
    rect = ClampRectToDesktop(rect);
    const codex_widget::WidgetGroupGeometry group = codex_widget::ShiftGroupIntoBounds(
        codex_widget::GroupGeometryForPanel(
            ToWidgetRect(rect),
            GetBubbleSizeForState(),
            ScaleForDpi(hwnd_, kPanelBubbleGap)),
        ToWidgetRect(GetDesktopClientRect()));
    return ToWin32Rect(group.panel);
}

int AppBarWindow::GetBubbleSizeForState() const {
    const int requestedSize = presentationState_ == codex_widget::PresentationState::HoverExpanded
        ? kBubbleHoverSize
        : kBubbleWidgetSize;
    const int scaledSize = ScaleForDpi(hwnd_, requestedSize);
    return taskbarMode_
        ? std::min(scaledSize, CalculateTaskbarWidgetHeight(hwnd_))
        : scaledSize;
}

codex_widget::WidgetGroupGeometry AppBarWindow::GetCurrentGroupGeometry() const {
    return codex_widget::ShiftGroupIntoBounds(
        codex_widget::GroupGeometryForPanel(
            ToWidgetRect(savedRect_),
            GetBubbleSizeForState(),
            ScaleForDpi(hwnd_, kPanelBubbleGap)),
        ToWidgetRect(GetDesktopClientRect()));
}

RECT AppBarWindow::GetExpandedWindowRect(const RECT& panelRect) const {
    const codex_widget::WidgetGroupGeometry group = codex_widget::ShiftGroupIntoBounds(
        codex_widget::GroupGeometryForPanel(
            ToWidgetRect(panelRect),
            GetBubbleSizeForState(),
            ScaleForDpi(hwnd_, kPanelBubbleGap)),
        ToWidgetRect(GetDesktopClientRect()));
    return ToWin32Rect(group.window);
}

RECT AppBarWindow::GetBubbleWindowRect(const RECT& expandedRect) const {
    const codex_widget::WidgetGroupGeometry group = codex_widget::ShiftGroupIntoBounds(
        codex_widget::GroupGeometryForPanel(
            ToWidgetRect(expandedRect),
            GetBubbleSizeForState(),
            ScaleForDpi(hwnd_, kPanelBubbleGap)),
        ToWidgetRect(GetDesktopClientRect()));
    return ToWin32Rect(group.bubble);
}

RECT AppBarWindow::GetSettingsWindowRect(const RECT& expandedRect) const {
    const int width = ScaleForDpi(hwnd_, 340);
    const int height = ScaleForDpi(hwnd_, 190);
    RECT workRect = GetCurrentMonitorWorkRect();
    RECT rect = MakeRect(expandedRect.right - width, expandedRect.top,
        expandedRect.right, expandedRect.top + height);
    if (rect.left < workRect.left) {
        OffsetRect(&rect, workRect.left - rect.left, 0);
    }
    if (rect.top < workRect.top) {
        OffsetRect(&rect, 0, workRect.top - rect.top);
    }
    if (rect.right > workRect.right) {
        OffsetRect(&rect, workRect.right - rect.right, 0);
    }
    if (rect.bottom > workRect.bottom) {
        OffsetRect(&rect, 0, workRect.bottom - rect.bottom);
    }
    return rect;
}

void AppBarWindow::UpdateWindowBounds(bool useSavedPosition) {
    if (taskbarMode_ && !settingsOpen_) {
        const RECT taskbarRect = BuildTaskbarDockRect();
        savedRect_ = taskbarRect;
        hasSavedRect_ = true;
        MoveWindow(hwnd_, taskbarRect.left, taskbarRect.top,
            RectWidth(taskbarRect), RectHeight(taskbarRect), FALSE);
        HRGN circleRegion = CreateEllipticRgn(
            0,
            0,
            RectWidth(taskbarRect) + 1,
            RectHeight(taskbarRect) + 1);
        if (circleRegion != nullptr && SetWindowRgn(hwnd_, circleRegion, TRUE) == FALSE) {
            DeleteObject(circleRegion);
        }
        SetWindowPos(hwnd_, HWND_TOPMOST,
            taskbarRect.left, taskbarRect.top, RectWidth(taskbarRect), RectHeight(taskbarRect), SWP_NOACTIVATE);
        if (!useSavedPosition) {
            SaveSettings();
        }
        RenderLayeredSurface();
        return;
    }

    if (taskbarMode_ && settingsOpen_) {
        const RECT taskbarRect = BuildTaskbarDockRect();
        savedRect_ = taskbarRect;
        hasSavedRect_ = true;
        SetWindowRgn(hwnd_, nullptr, TRUE);
        const RECT settingsRect = GetSettingsWindowRect(taskbarRect);
        MoveWindow(hwnd_, settingsRect.left, settingsRect.top,
            RectWidth(settingsRect), RectHeight(settingsRect), FALSE);
        SetWindowPos(hwnd_, HWND_TOPMOST,
            settingsRect.left, settingsRect.top, RectWidth(settingsRect), RectHeight(settingsRect), SWP_NOACTIVATE);
        RenderLayeredSurface();
        return;
    }

    const bool usingPersistedRect = useSavedPosition && hasSavedRect_ && !taskbarMode_;
    SetWindowRgn(hwnd_, nullptr, TRUE);
    RECT expandedRect = usingPersistedRect ? savedRect_ : BuildDefaultRect(GetDesktopClientRect());
    expandedRect = ClampRectToDesktop(expandedRect);
    const codex_widget::WidgetGroupGeometry group = codex_widget::ShiftGroupIntoBounds(
        codex_widget::GroupGeometryForPanel(
            ToWidgetRect(expandedRect),
            GetBubbleSizeForState(),
            ScaleForDpi(hwnd_, kPanelBubbleGap)),
        ToWidgetRect(GetDesktopClientRect()));
    savedRect_ = ToWin32Rect(group.panel);
    hasSavedRect_ = true;
    RECT windowRect = ToWin32Rect(group.window);
    if (settingsOpen_) {
        windowRect = GetSettingsWindowRect(expandedRect);
    } else if (presentationState_ == codex_widget::PresentationState::Bubble) {
        windowRect = ToWin32Rect(group.bubble);
    }
    MoveWindow(hwnd_, windowRect.left, windowRect.top, RectWidth(windowRect), RectHeight(windowRect), FALSE);
    SetWindowPos(hwnd_, (alwaysOnTop_ || taskbarMode_) ? HWND_TOPMOST : HWND_NOTOPMOST,
        windowRect.left, windowRect.top, RectWidth(windowRect), RectHeight(windowRect), SWP_NOACTIVATE);
    if (!usingPersistedRect) {
        SaveSettings();
    }
    RenderLayeredSurface();
}

void AppBarWindow::SetDisplayMode(bool simpleMode, bool taskbarMode) {
    const bool normalizedSimpleMode = simpleMode && !taskbarMode;
    if (simpleMode_ == normalizedSimpleMode && taskbarMode_ == taskbarMode) {
        return;
    }

    simpleMode_ = normalizedSimpleMode;
    taskbarMode_ = taskbarMode;
    UpdateWindowBounds(false);
    RenderLayeredSurface();
}

void AppBarWindow::CancelMouseLeaveTracking() {
    if (!mouseTracking_ || hwnd_ == nullptr) {
        mouseTracking_ = false;
        return;
    }

    TRACKMOUSEEVENT track = {};
    track.cbSize = sizeof(track);
    track.dwFlags = TME_CANCEL | TME_LEAVE;
    track.hwndTrack = hwnd_;
    TrackMouseEvent(&track);
    mouseTracking_ = false;
}

void AppBarWindow::ArmMouseLeaveTracking() {
    if (hwnd_ == nullptr || mouseTracking_) {
        return;
    }

    TRACKMOUSEEVENT track = {};
    track.cbSize = sizeof(track);
    track.dwFlags = TME_LEAVE;
    track.hwndTrack = hwnd_;
    mouseTracking_ = TrackMouseEvent(&track) != FALSE;
}

bool AppBarWindow::IsCursorInsideCurrentWindow() const {
    if (hwnd_ == nullptr) {
        return false;
    }

    POINT cursor = {};
    if (!GetCursorPos(&cursor)) {
        return false;
    }

    const codex_widget::WidgetGroupGeometry group = GetCurrentGroupGeometry();
    if (presentationState_ == codex_widget::PresentationState::Bubble) {
        return cursor.x >= group.bubble.left
            && cursor.x < group.bubble.right
            && cursor.y >= group.bubble.top
            && cursor.y < group.bubble.bottom;
    }
    return codex_widget::IsPointInsideWidgetGroup(group, cursor.x, cursor.y);
}

void AppBarWindow::UpdateHoverStateFromCursor() {
    if (hwnd_ == nullptr || taskbarMode_ || settingsOpen_ || settingsDragging_ || dragMode_ != DragMode::None) {
        return;
    }

    POINT cursorScreen = {};
    if (!GetCursorPos(&cursorScreen)) {
        return;
    }

    const codex_widget::WidgetGroupGeometry group = GetCurrentGroupGeometry();
    const auto contains = [&cursorScreen](const codex_widget::WidgetRect& rect) {
        return cursorScreen.x >= rect.left
            && cursorScreen.x < rect.right
            && cursorScreen.y >= rect.top
            && cursorScreen.y < rect.bottom;
    };
    const bool cursorInsideBubble = contains(group.bubble);
    const bool cursorInsideVisibleGroup = codex_widget::IsPointInsideWidgetGroup(
        group, cursorScreen.x, cursorScreen.y)
        || (cursorScreen.x >= group.panel.right
            && cursorScreen.x < group.bubble.left
            && cursorScreen.y >= group.panel.top
            && cursorScreen.y < group.panel.bottom);
    if (hoverSuppressedUntilCursorLeavesBubble_) {
        if (!cursorInsideBubble) {
            hoverSuppressedUntilCursorLeavesBubble_ = false;
        } else {
            return;
        }
    }

    const codex_widget::DisplayMode displayMode = simpleMode_
        ? codex_widget::DisplayMode::Simple
        : codex_widget::DisplayMode::Full;
    const codex_widget::PresentationState nextState = codex_widget::HoverStateForCursor(
        presentationState_,
        displayMode,
        cursorInsideBubble,
        cursorInsideVisibleGroup);
    if (nextState != presentationState_) {
        SetPresentationState(nextState);
    } else if (cursorInsideVisibleGroup) {
        ArmMouseLeaveTracking();
    }
}

void AppBarWindow::StartHoverExitGuard() {
    if (hwnd_ == nullptr || presentationState_ != codex_widget::PresentationState::HoverExpanded) {
        return;
    }

    hoverExitGuardActive_ = true;
    SetTimer(hwnd_, kHoverExitTimerId, kHoverExitGuardMilliseconds, nullptr);
}

void AppBarWindow::StopHoverExitGuard() {
    hoverExitGuardActive_ = false;
    if (hwnd_ != nullptr) {
        KillTimer(hwnd_, kHoverExitTimerId);
    }
}

void AppBarWindow::SetPresentationState(codex_widget::PresentationState state) {
    if (presentationState_ == state) {
        return;
    }

    CancelMouseLeaveTracking();
    StopHoverExitGuard();
    presentationState_ = state;
    UpdateWindowBounds(true);
    RenderLayeredSurface();
    ArmMouseLeaveTracking();
    if (presentationState_ == codex_widget::PresentationState::Bubble) {
        hoverSuppressedUntilCursorLeavesBubble_ = IsCursorInsideCurrentWindow();
    }
}

void AppBarWindow::OpenSettings() {
    settingsOpen_ = true;
    settingsDragging_ = false;
    hoverSuppressedUntilCursorLeavesBubble_ = false;
    presentationState_ = codex_widget::PresentationState::PinnedExpanded;
    UpdateWindowBounds(true);
    RenderLayeredSurface();
}

void AppBarWindow::CloseSettings() {
    settingsOpen_ = false;
    settingsDragging_ = false;
    hoverSuppressedUntilCursorLeavesBubble_ = false;
    presentationState_ = codex_widget::TransitionOnSettingsClose();
    UpdateWindowBounds(true);
    RenderLayeredSurface();
}

void AppBarWindow::UpdateTransparencyFromPoint(POINT clientPoint) {
    if (!settingsOpen_ || settingsSliderRect_.right <= settingsSliderRect_.left) {
        return;
    }

    const int nextValue = codex_widget::TransparencyPercentForSlider(
        clientPoint.x,
        settingsSliderRect_.left,
        settingsSliderRect_.right);
    if (glassTransparencyPercent_ == nextValue) {
        return;
    }

    glassTransparencyPercent_ = nextValue;
    SaveSettings();
    EnableNativeGlassBackdrop();
    RenderLayeredSurface();
}

void AppBarWindow::LoadSettings() {
    const std::wstring path = GetSettingsPath();
    const int version = GetPrivateProfileIntW(L"layout", L"layout_version", 0, path.c_str());
    alwaysOnTop_ = GetPrivateProfileIntW(L"layout", L"always_on_top", 0, path.c_str()) != 0;
    lockPosition_ = GetPrivateProfileIntW(L"layout", L"lock_position", 0, path.c_str()) != 0;
    simpleMode_ = GetPrivateProfileIntW(L"layout", L"simple_mode", 0, path.c_str()) != 0;
    taskbarMode_ = GetPrivateProfileIntW(L"layout", L"taskbar_mode", 0, path.c_str()) != 0;
    if (taskbarMode_) {
        simpleMode_ = false;
    }
    refreshIntervalSeconds_ = SanitizeRefreshIntervalSeconds(
        GetPrivateProfileIntW(L"layout", L"refresh_interval_seconds", 60, path.c_str()));
    refreshCountdownSeconds_ = refreshIntervalSeconds_;
    glassTransparencyPercent_ = codex_widget::ClampTransparencyPercent(
        GetPrivateProfileIntW(L"layout", L"glass_transparency_percent", 42, path.c_str()));
    language_ = GetPrivateProfileIntW(L"layout", L"language", 0, path.c_str()) == 1
        ? Language::Chinese
        : Language::English;
    if (version < kLayoutVersion) {
        hasSavedRect_ = false;
        return;
    }

    const int width = GetPrivateProfileIntW(L"layout", L"width", 0, path.c_str());
    const int height = GetPrivateProfileIntW(L"layout", L"height", 0, path.c_str());
    if (width <= 0 || height <= 0) {
        hasSavedRect_ = false;
        return;
    }

    savedRect_.left = GetPrivateProfileIntW(L"layout", L"x", 0, path.c_str());
    savedRect_.top = GetPrivateProfileIntW(L"layout", L"y", 0, path.c_str());
    savedRect_.right = savedRect_.left + width;
    savedRect_.bottom = savedRect_.top + height;
    savedRect_ = ClampRectToDesktop(savedRect_);
    hasSavedRect_ = true;
}

void AppBarWindow::SaveSettings() const {
    if (!hasSavedRect_) {
        return;
    }

    const std::wstring path = GetSettingsPath();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    WritePrivateProfileStringW(L"layout", L"layout_version", std::to_wstring(kLayoutVersion).c_str(), path.c_str());
    WritePrivateProfileStringW(L"layout", L"always_on_top", alwaysOnTop_ ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"layout", L"lock_position", lockPosition_ ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"layout", L"simple_mode", simpleMode_ ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"layout", L"taskbar_mode", taskbarMode_ ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"layout", L"refresh_interval_seconds", std::to_wstring(refreshIntervalSeconds_).c_str(), path.c_str());
    WritePrivateProfileStringW(L"layout", L"glass_transparency_percent", std::to_wstring(glassTransparencyPercent_).c_str(), path.c_str());
    WritePrivateProfileStringW(L"layout", L"language", language_ == Language::Chinese ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"layout", L"x", std::to_wstring(savedRect_.left).c_str(), path.c_str());
    WritePrivateProfileStringW(L"layout", L"y", std::to_wstring(savedRect_.top).c_str(), path.c_str());
    WritePrivateProfileStringW(L"layout", L"width", std::to_wstring(RectWidth(savedRect_)).c_str(), path.c_str());
    WritePrivateProfileStringW(L"layout", L"height", std::to_wstring(RectHeight(savedRect_)).c_str(), path.c_str());
}

std::wstring AppBarWindow::GetSettingsPath() const {
    PWSTR appDataPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath))) {
        const std::filesystem::path path = std::filesystem::path(appDataPath) / L"CodexUsageMonitor" / L"settings.ini";
        CoTaskMemFree(appDataPath);
        return path.wstring();
    }

    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(instance_, modulePath, MAX_PATH);
    return (std::filesystem::path(modulePath).parent_path() / L"settings.ini").wstring();
}

std::wstring AppBarWindow::GetExecutablePath() const {
    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(instance_, modulePath, MAX_PATH);
    return modulePath;
}

std::wstring AppBarWindow::GetAssetPath(const wchar_t* relativePath) const {
    const std::filesystem::path executablePath(GetExecutablePath());
    return (executablePath.parent_path() / relativePath).wstring();
}

bool AppBarWindow::RegisterPrivateFonts() {
    if (!registeredFontPaths_.empty()) {
        return true;
    }

    const wchar_t* relativePaths[] = {
        L"assets\\fonts\\Iansui-Regular.ttf",
        L"assets\\fonts\\Quantico-Regular.ttf",
        L"assets\\fonts\\Quantico-Italic.ttf",
        L"assets\\fonts\\Quantico-Bold.ttf",
        L"assets\\fonts\\Quantico-BoldItalic.ttf",
        L"assets\\fonts\\StoryScript-Regular.ttf",
    };

    bool registeredAny = false;
    for (const wchar_t* relativePath : relativePaths) {
        const std::wstring path = GetAssetPath(relativePath);
        if (AddFontResourceExW(path.c_str(), FR_PRIVATE, nullptr) > 0) {
            registeredFontPaths_.push_back(path);
            registeredAny = true;
        }
    }

    if (registeredAny) {
        SendMessageTimeoutW(
            HWND_BROADCAST,
            WM_FONTCHANGE,
            0,
            0,
            SMTO_ABORTIFHUNG,
            100,
            nullptr);
    }
    return registeredAny;
}

void AppBarWindow::UnregisterPrivateFonts() {
    for (const std::wstring& path : registeredFontPaths_) {
        RemoveFontResourceExW(path.c_str(), FR_PRIVATE, nullptr);
    }
    if (!registeredFontPaths_.empty()) {
        SendMessageTimeoutW(
            HWND_BROADCAST,
            WM_FONTCHANGE,
            0,
            0,
            SMTO_ABORTIFHUNG,
            100,
            nullptr);
    }
    registeredFontPaths_.clear();
}

void AppBarWindow::RefreshTheme() {
    lightTheme_ = IsDesktopLightTheme();
}

bool AppBarWindow::IsDesktopLightTheme() const {
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LONG status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &size);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    return value != 0;
}

bool AppBarWindow::IsLaunchAtStartupEnabled() const {
    wchar_t value[2048] = {};
    DWORD size = sizeof(value);
    const LONG status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"CodexUsageMonitor",
        RRF_RT_REG_SZ,
        nullptr,
        value,
        &size);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    const std::wstring expected = L"\"" + GetExecutablePath() + L"\"";
    return std::wstring(value) == expected;
}

bool AppBarWindow::SetLaunchAtStartupEnabled(bool enabled) const {
    const wchar_t* subkey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    const wchar_t* valueName = L"CodexUsageMonitor";

    if (enabled) {
        const std::wstring command = L"\"" + GetExecutablePath() + L"\"";
        const LONG status = RegSetKeyValueW(
            HKEY_CURRENT_USER,
            subkey,
            valueName,
            REG_SZ,
            command.c_str(),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        return status == ERROR_SUCCESS;
    }

    const LONG status = RegDeleteKeyValueW(HKEY_CURRENT_USER, subkey, valueName);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

HRESULT AppBarWindow::CreateDeviceIndependentResources() {
    if (!d2dFactory_) {
        const HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf());
        if (FAILED(hr)) {
            return hr;
        }
    }

    if (!dwriteFactory_) {
        const HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
        if (FAILED(hr)) {
            return hr;
        }
    }

    if (!wicFactory_) {
        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory2,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(wicFactory_.GetAddressOf()));
        if (FAILED(hr)) {
            CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(wicFactory_.GetAddressOf()));
        }
    }

    return S_OK;
}

HRESULT AppBarWindow::CreateTextFormat(
    float sizePixels,
    DWRITE_FONT_WEIGHT weight,
    const wchar_t* familyName,
    IDWriteTextFormat** format) {
    return dwriteFactory_->CreateTextFormat(
        familyName,
        nullptr,
        weight,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        sizePixels,
        L"zh-CN",
        format);
}

void AppBarWindow::DiscardTextFormats() {
    textFormatKicker_.Reset();
    textFormatTitle_.Reset();
    textFormatDelta_.Reset();
    textFormatMetricLabel_.Reset();
    textFormatMetricValue_.Reset();
    textFormatFoot_.Reset();
    textFormatDpi_ = 0;
}

HRESULT AppBarWindow::EnsureTextFormats() {
    const UINT dpi = GetDpiForWindow(hwnd_);
    if (textFormatDpi_ == dpi &&
        textFormatKicker_ &&
        textFormatTitle_ &&
        textFormatDelta_ &&
        textFormatMetricLabel_ &&
        textFormatMetricValue_ &&
        textFormatFoot_) {
        return S_OK;
    }

    DiscardTextFormats();

    const wchar_t* primaryFamily = language_ == Language::Chinese ? L"Iansui" : L"Story Script";
    HRESULT hr = CreateTextFormat(
        static_cast<float>(ScaleForDpi(hwnd_, 12)),
        DWRITE_FONT_WEIGHT_NORMAL,
        primaryFamily,
        textFormatKicker_.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = CreateTextFormat(
        static_cast<float>(ScaleForDpi(hwnd_, 18)),
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        primaryFamily,
        textFormatTitle_.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = CreateTextFormat(
        static_cast<float>(ScaleForDpi(hwnd_, 28)),
        DWRITE_FONT_WEIGHT_BOLD,
        primaryFamily,
        textFormatDelta_.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = CreateTextFormat(
        static_cast<float>(ScaleForDpi(hwnd_, 12)),
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        primaryFamily,
        textFormatMetricLabel_.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = CreateTextFormat(
        static_cast<float>(ScaleForDpi(hwnd_, 17)),
        DWRITE_FONT_WEIGHT_BOLD,
        primaryFamily,
        textFormatMetricValue_.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = CreateTextFormat(
        static_cast<float>(ScaleForDpi(hwnd_, 12)),
        DWRITE_FONT_WEIGHT_NORMAL,
        primaryFamily,
        textFormatFoot_.GetAddressOf());
    if (FAILED(hr)) return hr;

    textFormatDpi_ = dpi;
    return S_OK;
}

void AppBarWindow::ApplyFontRuns(IDWriteTextLayout* layout, const std::wstring& text) const {
    if (layout == nullptr || text.empty()) {
        return;
    }

    const auto isCjk = [](wchar_t value) {
        return (value >= 0x2E80 && value <= 0x9FFF)
            || (value >= 0xF900 && value <= 0xFAFF);
    };

    size_t runStart = 0;
    bool currentIsCjk = isCjk(text.front());
    for (size_t index = 1; index <= text.size(); ++index) {
        const bool atEnd = index == text.size();
        const bool nextIsCjk = atEnd ? currentIsCjk : isCjk(text[index]);
        if (!atEnd && nextIsCjk == currentIsCjk) {
            continue;
        }

        const wchar_t* family = currentIsCjk ? L"Iansui" : L"Story Script";
        layout->SetFontFamilyName(
            family,
            DWRITE_TEXT_RANGE{
                static_cast<UINT32>(runStart),
                static_cast<UINT32>(index - runStart),
            });
        if (!atEnd) {
            runStart = index;
            currentIsCjk = nextIsCjk;
        }
    }
}

HRESULT AppBarWindow::CreateDeviceResources() {
    if (FAILED(CreateDeviceIndependentResources())) {
        return E_FAIL;
    }

    if (!renderTarget_) {
        const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f,
            96.0f,
            D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
        HRESULT hr = d2dFactory_->CreateDCRenderTarget(&properties, renderTarget_.GetAddressOf());
        if (FAILED(hr)) {
            return hr;
        }

        hr = renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0, 0.0f), solidBrush_.GetAddressOf());
        if (FAILED(hr)) {
            return hr;
        }
    }

    (void)EnsureAssetResources();
    return EnsureTextFormats();
}

void AppBarWindow::DiscardDeviceResources() {
    DiscardAssetResources();
    solidBrush_.Reset();
    renderTarget_.Reset();
}

HRESULT AppBarWindow::EnsureAssetResources() {
    if (assetResourcesLoaded_) {
        return S_OK;
    }
    if (!wicFactory_ || !renderTarget_) {
        return E_NOINTERFACE;
    }

    Microsoft::WRL::ComPtr<ID2D1Bitmap> codexBitmap;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> undoBitmap;
    HRESULT hr = LoadAssetBitmap(
        L"assets\\icons\\codex-liquid.png",
        &codexBitmap);
    if (FAILED(hr)) {
        return hr;
    }
    hr = LoadTintedAssetBitmap(
        L"assets\\icons\\undo.png",
        &undoBitmap);
    if (FAILED(hr)) {
        return hr;
    }

    codexIconBitmap_ = std::move(codexBitmap);
    undoIconBitmap_ = std::move(undoBitmap);
    assetResourcesLoaded_ = true;
    return S_OK;
}

void AppBarWindow::DiscardAssetResources() {
    assetResourcesLoaded_ = false;
    codexIconBitmap_.Reset();
    undoIconBitmap_.Reset();
}

HRESULT AppBarWindow::LoadAssetBitmap(
    const wchar_t* relativePath,
    Microsoft::WRL::ComPtr<ID2D1Bitmap>* bitmap) {
    if (bitmap == nullptr || !wicFactory_ || !renderTarget_) {
        return E_INVALIDARG;
    }

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wicFactory_->CreateDecoderFromFilename(
        GetAssetPath(relativePath).c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,
        decoder.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = wicFactory_->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }

    hr = converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        return hr;
    }

    return renderTarget_->CreateBitmapFromWicBitmap(
        converter.Get(),
        nullptr,
        bitmap->ReleaseAndGetAddressOf());
}

HRESULT AppBarWindow::LoadTintedAssetBitmap(
    const wchar_t* relativePath,
    Microsoft::WRL::ComPtr<ID2D1Bitmap>* bitmap) {
    if (bitmap == nullptr || !wicFactory_ || !renderTarget_) {
        return E_INVALIDARG;
    }

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wicFactory_->CreateDecoderFromFilename(
        GetAssetPath(relativePath).c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,
        decoder.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }

    UINT width = 0;
    UINT height = 0;
    hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0) {
        return FAILED(hr) ? hr : E_INVALIDARG;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = wicFactory_->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }

    hr = converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        return hr;
    }

    const UINT stride = width * 4;
    std::vector<BYTE> pixels(static_cast<size_t>(stride) * static_cast<size_t>(height));
    hr = converter->CopyPixels(
        nullptr,
        stride,
        static_cast<UINT>(pixels.size()),
        pixels.data());
    if (FAILED(hr)) {
        return hr;
    }

    // The supplied undo asset is an alpha-only black PNG. Recolour its
    // premultiplied pixels while preserving the original alpha contour so it
    // can be drawn as a normal bitmap without FillOpacityMask state hazards.
    constexpr BYTE tintRed = 235;
    constexpr BYTE tintGreen = 242;
    constexpr BYTE tintBlue = 255;
    for (size_t index = 0; index + 3 < pixels.size(); index += 4) {
        const unsigned int alpha = pixels[index + 3];
        pixels[index + 0] = static_cast<BYTE>((tintBlue * alpha + 127) / 255);
        pixels[index + 1] = static_cast<BYTE>((tintGreen * alpha + 127) / 255);
        pixels[index + 2] = static_cast<BYTE>((tintRed * alpha + 127) / 255);
    }

    const D2D1_BITMAP_PROPERTIES bitmapProperties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    return renderTarget_->CreateBitmap(
        D2D1::SizeU(width, height),
        pixels.data(),
        stride,
        &bitmapProperties,
        bitmap->ReleaseAndGetAddressOf());
}

void AppBarWindow::DrawAssetBitmap(
    ID2D1Bitmap* bitmap,
    const RECT& destination,
    float opacity) const {
    if (bitmap == nullptr || !renderTarget_) {
        return;
    }
    renderTarget_->DrawBitmap(
        bitmap,
        ToRectF(destination),
        opacity,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void AppBarWindow::EnableNativeGlassBackdrop() {
    // The widget is published as a per-pixel-alpha layered surface. Applying
    // DWM acrylic/blur to that HWND also paints the transparent pixels in its
    // rectangular bounds, which creates a visible frame around the bubble,
    // panel, and taskbar presentation. The actual glass material is already
    // rendered by Direct2D inside the non-transparent shapes, so the native
    // backdrop must stay disabled for every presentation mode.
    if (nativeGlassEnabled_) {
        DisableNativeGlassBackdrop();
    }
}

void AppBarWindow::DisableNativeGlassBackdrop() {
    if (hwnd_ == nullptr || !nativeGlassEnabled_) {
        nativeGlassEnabled_ = false;
        return;
    }

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto setWindowCompositionAttribute = user32 == nullptr
        ? nullptr
        : reinterpret_cast<SetWindowCompositionAttributeFunction>(
            GetProcAddress(user32, "SetWindowCompositionAttribute"));
    if (setWindowCompositionAttribute != nullptr) {
        ACCENT_POLICY policy = { ACCENT_DISABLED, 0, 0, 0 };
        WINDOWCOMPOSITIONATTRIBDATA data = {
            WCA_ACCENT_POLICY,
            &policy,
            sizeof(policy),
        };
        setWindowCompositionAttribute(&data);
    }
    DWM_BLURBEHIND blurBehind = {};
    blurBehind.dwFlags = DWM_BB_ENABLE;
    blurBehind.fEnable = FALSE;
    DwmEnableBlurBehindWindow(hwnd_, &blurBehind);
    nativeGlassEnabled_ = false;
}

void AppBarWindow::DrawGlassSurface(
    const RECT& rect,
    COLORREF topColor,
    COLORREF bottomColor,
    float alpha) {
    if (!renderTarget_) {
        return;
    }

    const D2D1_GRADIENT_STOP stops[] = {
        { 0.0f, ToColorF(topColor, alpha) },
        { 1.0f, ToColorF(bottomColor, alpha * 0.78f) },
    };
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stopCollection;
    if (FAILED(renderTarget_->CreateGradientStopCollection(
            stops,
            ARRAYSIZE(stops),
            D2D1_GAMMA_2_2,
            D2D1_EXTEND_MODE_CLAMP,
            stopCollection.GetAddressOf()))) {
        return;
    }

    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> gradientBrush;
    const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES gradientProperties =
        D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(static_cast<float>(rect.left), static_cast<float>(rect.top)),
            D2D1::Point2F(static_cast<float>(rect.left), static_cast<float>(rect.bottom)));
    const D2D1_BRUSH_PROPERTIES brushProperties = D2D1::BrushProperties();
    if (FAILED(renderTarget_->CreateLinearGradientBrush(
            &gradientProperties,
            &brushProperties,
            stopCollection.Get(),
            gradientBrush.GetAddressOf()))) {
        return;
    }

    const float radius = static_cast<float>(ScaleForDpi(hwnd_, 18));
    renderTarget_->FillRoundedRectangle(
        D2D1::RoundedRect(ToRectF(rect), radius, radius),
        gradientBrush.Get());
}

void AppBarWindow::DrawGlassCard(const RECT& rect, COLORREF tint, float alpha) {
    if (!renderTarget_ || !solidBrush_) {
        return;
    }
    solidBrush_->SetColor(ToColorF(tint, alpha));
    const float radius = static_cast<float>(ScaleForDpi(hwnd_, 13));
    renderTarget_->FillRoundedRectangle(
        D2D1::RoundedRect(ToRectF(rect), radius, radius),
        solidBrush_.Get());
}

void AppBarWindow::DrawGlassEdgeHighlight(const RECT& rect) {
    if (!renderTarget_ || !solidBrush_) {
        return;
    }
    const RECT inner = ShrinkRect(rect, ScaleForDpi(hwnd_, 2), ScaleForDpi(hwnd_, 2));
    const float radius = static_cast<float>(ScaleForDpi(hwnd_, 16));
    solidBrush_->SetColor(ToColorF(RGB(255, 255, 255), 0.42f));
    renderTarget_->DrawRoundedRectangle(
        D2D1::RoundedRect(ToRectF(inner), radius, radius),
        solidBrush_.Get(),
        static_cast<float>(ScaleForDpi(hwnd_, 1)));

    solidBrush_->SetColor(ToColorF(RGB(255, 255, 255), 0.55f));
    renderTarget_->DrawLine(
        D2D1::Point2F(static_cast<float>(inner.left + radius), static_cast<float>(inner.top)),
        D2D1::Point2F(static_cast<float>(inner.right - radius), static_cast<float>(inner.top)),
        solidBrush_.Get(),
        static_cast<float>(ScaleForDpi(hwnd_, 1)));
    renderTarget_->DrawLine(
        D2D1::Point2F(static_cast<float>(inner.left), static_cast<float>(inner.top + radius)),
        D2D1::Point2F(static_cast<float>(inner.left), static_cast<float>(inner.bottom - radius)),
        solidBrush_.Get(),
        static_cast<float>(ScaleForDpi(hwnd_, 1)));
}

void AppBarWindow::DiscardLayeredSurface() {
    if (layeredDc_ != nullptr && layeredPreviousBitmap_ != nullptr) {
        SelectObject(layeredDc_, layeredPreviousBitmap_);
    }
    if (layeredBitmap_ != nullptr) {
        DeleteObject(layeredBitmap_);
    }
    if (layeredDc_ != nullptr) {
        DeleteDC(layeredDc_);
    }
    layeredDc_ = nullptr;
    layeredBitmap_ = nullptr;
    layeredPreviousBitmap_ = nullptr;
    layeredBits_ = nullptr;
    layeredSurfaceSize_ = {};
}

bool AppBarWindow::EnsureLayeredSurface(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (layeredDc_ != nullptr
        && layeredSurfaceSize_.cx == width
        && layeredSurfaceSize_.cy == height) {
        return true;
    }

    DiscardLayeredSurface();

    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr) {
        return false;
    }

    layeredDc_ = CreateCompatibleDC(screenDc);
    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    layeredBitmap_ = CreateDIBSection(
        screenDc,
        &bitmapInfo,
        DIB_RGB_COLORS,
        &layeredBits_,
        nullptr,
        0);
    ReleaseDC(nullptr, screenDc);

    if (layeredDc_ == nullptr || layeredBitmap_ == nullptr || layeredBits_ == nullptr) {
        DiscardLayeredSurface();
        return false;
    }

    layeredPreviousBitmap_ = static_cast<HBITMAP>(SelectObject(layeredDc_, layeredBitmap_));
    layeredSurfaceSize_.cx = width;
    layeredSurfaceSize_.cy = height;
    return true;
}

void AppBarWindow::RenderLayeredSurface() {
    if (hwnd_ == nullptr) {
        return;
    }

    RECT clientRect = {};
    GetClientRect(hwnd_, &clientRect);
    const int width = RectWidth(clientRect);
    const int height = RectHeight(clientRect);
    if (!EnsureLayeredSurface(width, height)) {
        return;
    }

    std::memset(layeredBits_, 0, static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    Paint(layeredDc_);

    RECT windowRect = {};
    GetWindowRect(hwnd_, &windowRect);
    POINT destination = { windowRect.left, windowRect.top };
    POINT source = { 0, 0 };
    SIZE size = { width, height };
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    HDC screenDc = GetDC(nullptr);
    if (screenDc != nullptr) {
        UpdateLayeredWindow(
            hwnd_,
            screenDc,
            &destination,
            &size,
            layeredDc_,
            &source,
            0,
            &blend,
            ULW_ALPHA);
        ReleaseDC(nullptr, screenDc);
    }
}

bool AppBarWindow::IsPointInsideBubble(POINT clientPoint) const {
    RECT clientRect = {};
    GetClientRect(hwnd_, &clientRect);
    if (presentationState_ == codex_widget::PresentationState::Bubble) {
        return PtInRect(&clientRect, clientPoint) != FALSE;
    }

    const int size = GetBubbleSizeForState();
    const RECT bubbleRect = MakeRect(
        clientRect.right - size,
        clientRect.top,
        clientRect.right,
        clientRect.top + size);
    return PtInRect(&bubbleRect, clientPoint) != FALSE;
}

AppBarWindow::DragMode AppBarWindow::HitTestDragMode(POINT clientPoint) const {
    if (lockPosition_) {
        return DragMode::None;
    }
    if (taskbarMode_) {
        return DragMode::None;
    }
    if (presentationState_ == codex_widget::PresentationState::Bubble) {
        return DragMode::Move;
    }
    if (IsPointInsideBubble(clientPoint)) {
        return DragMode::Move;
    }

    RECT clientRect = {};
    GetClientRect(hwnd_, &clientRect);
    const int bubbleSize = GetBubbleSizeForState();
    clientRect.right -= bubbleSize + ScaleForDpi(hwnd_, kPanelBubbleGap);
    const int grip = ScaleForDpi(hwnd_, kResizeGrip);
    const bool nearRight = clientPoint.x >= clientRect.right - grip;
    const bool nearBottom = clientPoint.y >= clientRect.bottom - grip;
    if (nearRight && nearBottom) {
        return DragMode::ResizeCorner;
    }
    if (nearRight) {
        return DragMode::ResizeRight;
    }
    if (nearBottom) {
        return DragMode::ResizeBottom;
    }
    return DragMode::Move;
}

void AppBarWindow::BeginDrag(DragMode mode, POINT screenPoint) {
    if (mode == DragMode::None) {
        return;
    }

    dragMode_ = mode;
    dragStartPoint_ = screenPoint;
    dragStartRect_ = savedRect_;
    SetCapture(hwnd_);
}

void AppBarWindow::UpdateDrag(POINT screenPoint) {
    if (dragMode_ == DragMode::None) {
        return;
    }

    RECT rect = dragStartRect_;
    const int deltaX = screenPoint.x - dragStartPoint_.x;
    const int deltaY = screenPoint.y - dragStartPoint_.y;

    switch (dragMode_) {
        case DragMode::Move:
            OffsetRect(&rect, deltaX, deltaY);
            break;
        case DragMode::ResizeRight:
            rect.right += deltaX;
            break;
        case DragMode::ResizeBottom:
            rect.bottom += deltaY;
            break;
        case DragMode::ResizeCorner:
            rect.right += deltaX;
            rect.bottom += deltaY;
            break;
        case DragMode::None:
            break;
    }

    savedRect_ = dragMode_ == DragMode::Move
        ? ClampPanelGroupRect(rect)
        : ClampRectToDesktop(rect);
    UpdateWindowBounds(true);
}

void AppBarWindow::EndDrag(bool saveSettings) {
    dragMode_ = DragMode::None;
    ReleaseCapture();
    if (saveSettings) {
        SaveSettings();
    }
}

void AppBarWindow::ActivateBubbleClick() {
    const bool collapsingPinnedBubble =
        presentationState_ == codex_widget::PresentationState::PinnedExpanded;
    hoverSuppressedUntilCursorLeavesBubble_ = collapsingPinnedBubble;
    SetPresentationState(codex_widget::TransitionOnBubbleClick(presentationState_));
}

void AppBarWindow::RequestRefresh(bool force) {
    bool expected = false;
    if (!force && !refreshInFlight_.compare_exchange_strong(expected, true)) {
        return;
    }
    if (force && refreshInFlight_.exchange(true)) {
        return;
    }

    refreshCountdownSeconds_ = refreshIntervalSeconds_;
    RestartRefreshTimer();
    RenderLayeredSurface();

    const HWND target = hwnd_;
    std::thread([this, target]() {
        auto* result = new UsageSnapshot(fetcher_.Fetch());
        PostMessageW(target, kUsageUpdatedMessage, 0, reinterpret_cast<LPARAM>(result));
    }).detach();
}

void AppBarWindow::OnUsageUpdated(UsageSnapshot* snapshot) {
    std::unique_ptr<UsageSnapshot> holder(snapshot);
    refreshInFlight_ = false;
    lastRefreshCompletedUnixSeconds_ = static_cast<long long>(std::time(nullptr));
    lastRefreshSucceeded_ = false;
    if (snapshot != nullptr) {
        snapshot_ = *snapshot;
        lastRefreshSucceeded_ = snapshot_.success;
        if (snapshot_.success) {
            lastSuccessfulRefreshUnixSeconds_ = static_cast<long long>(std::time(nullptr));
        }
    }
    // Credit-row count affects preferred height; keep geometry tight.
    if (!taskbarMode_ && !simpleMode_) {
        UpdateWindowBounds(true);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    RenderLayeredSurface();
}

void AppBarWindow::RequestLatestReleaseCheck(bool force) {
    bool expected = false;
    if (!force && !releaseCheckInFlight_.compare_exchange_strong(expected, true)) {
        return;
    }
    if (force && releaseCheckInFlight_.exchange(true)) {
        return;
    }

    releaseCheckCountdownSeconds_ = kReleaseCheckIntervalSeconds;

    const HWND target = hwnd_;
    std::thread([this, target]() {
        auto* result = new ReleaseVersionInfo(fetcher_.FetchLatestRelease());
        PostMessageW(target, kReleaseVersionUpdatedMessage, 0, reinterpret_cast<LPARAM>(result));
    }).detach();
}

void AppBarWindow::OnLatestReleaseChecked(ReleaseVersionInfo* info) {
    std::unique_ptr<ReleaseVersionInfo> holder(info);
    releaseCheckInFlight_ = false;
    lastReleaseCheckUnixSeconds_ = static_cast<long long>(std::time(nullptr));

    if (info != nullptr) {
        hasReleaseCheckResult_ = info->success;
        releaseCheckErrorMessage_ = info->errorMessage;
        if (info->success) {
            latestReleaseTag_ = info->latestTag;
            updateAvailable_ = CompareVersions(kCurrentVersion, latestReleaseTag_) < 0;
        } else {
            latestReleaseTag_.clear();
            updateAvailable_ = false;
        }
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::wstring AppBarWindow::BuildResetCreditsSummaryText() const {
    if (!snapshot_.success) {
            return LocalizeText(L"Reset credits: --", L"額度重設卡：--");
    }
    if (!snapshot_.resetCredits.fetched) {
        if (!snapshot_.resetCredits.errorMessage.empty()) {
            return LocalizeText(L"Reset credits: unavailable", L"額度重設卡：目前無法取得");
        }
        return LocalizeText(L"Reset credits: --", L"額度重設卡：--");
    }
    return std::wstring(LocalizeText(L"Reset credits: ", L"額度重設卡："))
        + std::to_wstring(snapshot_.resetCredits.availableCount)
        + LocalizeText(L" available", L" 張可用");
}

std::wstring AppBarWindow::BuildResetCreditsExpiryText() const {
    if (!snapshot_.success || !snapshot_.resetCredits.fetched) {
        return LocalizeText(L"Expiry: --", L"到期時間：--");
    }
    if (snapshot_.resetCredits.availableCount <= 0) {
        return LocalizeText(L"Expiry: none", L"到期時間：無");
    }
    if (!snapshot_.resetCredits.hasNextExpiry) {
        return LocalizeText(L"Next expiry: none", L"最近到期：無期限");
    }
    return std::wstring(LocalizeText(L"Next expiry: ", L"最近到期："))
        + FormatDateTime(snapshot_.resetCredits.nextExpiresAtUnixSeconds);
}

bool AppBarWindow::TryHandleControlClick(POINT clientPoint) {
    if (settingsOpen_) {
        if (closeButtonRect_.right > closeButtonRect_.left
            && PtInRect(&closeButtonRect_, clientPoint)) {
            CloseSettings();
            return true;
        }
        if (settingsSliderRect_.right > settingsSliderRect_.left
            && PtInRect(&settingsSliderRect_, clientPoint)) {
            settingsDragging_ = true;
            SetCapture(hwnd_);
            UpdateTransparencyFromPoint(clientPoint);
            return true;
        }
        return false;
    }

    if (closeButtonRect_.right > closeButtonRect_.left
        && PtInRect(&closeButtonRect_, clientPoint)) {
        SetPresentationState(codex_widget::PresentationState::Bubble);
        return true;
    }

    return TryHandleRefreshButtonClick(clientPoint);
}

bool AppBarWindow::TryHandleRefreshButtonClick(POINT clientPoint) {
    if (taskbarMode_) {
        return false;
    }

    if (refreshButtonRect_.right > refreshButtonRect_.left && PtInRect(&refreshButtonRect_, clientPoint)) {
        RequestRefresh(true);
        return true;
    }
    return false;
}

void AppBarWindow::Paint(HDC hdc) {
    RECT clientRect = {};
    GetClientRect(hwnd_, &clientRect);

    if (RectWidth(clientRect) <= 0 || RectHeight(clientRect) <= 0) {
        return;
    }

    if (FAILED(CreateDeviceResources())) {
        return;
    }

    if (FAILED(renderTarget_->BindDC(hdc, &clientRect))) {
        DiscardDeviceResources();
        return;
    }

    const UINT dpi = GetDpiForWindow(hwnd_);
    renderTarget_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
    renderTarget_->SetTransform(D2D1::Matrix3x2F::Scale(96.0f / dpi, 96.0f / dpi));
    renderTarget_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    renderTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

    renderTarget_->BeginDraw();
    renderTarget_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    PaintContent(clientRect);
    const HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
    }
}

void AppBarWindow::PaintContent(const RECT& clientRect) {
    refreshButtonRect_ = {};
    const PaceInfo pace = BuildPaceInfo(snapshot_);
    const int padX = ScaleForDpi(hwnd_, kHorizontalPadding);
    const int padY = ScaleForDpi(hwnd_, kVerticalPadding);
    const int meterHeight = ScaleForDpi(hwnd_, 12);
    const int footerTop = ScaleForDpi(hwnd_, 10);
    const int footerGap = ScaleForDpi(hwnd_, 12);
    const int sectionGap = ScaleForDpi(hwnd_, 10);
    const int heroHeight = ScaleForDpi(hwnd_, 76);
    const int metricRowHeight = ScaleForDpi(hwnd_, 52);
    (void)meterHeight;
    (void)footerTop;
    (void)footerGap;
    (void)sectionGap;
    (void)heroHeight;
    (void)metricRowHeight;

    // Background follows the lower remaining of 5h/weekly (100 green -> 0 red).
    const bool hasUsage = snapshot_.success && HasAvailableUsageWindow(snapshot_);
    const int lowestRemaining = hasUsage ? LowestAvailableRemainingPercent(snapshot_) : 100;
    const int transparencyPercent = codex_widget::ClampTransparencyPercent(glassTransparencyPercent_);
    const float glassAlpha = 0.46f - static_cast<float>(transparencyPercent) * 0.0025f;
    const COLORREF background = lightTheme_ ? RGB(239, 244, 255) : RGB(34, 47, 83);
    const COLORREF glassSurface = lightTheme_ ? RGB(241, 246, 255) : RGB(38, 54, 96);
    const COLORREF glassCard = lightTheme_ ? RGB(255, 255, 255) : RGB(67, 83, 131);
    const COLORREF glassLavender = lightTheme_ ? RGB(237, 234, 255) : RGB(72, 66, 128);
    const COLORREF textPrimary = lightTheme_ ? RGB(27, 35, 68) : RGB(244, 247, 255);
    const COLORREF textSecondary = lightTheme_ ? RGB(86, 97, 130) : RGB(186, 197, 230);
    const COLORREF border = lightTheme_ ? RGB(255, 255, 255) : RGB(177, 196, 255);
    const COLORREF shadow = lightTheme_ ? RGB(73, 93, 167) : RGB(5, 10, 31);
    const COLORREF heroValue = hasUsage
        ? ColorForRemainingPercent(lowestRemaining, false)
        : textPrimary;
    const COLORREF trackColor = lightTheme_ ? RGB(205, 216, 245) : RGB(75, 91, 137);
    const COLORREF budgetMarkerColor = lightTheme_ ? RGB(55, 70, 117) : RGB(235, 242, 255);
    auto fillRect = [&](const RECT& rect, COLORREF color) {
        solidBrush_->SetColor(ToColorF(color, glassAlpha));
        renderTarget_->FillRoundedRectangle(
            D2D1::RoundedRect(ToRectF(rect), static_cast<float>(ScaleForDpi(hwnd_, 10)), static_cast<float>(ScaleForDpi(hwnd_, 10))),
            solidBrush_.Get());
    };

    auto drawRectBorder = [&](const RECT& rect, COLORREF color) {
        solidBrush_->SetColor(ToColorF(color, std::min(0.95f, glassAlpha + 0.18f)));
        renderTarget_->DrawRoundedRectangle(
            D2D1::RoundedRect(ToRectF(rect), static_cast<float>(ScaleForDpi(hwnd_, 10)), static_cast<float>(ScaleForDpi(hwnd_, 10))),
            solidBrush_.Get(),
            static_cast<float>(ScaleForDpi(hwnd_, 1)));
    };

    auto drawGlassPanel = [&](const RECT& rect) {
        const COLORREF topTint = lightTheme_ ? RGB(248, 246, 255) : RGB(57, 67, 111);
        const COLORREF bottomTint = lightTheme_ ? RGB(224, 220, 255) : RGB(53, 48, 96);
        DrawGlassSurface(rect, topTint, bottomTint, std::min(0.52f, glassAlpha + 0.04f));
        DrawGlassEdgeHighlight(rect);
    };

    auto fillCard = [&](const RECT& rect, COLORREF tint) {
        DrawGlassCard(rect, tint, std::min(0.72f, glassAlpha + 0.24f));
    };

    auto fillEllipse = [&](const D2D1_ELLIPSE& ellipse, COLORREF color, float alpha) {
        solidBrush_->SetColor(ToColorF(color, alpha));
        renderTarget_->FillEllipse(ellipse, solidBrush_.Get());
    };

    auto drawLine = [&](D2D1_POINT_2F start, D2D1_POINT_2F end, COLORREF color, float width, float alpha) {
        solidBrush_->SetColor(ToColorF(color, alpha));
        renderTarget_->DrawLine(start, end, solidBrush_.Get(), width);
    };

    auto drawVectorIcon = [&](const RECT& iconRect, int iconKind, COLORREF color) {
        const float left = static_cast<float>(iconRect.left);
        const float top = static_cast<float>(iconRect.top);
        const float width = static_cast<float>(RectWidth(iconRect));
        const float height = static_cast<float>(RectHeight(iconRect));
        const D2D1_POINT_2F center = D2D1::Point2F(left + width * 0.5f, top + height * 0.5f);
        const float stroke = std::max(1.5f, static_cast<float>(ScaleForDpi(hwnd_, 1)));
        const float inner = std::min(width, height) * 0.30f;

        solidBrush_->SetColor(ToColorF(color, 0.96f));
        switch (iconKind) {
            case 1: {  // clock / five-hour window
                renderTarget_->DrawEllipse(
                    D2D1::Ellipse(center, inner, inner), solidBrush_.Get(), stroke);
                drawLine(
                    D2D1::Point2F(center.x, center.y),
                    D2D1::Point2F(center.x, center.y - inner * 0.55f),
                    color, stroke, 0.96f);
                drawLine(
                    D2D1::Point2F(center.x, center.y),
                    D2D1::Point2F(center.x + inner * 0.48f, center.y + inner * 0.20f),
                    color, stroke, 0.96f);
                break;
            }
            case 2: {  // weekly cycle
                renderTarget_->DrawEllipse(
                    D2D1::Ellipse(center, inner, inner), solidBrush_.Get(), stroke);
                drawLine(
                    D2D1::Point2F(center.x - inner * 0.65f, center.y - inner * 0.08f),
                    D2D1::Point2F(center.x - inner * 0.12f, center.y - inner * 0.08f),
                    color, stroke, 0.96f);
                drawLine(
                    D2D1::Point2F(center.x - inner * 0.12f, center.y - inner * 0.08f),
                    D2D1::Point2F(center.x - inner * 0.32f, center.y - inner * 0.36f),
                    color, stroke, 0.96f);
                drawLine(
                    D2D1::Point2F(center.x + inner * 0.18f, center.y + inner * 0.42f),
                    D2D1::Point2F(center.x + inner * 0.65f, center.y + inner * 0.42f),
                    color, stroke, 0.96f);
                break;
            }
            case 3: {  // pace waveform
                drawLine(
                    D2D1::Point2F(left + width * 0.08f, center.y),
                    D2D1::Point2F(left + width * 0.30f, center.y),
                    color, stroke, 0.96f);
                drawLine(
                    D2D1::Point2F(left + width * 0.30f, center.y),
                    D2D1::Point2F(left + width * 0.42f, top + height * 0.22f),
                    color, stroke, 0.96f);
                drawLine(
                    D2D1::Point2F(left + width * 0.42f, top + height * 0.22f),
                    D2D1::Point2F(left + width * 0.57f, top + height * 0.78f),
                    color, stroke, 0.96f);
                drawLine(
                    D2D1::Point2F(left + width * 0.57f, top + height * 0.78f),
                    D2D1::Point2F(left + width * 0.70f, center.y),
                    color, stroke, 0.96f);
                drawLine(
                    D2D1::Point2F(left + width * 0.70f, center.y),
                    D2D1::Point2F(left + width * 0.92f, center.y),
                    color, stroke, 0.96f);
                break;
            }
            case 4: {  // reset ticket
                renderTarget_->DrawRoundedRectangle(
                    D2D1::RoundedRect(
                        D2D1::RectF(left + width * 0.12f, top + height * 0.23f,
                            left + width * 0.88f, top + height * 0.77f),
                        height * 0.16f,
                        height * 0.16f),
                    solidBrush_.Get(), stroke);
                drawLine(
                    D2D1::Point2F(left + width * 0.30f, top + height * 0.50f),
                    D2D1::Point2F(left + width * 0.70f, top + height * 0.50f),
                    color, stroke, 0.96f);
                drawLine(
                    D2D1::Point2F(left + width * 0.62f, top + height * 0.39f),
                    D2D1::Point2F(left + width * 0.74f, top + height * 0.50f),
                    color, stroke, 0.96f);
                drawLine(
                    D2D1::Point2F(left + width * 0.74f, top + height * 0.50f),
                    D2D1::Point2F(left + width * 0.62f, top + height * 0.61f),
                    color, stroke, 0.96f);
                break;
            }
            case 5: {  // usage chart
                drawLine(
                    D2D1::Point2F(left + width * 0.16f, top + height * 0.82f),
                    D2D1::Point2F(left + width * 0.16f, top + height * 0.42f),
                    color, stroke, 0.96f);
                drawLine(
                    D2D1::Point2F(left + width * 0.44f, top + height * 0.82f),
                    D2D1::Point2F(left + width * 0.44f, top + height * 0.24f),
                    color, stroke, 0.96f);
                drawLine(
                    D2D1::Point2F(left + width * 0.72f, top + height * 0.82f),
                    D2D1::Point2F(left + width * 0.72f, top + height * 0.56f),
                    color, stroke, 0.96f);
                drawLine(
                    D2D1::Point2F(left + width * 0.08f, top + height * 0.82f),
                    D2D1::Point2F(left + width * 0.90f, top + height * 0.82f),
                    color, stroke, 0.96f);
                break;
            }
            default:
                break;
        }
    };

    auto drawGlassIcon = [&](const RECT& iconRect) {
        const float left = static_cast<float>(iconRect.left);
        const float top = static_cast<float>(iconRect.top);
        const float width = static_cast<float>(RectWidth(iconRect));
        const float height = static_cast<float>(RectHeight(iconRect));
        const D2D1_POINT_2F center = D2D1::Point2F(left + width * 0.5f, top + height * 0.5f);
        fillEllipse(D2D1::Ellipse(center, width * 0.46f, height * 0.46f), RGB(55, 107, 255), 0.22f);
        fillEllipse(D2D1::Ellipse(D2D1::Point2F(center.x - width * 0.14f, center.y + height * 0.03f), width * 0.28f, height * 0.29f), RGB(68, 123, 255), 0.80f);
        fillEllipse(D2D1::Ellipse(D2D1::Point2F(center.x + width * 0.12f, center.y - height * 0.08f), width * 0.30f, height * 0.32f), RGB(91, 139, 255), 0.78f);
        fillEllipse(D2D1::Ellipse(D2D1::Point2F(center.x, center.y + height * 0.14f), width * 0.34f, height * 0.25f), RGB(48, 89, 235), 0.75f);
        const float stroke = std::max(2.0f, width * 0.055f);
        const COLORREF mark = RGB(247, 250, 255);
        drawLine(
            D2D1::Point2F(center.x - width * 0.19f, center.y - height * 0.16f),
            D2D1::Point2F(center.x + width * 0.02f, center.y),
            mark,
            stroke,
            0.92f);
        drawLine(
            D2D1::Point2F(center.x + width * 0.02f, center.y),
            D2D1::Point2F(center.x - width * 0.19f, center.y + height * 0.16f),
            mark,
            stroke,
            0.92f);
        drawLine(
            D2D1::Point2F(center.x + width * 0.13f, center.y + height * 0.16f),
            D2D1::Point2F(center.x + width * 0.31f, center.y + height * 0.16f),
            mark,
            stroke,
            0.92f);
        drawLine(
            D2D1::Point2F(center.x - width * 0.24f, center.y - height * 0.33f),
            D2D1::Point2F(center.x + width * 0.19f, center.y - height * 0.35f),
            RGB(255, 255, 255),
            std::max(1.0f, stroke * 0.45f),
            0.55f);
    };

    auto drawCodexAsset = [&](const RECT& iconRect, float opacity) {
        if (codexIconBitmap_) {
            DrawAssetBitmap(codexIconBitmap_.Get(), iconRect, opacity);
        } else {
            drawGlassIcon(iconRect);
        }
    };

    auto drawCloseGlyph = [&](const RECT& closeRect) {
        const float inset = static_cast<float>(ScaleForDpi(hwnd_, 8));
        drawLine(
            D2D1::Point2F(static_cast<float>(closeRect.left) + inset, static_cast<float>(closeRect.top) + inset),
            D2D1::Point2F(static_cast<float>(closeRect.right) - inset, static_cast<float>(closeRect.bottom) - inset),
            textSecondary,
            static_cast<float>(ScaleForDpi(hwnd_, 2)),
            0.9f);
        drawLine(
            D2D1::Point2F(static_cast<float>(closeRect.right) - inset, static_cast<float>(closeRect.top) + inset),
            D2D1::Point2F(static_cast<float>(closeRect.left) + inset, static_cast<float>(closeRect.bottom) - inset),
            textSecondary,
            static_cast<float>(ScaleForDpi(hwnd_, 2)),
            0.9f);
    };

    auto drawTextBlock = [&](IDWriteTextFormat* format,
                             const std::wstring& text,
                             const RECT& rect,
                             COLORREF color,
                             DWRITE_TEXT_ALIGNMENT textAlignment,
                             DWRITE_PARAGRAPH_ALIGNMENT paragraphAlignment,
                             DWRITE_WORD_WRAPPING wrapping,
                             bool trimEllipsis) {
        format->SetTextAlignment(textAlignment);
        format->SetParagraphAlignment(paragraphAlignment);
        format->SetWordWrapping(wrapping);

        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        const float layoutWidth = std::max(1.0f, static_cast<float>(RectWidth(rect)));
        const float layoutHeight = std::max(1.0f, static_cast<float>(RectHeight(rect)));
        if (FAILED(dwriteFactory_->CreateTextLayout(
                text.c_str(),
                static_cast<UINT32>(text.size()),
                format,
                layoutWidth,
                layoutHeight,
                layout.GetAddressOf()))) {
            return;
        }
        ApplyFontRuns(layout.Get(), text);

        if (trimEllipsis) {
            Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsisSign;
            const DWRITE_TRIMMING trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(format, ellipsisSign.GetAddressOf()))) {
                layout->SetTrimming(&trimming, ellipsisSign.Get());
            }
        }

        solidBrush_->SetColor(ToColorF(color));
        renderTarget_->DrawTextLayout(
            D2D1::Point2F(static_cast<float>(rect.left), static_cast<float>(rect.top)),
            layout.Get(),
            solidBrush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    auto measureTextWidth = [&](IDWriteTextFormat* format, const std::wstring& text) -> float {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(
                text.c_str(),
                static_cast<UINT32>(text.size()),
                format,
                4096.0f,
                256.0f,
                layout.GetAddressOf()))) {
            return 0.0f;
        }
        ApplyFontRuns(layout.Get(), text);

        DWRITE_TEXT_METRICS metrics = {};
        if (FAILED(layout->GetMetrics(&metrics))) {
            return 0.0f;
        }
        return metrics.widthIncludingTrailingWhitespace;
    };

    if (taskbarMode_ && !settingsOpen_) {
        bubbleButtonRect_ = {};
        closeButtonRect_ = {};
        refreshButtonRect_ = {};
        settingsSliderRect_ = {};

        const int circleInset = ScaleForDpi(hwnd_, 2);
        const RECT circleRect = MakeRect(
            clientRect.left + circleInset,
            clientRect.top + circleInset,
            clientRect.right - circleInset,
            clientRect.bottom - circleInset);
        const float circleCenterX = static_cast<float>(circleRect.left + RectWidth(circleRect) / 2);
        const float circleCenterY = static_cast<float>(circleRect.top + RectHeight(circleRect) / 2);
        const float circleRadius = static_cast<float>(std::min(RectWidth(circleRect), RectHeight(circleRect))) * 0.5f;
        const D2D1_ELLIPSE shadowEllipse = D2D1::Ellipse(
            D2D1::Point2F(circleCenterX + static_cast<float>(ScaleForDpi(hwnd_, 2)),
                circleCenterY + static_cast<float>(ScaleForDpi(hwnd_, 4))),
            circleRadius,
            circleRadius);
        fillEllipse(shadowEllipse, shadow, 0.30f);

        const D2D1_ELLIPSE outerEllipse = D2D1::Ellipse(
            D2D1::Point2F(circleCenterX, circleCenterY), circleRadius, circleRadius);
        fillEllipse(outerEllipse, glassSurface, std::min(0.56f, glassAlpha + 0.10f));

        const int innerInset = ScaleForDpi(hwnd_, 8);
        const RECT innerCircleRect = MakeRect(
            circleRect.left + innerInset,
            circleRect.top + innerInset,
            circleRect.right - innerInset,
            circleRect.bottom - innerInset);
        const D2D1_ELLIPSE innerEllipse = D2D1::Ellipse(
            D2D1::Point2F(
                static_cast<float>(innerCircleRect.left + RectWidth(innerCircleRect) / 2),
                static_cast<float>(innerCircleRect.top + RectHeight(innerCircleRect) / 2)),
            static_cast<float>(RectWidth(innerCircleRect)) * 0.5f,
            static_cast<float>(RectHeight(innerCircleRect)) * 0.5f);
        fillEllipse(innerEllipse, lightTheme_ ? RGB(246, 248, 255) : RGB(49, 65, 110), 0.24f);

        const int centerY = circleRect.top + RectHeight(circleRect) / 2;
        const RECT topHalfClip = MakeRect(
            innerCircleRect.left,
            innerCircleRect.top,
            innerCircleRect.right,
            centerY);
        const RECT bottomHalfClip = MakeRect(
            innerCircleRect.left,
            centerY,
            innerCircleRect.right,
            innerCircleRect.bottom);
        renderTarget_->PushAxisAlignedClip(ToRectF(topHalfClip), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        fillEllipse(innerEllipse, lightTheme_ ? RGB(224, 239, 255) : RGB(43, 70, 125), 0.54f);
        renderTarget_->PopAxisAlignedClip();
        renderTarget_->PushAxisAlignedClip(ToRectF(bottomHalfClip), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        fillEllipse(innerEllipse, lightTheme_ ? RGB(238, 233, 255) : RGB(69, 59, 121), 0.58f);
        renderTarget_->PopAxisAlignedClip();

        solidBrush_->SetColor(ToColorF(border, std::min(0.95f, glassAlpha + 0.30f)));
        renderTarget_->DrawEllipse(outerEllipse, solidBrush_.Get(), static_cast<float>(ScaleForDpi(hwnd_, 1)));
        solidBrush_->SetColor(ToColorF(RGB(255, 255, 255), 0.28f));
        renderTarget_->DrawEllipse(innerEllipse, solidBrush_.Get(), static_cast<float>(ScaleForDpi(hwnd_, 1)));

        const bool hasUsage = snapshot_.success && HasAvailableUsageWindow(snapshot_);
        const bool exhausted = hasUsage && IsAnyUsageWindowExhausted(snapshot_);
        const bool warning = hasUsage && !exhausted && (IsAnyUsageWindowTight(snapshot_) || pace.isOver);
        const COLORREF statusColor = !hasUsage
            ? textSecondary
            : (exhausted ? (lightTheme_ ? RGB(196, 54, 32) : RGB(255, 144, 120))
                         : (warning ? (lightTheme_ ? RGB(184, 121, 38) : RGB(233, 180, 91))
                                    : (lightTheme_ ? RGB(21, 148, 78) : RGB(118, 216, 163))));
        const int textInset = std::max(
            ScaleForDpi(hwnd_, 10),
            RectHeight(circleRect) / 10);
        const int halfGap = std::max(ScaleForDpi(hwnd_, 1), RectHeight(circleRect) / 48);
        const RECT fiveHalf = MakeRect(
            circleRect.left + textInset,
            circleRect.top + textInset,
            circleRect.right - textInset,
            centerY - halfGap);
        const RECT weekHalf = MakeRect(
            circleRect.left + textInset,
            centerY + halfGap,
            circleRect.right - textInset,
            circleRect.bottom - textInset);
        auto drawTaskbarMetric = [&](const RECT& half, const wchar_t* label, const UsageWindow& window) {
            const int split = half.left + (RectWidth(half) * 35) / 100;
            const std::wstring value = window.available ? FormatPercent(window.remainingPercent) : L"--";
            drawTextBlock(textFormatKicker_.Get(), label,
                MakeRect(half.left, half.top, split, half.bottom), textSecondary,
                DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                DWRITE_WORD_WRAPPING_NO_WRAP, false);
            drawTextBlock(textFormatMetricLabel_.Get(), value,
                MakeRect(split, half.top, half.right, half.bottom), textPrimary,
                DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                DWRITE_WORD_WRAPPING_NO_WRAP, false);
        };
        drawTaskbarMetric(fiveHalf, LocalizeText(L"5 hr", L"5h"), snapshot_.fiveHour);
        drawTaskbarMetric(weekHalf, LocalizeText(L"Week", L"週"), snapshot_.weekly);
        drawLine(
            D2D1::Point2F(circleCenterX - static_cast<float>(ScaleForDpi(hwnd_, 44)), static_cast<float>(centerY)),
            D2D1::Point2F(circleCenterX + static_cast<float>(ScaleForDpi(hwnd_, 44)), static_cast<float>(centerY)),
            border, static_cast<float>(ScaleForDpi(hwnd_, 1)), 0.56f);
        fillEllipse(
            D2D1::Ellipse(
                D2D1::Point2F(circleCenterX, static_cast<float>(circleRect.bottom - ScaleForDpi(hwnd_, 8))),
                static_cast<float>(ScaleForDpi(hwnd_, 2)),
                static_cast<float>(ScaleForDpi(hwnd_, 2))),
            statusColor, 0.92f);
        return;
    }

    if (presentationState_ == codex_widget::PresentationState::Bubble) {
        bubbleButtonRect_ = clientRect;
        closeButtonRect_ = {};
        refreshButtonRect_ = {};
        const D2D1_ELLIPSE shadowEllipse = D2D1::Ellipse(
            D2D1::Point2F(
                static_cast<float>(clientRect.left + RectWidth(clientRect) / 2 + ScaleForDpi(hwnd_, 2)),
                static_cast<float>(clientRect.top + RectHeight(clientRect) / 2 + ScaleForDpi(hwnd_, 4))),
            static_cast<float>(RectWidth(clientRect) / 2 - ScaleForDpi(hwnd_, 3)),
            static_cast<float>(RectHeight(clientRect) / 2 - ScaleForDpi(hwnd_, 3)));
        fillEllipse(shadowEllipse, shadow, 0.28f);
        drawCodexAsset(MakeRect(
            clientRect.left + ScaleForDpi(hwnd_, 3),
            clientRect.top + ScaleForDpi(hwnd_, 3),
            clientRect.right - ScaleForDpi(hwnd_, 3),
            clientRect.bottom - ScaleForDpi(hwnd_, 3)),
            1.0f);
        return;
    }

    const bool usesFloatingBubble = !taskbarMode_;
    const int bubbleSize = usesFloatingBubble ? GetBubbleSizeForState() : 0;
    const int bubbleGap = usesFloatingBubble ? ScaleForDpi(hwnd_, kPanelBubbleGap) : 0;
    const RECT panelRect = MakeRect(
        clientRect.left,
        clientRect.top,
        clientRect.right - bubbleSize - bubbleGap,
        clientRect.bottom);
    bubbleButtonRect_ = usesFloatingBubble
        ? MakeRect(clientRect.right - bubbleSize, clientRect.top, clientRect.right, clientRect.top + bubbleSize)
        : RECT{};
    closeButtonRect_ = MakeRect(
        panelRect.right - ScaleForDpi(hwnd_, 42),
        clientRect.top + ScaleForDpi(hwnd_, 12),
        panelRect.right - ScaleForDpi(hwnd_, 10),
        clientRect.top + ScaleForDpi(hwnd_, 44));
    refreshButtonRect_ = {};

    fillRect(MakeRect(panelRect.left + ScaleForDpi(hwnd_, 3), panelRect.top + ScaleForDpi(hwnd_, 5),
        panelRect.right + ScaleForDpi(hwnd_, 3), panelRect.bottom + ScaleForDpi(hwnd_, 5)), shadow);
    drawGlassPanel(panelRect);
    if (usesFloatingBubble) {
        drawCodexAsset(bubbleButtonRect_, presentationState_ == codex_widget::PresentationState::HoverExpanded ? 1.0f : 0.96f);
    }
    drawCloseGlyph(closeButtonRect_);

    if (settingsOpen_) {
        const int settingsPad = ScaleForDpi(hwnd_, 20);
        const int titleHeight = ScaleForDpi(hwnd_, 28);
        drawTextBlock(textFormatTitle_.Get(), LocalizeText(L"Appearance", L"外觀設定"),
            MakeRect(clientRect.left + settingsPad, clientRect.top + ScaleForDpi(hwnd_, 24),
                panelRect.right - settingsPad, clientRect.top + ScaleForDpi(hwnd_, 24) + titleHeight),
            textPrimary,
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
            DWRITE_WORD_WRAPPING_NO_WRAP, false);

        const int sliderTop = clientRect.top + ScaleForDpi(hwnd_, 82);
        settingsSliderRect_ = MakeRect(
            clientRect.left + settingsPad,
            sliderTop,
            panelRect.right - settingsPad,
            sliderTop + ScaleForDpi(hwnd_, 22));
        const int trackTop = sliderTop + ScaleForDpi(hwnd_, 7);
        RECT trackRect = MakeRect(settingsSliderRect_.left, trackTop,
            settingsSliderRect_.right, trackTop + ScaleForDpi(hwnd_, 8));
        fillRect(trackRect, trackColor);
        const int valueRange = 60;
        const int valueOffset = glassTransparencyPercent_ - 20;
        RECT valueRect = trackRect;
        valueRect.right = valueRect.left + (RectWidth(trackRect) * valueOffset / valueRange);
        if (valueRect.right > valueRect.left) {
            fillRect(valueRect, heroValue);
        }
        const float knobX = static_cast<float>(trackRect.left)
            + static_cast<float>(RectWidth(trackRect) * valueOffset) / static_cast<float>(valueRange);
        fillEllipse(
            D2D1::Ellipse(
                D2D1::Point2F(knobX, static_cast<float>(trackTop + ScaleForDpi(hwnd_, 4))),
                static_cast<float>(ScaleForDpi(hwnd_, 9)),
                static_cast<float>(ScaleForDpi(hwnd_, 9))),
            RGB(255, 255, 255),
            0.96f);
        const std::wstring valueText = std::to_wstring(glassTransparencyPercent_) + L"%";
        drawTextBlock(textFormatMetricValue_.Get(), valueText,
            MakeRect(clientRect.left + settingsPad, sliderTop + ScaleForDpi(hwnd_, 30),
                panelRect.right - settingsPad, sliderTop + ScaleForDpi(hwnd_, 62)),
            heroValue,
            DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
            DWRITE_WORD_WRAPPING_NO_WRAP, false);
        drawTextBlock(textFormatFoot_.Get(),
            LocalizeText(L"Higher values make the glass more transparent", L"數值越高，玻璃越透明"),
            MakeRect(clientRect.left + settingsPad, clientRect.bottom - ScaleForDpi(hwnd_, 38),
                panelRect.right - settingsPad, clientRect.bottom - ScaleForDpi(hwnd_, 16)),
            textSecondary,
            DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
            DWRITE_WORD_WRAPPING_NO_WRAP, true);
        refreshButtonRect_ = {};
        return;
    }

    if (simpleMode_) {
        const bool loadFailed = !snapshot_.success && !snapshot_.errorMessage.empty();
        const bool hasSimpleUsage = snapshot_.success && HasAvailableUsageWindow(snapshot_);
        const bool exhausted = hasSimpleUsage && IsAnyUsageWindowExhausted(snapshot_);
        const bool warning = hasSimpleUsage && !exhausted && (IsAnyUsageWindowTight(snapshot_) || pace.isOver);
        const wchar_t* statusText = !hasSimpleUsage
            ? (loadFailed
                ? LocalizeText(L"Failed", L"載入失敗")
                : LocalizeText(L"Loading", L"載入中"))
            : (exhausted
                ? LocalizeText(L"Exhausted", L"已用罄")
                : (warning ? LocalizeText(L"Tight", L"吃緊") : LocalizeText(L"Normal", L"正常")));
        const COLORREF statusColor = !hasSimpleUsage
            ? (loadFailed
                ? (lightTheme_ ? RGB(196, 54, 32) : RGB(255, 144, 120))
                : textSecondary)
            : (exhausted ? (lightTheme_ ? RGB(196, 54, 32) : RGB(255, 144, 120))
                         : (warning ? (lightTheme_ ? RGB(184, 121, 38) : RGB(233, 180, 91))
                                    : (lightTheme_ ? RGB(21, 148, 78) : RGB(118, 216, 163))));
        const COLORREF dayCard = lightTheme_ ? RGB(224, 239, 255) : RGB(43, 70, 125);
        const COLORREF weekCard = lightTheme_ ? RGB(238, 233, 255) : RGB(69, 59, 121);
        const std::wstring versionStatusText = GetVersionStatusText(true);
        const int topBandHeight = ScaleForDpi(hwnd_, 34);
        const int innerPad = ScaleForDpi(hwnd_, 12);
        const int cardGap = ScaleForDpi(hwnd_, 10);
        const int footerHeight = ScaleForDpi(hwnd_, 16);

        RECT titleRect = MakeRect(clientRect.left + innerPad, clientRect.top + ScaleForDpi(hwnd_, 6),
            panelRect.right - innerPad - ScaleForDpi(hwnd_, 66), clientRect.top + topBandHeight);
        RECT statusRect = MakeRect(panelRect.right - innerPad - ScaleForDpi(hwnd_, 54), clientRect.top + ScaleForDpi(hwnd_, 8),
            panelRect.right - innerPad, clientRect.top + ScaleForDpi(hwnd_, 28));
        drawTextBlock(textFormatMetricValue_.Get(), LocalizeText(L"Remaining", L"剩餘額度"), titleRect, textPrimary,
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, true);
        drawTextBlock(textFormatMetricLabel_.Get(), statusText, statusRect, statusColor,
            DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, false);

        const int resetBandHeight = ScaleForDpi(hwnd_, 18);
        RECT cardsRect = MakeRect(clientRect.left + innerPad, clientRect.top + topBandHeight + ScaleForDpi(hwnd_, 2),
            panelRect.right - innerPad,
            clientRect.bottom - footerHeight - resetBandHeight - ScaleForDpi(hwnd_, 6));
        const int cardWidth = (RectWidth(cardsRect) - cardGap) / 2;
        RECT dayRect = MakeRect(cardsRect.left, cardsRect.top, cardsRect.left + cardWidth, cardsRect.bottom);
        RECT weekRect = MakeRect(dayRect.right + cardGap, cardsRect.top, cardsRect.right, cardsRect.bottom);
        fillCard(dayRect, dayCard);
        fillCard(weekRect, weekCard);

        const std::wstring dayValue = snapshot_.fiveHour.available ? FormatPercent(snapshot_.fiveHour.remainingPercent) : L"--";
        const std::wstring weekValue = snapshot_.weekly.available ? FormatPercent(snapshot_.weekly.remainingPercent) : L"--";
        RECT dayLabelRect = MakeRect(dayRect.left + innerPad, dayRect.top + ScaleForDpi(hwnd_, 8), dayRect.right - innerPad, dayRect.top + ScaleForDpi(hwnd_, 24));
        RECT dayValueRect = MakeRect(dayRect.left + innerPad, dayRect.top + ScaleForDpi(hwnd_, 24), dayRect.right - innerPad, dayRect.bottom - ScaleForDpi(hwnd_, 8));
        RECT weekLabelRect = MakeRect(weekRect.left + innerPad, weekRect.top + ScaleForDpi(hwnd_, 8), weekRect.right - innerPad, weekRect.top + ScaleForDpi(hwnd_, 24));
        RECT weekValueRect = MakeRect(weekRect.left + innerPad, weekRect.top + ScaleForDpi(hwnd_, 24), weekRect.right - innerPad, weekRect.bottom - ScaleForDpi(hwnd_, 8));
        drawTextBlock(textFormatMetricLabel_.Get(), LocalizeText(L"5h left", L"5 小時剩餘"), dayLabelRect, textSecondary,
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_WORD_WRAPPING_NO_WRAP, false);
        drawTextBlock(textFormatDelta_.Get(), dayValue, dayValueRect, textPrimary,
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, false);
        drawTextBlock(textFormatMetricLabel_.Get(), LocalizeText(L"Week left", L"本週剩餘"), weekLabelRect, textSecondary,
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_WORD_WRAPPING_NO_WRAP, false);
        drawTextBlock(textFormatDelta_.Get(), weekValue, weekValueRect, textPrimary,
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, false);

        const std::wstring resetSummary = BuildResetCreditsSummaryText();
        const std::wstring resetExpiry = BuildResetCreditsExpiryText();
        RECT resetSummaryRect = MakeRect(clientRect.left + innerPad, cardsRect.bottom + ScaleForDpi(hwnd_, 2),
            panelRect.right - innerPad, cardsRect.bottom + resetBandHeight);
        drawTextBlock(textFormatFoot_.Get(), resetSummary + L" · " + resetExpiry, resetSummaryRect, textSecondary,
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, true);

        const std::wstring refreshCountdownText = FormatRefreshCountdown(refreshCountdownSeconds_);
        const std::wstring refreshStatusText = GetRefreshStatusText();
        RECT footerLeftRect = MakeRect(clientRect.left + innerPad, clientRect.bottom - footerHeight - ScaleForDpi(hwnd_, 1),
            panelRect.left + RectWidth(panelRect) / 2, clientRect.bottom - ScaleForDpi(hwnd_, 1));
        RECT footerRightRect = MakeRect(panelRect.left + RectWidth(panelRect) / 2, clientRect.bottom - footerHeight - ScaleForDpi(hwnd_, 1),
            panelRect.right - innerPad, clientRect.bottom - ScaleForDpi(hwnd_, 1));
        drawTextBlock(textFormatFoot_.Get(), versionStatusText, footerLeftRect, updateAvailable_ ? heroValue : textSecondary,
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, false);
        drawTextBlock(textFormatFoot_.Get(), refreshStatusText + L" · " + refreshCountdownText, footerRightRect,
            refreshInFlight_ ? heroValue : textPrimary,
            DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, true);
        return;
    }

    // Full mode card layout: spacious grouped glass sections.
    {
        const int fullPad = ScaleForDpi(hwnd_, 24);
        const int cardGap = ScaleForDpi(hwnd_, 12);
        const int headerHeight = ScaleForDpi(hwnd_, 52);
        const int cardRadius = ScaleForDpi(hwnd_, 12);
        const COLORREF cardBlue = lightTheme_ ? RGB(222, 236, 255) : RGB(40, 64, 114);
        const COLORREF cardLavender = lightTheme_ ? RGB(236, 232, 255) : RGB(66, 56, 116);
        const COLORREF cardNeutral = lightTheme_ ? RGB(247, 246, 255) : RGB(52, 61, 101);

        auto drawOpaqueRounded = [&](const RECT& rect, COLORREF color, float alpha) {
            solidBrush_->SetColor(ToColorF(color, alpha));
            renderTarget_->FillRoundedRectangle(
                D2D1::RoundedRect(ToRectF(rect), static_cast<float>(cardRadius), static_cast<float>(cardRadius)),
                solidBrush_.Get());
        };

        auto drawOpaqueTextLine = [&](IDWriteTextFormat* format,
                                      const std::wstring& text,
                                      const RECT& rect,
                                      COLORREF color,
                                      DWRITE_TEXT_ALIGNMENT alignment) {
            drawTextBlock(
                format,
                text,
                rect,
                color,
                alignment,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                DWRITE_WORD_WRAPPING_NO_WRAP,
                true);
        };

        int y = panelRect.top + fullPad;
        const RECT headerRect = MakeRect(
            panelRect.left + fullPad,
            y,
            panelRect.right - fullPad,
            y + headerHeight);
        const RECT headerIconRect = MakeRect(
            headerRect.left,
            headerRect.top + ScaleForDpi(hwnd_, 4),
            headerRect.left + ScaleForDpi(hwnd_, 44),
            headerRect.top + ScaleForDpi(hwnd_, 48));
        drawCodexAsset(headerIconRect, 1.0f);

        const std::wstring emailText = snapshot_.email.empty() ? L"--" : snapshot_.email;
        const RECT emailRect = MakeRect(
            headerIconRect.right + ScaleForDpi(hwnd_, 12),
            headerRect.top,
            headerRect.right,
            headerRect.top + ScaleForDpi(hwnd_, 26));
        drawOpaqueTextLine(textFormatTitle_.Get(), emailText, emailRect, textPrimary, DWRITE_TEXT_ALIGNMENT_LEADING);
        const std::wstring planLine =
            std::wstring(LocalizeText(L"Plan: ", L"方案："))
            + FormatPlanDisplayName()
            + L"  ·  "
            + std::wstring(LocalizeText(L"Until ", L"到期 "))
            + (snapshot_.hasPlanUntil ? FormatDateTime(snapshot_.planUntilUnixSeconds) : LocalizeText(L"none", L"無期限"));
        const RECT planRect = MakeRect(
            emailRect.left,
            headerRect.top + ScaleForDpi(hwnd_, 26),
            headerRect.right,
            headerRect.bottom);
        drawOpaqueTextLine(textFormatFoot_.Get(), planLine, planRect, textSecondary, DWRITE_TEXT_ALIGNMENT_LEADING);
        y = headerRect.bottom + cardGap;

        auto drawSummaryCard = [&](const RECT& cardRect,
                                   const std::wstring& title,
                                   const UsageWindow& window,
                                   COLORREF tint,
                                   int iconKind) {
            fillCard(cardRect, tint);
            const int inner = ScaleForDpi(hwnd_, 14);
            const RECT iconRect = MakeRect(
                cardRect.left + inner,
                cardRect.top + ScaleForDpi(hwnd_, 8),
                cardRect.left + inner + ScaleForDpi(hwnd_, 26),
                cardRect.top + ScaleForDpi(hwnd_, 34));
            drawVectorIcon(iconRect, iconKind, textPrimary);
            const int textLeft = iconRect.right + ScaleForDpi(hwnd_, 7);
            drawOpaqueTextLine(
                textFormatMetricLabel_.Get(),
                title,
                MakeRect(textLeft, cardRect.top + ScaleForDpi(hwnd_, 10),
                    cardRect.right - inner, cardRect.top + ScaleForDpi(hwnd_, 28)),
                textSecondary,
                DWRITE_TEXT_ALIGNMENT_LEADING);
            const std::wstring remaining = window.available ? FormatPercent(window.remainingPercent) : L"--";
            drawOpaqueTextLine(
                textFormatDelta_.Get(),
                remaining,
                MakeRect(textLeft, cardRect.top + ScaleForDpi(hwnd_, 25),
                    cardRect.right - inner, cardRect.top + ScaleForDpi(hwnd_, 67)),
                textPrimary,
                DWRITE_TEXT_ALIGNMENT_LEADING);
            const std::wstring resetText = window.available
                ? std::wstring(LocalizeText(L"Reset in ", L"重設倒數 ")) + FormatDuration(window.resetAfterSeconds)
                : LocalizeText(L"Unavailable", L"目前無法取得");
            drawOpaqueTextLine(
                textFormatFoot_.Get(),
                resetText,
                MakeRect(cardRect.left + inner, cardRect.bottom - ScaleForDpi(hwnd_, 33),
                    cardRect.right - inner, cardRect.bottom - ScaleForDpi(hwnd_, 17)),
                textSecondary,
                DWRITE_TEXT_ALIGNMENT_LEADING);
            const RECT track = MakeRect(
                cardRect.left + inner,
                cardRect.bottom - ScaleForDpi(hwnd_, 13),
                cardRect.right - inner,
                cardRect.bottom - ScaleForDpi(hwnd_, 7));
            drawOpaqueRounded(track, trackColor, 0.42f);
            if (window.available) {
                RECT fill = track;
                fill.right = fill.left + static_cast<int>(
                    RectWidth(track) * ClampDouble(window.remainingPercent / 100.0, 0.0, 1.0));
                if (fill.right > fill.left) {
                    drawOpaqueRounded(fill, ColorForRemainingPercent(window.remainingPercent, false), 1.0f);
                }
            }
        };

        const int summaryHeight = ScaleForDpi(hwnd_, 116);
        const int summaryWidth = (RectWidth(panelRect) - fullPad * 2 - cardGap) / 2;
        const RECT fiveSummary = MakeRect(
            panelRect.left + fullPad,
            y,
            panelRect.left + fullPad + summaryWidth,
            y + summaryHeight);
        const RECT weekSummary = MakeRect(
            fiveSummary.right + cardGap,
            y,
            panelRect.right - fullPad,
            y + summaryHeight);
        drawSummaryCard(fiveSummary, LocalizeText(L"5-hour limit", L"5 小時限額"), snapshot_.fiveHour, cardBlue, 1);
        drawSummaryCard(weekSummary, LocalizeText(L"Weekly limit", L"週限額"), snapshot_.weekly, cardLavender, 2);
        y = summaryHeight + y + cardGap;

        const int paceHeight = ScaleForDpi(hwnd_, 58);
        const RECT paceRect = MakeRect(panelRect.left + fullPad, y, panelRect.right - fullPad, y + paceHeight);
        fillCard(paceRect, glassLavender);
        drawVectorIcon(
            MakeRect(paceRect.left + ScaleForDpi(hwnd_, 14), paceRect.top + ScaleForDpi(hwnd_, 14),
                paceRect.left + ScaleForDpi(hwnd_, 42), paceRect.top + ScaleForDpi(hwnd_, 42)),
            3,
            textPrimary);
        const std::wstring paceText = pace.valid
            ? (std::wstring(LocalizeText(L"Cycle day ", L"週期第 "))
                + std::to_wstring(pace.cycleDay)
                + LocalizeText(L" · Actual ", L" 天 · 實際 ")
                + FormatPercent(pace.actualUsedPercent)
                + LocalizeText(L" · Budget ", L" · 預算 ")
                + FormatNumber(pace.expectedUsedPercent)
                + L"% · "
                + (pace.isOver
                    ? std::wstring(LocalizeText(L"above budget ", L"高於週期預算 "))
                    : std::wstring(LocalizeText(L"below budget ", L"低於週期預算 ")))
                + FormatNumber(std::abs(pace.deltaPercent)) + L"%")
            : LocalizeText(L"Cycle pace unavailable", L"目前無法取得週期進度");
        drawOpaqueTextLine(
            textFormatFoot_.Get(),
            paceText,
            MakeRect(paceRect.left + ScaleForDpi(hwnd_, 50), paceRect.top,
                paceRect.right - ScaleForDpi(hwnd_, 14), paceRect.bottom),
            textPrimary,
            DWRITE_TEXT_ALIGNMENT_LEADING);
        y = paceRect.bottom + cardGap;

        const int creditCount = snapshot_.resetCredits.fetched
            ? static_cast<int>(snapshot_.resetCredits.availableCredits.size())
            : 0;
        const int creditRows = snapshot_.resetCredits.fetched ? std::max(1, creditCount) : 1;
        const int creditRowHeight = ScaleForDpi(hwnd_, 22);
        const int creditCardHeight = ScaleForDpi(hwnd_, 18) + creditRows * creditRowHeight + ScaleForDpi(hwnd_, 18);
        const RECT creditCard = MakeRect(panelRect.left + fullPad, y, panelRect.right - fullPad, y + creditCardHeight);
        fillCard(creditCard, cardNeutral);
        drawVectorIcon(
            MakeRect(creditCard.left + ScaleForDpi(hwnd_, 12), creditCard.top + ScaleForDpi(hwnd_, 5),
                creditCard.left + ScaleForDpi(hwnd_, 40), creditCard.top + ScaleForDpi(hwnd_, 33)),
            4,
            textPrimary);
        const std::wstring creditsTitle =
            std::wstring(LocalizeText(L"Usage limit reset (Full reset Weekly + 5 hr)", L"使用量限制重設 (Full reset Weekly + 5 hr)"))
            + L"  ·  "
            + std::to_wstring(snapshot_.resetCredits.fetched ? snapshot_.resetCredits.availableCount : 0)
            + LocalizeText(L" available", L" 張");
        drawOpaqueTextLine(
            textFormatMetricLabel_.Get(),
            creditsTitle,
            MakeRect(creditCard.left + ScaleForDpi(hwnd_, 48), creditCard.top + ScaleForDpi(hwnd_, 7),
                creditCard.right - ScaleForDpi(hwnd_, 12), creditCard.top + ScaleForDpi(hwnd_, 25)),
            textPrimary,
            DWRITE_TEXT_ALIGNMENT_LEADING);

        int creditY = creditCard.top + ScaleForDpi(hwnd_, 29);
        if (!snapshot_.resetCredits.fetched) {
            drawOpaqueTextLine(
                textFormatFoot_.Get(),
                snapshot_.resetCredits.errorMessage.empty()
                    ? LocalizeText(L"Unavailable", L"目前無法取得")
                    : snapshot_.resetCredits.errorMessage,
                MakeRect(creditCard.left + ScaleForDpi(hwnd_, 12), creditY,
                    creditCard.right - ScaleForDpi(hwnd_, 12), creditY + creditRowHeight),
                textSecondary,
                DWRITE_TEXT_ALIGNMENT_LEADING);
        } else if (creditCount == 0) {
            drawOpaqueTextLine(
                textFormatFoot_.Get(),
                LocalizeText(L"None available", L"目前沒有可用"),
                MakeRect(creditCard.left + ScaleForDpi(hwnd_, 12), creditY,
                    creditCard.right - ScaleForDpi(hwnd_, 12), creditY + creditRowHeight),
                textSecondary,
                DWRITE_TEXT_ALIGNMENT_LEADING);
        } else {
            for (int index = 0; index < creditCount; ++index) {
                const RateLimitResetCredit& credit = snapshot_.resetCredits.availableCredits[static_cast<size_t>(index)];
                const std::wstring indexText = language_ == Language::Chinese
                    ? (L"第 " + std::to_wstring(index + 1) + L" 張")
                    : (L"#" + std::to_wstring(index + 1));
                const std::wstring expiryText = credit.hasExpiry
                    ? FormatFullDateTime(credit.expiresAtUnixSeconds)
                    : LocalizeText(L"No expiry", L"無期限");
                drawOpaqueTextLine(
                    textFormatFoot_.Get(), indexText,
                    MakeRect(creditCard.left + ScaleForDpi(hwnd_, 12), creditY,
                        creditCard.left + ScaleForDpi(hwnd_, 70), creditY + creditRowHeight),
                    textPrimary,
                    DWRITE_TEXT_ALIGNMENT_LEADING);
                drawOpaqueTextLine(
                    textFormatFoot_.Get(), expiryText,
                    MakeRect(creditCard.left + ScaleForDpi(hwnd_, 78), creditY,
                        creditCard.right - ScaleForDpi(hwnd_, 12), creditY + creditRowHeight),
                    textSecondary,
                    DWRITE_TEXT_ALIGNMENT_TRAILING);
                creditY += creditRowHeight;
            }
        }
        y = creditCard.bottom + cardGap;

        auto estimateExhaustAt = [&](const UsageWindow& window) -> long long {
            if (!window.available || window.usedPercent <= 0 || window.windowSeconds <= 0) {
                return 0;
            }
            const int elapsed = ClampInt(window.windowSeconds - window.resetAfterSeconds, 1, window.windowSeconds);
            const double secondsToExhaust =
                (static_cast<double>(window.remainingPercent) / static_cast<double>(window.usedPercent)) * elapsed;
            if (!std::isfinite(secondsToExhaust) || secondsToExhaust <= 0.0) {
                return 0;
            }
            return static_cast<long long>(std::time(nullptr)) + static_cast<long long>(std::llround(secondsToExhaust));
        };

        const int detailRowHeight = ScaleForDpi(hwnd_, 78);
        const int detailCardHeight = ScaleForDpi(hwnd_, 30) + detailRowHeight * 2;
        const RECT detailCard = MakeRect(panelRect.left + fullPad, y, panelRect.right - fullPad, y + detailCardHeight);
        fillCard(detailCard, cardNeutral);
        drawVectorIcon(
            MakeRect(detailCard.left + ScaleForDpi(hwnd_, 14), detailCard.top + ScaleForDpi(hwnd_, 6),
                detailCard.left + ScaleForDpi(hwnd_, 42), detailCard.top + ScaleForDpi(hwnd_, 34)),
            5,
            textPrimary);
        drawOpaqueTextLine(
            textFormatMetricLabel_.Get(),
            LocalizeText(L"Usage details", L"用量詳情"),
            MakeRect(detailCard.left + ScaleForDpi(hwnd_, 50), detailCard.top + ScaleForDpi(hwnd_, 7),
                detailCard.right - ScaleForDpi(hwnd_, 14), detailCard.top + ScaleForDpi(hwnd_, 25)),
            textPrimary,
            DWRITE_TEXT_ALIGNMENT_LEADING);

        auto drawDetailRow = [&](const RECT& rowRect,
                                 const std::wstring& title,
                                 const UsageWindow& window,
                                 double expectedUsedPercent) {
            const int inner = ScaleForDpi(hwnd_, 14);
            drawOpaqueTextLine(
                textFormatMetricLabel_.Get(),
                title,
                MakeRect(rowRect.left + inner, rowRect.top, rowRect.left + ScaleForDpi(hwnd_, 150), rowRect.top + ScaleForDpi(hwnd_, 18)),
                textPrimary,
                DWRITE_TEXT_ALIGNMENT_LEADING);
            drawOpaqueTextLine(
                textFormatMetricLabel_.Get(),
                window.available ? FormatPercent(window.remainingPercent) : L"--",
                MakeRect(rowRect.right - ScaleForDpi(hwnd_, 110), rowRect.top, rowRect.right - inner, rowRect.top + ScaleForDpi(hwnd_, 18)),
                textPrimary,
                DWRITE_TEXT_ALIGNMENT_TRAILING);
            const long long exhaustAt = estimateExhaustAt(window);
            const std::wstring meta =
                std::wstring(LocalizeText(L"Start ", L"開始 "))
                + (window.hasStartAt ? FormatDateTime(window.startAtUnixSeconds) : L"--")
                + L"  ·  "
                + std::wstring(LocalizeText(L"Reset ", L"重設 "))
                + FormatDateTime(window.resetAtUnixSeconds)
                + L"  ·  "
                + (exhaustAt > 0
                    ? std::wstring(LocalizeText(L"ETA ", L"預計用完 ")) + FormatDateTime(exhaustAt)
                    : std::wstring(LocalizeText(L"ETA --", L"預計用完 --")));
            drawOpaqueTextLine(
                textFormatFoot_.Get(), meta,
                MakeRect(rowRect.left + inner, rowRect.top + ScaleForDpi(hwnd_, 18), rowRect.right - inner, rowRect.top + ScaleForDpi(hwnd_, 36)),
                textSecondary,
                DWRITE_TEXT_ALIGNMENT_LEADING);
            const RECT track = MakeRect(
                rowRect.left + inner,
                rowRect.bottom - ScaleForDpi(hwnd_, 17),
                rowRect.right - inner,
                rowRect.bottom - ScaleForDpi(hwnd_, 10));
            drawOpaqueRounded(track, trackColor, 0.42f);
            if (window.available) {
                RECT fill = track;
                fill.right = fill.left + static_cast<int>(
                    RectWidth(track) * ClampDouble(window.remainingPercent / 100.0, 0.0, 1.0));
                if (fill.right > fill.left) {
                    drawOpaqueRounded(fill, ColorForRemainingPercent(window.remainingPercent, false), 1.0f);
                }
                const double expectedRemaining = ClampDouble(100.0 - expectedUsedPercent, 0.0, 100.0);
                const int markerX = track.left + static_cast<int>(RectWidth(track) * expectedRemaining / 100.0);
                drawOpaqueRounded(
                    MakeRect(markerX - ScaleForDpi(hwnd_, 1), track.top - ScaleForDpi(hwnd_, 2),
                        markerX + ScaleForDpi(hwnd_, 1), track.bottom + ScaleForDpi(hwnd_, 2)),
                    budgetMarkerColor,
                    1.0f);
            }
        };

        drawDetailRow(
            MakeRect(detailCard.left, detailCard.top + ScaleForDpi(hwnd_, 29), detailCard.right,
                detailCard.top + ScaleForDpi(hwnd_, 29) + detailRowHeight),
            LocalizeText(L"5-hour limit", L"5 小時限額"),
            snapshot_.fiveHour,
            pace.fiveHourExpectedUsedPercent);
        drawDetailRow(
            MakeRect(detailCard.left, detailCard.top + ScaleForDpi(hwnd_, 29) + detailRowHeight, detailCard.right,
                detailCard.bottom),
            LocalizeText(L"Weekly limit", L"週限額"),
            snapshot_.weekly,
            pace.expectedUsedPercent);

        const int refreshSize = ScaleForDpi(hwnd_, 34);
        refreshButtonRect_ = MakeRect(
            panelRect.right - fullPad - refreshSize,
            panelRect.bottom - ScaleForDpi(hwnd_, 46),
            panelRect.right - fullPad,
            panelRect.bottom - ScaleForDpi(hwnd_, 12));
        if (refreshInFlight_) {
            fillEllipse(
                D2D1::Ellipse(
                    D2D1::Point2F(
                        static_cast<float>(refreshButtonRect_.left + RectWidth(refreshButtonRect_) / 2),
                        static_cast<float>(refreshButtonRect_.top + RectHeight(refreshButtonRect_) / 2)),
                    static_cast<float>(refreshSize) * 0.48f,
                    static_cast<float>(refreshSize) * 0.48f),
                heroValue,
                0.14f);
        }
        if (undoIconBitmap_) {
            DrawAssetBitmap(undoIconBitmap_.Get(), refreshButtonRect_, 1.0f);
        } else {
            solidBrush_->SetColor(ToColorF(refreshInFlight_ ? heroValue : textPrimary, 0.92f));
            const D2D1_POINT_2F center = D2D1::Point2F(
                static_cast<float>(refreshButtonRect_.left + RectWidth(refreshButtonRect_) / 2),
                static_cast<float>(refreshButtonRect_.top + RectHeight(refreshButtonRect_) / 2));
            renderTarget_->DrawEllipse(
                D2D1::Ellipse(center, static_cast<float>(refreshSize) * 0.31f, static_cast<float>(refreshSize) * 0.31f),
                solidBrush_.Get(),
                static_cast<float>(ScaleForDpi(hwnd_, 2)));
        }

        const std::wstring footerLeft = GetVersionStatusText(true);
        const RECT footerLeftRect = MakeRect(panelRect.left + fullPad, panelRect.bottom - ScaleForDpi(hwnd_, 46),
            panelRect.left + RectWidth(panelRect) / 2, panelRect.bottom - ScaleForDpi(hwnd_, 12));
        const RECT footerRightRect = MakeRect(panelRect.left + RectWidth(panelRect) / 2, panelRect.bottom - ScaleForDpi(hwnd_, 46),
            refreshButtonRect_.left - ScaleForDpi(hwnd_, 10), panelRect.bottom - ScaleForDpi(hwnd_, 12));
        drawOpaqueTextLine(textFormatFoot_.Get(), footerLeft, footerLeftRect,
            updateAvailable_ ? heroValue : textSecondary, DWRITE_TEXT_ALIGNMENT_LEADING);
        const int footerSplit = footerRightRect.top + RectHeight(footerRightRect) / 2;
        drawOpaqueTextLine(textFormatFoot_.Get(), GetRefreshStatusText(),
            MakeRect(footerRightRect.left, footerRightRect.top, footerRightRect.right, footerSplit),
            refreshInFlight_ ? heroValue : textPrimary, DWRITE_TEXT_ALIGNMENT_TRAILING);
        drawOpaqueTextLine(textFormatFoot_.Get(), FormatRefreshCountdown(refreshCountdownSeconds_),
            MakeRect(footerRightRect.left, footerSplit, footerRightRect.right, footerRightRect.bottom),
            textSecondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
        return;
    }

#if 0
    // Previous full-mode implementation retained in source history during the layout migration.
    fillRect(MakeRect(panelRect.left + 2, panelRect.top + 3, panelRect.right + 2, panelRect.bottom + 3), shadow);
    drawGlassPanel(panelRect);

    if (!snapshot_.success) {
        const bool loadFailed = !snapshot_.errorMessage.empty();
        RECT titleRect = MakeRect(clientRect.left + padX, clientRect.top + padY,
            clientRect.right - padX, clientRect.top + padY + ScaleForDpi(hwnd_, 28));
        drawTextBlock(textFormatMetricValue_.Get(),
            loadFailed
                ? LocalizeText(L"Failed to load usage data", L"載入用量失敗")
                : LocalizeText(L"Loading usage data", L"正在載入用量資訊"),
            titleRect, textPrimary,
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, true);
        if (!snapshot_.errorMessage.empty()) {
            RECT errorRect = MakeRect(clientRect.left + padX, titleRect.bottom + ScaleForDpi(hwnd_, 6),
                clientRect.right - padX, clientRect.bottom - ScaleForDpi(hwnd_, 24));
            drawTextBlock(textFormatFoot_.Get(), snapshot_.errorMessage, errorRect, RGB(215, 73, 73),
                DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_WORD_WRAPPING_WRAP, false);
        }
        RECT versionRect = MakeRect(clientRect.left + padX, clientRect.bottom - ScaleForDpi(hwnd_, 18),
            clientRect.right - padX, clientRect.bottom - ScaleForDpi(hwnd_, 4));
        drawTextBlock(textFormatFoot_.Get(), GetVersionStatusText(true), versionRect, updateAvailable_ ? heroValue : textSecondary,
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, false);
        return;
    }

    auto estimateExhaustAt = [&](const UsageWindow& window) -> long long {
        if (window.usedPercent <= 0 || window.windowSeconds <= 0) {
            return 0;
        }
        const int elapsed = ClampInt(window.windowSeconds - window.resetAfterSeconds, 1, window.windowSeconds);
        const double secondsToExhaust =
            (static_cast<double>(window.remainingPercent) / static_cast<double>(window.usedPercent)) * elapsed;
        if (!std::isfinite(secondsToExhaust) || secondsToExhaust <= 0.0) {
            return 0;
        }
        return static_cast<long long>(std::time(nullptr)) + static_cast<long long>(std::llround(secondsToExhaust));
    };

    auto drawUsageBar = [&](int top,
                            const std::wstring& title,
                            const UsageWindow& window,
                            double expectedUsedPercent) {
        const int rowHeight = ScaleForDpi(hwnd_, 40);
        // Bar/background color by remaining% (100 green -> 0 red).
        const COLORREF barColor = ColorForRemainingPercent(window.remainingPercent, false);

        RECT titleRect = MakeRect(clientRect.left + padX, top, clientRect.right - padX - ScaleForDpi(hwnd_, 120),
            top + ScaleForDpi(hwnd_, 16));
        drawTextBlock(textFormatMetricLabel_.Get(), title, titleRect, textPrimary,
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, true);

        const std::wstring percentText = FormatPercent(window.remainingPercent);
        const std::wstring resetText = FormatDateTime(window.resetAtUnixSeconds);
        // Percent uses the same semi-bold primary style as the limit title.
        // Reset time stays secondary on the far right.
        RECT percentRect = MakeRect(
            clientRect.right - padX - ScaleForDpi(hwnd_, 150),
            top,
            clientRect.right - padX - ScaleForDpi(hwnd_, 78),
            top + ScaleForDpi(hwnd_, 16));
        RECT resetTimeRect = MakeRect(
            clientRect.right - padX - ScaleForDpi(hwnd_, 74),
            top,
            clientRect.right - padX,
            top + ScaleForDpi(hwnd_, 16));
        drawTextBlock(textFormatMetricLabel_.Get(), percentText, percentRect, textPrimary,
            DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, true);
        drawTextBlock(textFormatFoot_.Get(), resetText, resetTimeRect, textSecondary,
            DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, true);

        const std::wstring startText = window.hasStartAt ? FormatDateTime(window.startAtUnixSeconds) : L"--";
        const long long exhaustAt = estimateExhaustAt(window);
        const std::wstring etaText = exhaustAt > 0
            ? (std::wstring(LocalizeText(L"ETA ", L"預計用完 ")) + FormatDateTime(exhaustAt))
            : LocalizeText(L"ETA --", L"預計用完 --");
        RECT metaRect = MakeRect(clientRect.left + padX, top + ScaleForDpi(hwnd_, 15),
            clientRect.right - padX, top + ScaleForDpi(hwnd_, 28));
        const std::wstring meta =
            std::wstring(LocalizeText(L"Start ", L"開始 ")) + startText
            + L"  ·  " + std::wstring(LocalizeText(L"Reset ", L"重設 ")) + resetText
            + L"  ·  " + etaText;
        drawTextBlock(textFormatFoot_.Get(), meta, metaRect, textSecondary,
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, true);

        RECT track = MakeRect(clientRect.left + padX, top + ScaleForDpi(hwnd_, 30),
            clientRect.right - padX, top + ScaleForDpi(hwnd_, 30) + ScaleForDpi(hwnd_, 7));
        fillRect(track, trackColor);

        // Fill = remaining%, growing from the left.
        RECT fill = track;
        fill.right = fill.left + static_cast<int>(
            RectWidth(track) * ClampDouble(static_cast<double>(window.remainingPercent), 0.0, 100.0) / 100.0);
        if (fill.right > fill.left) {
            fillRect(fill, barColor);
        }

        // Black budget marker on remaining scale: expected remaining = 100 - expected used.
        // Left of marker = still above budget remaining; right of marker = burned past budget.
        const double expectedRemainingPercent = ClampDouble(100.0 - expectedUsedPercent, 0.0, 100.0);
        const int markerX = track.left + static_cast<int>(
            RectWidth(track) * expectedRemainingPercent / 100.0);
        fillRect(
            MakeRect(markerX - 1, track.top - ScaleForDpi(hwnd_, 2), markerX + 1, track.bottom + ScaleForDpi(hwnd_, 3)),
            budgetMarkerColor);
        return rowHeight;
    };

    // Header: badge + email
    RECT badgeRect = MakeRect(clientRect.left + padX, clientRect.top + padY,
        clientRect.left + padX + ScaleForDpi(hwnd_, 48), clientRect.top + padY + ScaleForDpi(hwnd_, 20));
    fillRect(badgeRect, lightTheme_ ? RGB(236, 233, 255) : RGB(52, 46, 84));
    drawTextBlock(textFormatFoot_.Get(), L"Codex", badgeRect,
        lightTheme_ ? RGB(96, 74, 210) : RGB(190, 176, 255),
        DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, false);

    const std::wstring emailText = !snapshot_.email.empty() ? snapshot_.email : L"--";
    RECT emailRect = MakeRect(badgeRect.right + ScaleForDpi(hwnd_, 8), badgeRect.top,
        clientRect.right - padX, badgeRect.bottom);
    drawTextBlock(textFormatMetricLabel_.Get(), emailText, emailRect, textPrimary,
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, true);

    // Plan row
    int y = badgeRect.bottom + ScaleForDpi(hwnd_, 8);
    RECT planLabelRect = MakeRect(clientRect.left + padX, y, clientRect.left + padX + ScaleForDpi(hwnd_, 32), y + ScaleForDpi(hwnd_, 18));
    drawTextBlock(textFormatFoot_.Get(), LocalizeText(L"Plan", L"方案"), planLabelRect, textSecondary,
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, false);

    const std::wstring planText = FormatPlanDisplayName();
    const float planWidth = measureTextWidth(textFormatFoot_.Get(), planText) + ScaleForDpi(hwnd_, 12);
    RECT planPillRect = MakeRect(planLabelRect.right + ScaleForDpi(hwnd_, 4), y,
        planLabelRect.right + ScaleForDpi(hwnd_, 4) + static_cast<int>(std::ceil(planWidth)), y + ScaleForDpi(hwnd_, 18));
    fillRect(planPillRect, lightTheme_ ? RGB(224, 239, 255) : RGB(43, 70, 125));
    drawTextBlock(textFormatFoot_.Get(), planText, planPillRect,
        lightTheme_ ? RGB(46, 90, 180) : RGB(204, 222, 255),
        DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, false);

    const std::wstring planStartText = snapshot_.hasPlanStart ? FormatDateTime(snapshot_.planStartUnixSeconds) : L"--";
    const std::wstring planUntilText = snapshot_.hasPlanUntil ? FormatDateTime(snapshot_.planUntilUnixSeconds) : L"--";
    const std::wstring planDates =
        std::wstring(LocalizeText(L"Start ", L"開始 ")) + planStartText
        + L"  ·  " + std::wstring(LocalizeText(L"Until ", L"到期 ")) + planUntilText;
    RECT planDatesRect = MakeRect(planPillRect.right + ScaleForDpi(hwnd_, 8), y,
        clientRect.right - padX, y + ScaleForDpi(hwnd_, 18));
    drawTextBlock(textFormatFoot_.Get(), planDates, planDatesRect, textSecondary,
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, true);

    y += ScaleForDpi(hwnd_, 24);
    const int paceCardHeight = ScaleForDpi(hwnd_, 42);
    RECT paceCard = MakeRect(clientRect.left + padX, y, clientRect.right - padX, y + paceCardHeight);
    fillRect(paceCard, glassLavender);
    const std::wstring paceText = pace.valid
        ? (std::wstring(LocalizeText(L"Cycle day ", L"週期第 "))
            + std::to_wstring(pace.cycleDay)
            + LocalizeText(L" · Actual ", L" 天 · 實際 ")
            + FormatPercent(pace.actualUsedPercent)
            + LocalizeText(L" · Budget ", L" · 預算 ")
            + FormatNumber(pace.expectedUsedPercent)
            + L"% · "
            + (pace.isOver
                ? std::wstring(LocalizeText(L"above budget ", L"高於週期預算 "))
                : std::wstring(LocalizeText(L"below budget ", L"低於週期預算 ")))
            + FormatNumber(std::abs(pace.deltaPercent)) + L"%")
        : LocalizeText(L"Cycle pace unavailable", L"目前無法取得週期進度");
    drawTextBlock(textFormatFoot_.Get(), paceText,
        MakeRect(paceCard.left + ScaleForDpi(hwnd_, 10), paceCard.top,
            paceCard.right - ScaleForDpi(hwnd_, 10), paceCard.bottom),
        textPrimary,
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, true);

    y += paceCardHeight + ScaleForDpi(hwnd_, 8);

    // Reset credits inventory: header + one row per credit (screenshot style).
    const int creditCount = snapshot_.resetCredits.fetched
        ? static_cast<int>(snapshot_.resetCredits.availableCredits.size())
        : 0;
    const int creditRows = snapshot_.resetCredits.fetched
        ? std::max(1, creditCount)
        : 1;
    const int creditRowH = ScaleForDpi(hwnd_, 18);
    const int creditHeaderH = ScaleForDpi(hwnd_, 18);
    const int creditBoxH = ScaleForDpi(hwnd_, 8) + creditHeaderH + creditRows * creditRowH + ScaleForDpi(hwnd_, 4);
    RECT creditBox = MakeRect(clientRect.left + padX, y, clientRect.right - padX, y + creditBoxH);
    fillRect(creditBox, glassCard);
    drawRectBorder(creditBox, border);

    RECT creditsTitleRect = MakeRect(creditBox.left + ScaleForDpi(hwnd_, 8), creditBox.top + ScaleForDpi(hwnd_, 3),
        creditBox.right - ScaleForDpi(hwnd_, 8), creditBox.top + ScaleForDpi(hwnd_, 3) + creditHeaderH);
    const std::wstring creditsTitle =
        std::wstring(LocalizeText(L"Usage limit reset (Full reset Weekly + 5 hr)", L"使用量限制重設 (Full reset Weekly + 5 hr)"))
        + L"  ·  "
        + std::to_wstring(snapshot_.resetCredits.fetched ? snapshot_.resetCredits.availableCount : 0)
        + LocalizeText(L" available", L" 張");
    drawTextBlock(textFormatFoot_.Get(), creditsTitle, creditsTitleRect, textSecondary,
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, true);

    int creditY = creditsTitleRect.bottom;
    if (!snapshot_.resetCredits.fetched) {
        RECT row = MakeRect(creditBox.left + ScaleForDpi(hwnd_, 8), creditY,
            creditBox.right - ScaleForDpi(hwnd_, 8), creditY + creditRowH);
        drawTextBlock(textFormatFoot_.Get(),
            snapshot_.resetCredits.errorMessage.empty()
                ? LocalizeText(L"Unavailable", L"目前無法取得")
                : snapshot_.resetCredits.errorMessage,
            row, textSecondary,
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, true);
    } else if (creditCount == 0) {
        RECT row = MakeRect(creditBox.left + ScaleForDpi(hwnd_, 8), creditY,
            creditBox.right - ScaleForDpi(hwnd_, 8), creditY + creditRowH);
        drawTextBlock(textFormatFoot_.Get(), LocalizeText(L"None available", L"目前沒有可用"), row, textSecondary,
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, true);
    } else {
        for (int i = 0; i < creditCount; ++i) {
            const RateLimitResetCredit& credit = snapshot_.resetCredits.availableCredits[static_cast<size_t>(i)];
            RECT left = MakeRect(creditBox.left + ScaleForDpi(hwnd_, 8), creditY,
                creditBox.left + ScaleForDpi(hwnd_, 72), creditY + creditRowH);
            RECT right = MakeRect(left.right, creditY, creditBox.right - ScaleForDpi(hwnd_, 8), creditY + creditRowH);
            const std::wstring indexText = language_ == Language::Chinese
                ? (L"第 " + std::to_wstring(i + 1) + L" 張")
                : (L"#" + std::to_wstring(i + 1));
            const std::wstring expiryText = credit.hasExpiry
                ? FormatFullDateTime(credit.expiresAtUnixSeconds)
                : LocalizeText(L"No expiry", L"無期限");
            drawTextBlock(textFormatFoot_.Get(), indexText, left, textPrimary,
                DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, false);
            drawTextBlock(textFormatFoot_.Get(), expiryText, right, textSecondary,
                DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, false);
            creditY += creditRowH;
        }
    }
    // 5h + weekly bars: used% fill + expected budget black marker.
    y = creditBox.bottom + ScaleForDpi(hwnd_, 10);
    y += drawUsageBar(
        y,
        LocalizeText(L"5-hour limit", L"5 小時限額"),
        snapshot_.fiveHour,
        pace.fiveHourExpectedUsedPercent);
    y += ScaleForDpi(hwnd_, 6);
    y += drawUsageBar(
        y,
        LocalizeText(L"Weekly limit", L"週限額"),
        snapshot_.weekly,
        pace.expectedUsedPercent);

    // Borderless refresh glyph fixed to the bottom-right.
    const int refreshSize = ScaleForDpi(hwnd_, 34);
    const int refreshBottom = clientRect.bottom - ScaleForDpi(hwnd_, 20);
    RECT refreshRect = MakeRect(
        clientRect.right - padX - refreshSize,
        refreshBottom - refreshSize,
        clientRect.right - padX,
        refreshBottom);
    refreshButtonRect_ = refreshRect;
    const D2D1_POINT_2F refreshCenter = D2D1::Point2F(
        static_cast<float>(refreshRect.left + RectWidth(refreshRect) / 2),
        static_cast<float>(refreshRect.top + RectHeight(refreshRect) / 2));
    solidBrush_->SetColor(ToColorF(refreshInFlight_ ? heroValue : textPrimary, 0.92f));
    renderTarget_->DrawEllipse(
        D2D1::Ellipse(refreshCenter, static_cast<float>(refreshSize) * 0.31f, static_cast<float>(refreshSize) * 0.31f),
        solidBrush_.Get(),
        static_cast<float>(ScaleForDpi(hwnd_, 2)));
    const float arrowHalf = static_cast<float>(refreshSize) * 0.16f;
    drawLine(
        D2D1::Point2F(refreshCenter.x + arrowHalf, refreshCenter.y - arrowHalf * 1.45f),
        D2D1::Point2F(refreshCenter.x + arrowHalf * 1.45f, refreshCenter.y - arrowHalf * 0.15f),
        refreshInFlight_ ? heroValue : textPrimary,
        static_cast<float>(ScaleForDpi(hwnd_, 2)),
        0.95f);
    drawLine(
        D2D1::Point2F(refreshCenter.x + arrowHalf * 1.45f, refreshCenter.y - arrowHalf * 0.15f),
        D2D1::Point2F(refreshCenter.x + arrowHalf * 0.25f, refreshCenter.y - arrowHalf * 0.10f),
        refreshInFlight_ ? heroValue : textPrimary,
        static_cast<float>(ScaleForDpi(hwnd_, 2)),
        0.95f);

    // Footer under buttons.
    const std::wstring footerLeft = GetVersionStatusText(true);
    const std::wstring footerRight = refreshInFlight_
        ? LocalizeText(L"Refreshing", L"重新整理中")
        : FormatRefreshCountdown(refreshCountdownSeconds_);
    RECT footerLeftRect = MakeRect(clientRect.left + padX, clientRect.bottom - ScaleForDpi(hwnd_, 16),
        clientRect.left + RectWidth(clientRect) / 2, clientRect.bottom - ScaleForDpi(hwnd_, 2));
    RECT footerRightRect = MakeRect(clientRect.left + RectWidth(clientRect) / 2, clientRect.bottom - ScaleForDpi(hwnd_, 16),
        clientRect.right - padX, clientRect.bottom - ScaleForDpi(hwnd_, 2));
    drawTextBlock(textFormatFoot_.Get(), footerLeft, footerLeftRect, updateAvailable_ ? heroValue : textSecondary,
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, false);
    drawTextBlock(textFormatFoot_.Get(), footerRight, footerRightRect, textSecondary,
        DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP, false);
}

#endif

}

void AppBarWindow::ShowContextMenu(POINT screenPoint) {
    HMENU menu = CreatePopupMenu();
    HMENU languageMenu = CreatePopupMenu();
    HMENU refreshIntervalMenu = CreatePopupMenu();
    HMENU displayModeMenu = CreatePopupMenu();
    const bool launchAtStartup = IsLaunchAtStartupEnabled();
    const UINT alwaysOnTopMenuState = MF_STRING
        | ((alwaysOnTop_ || taskbarMode_) ? MF_CHECKED : MF_UNCHECKED)
        | (taskbarMode_ ? MF_GRAYED : 0);
    AppendMenuW(languageMenu, MF_STRING | (language_ == Language::English ? MF_CHECKED : MF_UNCHECKED),
        kCommandLanguageEnglish, L"English");
    AppendMenuW(languageMenu, MF_STRING | (language_ == Language::Chinese ? MF_CHECKED : MF_UNCHECKED),
        kCommandLanguageChinese, L"繁體中文");
    AppendMenuW(refreshIntervalMenu, MF_STRING | (refreshIntervalSeconds_ == 60 ? MF_CHECKED : MF_UNCHECKED),
        kCommandRefreshInterval1Minute, LocalizeText(L"1 minute", L"1 分鐘"));
    AppendMenuW(refreshIntervalMenu, MF_STRING | (refreshIntervalSeconds_ == 180 ? MF_CHECKED : MF_UNCHECKED),
        kCommandRefreshInterval3Minutes, LocalizeText(L"3 minutes", L"3 分鐘"));
    AppendMenuW(refreshIntervalMenu, MF_STRING | (refreshIntervalSeconds_ == 300 ? MF_CHECKED : MF_UNCHECKED),
        kCommandRefreshInterval5Minutes, LocalizeText(L"5 minutes", L"5 分鐘"));
    AppendMenuW(refreshIntervalMenu, MF_STRING | (refreshIntervalSeconds_ == 600 ? MF_CHECKED : MF_UNCHECKED),
        kCommandRefreshInterval10Minutes, LocalizeText(L"10 minutes", L"10 分鐘"));
    AppendMenuW(refreshIntervalMenu, MF_STRING | (refreshIntervalSeconds_ == 1800 ? MF_CHECKED : MF_UNCHECKED),
        kCommandRefreshInterval30Minutes, LocalizeText(L"30 minutes", L"30 分鐘"));
    AppendMenuW(displayModeMenu, MF_STRING | (!simpleMode_ && !taskbarMode_ ? MF_CHECKED : MF_UNCHECKED),
        kCommandFullMode, LocalizeText(L"Full mode", L"完整模式"));
    AppendMenuW(displayModeMenu, MF_STRING | (simpleMode_ ? MF_CHECKED : MF_UNCHECKED),
        kCommandSimpleMode, LocalizeText(L"Simple mode", L"簡單模式"));
    AppendMenuW(displayModeMenu, MF_STRING | (taskbarMode_ ? MF_CHECKED : MF_UNCHECKED),
        kCommandTaskbarMode, LocalizeText(L"Taskbar mode", L"任務模式"));

    AppendMenuW(menu, MF_STRING, kCommandRefresh, LocalizeText(L"Refresh now", L"立即重新整理"));
    AppendMenuW(menu, MF_STRING, kCommandCheckVersion, LocalizeText(L"Check version", L"檢查版本"));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(refreshIntervalMenu), LocalizeText(L"Refresh interval", L"重新整理間隔"));
    AppendMenuW(menu, MF_STRING | (launchAtStartup ? MF_CHECKED : MF_UNCHECKED),
        kCommandLaunchAtStartup, LocalizeText(L"Launch at startup", L"開機自動啟動"));
    AppendMenuW(menu, alwaysOnTopMenuState, kCommandAlwaysOnTop, LocalizeText(L"Always on top", L"始終置頂"));
    AppendMenuW(menu, MF_STRING | (lockPosition_ ? MF_CHECKED : MF_UNCHECKED),
        kCommandLockPosition, LocalizeText(L"Lock position", L"鎖定位置"));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(displayModeMenu), LocalizeText(L"Display mode", L"顯示模式"));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(languageMenu), LocalizeText(L"Language", L"語言"));
    AppendMenuW(menu, MF_STRING, kCommandSettings, LocalizeText(L"Transparency", L"透明度"));
    AppendMenuW(menu, MF_STRING, kCommandResetPosition, LocalizeText(L"Reset widget position", L"重設小工具位置"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCommandExit, LocalizeText(L"Exit", L"離開"));

    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);

    if (command == kCommandRefresh) {
        RequestRefresh(true);
    } else if (command == kCommandCheckVersion) {
        RequestLatestReleaseCheck(true);
    } else if (command == kCommandRefreshInterval1Minute) {
        SetRefreshIntervalSeconds(60);
    } else if (command == kCommandRefreshInterval3Minutes) {
        SetRefreshIntervalSeconds(180);
    } else if (command == kCommandRefreshInterval5Minutes) {
        SetRefreshIntervalSeconds(300);
    } else if (command == kCommandRefreshInterval10Minutes) {
        SetRefreshIntervalSeconds(600);
    } else if (command == kCommandRefreshInterval30Minutes) {
        SetRefreshIntervalSeconds(1800);
    } else if (command == kCommandLaunchAtStartup) {
        SetLaunchAtStartupEnabled(!launchAtStartup);
    } else if (command == kCommandAlwaysOnTop) {
        alwaysOnTop_ = !alwaysOnTop_;
        UpdateWindowBounds(true);
        SaveSettings();
    } else if (command == kCommandLockPosition) {
        lockPosition_ = !lockPosition_;
        SaveSettings();
    } else if (command == kCommandFullMode) {
        SetDisplayMode(false, false);
    } else if (command == kCommandSimpleMode) {
        SetDisplayMode(true, false);
    } else if (command == kCommandTaskbarMode) {
        SetDisplayMode(false, true);
    } else if (command == kCommandLanguageEnglish) {
        SetLanguage(Language::English);
    } else if (command == kCommandLanguageChinese) {
        SetLanguage(Language::Chinese);
    } else if (command == kCommandSettings) {
        OpenSettings();
    } else if (command == kCommandResetPosition) {
        hasSavedRect_ = false;
        UpdateWindowBounds(false);
        SaveSettings();
    } else if (command == kCommandExit) {
        DestroyWindow(hwnd_);
    }
}

std::wstring AppBarWindow::FormatDuration(int totalSeconds) const {
    const int days = totalSeconds / 86400;
    const int hours = (totalSeconds % 86400) / 3600;

    if (days > 0) {
        if (language_ == Language::Chinese) {
            return std::to_wstring(days) + L" 天 " + std::to_wstring(hours) + L" 小時";
        }
        return std::to_wstring(days) + L"d " + std::to_wstring(hours) + L"h";
    }
    const int minutes = (totalSeconds % 3600) / 60;
    if (language_ == Language::Chinese) {
        return std::to_wstring(hours) + L" 小時 " + std::to_wstring(minutes) + L" 分鐘";
    }
    return std::to_wstring(hours) + L"h " + std::to_wstring(minutes) + L"m";
}

std::wstring AppBarWindow::FormatRefreshCountdown(int totalSeconds) const {
    const int hours = totalSeconds / 3600;
    const int minutes = (totalSeconds % 3600) / 60;
    const int seconds = totalSeconds % 60;

    if (language_ == Language::Chinese) {
        if (hours > 0) {
            return std::to_wstring(hours) + L" 小時 " + std::to_wstring(minutes) + L" 分";
        }
        if (minutes > 0) {
            return std::to_wstring(minutes) + L" 分 " + std::to_wstring(seconds) + L" 秒";
        }
        return std::to_wstring(seconds) + L" 秒";
    }

    if (hours > 0) {
        return std::to_wstring(hours) + L"h " + std::to_wstring(minutes) + L"m";
    }
    if (minutes > 0) {
        return std::to_wstring(minutes) + L"m " + std::to_wstring(seconds) + L"s";
    }
    return std::to_wstring(seconds) + L"s";
}

std::wstring AppBarWindow::FormatDateTime(long long unixSeconds) const {
    if (unixSeconds <= 0) {
        return L"--";
    }

    std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm localTime = {};
    localtime_s(&localTime, &t);

    wchar_t buffer[64] = {};
    wcsftime(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%m/%d %H:%M", &localTime);
    return buffer;
}

std::wstring AppBarWindow::FormatFullDateTime(long long unixSeconds) const {
    if (unixSeconds <= 0) {
        return L"--";
    }

    std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm localTime = {};
    localtime_s(&localTime, &t);

    wchar_t buffer[64] = {};
    // Full local datetime, e.g. 2026-08-01 05:28
    wcsftime(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%Y-%m-%d %H:%M", &localTime);
    return buffer;
}

std::wstring AppBarWindow::FormatClockTime(long long unixSeconds) const {
    if (unixSeconds <= 0) {
        return L"--";
    }

    std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm localTime = {};
    localtime_s(&localTime, &t);

    wchar_t buffer[64] = {};
    wcsftime(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%H:%M:%S", &localTime);
    return buffer;
}

std::wstring AppBarWindow::FormatPercent(double value) const {
    return FormatNumber(value) + L"%";
}

std::wstring AppBarWindow::FormatPlanDisplayName() const {
    std::wstring plan = snapshot_.planType;
    if (plan.empty()) {
        return LocalizeText(L"Unknown", L"未知");
    }

    // Normalize casing: pro -> Pro
    for (wchar_t& ch : plan) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    if (!plan.empty()) {
        plan[0] = static_cast<wchar_t>(towupper(plan[0]));
    }

    // API currently only returns base plan_type (e.g. "pro"). Community tools
    // commonly label Pro tiers as 20x / 5x. Prefer explicit multiplier if the
    // plan string already contains it; otherwise map known Codex Pro display.
    const std::wstring lower = snapshot_.planType;
    if (lower.find(L"20") != std::wstring::npos || lower.find(L"20x") != std::wstring::npos) {
        return L"Pro 20x";
    }
    if (lower.find(L"5x") != std::wstring::npos || lower.find(L"5") != std::wstring::npos) {
        // Avoid mislabeling plain strings; only if 5x-like marker exists.
        if (lower.find(L"x") != std::wstring::npos) {
            return L"Pro 5x";
        }
    }

    if (_wcsicmp(snapshot_.planType.c_str(), L"pro") == 0) {
        // Default Codex Pro pool quota label used by quota dashboards.
        return L"Pro 20x";
    }
    if (_wcsicmp(snapshot_.planType.c_str(), L"plus") == 0) {
        return L"Plus";
    }
    if (_wcsicmp(snapshot_.planType.c_str(), L"team") == 0) {
        return L"Team";
    }
    if (_wcsicmp(snapshot_.planType.c_str(), L"enterprise") == 0) {
        return L"Enterprise";
    }
    if (_wcsicmp(snapshot_.planType.c_str(), L"free") == 0) {
        return L"Free";
    }
    return plan;
}

COLORREF AppBarWindow::ColorForRemainingPercent(int remainingPercent, bool forBackground) const {
    // 100 remaining = original green, 0 remaining = original soft red (not pure).
    // Original baselines from git UI:
    //   green bar: RGB(41, 185, 128) / dark RGB(84, 208, 154)
    //   red text:  RGB(189, 54, 31)  / dark RGB(255, 144, 120)
    //   soft bg:   green RGB(233, 248, 239) / red RGB(255, 240, 234)
    const int remaining = ClampInt(remainingPercent, 0, 100);
    const double t = 1.0 - (remaining / 100.0);  // 0 healthy -> 1 critical

    auto lerp = [](int a, int b, double x) {
        return static_cast<int>(std::lround(a + (b - a) * x));
    };

    if (forBackground) {
        if (lightTheme_) {
            // Soft card tints (not pure white/red/green).
            const int r = lerp(233, 255, t);
            const int g = lerp(248, 240, t);
            const int b = lerp(239, 234, t);
            return RGB(r, g, b);
        }
        const int r = lerp(27, 60, t);
        const int g = lerp(48, 34, t);
        const int b = lerp(36, 28, t);
        return RGB(r, g, b);
    }

    if (lightTheme_) {
        // Bar color: green -> amber -> soft red.
        const int r0 = 41, g0 = 185, b0 = 128;
        const int r1 = 214, g1 = 149, b1 = 57;
        const int r2 = 189, g2 = 54, b2 = 31;
        if (t <= 0.5) {
            const double u = t / 0.5;
            return RGB(lerp(r0, r1, u), lerp(g0, g1, u), lerp(b0, b1, u));
        }
        const double u = (t - 0.5) / 0.5;
        return RGB(lerp(r1, r2, u), lerp(g1, g2, u), lerp(b1, b2, u));
    }

    const int r0 = 84, g0 = 208, b0 = 154;
    const int r1 = 227, g1 = 165, b1 = 79;
    const int r2 = 255, g2 = 144, b2 = 120;
    if (t <= 0.5) {
        const double u = t / 0.5;
        return RGB(lerp(r0, r1, u), lerp(g0, g1, u), lerp(b0, b1, u));
    }
    const double u = (t - 0.5) / 0.5;
    return RGB(lerp(r1, r2, u), lerp(g1, g2, u), lerp(b1, b2, u));
}
