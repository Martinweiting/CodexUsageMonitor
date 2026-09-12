#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace codex_usage {

struct TokenUsageTotals {
    std::uint64_t inputTokens = 0;
    std::uint64_t cachedInputTokens = 0;
    std::uint64_t cacheWriteInputTokens = 0;
    std::uint64_t outputTokens = 0;
    std::uint64_t reasoningOutputTokens = 0;
    std::uint64_t totalTokens = 0;
};

// Numeric metadata only. No titles, message text, paths or tool arguments.
struct ActivityEvent {
    std::uint32_t session = 0;
    long long time = 0;
    TokenUsageTotals tokens;
    bool userTurn = false;
    unsigned validFields = 0;
    bool initialCounter = false;
    std::string model, project;
};
struct ActivitySession {
    std::string id;
    long long created = 0;
    bool topLevel = true;
    unsigned origin = 2; // 0 explicit top-level, 1 non-top-level, 2 unknown
    std::string client;
    bool archived = false;
};
struct CompletedActivityTurn {
    std::string sessionId, turnId;
    long long completed = 0;
    std::uint64_t durationMilliseconds = 0;
    bool failed = false;
};
struct ActivityData {
    std::vector<ActivitySession> sessions;
    std::vector<ActivityEvent> events;
    std::uint64_t malformedLines = 0, invalidTimes = 0, duplicates = 0, resets = 0;
    long long generation = 0;
    std::vector<CompletedActivityTurn> completedTurns;
};

struct LocalUsageSnapshot {
    bool available = false;
    bool cacheLoaded = false;
    bool partial = false;
    bool rebuilt = false;
    bool changed = false;
    TokenUsageTotals today;
    TokenUsageTotals recordedTotal;
    std::uint64_t todayTaskCount = 0;
    std::uint64_t recordedTaskCount = 0;
    std::uint64_t todayTurnCount = 0;
    std::uint64_t recordedTurnCount = 0;
    std::uint64_t filesDiscovered = 0;
    std::uint64_t filesReadThisScan = 0;
    std::uint64_t bytesReadThisScan = 0;
    std::uint64_t tokenEventCount = 0;
    long long coverageStartUnixSeconds = 0;
    long long lastEventUnixSeconds = 0;
    long long lastScanUnixSeconds = 0;
    std::wstring errorMessage;
    std::shared_ptr<const ActivityData> activity;
    std::uint64_t scanMilliseconds = 0, cacheBytes = 0;
};

struct LocalUsagePaths {
    std::filesystem::path codexHome;
    std::filesystem::path cacheFile;
};

class LocalUsageStatsCollector {
public:
    struct State;

    LocalUsageStatsCollector();
    explicit LocalUsageStatsCollector(LocalUsagePaths paths);
    ~LocalUsageStatsCollector();

    LocalUsageStatsCollector(const LocalUsageStatsCollector&) = delete;
    LocalUsageStatsCollector& operator=(const LocalUsageStatsCollector&) = delete;

    LocalUsageSnapshot Refresh(
        std::stop_token stopToken = {},
        long long nowUnixSecondsOverride = 0,
        long long todayStartUnixSecondsOverride = 0);
    void RequestRebuild() { rebuildRequested_ = true; }

    static LocalUsagePaths ResolveDefaultPaths();

private:
    LocalUsagePaths paths_;
    std::unique_ptr<State> state_;
    bool stateLoaded_ = false;
    bool loadedFromCache_ = false;
    bool rebuildRequested_ = false;
};

std::wstring FormatCompactCount(std::uint64_t value);
// Formats large counts with Traditional-Chinese units (萬／億／兆／京).
// Values below 10,000 remain plain integers so small activity counts stay
// precise and easy to scan.
std::wstring FormatTraditionalChineseCount(std::uint64_t value);

}  // namespace codex_usage
