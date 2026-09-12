#include "WidgetPresentation.h"
#include "LocalUsageStats.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using codex_widget::DisplayMode;
using codex_widget::PresentationState;

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
            / (L"CodexUsageMonitorTests-" + std::to_wstring(nonce));
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

void WriteText(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    assert(output);
}

void AppendText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    assert(output);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    assert(output);
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

std::string TokenLine(
    const char* timestamp,
    std::uint64_t input,
    std::uint64_t cached,
    std::uint64_t cacheWrite,
    std::uint64_t output,
    std::uint64_t reasoning,
    std::uint64_t total) {
    std::ostringstream line;
    line << "{\"timestamp\":\"" << timestamp
         << "\",\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\","
            "\"info\":{\"total_token_usage\":{"
         << "\"input_tokens\":" << input
         << ",\"cached_input_tokens\":" << cached
         << ",\"cache_write_input_tokens\":" << cacheWrite
         << ",\"output_tokens\":" << output
         << ",\"reasoning_output_tokens\":" << reasoning
         << ",\"total_tokens\":" << total << "}}}}";
    return line.str();
}

void VerifyExactTokenCounters() {
    TemporaryDirectory temporary;
    const auto home = temporary.path / L"home";
    const auto source = home / L"sessions" / L"exact.jsonl";
    std::string fixture = R"({"timestamp":"2026-09-08T00:00:00Z","type":"session_meta","payload":{"id":"exact","thread_source":"user"}})" "\n";
    fixture += TokenLine("2026-09-08T00:00:01Z", 100, 50, 0, 0, 0, 9007199254740993ULL) + "\n";
    fixture += R"({"timestamp":"2026-09-08T00:00:02Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":110,"total_tokens":9007199254740994}}}})" "\n";
    fixture += TokenLine("2026-09-08T00:00:03Z", 120, 60, 0, 0, 0, 9007199254740995ULL) + "\n";
    WriteText(source, fixture);
    codex_usage::LocalUsageStatsCollector collector({home, temporary.path / L"cache.tsv"});
    const auto snapshot = collector.Refresh({}, 1788836400, 1788825600);
    assert(snapshot.recordedTotal.totalTokens == 9007199254740995ULL);
    assert(snapshot.recordedTotal.cachedInputTokens == 60);
    assert(snapshot.activity->resets == 0);
    assert(snapshot.activity->events[1].tokens.totalTokens == 1);
    assert((snapshot.activity->events[1].validFields & 2) == 0);
    assert(snapshot.activity->events[2].tokens.cachedInputTokens == 10);
}

