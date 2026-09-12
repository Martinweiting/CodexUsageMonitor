#include "ActivityAnalytics.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace activity {
std::tm LocalTime(long long time, const DYNAMIC_TIME_ZONE_INFORMATION* zone) {
    std::tm t{};
    const time_t v = time;
    gmtime_s(&t, &v);
    SYSTEMTIME utc{static_cast<WORD>(t.tm_year + 1900), static_cast<WORD>(t.tm_mon + 1), 0,
        static_cast<WORD>(t.tm_mday), static_cast<WORD>(t.tm_hour), static_cast<WORD>(t.tm_min), static_cast<WORD>(t.tm_sec), 0}, local{};
    if (SystemTimeToTzSpecificLocalTimeEx(zone, &utc, &local)) {
        t.tm_year = local.wYear - 1900; t.tm_mon = local.wMonth - 1; t.tm_mday = local.wDay;
        t.tm_hour = local.wHour; t.tm_min = local.wMinute; t.tm_sec = local.wSecond;
        t.tm_wday = local.wDayOfWeek; t.tm_isdst = -1;
    }
    return t;
}
long long FromLocalTime(std::tm value, const DYNAMIC_TIME_ZONE_INFORMATION* zone) {
    // Normalize calendar arithmetic without a process-global CRT time zone.
    const auto normalized = _mkgmtime64(&value);
    if (normalized == -1) return 0;
    SYSTEMTIME local{static_cast<WORD>(value.tm_year + 1900), static_cast<WORD>(value.tm_mon + 1), 0,
        static_cast<WORD>(value.tm_mday), static_cast<WORD>(value.tm_hour), static_cast<WORD>(value.tm_min), static_cast<WORD>(value.tm_sec), 0}, utc{};
    if (!TzSpecificLocalTimeToSystemTimeEx(zone, &local, &utc)) return 0;
    std::tm result{}; result.tm_year = utc.wYear - 1900; result.tm_mon = utc.wMonth - 1;
    result.tm_mday = utc.wDay; result.tm_hour = utc.wHour; result.tm_min = utc.wMinute; result.tm_sec = utc.wSecond;
    return _mkgmtime64(&result);
}
namespace {
std::tm Local(long long time) { return LocalTime(time); }
std::uint64_t Plus(std::uint64_t a, std::uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }
void Add(TokenUsageTotals &a, const TokenUsageTotals &b) {
    a.inputTokens = Plus(a.inputTokens, b.inputTokens);
    a.cachedInputTokens = Plus(a.cachedInputTokens, b.cachedInputTokens);
    a.cacheWriteInputTokens = Plus(a.cacheWriteInputTokens, b.cacheWriteInputTokens);
    a.outputTokens = Plus(a.outputTokens, b.outputTokens);
    a.reasoningOutputTokens = Plus(a.reasoningOutputTokens, b.reasoningOutputTokens);
    a.totalTokens = Plus(a.totalTokens, b.totalTokens);
}
std::string Utf8(const std::wstring &s) {
    if (s.empty())
        return {};
    int n =
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::string Escape(const std::string &s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else if (c >= 32)
            out += c;
    }
    return out;
}
struct Group {
    Bucket b;
    std::set<std::uint32_t> tasks;
};
} // namespace
long long DayStart(long long time, const DYNAMIC_TIME_ZONE_INFORMATION* zone) {
    auto t = LocalTime(time, zone);
    t.tm_hour = t.tm_min = t.tm_sec = 0;
    t.tm_isdst = -1;
    return FromLocalTime(t, zone);
}
long long ShiftDays(long long time, int days, const DYNAMIC_TIME_ZONE_INFORMATION* zone) {
    auto t = LocalTime(time, zone);
    t.tm_mday += days;
    t.tm_isdst = -1;
    return FromLocalTime(t, zone);
}
std::wstring Date(long long time, bool withTime) {
    if (time <= 0)
        return L"—";
    auto t = Local(time);
    wchar_t b[64]{};
    wcsftime(b, 64, withTime ? L"%Y/%m/%d %H:%M:%S" : L"%Y/%m/%d", &t);
    return b;
}
std::wstring Timezone() {
    DYNAMIC_TIME_ZONE_INFORMATION z{};
    auto mode = GetDynamicTimeZoneInformation(&z);
    return mode == TIME_ZONE_ID_DAYLIGHT ? z.DaylightName : z.StandardName;
}
std::wstring Number(std::uint64_t value, bool chinese, bool exact) {
    if (exact) {
        auto s = std::to_wstring(value);
        for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
            s.insert(i, L",");
        return s;
    }
    return chinese ? codex_usage::FormatTraditionalChineseCount(value)
                   : codex_usage::FormatCompactCount(value);
}
std::wstring Bytes(std::uint64_t v, bool zh) {
    if (v < 1024)
        return Number(v, zh, true) + (zh ? L" 位元組" : L" B");
    const wchar_t *unit = L" KiB";
    double n = v / 1024.0;
    if (n >= 1024) {
        n /= 1024;
        unit = L" MiB";
    }
    if (n >= 1024) {
        n /= 1024;
        unit = L" GiB";
    }
    std::wostringstream o;
    o << std::fixed << std::setprecision(1) << n << unit;
    return o.str();
}
std::wstring Ratio(std::uint64_t a, std::uint64_t b, bool valid, bool zh) {
    if (!valid || !b)
        return L"—";
    if (a > b)
        return zh ? L"分項不一致" : L"Inconsistent fields";
    std::wostringstream o;
    o << std::fixed << std::setprecision(1) << 100.0 * static_cast<double>(a) / b << L"%";
    return o.str();
}
std::wstring Change(std::uint64_t a, std::uint64_t b, bool known, bool zh) {
    if (!known)
        return zh ? L"前期資料不足" : L"Insufficient comparison";
    if (!b)
        return a ? (zh ? L"前期為 0" : L"Previous: 0") : (zh ? L"無變化" : L"No change");
    std::wostringstream o;
    if (a >= b)
        o << L"+";
    o << std::fixed << std::setprecision(1) << (static_cast<long double>(a) - b) * 100 / b << L"%";
    return o.str();
}
std::wstring AnonymousId(const std::string &id) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : id) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    std::wostringstream o;
    o << L"#" << std::hex << std::uppercase << std::setw(16) << std::setfill(L'0') << hash;
    return o.str();
}

