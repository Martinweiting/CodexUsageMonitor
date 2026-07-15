#pragma once

#include "CodexUsageFetcher.h"
#include "WidgetPresentation.h"

#include <Windows.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

class AppBarWindow {
public:
    explicit AppBarWindow(HINSTANCE instance);
    ~AppBarWindow();

    bool Create();
    int Run();

private:
    static constexpr UINT kUsageUpdatedMessage = WM_APP + 1;
    static constexpr UINT kReleaseVersionUpdatedMessage = WM_APP + 2;
    static constexpr UINT_PTR kCountdownTimerId = 1;
    static constexpr UINT_PTR kRefreshTimerId = 2;
    static constexpr UINT_PTR kHoverExitTimerId = 3;
    static constexpr UINT_PTR kHoverPollTimerId = 4;

    enum class Language {
        English = 0,
        Chinese = 1,
    };

    enum class DragMode {
        None,
        Move,
        ResizeRight,
        ResizeBottom,
        ResizeCorner,
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void RegisterWindowClass();
    RECT GetDesktopClientRect() const;
    bool GetCurrentMonitorInfo(MONITORINFO& monitorInfo) const;
    RECT GetCurrentMonitorWorkRect() const;
    RECT BuildDefaultRect(const RECT& desktopRect) const;
    RECT BuildTaskbarDockRect() const;
    RECT ClampRectToDesktop(RECT rect) const;
    RECT ClampPanelGroupRect(RECT rect) const;
    int GetBubbleSizeForState() const;
    RECT GetExpandedWindowRect(const RECT& panelRect) const;
    RECT GetBubbleWindowRect(const RECT& expandedRect) const;
    RECT GetSettingsWindowRect(const RECT& expandedRect) const;
    codex_widget::WidgetGroupGeometry GetCurrentGroupGeometry() const;
    void UpdateWindowBounds(bool useSavedPosition);
    void SetDisplayMode(bool simpleMode, bool taskbarMode);
    void SetPresentationState(codex_widget::PresentationState state);
    void CancelMouseLeaveTracking();
    void ArmMouseLeaveTracking();
    bool IsCursorInsideCurrentWindow() const;
    void UpdateHoverStateFromCursor();
    void StartHoverExitGuard();
    void StopHoverExitGuard();
    void OpenSettings();
    void CloseSettings();
    void UpdateTransparencyFromPoint(POINT clientPoint);

    void LoadSettings();
    void SaveSettings() const;
    std::wstring GetSettingsPath() const;
    std::wstring GetExecutablePath() const;
    void RefreshTheme();
    bool IsDesktopLightTheme() const;
    bool IsLaunchAtStartupEnabled() const;
    bool SetLaunchAtStartupEnabled(bool enabled) const;

    DragMode HitTestDragMode(POINT clientPoint) const;
    bool IsPointInsideBubble(POINT clientPoint) const;
    void BeginDrag(DragMode mode, POINT screenPoint);
    void UpdateDrag(POINT screenPoint);
    void EndDrag(bool saveSettings);
    void ActivateBubbleClick();

    void RequestRefresh(bool force);
    void OnUsageUpdated(UsageSnapshot* snapshot);
    void RequestLatestReleaseCheck(bool force);
    void OnLatestReleaseChecked(ReleaseVersionInfo* info);
    bool TryHandleControlClick(POINT clientPoint);
    bool TryHandleRefreshButtonClick(POINT clientPoint);
    std::wstring BuildResetCreditsSummaryText() const;
    std::wstring BuildResetCreditsExpiryText() const;

    HRESULT CreateDeviceIndependentResources();
    HRESULT CreateDeviceResources();
    void DiscardDeviceResources();
    std::wstring GetAssetPath(const wchar_t* relativePath) const;
    bool RegisterPrivateFonts();
    void UnregisterPrivateFonts();
    HRESULT EnsureAssetResources();
    void DiscardAssetResources();
    HRESULT LoadAssetBitmap(const wchar_t* relativePath, Microsoft::WRL::ComPtr<ID2D1Bitmap>* bitmap);
    HRESULT LoadTintedAssetBitmap(const wchar_t* relativePath, Microsoft::WRL::ComPtr<ID2D1Bitmap>* bitmap);
    void DrawAssetBitmap(ID2D1Bitmap* bitmap, const RECT& destination, float opacity) const;
    void EnableNativeGlassBackdrop();
    void DisableNativeGlassBackdrop();
    void DrawGlassSurface(const RECT& rect, COLORREF topColor, COLORREF bottomColor, float alpha);
    void DrawGlassCard(const RECT& rect, COLORREF tint, float alpha);
    void DrawGlassEdgeHighlight(const RECT& rect);
    void DiscardLayeredSurface();
    bool EnsureLayeredSurface(int width, int height);
    void RenderLayeredSurface();
    void DiscardTextFormats();
    HRESULT EnsureTextFormats();
    HRESULT CreateTextFormat(
        float sizePixels,
        DWRITE_FONT_WEIGHT weight,
        const wchar_t* familyName,
        IDWriteTextFormat** format);
    void ApplyFontRuns(IDWriteTextLayout* layout, const std::wstring& text) const;

