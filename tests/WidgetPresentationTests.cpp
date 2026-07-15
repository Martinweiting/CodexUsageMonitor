#include "WidgetPresentation.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>

using codex_widget::DisplayMode;
using codex_widget::PresentationState;

int main() {
    assert(codex_widget::ClampTransparencyPercent(5) == 20);
    assert(codex_widget::ClampTransparencyPercent(42) == 42);
    assert(codex_widget::ClampTransparencyPercent(95) == 80);

    assert(codex_widget::TransitionOnBubbleClick(
        PresentationState::Bubble) == PresentationState::PinnedExpanded);
    assert(codex_widget::TransitionOnBubbleClick(
        PresentationState::HoverExpanded) == PresentationState::PinnedExpanded);
    assert(codex_widget::TransitionOnBubbleClick(
        PresentationState::PinnedExpanded) == PresentationState::Bubble);
    assert(codex_widget::TransitionOnMouseLeave(
        PresentationState::HoverExpanded) == PresentationState::Bubble);
    assert(codex_widget::TransitionOnMouseLeave(
        PresentationState::PinnedExpanded) == PresentationState::PinnedExpanded);

    const codex_widget::WidgetRect expanded{100, 200, 520, 680};
    const codex_widget::WidgetRect bubble =
        codex_widget::BubbleGeometryForExpandedRect(expanded, 64);
    assert(bubble.right == expanded.right);
    assert(bubble.top == expanded.top);
    assert(bubble.left == expanded.right - 64);

    const codex_widget::WidgetGeometry hoverGeometry =
        codex_widget::GeometryForState(expanded, 64, PresentationState::HoverExpanded);
    assert(hoverGeometry.expanded.left == expanded.left);
    assert(hoverGeometry.expanded.top == expanded.top);
    assert(hoverGeometry.expanded.right == expanded.right);
    assert(hoverGeometry.expanded.bottom == expanded.bottom);
    assert(codex_widget::ContainsRect(hoverGeometry.expanded, hoverGeometry.bubble));

    const codex_widget::WidgetRect panel{100, 200, 520, 680};
    const codex_widget::WidgetGroupGeometry group =
        codex_widget::GroupGeometryForPanel(panel, 64, 14);
    assert(group.panel.left == 100);
    assert(group.panel.right == 520);
    assert(group.bubble.left == 534);
    assert(group.bubble.right == 598);
    assert(group.bubble.top == 200);
    assert(group.window.left == 100);
    assert(group.window.right == 598);
    assert(group.window.bottom == 680);
    assert(group.panel.right + 14 == group.bubble.left);
    assert(!codex_widget::ContainsRect(group.panel, group.bubble));
    assert(codex_widget::IsPointInsideWidgetGroup(group, 550, 220));
    assert(!codex_widget::IsPointInsideWidgetGroup(group, 620, 220));
    assert(!codex_widget::IsPointInsideWidgetGroup(group, 525, 220));
    assert(!codex_widget::IsPointInsideWidgetGroup(group, 550, 690));

    assert(!codex_widget::IsDragGesture(3, 0, 4));
    assert(!codex_widget::IsDragGesture(0, -4, 4));
    assert(codex_widget::IsDragGesture(5, 0, 4));
    assert(codex_widget::IsDragGesture(0, -5, 4));

    const codex_widget::WidgetGroupGeometry shifted =
        codex_widget::ShiftGroupIntoBounds(group, codex_widget::WidgetRect{0, 0, 560, 720});
    assert(shifted.panel.left == 62);
    assert(shifted.bubble.right == 560);
    assert(shifted.panel.right + 14 == shifted.bubble.left);

    const codex_widget::WidgetRect enlargedBubble =
        codex_widget::HoverBubbleRect(group.bubble, 74, 96, 96);
    assert(enlargedBubble.right - enlargedBubble.left == 74);
    assert(enlargedBubble.bottom - enlargedBubble.top == 74);
    assert(enlargedBubble.right <= group.bubble.right + 16);

    const codex_widget::WidgetRect moved =
        codex_widget::OffsetRect(expanded, 20, -10);
    assert(moved.left == 120);
    assert(moved.top == 190);
    assert(moved.right == 540);
    assert(moved.bottom == 670);

    const codex_widget::WidgetRect surface{0, 0, 420, 480};
    const codex_widget::WidgetRect closeRect =
        codex_widget::CloseControlRect(surface, 14, 28);
    assert(closeRect.left == 378);
    assert(closeRect.top == 14);
    assert(closeRect.right == 406);
    assert(closeRect.bottom == 42);

    const codex_widget::WidgetRect refreshRect =
        codex_widget::RefreshControlRect(surface, 14, 28);
    assert(refreshRect.left == 378);
    assert(refreshRect.top == 438);
    assert(refreshRect.right == 406);
    assert(refreshRect.bottom == 466);

    assert(codex_widget::FullModeHeightForCreditRows(286, 1, 18) == 286);
    assert(codex_widget::FullModeHeightForCreditRows(286, 3, 18) == 322);
    assert(codex_widget::FullModeHeightForCreditRows(620, 1, 20) == 620);
    assert(codex_widget::FullModeHeightForCreditRows(620, 3, 20) == 660);

    assert(codex_widget::TransparencyPercentForSlider(0, 0, 100) == 20);
    assert(codex_widget::TransparencyPercentForSlider(50, 0, 100) == 50);
    assert(codex_widget::TransparencyPercentForSlider(100, 0, 100) == 80);
    assert(codex_widget::TransitionOnSettingsClose() == PresentationState::Bubble);
    assert(codex_widget::kTaskbarWidgetLogicalDiameter == 96);
    assert(codex_widget::kTaskbarWidgetLogicalWidth == 96);
    assert(codex_widget::kTaskbarWidgetLogicalHeight == 96);
    assert(codex_widget::kTaskbarWidgetLogicalSize == 96);
    assert(codex_widget::DisplayModeUsesFloatingBubble(DisplayMode::Full));
    assert(codex_widget::DisplayModeUsesFloatingBubble(DisplayMode::Simple));
    assert(!codex_widget::DisplayModeUsesFloatingBubble(DisplayMode::Taskbar));
    assert(codex_widget::HoverStateForCursor(
        PresentationState::Bubble, DisplayMode::Full, true, true)
        == PresentationState::HoverExpanded);
    assert(codex_widget::HoverStateForCursor(
        PresentationState::HoverExpanded, DisplayMode::Simple, false, false)
        == PresentationState::Bubble);
    assert(codex_widget::HoverStateForCursor(
        PresentationState::PinnedExpanded, DisplayMode::Full, false, false)
        == PresentationState::PinnedExpanded);
    assert(codex_widget::HoverStateForCursor(
        PresentationState::Bubble, DisplayMode::Taskbar, true, true)
        == PresentationState::Bubble);

    UsageSnapshot snapshot;
    snapshot.success = true;
    snapshot.fiveHour.available = true;
    snapshot.fiveHour.usedPercent = 22;
    snapshot.fiveHour.remainingPercent = 78;
    snapshot.weekly.available = true;
    snapshot.weekly.usedPercent = 35;
    snapshot.weekly.remainingPercent = 65;
    snapshot.weekly.windowSeconds = 7 * 24 * 60 * 60;
    snapshot.weekly.resetAfterSeconds = 5 * 24 * 60 * 60;
    const codex_widget::UsageSummary summary = codex_widget::BuildUsageSummary(snapshot);
    assert(summary.lowestRemainingPercent == 65);
    assert(summary.cycleDay == 3);
    assert(summary.actualWeeklyUsedPercent == 35.0);
    assert(std::abs(summary.expectedWeeklyUsedPercent - (3.0 * (100.0 / 7.0))) < 0.001);
    assert(summary.paceDeltaPercent < 0.0);
    assert(std::wstring(codex_widget::ResetInventoryTitle(
        codex_widget::LanguageKind::TraditionalChinese)) ==
        L"使用量限制重設 (Full reset Weekly + 5 hr)");

    UsageSnapshot weeklyOnly;
    weeklyOnly.success = true;
    weeklyOnly.fiveHour.remainingPercent = 0;
    weeklyOnly.fiveHour.available = false;
    weeklyOnly.weekly.available = true;
    weeklyOnly.weekly.usedPercent = 41;
    weeklyOnly.weekly.remainingPercent = 59;
    weeklyOnly.weekly.windowSeconds = 7 * 24 * 60 * 60;
    weeklyOnly.weekly.resetAfterSeconds = 4 * 24 * 60 * 60;
    const codex_widget::UsageSummary weeklyOnlySummary = codex_widget::BuildUsageSummary(weeklyOnly);
    assert(weeklyOnlySummary.lowestRemainingPercent == 59);
    assert(weeklyOnlySummary.status != codex_widget::UsageStatus::Exhausted);
    assert(weeklyOnlySummary.actualWeeklyUsedPercent == 41.0);

    CodexUsageFetcher fetcher;
    std::wstring parseError;
    const UsageSnapshot partialPayload = fetcher.ParseUsageJson(
        R"({
            "email":"user@example.com",
            "plan_type":"pro",
            "rate_limit":{
                "primary_window":null,
                "secondary_window":{
                    "used_percent":12.6,
                    "limit_window_seconds":604800,
                    "reset_after_seconds":302400,
                    "reset_at":1780000000
                }
            }
        })",
        &parseError);
    assert(partialPayload.success);
    assert(!partialPayload.fiveHour.available);
    assert(partialPayload.weekly.available);
    assert(partialPayload.weekly.usedPercent == 13);
    assert(partialPayload.weekly.remainingPercent == 87);
    assert(partialPayload.weekly.windowSeconds == 604800);
    assert(partialPayload.weekly.resetAfterSeconds == 302400);

    const UsageSnapshot primaryWeeklyPayload = fetcher.ParseUsageJson(
        R"({"rate_limit":{"primary_window":{"used_percent":3,"limit_window_seconds":604800,"reset_after_seconds":250000,"reset_at":1780000000},"secondary_window":null}})",
        &parseError);
    assert(primaryWeeklyPayload.success);
    assert(!primaryWeeklyPayload.fiveHour.available);
    assert(primaryWeeklyPayload.weekly.available);
    assert(primaryWeeklyPayload.weekly.usedPercent == 3);
    assert(primaryWeeklyPayload.weekly.remainingPercent == 97);

    const UsageSnapshot bothWindowsPayload = fetcher.ParseUsageJson(
        R"({"rate_limit":{"primary_window":{"used_percent":2,"limit_window_seconds":18000,"reset_after_seconds":9000,"reset_at":1780000000},"secondary_window":{"used_percent":45,"limit_window_seconds":604800,"reset_after_seconds":250000,"reset_at":1780000000}}})",
        &parseError);
    assert(bothWindowsPayload.success);
    assert(bothWindowsPayload.fiveHour.available);
    assert(bothWindowsPayload.weekly.available);
    assert(bothWindowsPayload.fiveHour.remainingPercent == 98);
    assert(bothWindowsPayload.weekly.remainingPercent == 55);

    const UsageSnapshot invalidPayload = fetcher.ParseUsageJson(
        R"({"rate_limit":{"primary_window":null,"secondary_window":{"used_percent":"12"}}})",
        &parseError);
    assert(!invalidPayload.success);

    std::cout << "WidgetPresentationTests passed\n";
    return 0;
}
