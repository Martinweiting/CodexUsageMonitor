#include "WidgetPresentation.h"

#include <algorithm>
#include <string>

namespace codex_widget {

PresentationState TransitionOnBubbleClick(PresentationState state) {
    return state == PresentationState::PinnedExpanded
        ? PresentationState::Bubble
        : PresentationState::PinnedExpanded;
}

PresentationState TransitionOnMouseLeave(PresentationState state) {
    return state == PresentationState::HoverExpanded
        ? PresentationState::Bubble
        : state;
}

PresentationState TransitionOnSettingsClose() {
    return PresentationState::Bubble;
}

PresentationState HoverStateForCursor(
    PresentationState state,
    DisplayMode mode,
    bool cursorInsideBubble,
    bool cursorInsideWindow) {
    if (mode == DisplayMode::Taskbar) {
        return state;
    }
    if (state == PresentationState::Bubble && cursorInsideBubble) {
        return PresentationState::HoverExpanded;
    }
    if (state == PresentationState::HoverExpanded && !cursorInsideWindow) {
        return PresentationState::Bubble;
    }
    return state;
}

bool IsDragGesture(int deltaX, int deltaY, int threshold) {
    const int distance = std::max(0, threshold);
    const int absoluteX = deltaX < 0 ? -deltaX : deltaX;
    const int absoluteY = deltaY < 0 ? -deltaY : deltaY;
    return absoluteX > distance || absoluteY > distance;
}

bool DisplayModeUsesFloatingBubble(DisplayMode mode) {
    return mode != DisplayMode::Taskbar;
}

int ClampTransparencyPercent(int value) {
    return std::clamp(value, 20, 80);
}

int TransparencyPercentForSlider(int x, int left, int right) {
    if (right <= left) {
        return 20;
    }

    const int clampedX = std::clamp(x, left, right);
    const int span = right - left;
    const int offset = clampedX - left;
    return ClampTransparencyPercent(20 + (offset * 60 + span / 2) / span);
}

WidgetRect BubbleGeometryForExpandedRect(const WidgetRect& expanded, int bubbleSize) {
    const int size = std::max(0, bubbleSize);
    return WidgetRect{
        expanded.right - size,
        expanded.top,
        expanded.right,
        expanded.top + size,
    };
}

WidgetGeometry GeometryForState(const WidgetRect& expanded, int bubbleSize, PresentationState state) {
    (void)state;
    return WidgetGeometry{ expanded, BubbleGeometryForExpandedRect(expanded, bubbleSize) };
}

WidgetGroupGeometry GroupGeometryForPanel(const WidgetRect& panel, int bubbleSize, int gap) {
    const int size = std::max(0, bubbleSize);
    const int spacing = std::max(0, gap);
    const WidgetRect bubble{
        panel.right + spacing,
        panel.top,
        panel.right + spacing + size,
        panel.top + size,
    };
    return WidgetGroupGeometry{
        panel,
        bubble,
        WidgetRect{
            panel.left,
            std::min(panel.top, bubble.top),
            bubble.right,
            std::max(panel.bottom, bubble.bottom),
        },
    };
}

WidgetGroupGeometry ShiftGroupIntoBounds(
    const WidgetGroupGeometry& geometry,
    const WidgetRect& bounds) {
    int deltaX = 0;
    int deltaY = 0;
    if (geometry.window.right > bounds.right) {
        deltaX = bounds.right - geometry.window.right;
    }
    if (geometry.window.left + deltaX < bounds.left) {
        deltaX += bounds.left - (geometry.window.left + deltaX);
    }
    if (geometry.window.bottom > bounds.bottom) {
        deltaY = bounds.bottom - geometry.window.bottom;
    }
    if (geometry.window.top + deltaY < bounds.top) {
        deltaY += bounds.top - (geometry.window.top + deltaY);
    }

    return WidgetGroupGeometry{
        OffsetRect(geometry.panel, deltaX, deltaY),
        OffsetRect(geometry.bubble, deltaX, deltaY),
        OffsetRect(geometry.window, deltaX, deltaY),
    };
}

WidgetRect HoverBubbleRect(
    const WidgetRect& bubble,
    int hoverSize,
    int maxWidth,
    int maxHeight) {
    const int size = std::max(0, std::min({ hoverSize, maxWidth, maxHeight }));
    const int centerX = bubble.left + (bubble.right - bubble.left) / 2;
    const int centerY = bubble.top + (bubble.bottom - bubble.top) / 2;
    const int halfSize = size / 2;
    return WidgetRect{
        centerX - halfSize,
        centerY - halfSize,
        centerX - halfSize + size,
        centerY - halfSize + size,
    };
}

bool IsPointInsideWidgetGroup(const WidgetGroupGeometry& geometry, int x, int y) {
    const auto contains = [x, y](const WidgetRect& rect) {
        return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
    };
    return contains(geometry.panel) || contains(geometry.bubble);
}

bool ContainsRect(const WidgetRect& outer, const WidgetRect& inner) {
    return inner.left >= outer.left
        && inner.top >= outer.top
        && inner.right <= outer.right
        && inner.bottom <= outer.bottom;
}

WidgetRect OffsetRect(const WidgetRect& rect, int deltaX, int deltaY) {
    return WidgetRect{
        rect.left + deltaX,
        rect.top + deltaY,
        rect.right + deltaX,
        rect.bottom + deltaY,
    };
}

WidgetRect CloseControlRect(const WidgetRect& surface, int inset, int controlSize) {
    return WidgetRect{
        surface.right - inset - controlSize,
        surface.top + inset,
        surface.right - inset,
        surface.top + inset + controlSize,
    };
}

WidgetRect RefreshControlRect(const WidgetRect& surface, int inset, int controlSize) {
    return WidgetRect{
        surface.right - inset - controlSize,
        surface.bottom - inset - controlSize,
        surface.right - inset,
        surface.bottom - inset,
    };
}

int FullModeHeightForCreditRows(int baseHeight, int creditRows, int rowHeight) {
    return baseHeight + std::max(0, creditRows - 1) * std::max(0, rowHeight);
}

UsageSummary BuildUsageSummary(const UsageSnapshot& snapshot) {
    UsageSummary summary;
    summary.resetInventoryTitle = ResetInventoryTitle(LanguageKind::English);
    if (!snapshot.success) {
        summary.status = snapshot.errorMessage.empty() ? UsageStatus::Loading : UsageStatus::Failed;
        return summary;
    }

    bool hasAvailableWindow = false;
    summary.lowestRemainingPercent = 100;
    if (snapshot.fiveHour.available) {
        summary.lowestRemainingPercent = std::min(summary.lowestRemainingPercent, snapshot.fiveHour.remainingPercent);
        hasAvailableWindow = true;
    }
    if (snapshot.weekly.available) {
        summary.lowestRemainingPercent = std::min(summary.lowestRemainingPercent, snapshot.weekly.remainingPercent);
        summary.actualWeeklyUsedPercent = static_cast<double>(snapshot.weekly.usedPercent);
        hasAvailableWindow = true;
    }
    if (!hasAvailableWindow) {
        summary.status = UsageStatus::Loading;
        return summary;
    }

    summary.lowestRemainingPercent = std::clamp(summary.lowestRemainingPercent, 0, 100);

    const bool fiveHourExhausted = snapshot.fiveHour.available && snapshot.fiveHour.remainingPercent <= 0;
    const bool weeklyExhausted = snapshot.weekly.available && snapshot.weekly.remainingPercent <= 0;
    if (fiveHourExhausted || weeklyExhausted) {
        summary.status = UsageStatus::Exhausted;
    } else if (snapshot.weekly.available && snapshot.weekly.windowSeconds > 0) {
        const int elapsedSeconds = std::clamp(
            snapshot.weekly.windowSeconds - snapshot.weekly.resetAfterSeconds,
            0,
            snapshot.weekly.windowSeconds);
        const int elapsedDays = elapsedSeconds / (24 * 60 * 60);
        summary.cycleDay = std::clamp(elapsedDays + 1, 1, 7);
        summary.expectedWeeklyUsedPercent = summary.cycleDay * (100.0 / 7.0);
        summary.paceDeltaPercent = summary.actualWeeklyUsedPercent - summary.expectedWeeklyUsedPercent;
        summary.status = snapshot.weekly.remainingPercent <= 15 || summary.paceDeltaPercent > 0.001
            ? UsageStatus::Tight
            : UsageStatus::Normal;
    } else {
        summary.status = UsageStatus::Normal;
    }

    return summary;
}

const wchar_t* ResetInventoryTitle(LanguageKind language) {
    return language == LanguageKind::TraditionalChinese
        ? L"使用量限制重設 (Full reset Weekly + 5 hr)"
        : L"Usage limit reset (Full reset Weekly + 5 hr)";
}

}  // namespace codex_widget