    void Paint(HDC hdc);
    void PaintContent(const RECT& clientRect);
    void ShowContextMenu(POINT screenPoint);
    int GetMinimumWidgetWidth() const;
    int GetMinimumWidgetHeight(int width) const;
    void SetLanguage(Language language);
    void SetRefreshIntervalSeconds(int seconds);
    void RestartRefreshTimer();
    const wchar_t* LocalizeText(const wchar_t* english, const wchar_t* chinese) const;
    std::wstring GetVersionStatusText(bool compact) const;
    std::wstring GetRefreshStatusText() const;

    std::wstring FormatDuration(int totalSeconds) const;
    std::wstring FormatRefreshCountdown(int totalSeconds) const;
    std::wstring FormatDateTime(long long unixSeconds) const;
    std::wstring FormatFullDateTime(long long unixSeconds) const;
    std::wstring FormatClockTime(long long unixSeconds) const;
    std::wstring FormatPercent(double value) const;
    std::wstring FormatPlanDisplayName() const;
    // remainingPercent: 100 = healthy green, 0 = critical red (soft, not pure).
    COLORREF ColorForRemainingPercent(int remainingPercent, bool forBackground) const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    std::atomic_bool refreshInFlight_ = false;
    std::atomic_bool releaseCheckInFlight_ = false;
    bool lightTheme_ = false;
    bool alwaysOnTop_ = false;
    bool lockPosition_ = false;
    bool simpleMode_ = false;
    bool taskbarMode_ = false;
    bool hasReleaseCheckResult_ = false;
    bool updateAvailable_ = false;
    codex_widget::PresentationState presentationState_ = codex_widget::PresentationState::Bubble;
    bool mouseTracking_ = false;
    bool hoverExitGuardActive_ = false;
    bool hoverSuppressedUntilCursorLeavesBubble_ = false;
    bool settingsOpen_ = false;
    bool settingsDragging_ = false;
    Language language_ = Language::English;
    bool hasSavedRect_ = false;
    RECT savedRect_ = {};
    DragMode dragMode_ = DragMode::None;
    POINT dragStartPoint_ = {};
    RECT dragStartRect_ = {};
    bool bubbleClickPending_ = false;
    bool dragMoved_ = false;
    UINT textFormatDpi_ = 0;
    long long lastSuccessfulRefreshUnixSeconds_ = 0;
    long long lastRefreshCompletedUnixSeconds_ = 0;
    bool lastRefreshSucceeded_ = false;
    long long lastReleaseCheckUnixSeconds_ = 0;
    int refreshIntervalSeconds_ = 60;
    int refreshCountdownSeconds_ = 60;
    int releaseCheckCountdownSeconds_ = 6 * 60 * 60;
    int glassTransparencyPercent_ = 42;
    std::wstring latestReleaseTag_;
    std::wstring releaseCheckErrorMessage_;
    RECT refreshButtonRect_ = {};
    RECT closeButtonRect_ = {};
    RECT bubbleButtonRect_ = {};
    RECT settingsSliderRect_ = {};
    HDC layeredDc_ = nullptr;
    HBITMAP layeredBitmap_ = nullptr;
    HBITMAP layeredPreviousBitmap_ = nullptr;
    void* layeredBits_ = nullptr;
    SIZE layeredSurfaceSize_ = {};
    bool assetResourcesLoaded_ = false;
    bool nativeGlassEnabled_ = false;
    std::vector<std::wstring> registeredFontPaths_;

    UsageSnapshot snapshot_;
    CodexUsageFetcher fetcher_;

    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> renderTarget_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> solidBrush_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> codexIconBitmap_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> undoIconBitmap_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> textFormatKicker_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> textFormatTitle_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> textFormatDelta_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> textFormatMetricLabel_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> textFormatMetricValue_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> textFormatFoot_;
};