Result Analyze(const codex_usage::LocalUsageSnapshot &snapshot, Query query, std::stop_token stop) {
    Result r;
    r.query = query;
    r.generated = snapshot.lastScanUnixSeconds;
    r.available = snapshot.available && snapshot.activity != nullptr;
    r.partial = snapshot.partial;
    if (!r.available)
        return r;
    const auto &data = *snapshot.activity;
    r.quality = {{"malformed_lines", std::to_string(data.malformedLines)},
                 {"invalid_timestamps", std::to_string(data.invalidTimes)},
                 {"duplicate_events", std::to_string(data.duplicates)},
                 {"counter_resets", std::to_string(data.resets)},
                 {"coverage_start_utc", std::to_string(snapshot.coverageStartUnixSeconds)},
                 {"scan_milliseconds", std::to_string(snapshot.scanMilliseconds)},
                 {"cache_bytes", std::to_string(snapshot.cacheBytes)}};
    const long long now = snapshot.lastScanUnixSeconds + 1;
    const auto today = DayStart(now - 1);
    r.start = today;
    r.end = now;
    switch (query.period) {
    case Period::Yesterday:
        r.start = ShiftDays(today, -1);
        r.end = today;
        break;
    case Period::SevenDays:
        r.start = ShiftDays(today, -6);
        break;
    case Period::ThirtyDays:
        r.start = ShiftDays(today, -29);
        break;
    case Period::Week:
        r.start = ShiftDays(today, -((Local(today).tm_wday + 6) % 7));
        break;
    case Period::Month: {
        auto t = Local(today);
        t.tm_mday = 1;
        t.tm_isdst = -1;
        r.start = FromLocalTime(t);
        break;
    }
    case Period::All:
        r.start = DayStart(snapshot.coverageStartUnixSeconds);
        break;
    case Period::Custom:
        r.start = DayStart(query.start);
        r.end = std::min(query.end, now);
        break;
    default:
        break;
    }
    if (r.start <= 0 || r.end <= r.start) {
        r.available = false;
        return r;
    }
    int count = 0;
    for (auto d = r.start; d < r.end; d = ShiftDays(d, 1)) {
        if (++count > 36600) {
            r.available = false;
            return r;
        }
    }
    r.previousStart = ShiftDays(r.start, -count);
    r.previousEnd = ShiftDays(r.end, -count);
    if (r.end == now)
        r.previousEnd = ShiftDays(now, -count);
    r.comparisonKnown =
        query.period != Period::All && !r.partial && snapshot.coverageStartUnixSeconds <= r.previousStart;
    std::map<long long, Group> days, hours, prev, calendar;
    std::unordered_map<std::uint32_t, Task> tasks;
    tasks.reserve(data.sessions.size());
    std::set<std::uint32_t> active, prevTasks;
    std::vector<std::pair<long long, long long>> intervals;
    struct Minute {
        long long day = 0;
        int weekdayHour = 0;
        long long begin = 0, end = 0;
    };
    std::unordered_map<long long, Minute> localMinutes;
    const auto yearStart = ShiftDays(today, -365);
    for (auto d = r.start; d < r.end; d = ShiftDays(d, 1)) {
        days[d].b.start = d;
        days[d].b.known = !r.partial && d >= DayStart(snapshot.coverageStartUnixSeconds);
    }
    for (auto d = r.previousStart; d < r.previousEnd; d = ShiftDays(d, 1)) {
        prev[d].b.start = d;
        prev[d].b.known = !r.partial && d >= DayStart(snapshot.coverageStartUnixSeconds);
    }
    for (auto d = yearStart; d <= today; d = ShiftDays(d, 1)) {
        calendar[d].b.start = d;
        calendar[d].b.known = !r.partial && d >= DayStart(snapshot.coverageStartUnixSeconds);
    }
    // UTC-based hour buckets retain repeated DST hours as distinct entries.
    for (auto h = r.start; h < r.end; h += 3600)
        hours[h].b.start = h;
    auto addEvent = [&](Group &g, const codex_usage::ActivityEvent &e, bool top) {
        if (e.userTurn) {
            if (top) {
                ++g.b.turns;
                g.tasks.insert(e.session);
            }
        } else {
            Add(g.b.tokens, e.tokens);
            g.b.validFields &= e.validFields;
            g.b.hasTokens = true;
            g.b.initial |= e.initialCounter;
        }
    };
    for (size_t n = 0; n < data.events.size(); ++n) {
        if ((n & 1023) == 0 && stop.stop_requested()) {
            r.cancelled = true;
            return r;
        }
        const auto &e = data.events[n];
        if (e.session >= data.sessions.size() || e.time >= now)
            continue;
        const auto &s = data.sessions[e.session];
        if (query.source >= 0 && static_cast<unsigned>(query.source) != s.origin)
            continue;
        if (!query.session.empty() && query.session != s.id)
            continue;
        auto [cached, inserted] = localMinutes.try_emplace(e.time / 60);
        if (inserted) {
            auto lt = Local(e.time);
            cached->second = {DayStart(e.time), ((lt.tm_wday + 6) % 7) * 24 + lt.tm_hour};
        }
        const auto day = cached->second.day;
        if (e.time >= yearStart && e.time < now)
            addEvent(calendar[day], e, s.topLevel);
        if (e.time >= r.previousStart && e.time < r.previousEnd) {
            addEvent(prev[day], e, s.topLevel);
            if (e.userTurn) {
                if (s.topLevel) {
                    ++r.previousTurns;
                    prevTasks.insert(e.session);
                }
            } else
                Add(r.previous, e.tokens);
        }
        if (e.time < r.start || e.time >= r.end)
            continue;
        addEvent(days[day], e, s.topLevel);
        auto hour = r.start + ((e.time - r.start) / 3600) * 3600;
        addEvent(hours[hour], e, s.topLevel);
        auto &t = tasks[e.session];
        if (t.id.empty()) {
            t.id = s.id;
            t.label = AnonymousId(s.id);
            t.created = s.created;
            t.topLevel = s.topLevel;
            t.origin = s.origin;
        }
        if (!t.first)
            t.first = e.time;
        t.last = std::max(t.last, e.time);
        if (e.userTurn) {
            if (s.topLevel) {
                ++r.turns;
                ++t.turns;
                active.insert(e.session);
            }
        } else {
            Add(r.tokens, e.tokens);
            r.modelTokens[e.model] = Plus(r.modelTokens[e.model], e.tokens.totalTokens);
            r.projectTokens[e.project] = Plus(r.projectTokens[e.project], e.tokens.totalTokens);
            Add(t.tokens, e.tokens);
            t.validFields &= e.validFields;
            r.validFields &= e.validFields;
            r.initial |= e.initialCounter;
            if (s.topLevel)
                Add(r.topLevelTokens, e.tokens);
            r.sourceTokens[std::min(2u, s.origin)] =
                Plus(r.sourceTokens[std::min(2u, s.origin)], e.tokens.totalTokens);
            auto wi = cached->second.weekdayHour;
            r.weeklyHours[wi] = Plus(r.weeklyHours[wi], e.tokens.totalTokens);
        }
        if (e.userTurn || e.tokens.totalTokens) {
            auto &minute = cached->second;
            if (!minute.begin || e.time < minute.begin)
                minute.begin = e.time;
            minute.end = std::max(minute.end, std::min(e.time + 60, r.end));
        }
    }
    r.activeTasks = active.size();
    r.previousTasks = prevTasks.size();
    for (auto &[id, t] : tasks) {
        ++r.clientTasks[data.sessions[id].client];
        if (data.sessions[id].archived)
            ++r.archivedTasks;
        if (active.contains(id)) {
            Add(r.activeTaskTokens, t.tokens);
            if (!t.created)
                ++r.unknownAge;
            else if (t.created >= r.start)
                ++r.newTasks;
            else
                ++r.returningTasks;
        }
        if (t.tokens.totalTokens && !t.turns)
            ++r.tokenOnly;
        r.tasks.push_back(std::move(t));
    }
    std::sort(r.tasks.begin(), r.tasks.end(), [](const auto &a, const auto &b) {
        return a.tokens.totalTokens != b.tokens.totalTokens ? a.tokens.totalTokens > b.tokens.totalTokens
                                                            : a.id < b.id;
    });
    std::uint64_t streak = 0;
    for (auto &[d, g] : days) {
        g.b.tasks = g.tasks.size();
        if (g.b.tokens.totalTokens || g.b.turns) {
            ++r.activeDays;
            ++streak;
            r.longestStreak = std::max(streak, r.longestStreak);
        } else
            streak = 0;
        if (!g.b.known)
            streak = 0;
        r.days.push_back(g.b);
    }
    r.recentStreak = streak;
    for (auto &[h, g] : hours) {
        g.b.tasks = g.tasks.size();
        r.hours.push_back(g.b);
    }
    for (auto &[d, g] : prev) {
        g.b.tasks = g.tasks.size();
        r.previousDays.push_back(g.b);
    }
    for (auto &[d, g] : calendar) {
        g.b.tasks = g.tasks.size();
        r.calendar.push_back(g.b);
    }
    for (auto &b : r.days) {
        long double sum = 0;
        bool valid = true;
        for (int i = 0; i < 7; ++i) {
            auto it = calendar.find(ShiftDays(b.start, -i));
            if (it == calendar.end() || !it->second.b.known) {
                valid = false;
                break;
            }
            sum += it->second.b.tokens.totalTokens;
        }
        b.movingAverage = valid ? static_cast<double>(sum / 7) : -1;
        std::vector<std::uint64_t> prior;
        for (int i = 1; i <= 28; ++i) {
            auto it = calendar.find(ShiftDays(b.start, -i));
            if (it != calendar.end() && it->second.b.known)
                prior.push_back(it->second.b.tokens.totalTokens);
        }
        if (prior.size() >= 7) {
            std::sort(prior.begin(), prior.end());
            auto median = prior[prior.size() / 2];
            if (median && static_cast<long double>(b.tokens.totalTokens) > 2.0L * median)
                r.highDays.push_back(b.start);
        }
    }
    // Event windows beginning in the same minute always overlap; merge before sorting.
    for (const auto &[key, minute] : localMinutes)
        if (minute.begin)
            intervals.emplace_back(minute.begin, minute.end);
    std::sort(intervals.begin(), intervals.end());
    long long begin = 0, end = 0;
    for (auto [a, b] : intervals) {
        if (a > end) {
            r.estimatedSeconds += end - begin;
            begin = a;
            end = b;
        } else
            end = std::max(end, b);
    }
    r.estimatedSeconds += end - begin;
    std::map<std::string, unsigned> origins;
    for (const auto &session : data.sessions)
        origins.emplace(session.id, session.origin);
    std::vector<std::uint64_t> durations;
    for (const auto &turn : data.completedTurns) {
        if (turn.completed < r.start || turn.completed >= r.end ||
            (!query.session.empty() && query.session != turn.sessionId))
            continue;
        const auto origin = origins.find(turn.sessionId);
        if (query.source >= 0 &&
            (origin == origins.end() || origin->second != static_cast<unsigned>(query.source)))
            continue;
        durations.push_back(turn.durationMilliseconds);
        ++r.completedTurns;
        if (turn.failed)
            ++r.failedTurns;
    }
    if (!durations.empty()) {
        std::sort(durations.begin(), durations.end());
        r.durationP50 = durations[(durations.size() - 1) / 2];
        r.durationP95 = durations[static_cast<size_t>(std::ceil(durations.size() * .95)) - 1];
    }
    r.comparisonKnown = r.comparisonKnown && !r.initial && (r.validFields & 32);
    return r;
}
std::wstring Summary(const Result &r, bool zh) {
    if (!r.available)
        return zh ? L"尚無可用的本機紀錄。" : L"No local records available.";
    std::wstring s = r.partial ? (zh ? L"依已讀取紀錄，" : L"From available records, ") : L"";
    s += zh ? L"本期記錄 " : L"This period recorded ";
    s += Number(r.tokens.totalTokens, zh) + L" Token";
    s += zh ? L"，涵蓋 " : L" across ";
    s += Number(r.activeTasks, zh) + (zh ? L" 個活躍任務。" : L" active tasks.");
    if (!r.days.empty()) {
        auto peak = std::max_element(r.days.begin(), r.days.end(), [](auto &a, auto &b) {
            return a.tokens.totalTokens < b.tokens.totalTokens;
        });
        s += (zh ? L" 最高日為 " : L" Peak day: ") + Date(peak->start) + L" · " +
             Number(peak->tokens.totalTokens, zh) + L" Token。";
    }
    return s;
}
std::string Export(const Result &r, int table, bool json, bool zh) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> header = {"date_or_anonymous_id", "total_tokens",
                                       "input_tokens",         "output_tokens",
                                       "cached_input_tokens",  "reasoning_output_tokens",
                                       "user_turns",           "active_tasks",
                                       "valid_fields",         "cache_write_input_tokens"};
    auto row = [&](std::string name, const TokenUsageTotals &t, std::uint64_t turns, std::uint64_t tasks,
                   unsigned valid) {
        rows.push_back({name, std::to_string(t.totalTokens), std::to_string(t.inputTokens),
                        std::to_string(t.outputTokens), std::to_string(t.cachedInputTokens),
                        std::to_string(t.reasoningOutputTokens), std::to_string(turns), std::to_string(tasks),
                        std::to_string(valid), std::to_string(t.cacheWriteInputTokens)});
    };
    if (table == 1)
        for (auto &b : r.days)
            row(Utf8(Date(b.start)), b.tokens, b.turns, b.tasks, b.validFields);
    else if (table == 2)
        for (auto &t : r.tasks)
            row(Utf8(AnonymousId(t.id)), t.tokens, t.turns, t.turns ? 1 : 0, t.validFields);
    else
        row("summary", r.tokens, r.turns, r.activeTasks, r.validFields);
    if (table == 3) {
        rows.clear();
        header = {"metric", "recorded_value"};
        for (const auto &[key, value] : r.quality)
            rows.push_back({key, value});
    }
    std::ostringstream o;
    if (json) {
        o << "{\"schemaVersion\":1,\"definitionVersion\":1,\"startUtc\":\"" << r.start << "\",\"endUtc\":\""
          << r.end << "\",\"generatedAt\":\"" << r.generated << "\",\"timezone\":\""
          << Escape(Utf8(Timezone())) << "\",\"partial\":" << (r.partial ? "true" : "false")
          << ",\"initialCounter\":" << (r.initial ? "true" : "false")
          << ",\"sourceFilter\":" << r.query.source
          << ",\"localRecordsOnly\":true,\"endExclusive\":true,\"rows\":[";
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i)
                o << ',';
            o << '{';
            for (size_t j = 0; j < header.size(); ++j) {
                if (j)
                    o << ',';
                o << '"' << header[j] << "\":\"" << Escape(rows[i][j]) << '"';
            }
            o << '}';
        }
        o << "]}";
    } else {
        o << "\xEF\xBB\xBF";
        header.insert(header.end(), {"period_start_utc", "period_end_utc", "partial", "initial_counter",
                                     "generated_at_utc", "timezone", "source_filter"});
        for (size_t j = 0; j < header.size(); ++j) {
            if (j)
                o << ',';
            o << header[j];
        }
        o << "\r\n";
        for (auto values : rows) {
            values.insert(values.end(),
                          {std::to_string(r.start), std::to_string(r.end), r.partial ? "true" : "false",
                           r.initial ? "true" : "false", std::to_string(r.generated), Utf8(Timezone()),
                           std::to_string(r.query.source)});
            for (size_t j = 0; j < values.size(); ++j) {
                if (j)
                    o << ',';
                std::string v = values[j];
                if (!v.empty() && std::string("=+-@").find(v[0]) != std::string::npos)
                    v = "'" + v;
                o << '"';
                for (char c : v) {
                    if (c == '"')
                        o << '"';
                    o << c;
                }
                o << '"';
            }
            o << "\r\n";
        }
    }
    (void)zh;
    return o.str();
}
} // namespace activity
