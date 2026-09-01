#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>

namespace codex_usage {

struct TokenUsageTotals {
    std::uint64_t inputTokens = 0;
    std::uint64_t cachedInputTokens = 0;
    std::uint64_t cacheWriteInputTokens = 0;
    std::uint64_t outputTokens = 0;
    std::uint64_t reasoningOutputTokens = 0;
    std::uint64_t totalTokens = 0;
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

    static LocalUsagePaths ResolveDefaultPaths();

private:
    LocalUsagePaths paths_;
    std::unique_ptr<State> state_;
    bool stateLoaded_ = false;
    bool loadedFromCache_ = false;
};

std::wstring FormatCompactCount(std::uint64_t value);

}  // namespace codex_usage