void VerifyActivityMetadata() {
    TemporaryDirectory temporary;
    const auto home = temporary.path / L"home";
    const auto source = home / L"sessions" / L"metadata.jsonl";
    const auto cache = temporary.path / L"cache.tsv";
    std::string fixture = R"({"timestamp":"2026-09-08T00:00:00Z","type":"session_meta","payload":{"id":"metadata","thread_source":"user","originator":"codex_cli_rs"}})" "\n";
    fixture += R"({"timestamp":"2026-09-08T00:00:01Z","type":"turn_context","payload":{"model":"model-a","cwd":"SECRET_PROJECT_PATH","turn_id":"turn-a"}})" "\n";
    fixture += TokenLine("2026-09-08T00:00:02Z", 10, 0, 0, 0, 0, 10) + "\n";
    fixture += TokenLine("2026-09-08T00:00:03Z", 20, 0, 0, 0, 0, 20) + "\n";
    fixture += R"({"timestamp":"2026-09-08T00:00:04Z","type":"turn_context","payload":{"model":"model-b","cwd":"SECRET_PROJECT_PATH"}})" "\n";
    fixture += TokenLine("2026-09-08T00:00:05Z", 30, 0, 0, 0, 0, 30) + "\n";
    fixture += TokenLine("2026-09-08T00:00:06Z", 40, 0, 0, 0, 0, 40) + "\n";
    fixture += R"({"timestamp":"2026-09-08T00:00:07Z","type":"event_msg","payload":{"type":"task_complete","turn_id":"turn-a","duration_ms":1200,"error":null,"last_agent_message":"SECRET_RESPONSE"}})" "\n";
    fixture += R"({"timestamp":"2026-09-08T00:00:08Z","type":"turn_context","payload":{}})" "\n";
    fixture += TokenLine("2026-09-08T00:00:09Z", 50, 0, 0, 0, 0, 50) + "\n";
    WriteText(source, fixture);
    codex_usage::LocalUsageStatsCollector collector({home, cache});
    const auto first = collector.Refresh({}, 1788836400, 1788825600);
    assert(first.activity && first.activity->events.size() == 5);
    assert(first.activity->events[0].model.empty());
    assert(first.activity->events[1].model == "model-a");
    assert(first.activity->events[2].model.empty());
    assert(first.activity->events[3].model == "model-b");
    assert(first.activity->events[4].model.empty());
    assert(first.activity->completedTurns.size() == 1);
    assert(first.activity->completedTurns[0].durationMilliseconds == 1200);
    assert(first.activity->sessions[0].client == "codex_cli_rs");
    const auto saved = ReadText(cache);
    assert(saved.find("SECRET_") == std::string::npos);
    codex_usage::LocalUsageStatsCollector loaded({home, cache});
    const auto unchanged = loaded.Refresh({}, 1788836400, 1788825600);
    assert(unchanged.bytesReadThisScan == 0);
    assert(unchanged.activity->events[1].model == "model-a");
    assert(unchanged.activity->completedTurns.size() == 1);
    loaded.RequestRebuild();
    std::stop_source cancel;
    cancel.request_stop();
    assert(loaded.Refresh(cancel.get_token(), 1788836400, 1788825600).recordedTotal.totalTokens == 50);
    assert(ReadText(cache) == saved);
    // Same-sized replacement must invalidate its checkpoint, even without truncation.
    auto replacement = fixture;
    const auto at = replacement.find("model-a");
    replacement.replace(at, 7, "model-c");
    WriteText(source, replacement);
    std::filesystem::last_write_time(source, std::filesystem::last_write_time(source) + std::chrono::seconds(2));
    codex_usage::LocalUsageStatsCollector replaced({home, cache});
    const auto changed = replaced.Refresh({}, 1788836400, 1788825600);
    assert(changed.rebuilt && changed.activity->events[1].model == "model-c");
}

