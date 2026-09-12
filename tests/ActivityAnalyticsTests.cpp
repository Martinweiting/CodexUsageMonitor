#include "ActivityAnalytics.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>

int main() {
    using namespace activity;
    using namespace codex_usage;
    auto data = std::make_shared<ActivityData>();
    const long long today = DayStart(1788836400);
    const auto yesterday = ShiftDays(today, -1);
    data->sessions = {{"top", yesterday, true, 0}, {"agent", today, false, 1}, {"unknown", today, true, 2}};
    data->events = {{0, yesterday + 3600, {90, 20, 0, 10, 2, 100}, false, 63, true},
                    {0, today + 60, {40, 10, 0, 10, 2, 50}, false, 63, false},
                    {0, today + 70, {}, true, 63, false},
                    {0, today + 3600, {20, 4, 0, 5, 1, 25}, false, 63, false},
                    {0, today + 3610, {}, true, 63, false},
                    {1, today + 65, {10, 0, 0, 5, 0, 15}, false, 41, true},
                    {2, today + 75, {5, 0, 0, 0, 0, 5}, false, 33, true}};
    LocalUsageSnapshot snapshot;
    snapshot.available = true;
    snapshot.activity = data;
    snapshot.coverageStartUnixSeconds = ShiftDays(today, -40);
    snapshot.lastScanUnixSeconds = today + 7200;
    auto r = Analyze(snapshot, {});
    assert(r.available);
    assert(r.tokens.totalTokens == 95);
    assert(r.turns == 2);
    assert(r.activeTasks == 1);
    assert(r.tasks.size() == 3);
    assert(r.sourceTokens[0] == 75 && r.sourceTokens[1] == 15 && r.sourceTokens[2] == 5);
    assert(r.topLevelTokens.totalTokens == 80);
    assert(r.activeTaskTokens.totalTokens == 75);
    assert(r.tokenOnly == 2);
    std::uint64_t sum = 0;
    for (auto &d : r.days)
        sum += d.tokens.totalTokens;
    assert(sum == r.tokens.totalTokens);
    sum = 0;
    for (auto &t : r.tasks)
        sum += t.tokens.totalTokens;
    assert(sum == r.tokens.totalTokens);
    assert(r.previous.totalTokens == 100);
    assert(r.estimatedSeconds == 145);
    assert(r.validFields == 33);
    assert(r.initial);
    assert(!r.comparisonKnown);
    data->completedTurns = {{"top", "turn-a", today + 80, 1000, false},
                            {"agent", "turn-b", today + 90, 2000, true},
                            {"top", "outside", yesterday, 9999, false}};
    auto completed = Analyze(snapshot, {});
    assert(completed.completedTurns == 2 && completed.failedTurns == 1);
    assert(completed.durationP50 == 1000 && completed.durationP95 == 2000);
    Query q;
    q.period = Period::Yesterday;
    assert(Analyze(snapshot, q).tokens.totalTokens == 100);
    q.period = Period::Custom;
    q.start = today;
    q.end = today + 60;
    assert(Analyze(snapshot, q).tokens.totalTokens == 0);
    q.end = today + 61;
    assert(Analyze(snapshot, q).tokens.totalTokens == 50);
    q = {};
    q.source = 1;
    auto agent = Analyze(snapshot, q);
    assert(agent.tokens.totalTokens == 15 && agent.turns == 0 && agent.activeTasks == 0);
    q = {};
    q.session = "top";
    assert(Analyze(snapshot, q).tokens.totalTokens == 75);
    assert(Change(120, 100, true, true) == L"+20.0%");
    assert(Change(120, 0, true, true) == L"前期為 0");
    assert(Ratio(10, 5, true, true) == L"分項不一致");
    assert(Ratio(1, 5, false, true) == L"—");
    assert(Number(10000, true) == L"1 萬" || Number(10000, true) == L"1.0 萬");
    auto json = Export(r, 2, true, true);
    assert(json.find("\"total_tokens\":\"75\"") != std::string::npos);
    assert(json.find("\"top\"") == std::string::npos);
    auto csv = Export(r, 1, false, true);
    assert(csv.starts_with("\xEF\xBB\xBF"));
    assert(csv.find("period_start_utc") != std::string::npos);
    assert(csv.find("cache_write_input_tokens") != std::string::npos);
    assert(Export(r, 3, true, true).find("counter_resets") != std::string::npos);
    auto large = r;
    large.tokens.totalTokens = 18446744073709551615ULL;
    assert(Export(large, 0, true, true).find("18446744073709551615") != std::string::npos);
    std::stop_source cancellation;
    cancellation.request_stop();
    assert(Analyze(snapshot, {}, cancellation.get_token()).cancelled);
    snapshot.partial = true;
    assert(!Analyze(snapshot, {}).comparisonKnown);
    // Fixed-size throughput fixture: immutable metadata only, no user data.
    auto big = std::make_shared<ActivityData>();
    big->sessions.resize(10000);
    for (int i = 0; i < 10000; ++i)
        big->sessions[i] = {std::to_string(i), today, true, 0};
    big->events.reserve(1000000);
    for (int i = 0; i < 1000000; ++i)
        big->events.push_back({static_cast<std::uint32_t>(i % 10000),
                               today + (i % 7200),
                               {1, 0, 0, 0, 0, 1},
                               false,
                               63,
                               false});
    snapshot.partial = false;
    snapshot.activity = big;
    std::vector<long long> timings;
    for (int run = 0; run < 10; ++run) {
        auto start = std::chrono::steady_clock::now();
        auto stress = Analyze(snapshot, {});
        assert(stress.tokens.totalTokens == 1000000 && stress.tasks.size() == 10000);
        timings.push_back(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                .count());
    }
    std::sort(timings.begin(), timings.end());
    std::cout << "1M events / 10K sessions / 10 queries: p50=" << timings[4] << " ms, p95=" << timings[9]
              << " ms\n";
    DYNAMIC_TIME_ZONE_INFORMATION pacific{};
    bool foundPacific = false;
    for (DWORD index = 0; EnumDynamicTimeZoneInformation(index, &pacific) == ERROR_SUCCESS; ++index) {
        if (std::wstring(pacific.TimeZoneKeyName) == L"Pacific Standard Time") { foundPacific = true; break; }
    }
    assert(foundPacific);
    auto localDate = [&](int year, int month, int day) {
        std::tm value{};
        value.tm_year = year - 1900;
        value.tm_mon = month - 1;
        value.tm_mday = day;
        value.tm_isdst = -1;
        return FromLocalTime(value, &pacific);
    };
    const auto spring = localDate(2026, 3, 8), fall = localDate(2026, 11, 1);
    assert(ShiftDays(spring, 1, &pacific) - spring == 23 * 3600);
    assert(ShiftDays(fall, 1, &pacific) - fall == 25 * 3600);
    assert(ShiftDays(localDate(2024, 2, 28), 1, &pacific) == localDate(2024, 2, 29));
    assert(ShiftDays(localDate(2026, 12, 31), 1, &pacific) == localDate(2027, 1, 1));
    std::cout << "ActivityAnalyticsTests passed\n";
}
