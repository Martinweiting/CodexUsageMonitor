#pragma once

#include "CodexUsageFetcher.h"

#include <string>

namespace codex_widget {

enum class PresentationState { Bubble, HoverExpanded, PinnedExpanded };
enum class DisplayMode { Full, Simple, Taskbar };
enum class LanguageKind { English, TraditionalChinese };
enum class UsageStatus { Loading, Failed, Normal, Tight, Exhausted };

constexpr int kTaskbarWidgetLogicalDiameter = 96;
constexpr int kTaskbarWidgetLogicalWidth = kTaskbarWidgetLogicalDiameter;
constexpr int kTaskbarWidgetLogicalHeight = kTaskbarWidgetLogicalDiameter;
constexpr int kTaskbarWidgetLogicalSize = kTaskbarWidgetLogicalDiameter;

struct WidgetRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

struct WidgetGeometry {
    WidgetRect expanded;
    WidgetRect bubble;
};

struct WidgetGroupGeometry {
    WidgetRect panel;
    WidgetRect bubble;
    WidgetRect window;
};

struct UsageSummary {
    UsageStatus status = UsageStatus::Loading;
    int lowestRemainingPercent = 100;
    int cycleDay = 0;
    double expectedWeeklyUsedPercent = 0.0;
    double actualWeeklyUsedPercent = 0.0;
    double paceDeltaPercent = 0.0;
    std::wstring resetInventoryTitle;
};

PresentationState TransitionOnBubbleClick(PresentationState state);
PresentationState TransitionOnMouseLeave(PresentationState state);
PresentationState TransitionOnSettingsClose();
PresentationState HoverStateForCursor(
    PresentationState state,
    DisplayMode mode,
    bool cursorInsideBubble,
    bool cursorInsideWindow);
bool IsDragGesture(int deltaX, int deltaY, int threshold);
bool DisplayModeUsesFloatingBubble(DisplayMode mode);
int ClampTransparencyPercent(int value);
int TransparencyPercentForSlider(int x, int left, int right);
WidgetRect BubbleGeometryForExpandedRect(const WidgetRect& expanded, int bubbleSize);
WidgetGeometry GeometryForState(const WidgetRect& expanded, int bubbleSize, PresentationState state);
WidgetGroupGeometry GroupGeometryForPanel(const WidgetRect& panel, int bubbleSize, int gap);
WidgetGroupGeometry ShiftGroupIntoBounds(const WidgetGroupGeometry& geometry, const WidgetRect& bounds);
WidgetRect HoverBubbleRect(const WidgetRect& bubble, int hoverSize, int maxWidth, int maxHeight);
bool IsPointInsideWidgetGroup(const WidgetGroupGeometry& geometry, int x, int y);
bool ContainsRect(const WidgetRect& outer, const WidgetRect& inner);
WidgetRect OffsetRect(const WidgetRect& rect, int deltaX, int deltaY);
WidgetRect CloseControlRect(const WidgetRect& surface, int inset, int controlSize);
WidgetRect RefreshControlRect(const WidgetRect& surface, int inset, int controlSize);
int FullModeHeightForCreditRows(int baseHeight, int creditRows, int rowHeight);
UsageSummary BuildUsageSummary(const UsageSnapshot& snapshot);
const wchar_t* ResetInventoryTitle(LanguageKind language);

}  // namespace codex_widget