void VerifyLocalUsageIncrementalIndex() {
    TemporaryDirectory temporary;
    const std::filesystem::path codexHome = temporary.path / L"codex-home";
    const std::filesystem::path sessions = codexHome / L"sessions" / L"2026" / L"08" / L"31";
    const std::filesystem::path archive = codexHome / L"archived_sessions";
    const std::filesystem::path sessionA = sessions / L"session-a.jsonl";
    const std::filesystem::path fork = sessions / L"session-fork.jsonl";
    const std::filesystem::path subagent = sessions / L"session-subagent.jsonl";
    const std::filesystem::path index = codexHome / L"session_index.jsonl";
    const std::filesystem::path cache = temporary.path / L"cache" / L"local-usage-cache-v1.tsv";

    const std::string metaA =
        R"({"timestamp":"2026-08-30T11:00:00Z","type":"session_meta","payload":{"id":"A","thread_source":"user"}})";
    const std::string userYesterday =
        R"({"timestamp":"2026-08-30T12:00:00Z","type":"event_msg","payload":{"type":"user_message","message":"SECRET_TEST_PROMPT"}})";
    const std::string tokenYesterday = TokenLine("2026-08-30T12:01:00Z", 100, 40, 0, 10, 2, 110);
    const std::string tokenRepeated = TokenLine("2026-08-30T13:00:00Z", 100, 40, 0, 10, 2, 110);
    const std::string userToday =
        R"({"timestamp":"2026-08-31T01:00:00Z","type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"today"}]}})";
    const std::string tokenToday = TokenLine("2026-08-31T01:01:00Z", 150, 60, 0, 20, 4, 170);
    const std::string tokenReset = TokenLine("2026-08-31T02:00:00Z", 5, 1, 0, 2, 1, 7);

    const std::string aContent = metaA + "\n" + userYesterday + "\n" + tokenYesterday + "\n"
        + tokenRepeated + "\n" + userToday + "\n" + tokenToday + "\n" + tokenReset + "\n";
    WriteText(sessionA, aContent);
    // A fork contains copied parent history. The original session id and timestamps must deduplicate it.
    WriteText(fork, metaA + "\n" + userYesterday + "\n" + tokenYesterday + "\n"
        + tokenRepeated + "\n" + userToday + "\n" + tokenToday + "\n" + tokenReset + "\n");
    WriteText(subagent,
        R"({"timestamp":"2026-08-31T02:30:00Z","type":"session_meta","payload":{"id":"B","source":{"subagent":{"kind":"worker"}}}})"
        "\n"
        R"({"timestamp":"2026-08-31T02:31:00Z","type":"event_msg","payload":{"type":"user_message","message":"background"}})"
        "\n" + TokenLine("2026-08-31T02:32:00Z", 25, 5, 0, 5, 2, 30) + "\n");
    WriteText(index,
        R"({"id":"A","thread_name":"Main task","updated_at":"2026-08-31T02:00:00Z"})"
        "\n"
        R"({"id":"INDEX_ONLY","thread_name":"Indexed task","updated_at":"2026-08-31T02:00:00Z"})"
        "\n");

    constexpr long long todayStart = 1788134400;
    constexpr long long now = 1788220799;
    const codex_usage::LocalUsagePaths paths{ codexHome, cache };
    codex_usage::LocalUsageStatsCollector collector(paths);
    const codex_usage::LocalUsageSnapshot first = collector.Refresh({}, now, todayStart);
    assert(first.available);
    assert(!first.partial);
    assert(first.filesDiscovered == 4);
    assert(first.bytesReadThisScan > 0);
    assert(first.recordedTotal.totalTokens == 207);
    assert(first.today.totalTokens == 97);
    assert(first.recordedTotal.inputTokens == 180);
    assert(first.recordedTotal.outputTokens == 27);
    assert(first.recordedTotal.cachedInputTokens == 66);
    assert(first.recordedTaskCount == 2);
    assert(first.todayTaskCount == 1);
    assert(first.recordedTurnCount == 2);
    assert(first.todayTurnCount == 1);
    assert(ReadText(cache).find("SECRET_TEST_PROMPT") == std::string::npos);

    // A restarted collector must load the cache and read no JSONL bytes when nothing changed.
    codex_usage::LocalUsageStatsCollector restarted(paths);
    const codex_usage::LocalUsageSnapshot unchanged = restarted.Refresh({}, now, todayStart);
    assert(unchanged.available);
    assert(unchanged.cacheLoaded);
    assert(unchanged.bytesReadThisScan == 0);
    assert(!unchanged.changed);
    assert(unchanged.recordedTotal.totalTokens == 207);

    // Moving an active file to archived_sessions retains its filename checkpoint.
    std::filesystem::create_directories(archive);
    const std::filesystem::path archivedA = archive / sessionA.filename();
    std::filesystem::rename(sessionA, archivedA);
    const codex_usage::LocalUsageSnapshot moved = restarted.Refresh({}, now, todayStart);
    assert(moved.bytesReadThisScan == 0);
    assert(moved.recordedTotal.totalTokens == 207);

    const std::string appended = TokenLine("2026-08-31T04:00:00Z", 10, 2, 0, 5, 2, 15) + "\n"
        + R"({"timestamp":"2026-08-31T04:01:00Z","type":"event_msg","payload":{"type":"user_message","message":"new"}})"
        + "\n";
    AppendText(archivedA, appended);
    const codex_usage::LocalUsageSnapshot incremented = restarted.Refresh({}, now, todayStart);
    assert(incremented.bytesReadThisScan == appended.size());
    assert(incremented.filesReadThisScan == 1);
    assert(incremented.recordedTotal.totalTokens == 215);
    assert(incremented.today.totalTokens == 105);
    assert(incremented.recordedTurnCount == 3);
    assert(incremented.todayTurnCount == 2);

    // An unterminated tail is left at the old offset until its newline arrives.
    const std::string incomplete = TokenLine("2026-08-31T05:00:00Z", 13, 2, 0, 7, 2, 20);
    AppendText(archivedA, incomplete);
    const codex_usage::LocalUsageSnapshot deferred = restarted.Refresh({}, now, todayStart);
    assert(deferred.bytesReadThisScan == 0);
    assert(deferred.recordedTotal.totalTokens == 215);
    AppendText(archivedA, "\n");
    const codex_usage::LocalUsageSnapshot completed = restarted.Refresh({}, now, todayStart);
    assert(completed.bytesReadThisScan == incomplete.size() + 1);
    assert(completed.recordedTotal.totalTokens == 220);
    assert(completed.today.totalTokens == 110);

    // A shrunken file invalidates checkpoints and rebuilds instead of layering onto stale totals.
    WriteText(fork, "");
    const codex_usage::LocalUsageSnapshot rebuilt = restarted.Refresh({}, now, todayStart);
    assert(rebuilt.rebuilt);
    assert(rebuilt.recordedTotal.totalTokens == 220);
    assert(ReadText(cache).find("SECRET_TEST_PROMPT") == std::string::npos);
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    _set_error_mode(_OUT_TO_STDERR);
#endif
    if (argc == 4 && std::string(argv[1]) == "--scan-local") {
        const codex_usage::LocalUsagePaths paths{
            std::filesystem::path(argv[2]),
            std::filesystem::path(argv[3]),
        };
        codex_usage::LocalUsageStatsCollector collector(paths);
        const codex_usage::LocalUsageSnapshot first = collector.Refresh();
        const codex_usage::LocalUsageSnapshot second = collector.Refresh();
        const codex_usage::LocalUsageSnapshot stable = collector.Refresh();
        const auto printSnapshot = [](const char* label, const codex_usage::LocalUsageSnapshot& value) {
            std::cout << label
                      << " available=" << value.available
                      << " files=" << value.filesDiscovered
                      << " files_read=" << value.filesReadThisScan
                      << " bytes_read=" << value.bytesReadThisScan
                      << " token_events=" << value.tokenEventCount
                      << " today_tokens=" << value.today.totalTokens
                      << " local_tokens=" << value.recordedTotal.totalTokens
                      << " today_tasks=" << value.todayTaskCount
                      << " local_tasks=" << value.recordedTaskCount
                      << " today_turns=" << value.todayTurnCount
                      << " local_turns=" << value.recordedTurnCount
                      << " partial=" << value.partial
                      << " rebuilt=" << value.rebuilt
                      << '\n';
        };
        printSnapshot("first", first);
        printSnapshot("second", second);
        printSnapshot("stable", stable);
        if (!first.errorMessage.empty()) {
            std::wcerr << L"first_error=" << first.errorMessage << L'\n';
        }
        if (!second.errorMessage.empty()) {
            std::wcerr << L"second_error=" << second.errorMessage << L'\n';
        }
        return first.available && first.filesDiscovered > 0 && first.tokenEventCount > 0
                && second.available && stable.available && stable.bytesReadThisScan == 0
            ? 0
            : 2;
    }

    assert(codex_widget::ClampTransparencyPercent(-5) == 0);
    assert(codex_widget::ClampTransparencyPercent(5) == 5);
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

    assert(codex_widget::TransparencyPercentForSlider(0, 0, 100) == 0);
    assert(codex_widget::TransparencyPercentForSlider(50, 0, 100) == 40);
    assert(codex_widget::TransparencyPercentForSlider(100, 0, 100) == 80);
    assert(codex_widget::TransparencyPercentForSlider(40, 40, 40) == 0);
    assert(codex_widget::TransitionOnSettingsClose() == PresentationState::Bubble);
    assert(codex_widget::kTaskbarWidgetLogicalDiameter == 96);
    assert(codex_widget::kTaskbarWidgetLogicalWidth == 96);
    assert(codex_widget::kTaskbarWidgetLogicalHeight == 96);
    assert(codex_widget::kTaskbarWidgetLogicalSize == 96);
    assert(codex_widget::DisplayModeUsesFloatingBubble(DisplayMode::Full));
    assert(codex_widget::DisplayModeUsesFloatingBubble(DisplayMode::Simple));
    assert(!codex_widget::DisplayModeUsesFloatingBubble(DisplayMode::Taskbar));
    assert(codex_widget::kSimpleWidgetLogicalHeight == 154);
    assert(codex_widget::FullPageAfterTabClick(
        codex_widget::FullPage::Quota, false, true) == codex_widget::FullPage::Activity);
    assert(codex_widget::FullPageAfterTabClick(
        codex_widget::FullPage::Activity, true, false) == codex_widget::FullPage::Quota);
    assert(codex_usage::FormatCompactCount(999) == L"999");
    assert(codex_usage::FormatCompactCount(1500) == L"1.5K");
    assert(codex_usage::FormatCompactCount(1250000) == L"1.3M");
    assert(codex_usage::FormatTraditionalChineseCount(999) == L"999");
    assert(codex_usage::FormatTraditionalChineseCount(12345) == L"1.2 萬");
    assert(codex_usage::FormatTraditionalChineseCount(37700000) == L"3770 萬");
    assert(codex_usage::FormatTraditionalChineseCount(89300000000) == L"893 億");
    assert(codex_usage::FormatTraditionalChineseCount(1200000000000) == L"1.2 兆");
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

    const UsageSnapshot enrichedPayload = fetcher.ParseUsageJson(
        R"({
            "rate_limit":{
                "allowed":false,
                "limit_reached":true,
                "primary_window":{"used_percent":2,"limit_window_seconds":18000},
                "secondary_window":{"used_percent":45,"limit_window_seconds":604800}
            },
            "rate_limit_reached_type":"weekly",
            "credits":{
                "has_credits":true,
                "unlimited":false,
                "overage_limit_reached":false,
                "balance":12.5,
                "approx_local_messages":[4,8],
                "approx_cloud_messages":[2,5]
            },
            "spend_control":{"reached":false},
            "rate_limit_reset_credits":{"applicable_available_count":2}
        })",
        &parseError);
    assert(enrichedPayload.success);
    assert(enrichedPayload.endpointStatus.hasAllowed && !enrichedPayload.endpointStatus.allowed);
    assert(enrichedPayload.endpointStatus.hasLimitReached && enrichedPayload.endpointStatus.limitReached);
    assert(enrichedPayload.endpointStatus.rateLimitReachedType == L"weekly");
    assert(enrichedPayload.credits.available);
    assert(enrichedPayload.credits.hasCredits && enrichedPayload.credits.creditsEnabled);
    assert(enrichedPayload.credits.hasBalance && enrichedPayload.credits.balance == 12.5);
    assert(enrichedPayload.credits.hasApproxLocalMessages);
    assert(enrichedPayload.credits.approxLocalMessagesMin == 4);
    assert(enrichedPayload.credits.approxLocalMessagesMax == 8);
    assert(enrichedPayload.credits.hasApproxCloudMessages);
    assert(enrichedPayload.spendControl.hasReached && !enrichedPayload.spendControl.reached);
    assert(enrichedPayload.hasApplicableResetCredits && enrichedPayload.applicableResetCredits == 2);

    const UsageSnapshot nullOptionalPayload = fetcher.ParseUsageJson(
        R"({
            "rate_limit":{
                "allowed":null,
                "limit_reached":"no",
                "primary_window":{"used_percent":1,"limit_window_seconds":18000}
            },
            "credits":{"balance":"unknown","approx_local_messages":null},
            "spend_control":null,
            "rate_limit_reset_credits":{"applicable_available_count":"two"}
        })",
        &parseError);
    assert(nullOptionalPayload.success);
    assert(!nullOptionalPayload.endpointStatus.hasAllowed);
    assert(!nullOptionalPayload.endpointStatus.hasLimitReached);
    assert(nullOptionalPayload.credits.available && !nullOptionalPayload.credits.hasBalance);
    assert(!nullOptionalPayload.credits.hasApproxLocalMessages);
    assert(!nullOptionalPayload.spendControl.hasReached);
    assert(!nullOptionalPayload.hasApplicableResetCredits);

    const UsageSnapshot invalidPayload = fetcher.ParseUsageJson(
        R"({"rate_limit":{"primary_window":null,"secondary_window":{"used_percent":"12"}}})",
        &parseError);
    assert(!invalidPayload.success);

    VerifyLocalUsageIncrementalIndex();
    VerifyActivityMetadata();
    VerifyExactTokenCounters();

    std::cout << "WidgetPresentationTests passed\n";
    return 0;
}
