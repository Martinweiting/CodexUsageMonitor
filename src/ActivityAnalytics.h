#pragma once
#include "LocalUsageStats.h"
#include <Windows.h>
#include <ctime>
#include <array>
#include <map>
#include <optional>
#include <stop_token>

namespace activity {
using codex_usage::TokenUsageTotals;
enum class Period { Today, Yesterday, SevenDays, ThirtyDays, Week, Month, All, Custom };
struct Query {
    Period period = Period::Today;
    long long start = 0, end = 0;
    int source = -1;
    std::string session;
};
struct Bucket {
    long long start = 0;
    TokenUsageTotals tokens;
    std::uint64_t turns = 0, tasks = 0;
    unsigned validFields = 63;
    bool hasTokens = false, initial = false, known = false;
    double movingAverage = 0;
};
struct Task {
    std::string id;
    std::wstring label;
    bool topLevel = true;
    unsigned origin = 2, validFields = 63;
    long long created = 0, first = 0, last = 0;
    TokenUsageTotals tokens;
    std::uint64_t turns = 0;
};
struct Result {
    Query query;
    long long start = 0, end = 0, previousStart = 0, previousEnd = 0, generated = 0;
    bool available = false, partial = false, initial = false, comparisonKnown = false, cancelled = false;
    TokenUsageTotals tokens, previous, activeTaskTokens, topLevelTokens;
    unsigned validFields = 63;
    std::uint64_t turns = 0, activeTasks = 0, previousTurns = 0, previousTasks = 0;
    std::uint64_t activeDays = 0, longestStreak = 0, recentStreak = 0, newTasks = 0, returningTasks = 0,
                  unknownAge = 0, tokenOnly = 0;
    std::uint64_t estimatedSeconds = 0;
    std::array<std::uint64_t, 3> sourceTokens{};
    std::array<std::uint64_t, 168> weeklyHours{};
    std::vector<Bucket> days, hours, previousDays, calendar;
    std::vector<Task> tasks;
    std::vector<long long> highDays;
    std::map<std::string, std::uint64_t> modelTokens, projectTokens, clientTasks;
    std::uint64_t archivedTasks = 0, completedTurns = 0, failedTurns = 0;
    std::uint64_t durationP50 = 0, durationP95 = 0;
    std::map<std::string, std::string> quality;
};
std::tm LocalTime(long long time, const DYNAMIC_TIME_ZONE_INFORMATION* zone = nullptr);
long long FromLocalTime(std::tm value, const DYNAMIC_TIME_ZONE_INFORMATION* zone = nullptr);
long long DayStart(long long time, const DYNAMIC_TIME_ZONE_INFORMATION* zone = nullptr);
long long ShiftDays(long long time, int days, const DYNAMIC_TIME_ZONE_INFORMATION* zone = nullptr);
std::wstring Date(long long time, bool withTime = false);
std::wstring Timezone();
std::wstring Number(std::uint64_t value, bool chinese, bool exact = false);
std::wstring Bytes(std::uint64_t value, bool chinese);
std::wstring Ratio(std::uint64_t numerator, std::uint64_t denominator, bool valid, bool chinese);
std::wstring Change(std::uint64_t current, std::uint64_t previous, bool known, bool chinese);
std::wstring AnonymousId(const std::string &id);
Result Analyze(const codex_usage::LocalUsageSnapshot &, Query, std::stop_token = {});
std::wstring Summary(const Result &, bool chinese);
std::string Export(const Result &, int table, bool json, bool chinese);
} // namespace activity
