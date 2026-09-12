#include "LocalUsageStats.h"

#include "JsonLite.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace codex_usage {
namespace {

constexpr std::string_view kCacheHeader = "CodexUsageMonitorLocalStats\t2";
constexpr std::uintmax_t kMaximumCacheBytes = 1024ULL * 1024ULL * 1024ULL;

struct FileCheckpoint {
    std::uint64_t offset = 0;
    std::string currentSessionId;
    std::string model, project;
    std::string identity;
    long long modified = 0;
};

struct SessionRecord {
    bool topLevel = true;
    long long createdMicros = 0;
    unsigned origin = 2;
    std::string client;
    bool archived = false;
};

struct TokenEvent {
    std::uint64_t key = 0;
    std::string sessionId;
    long long timestampMicros = 0;
    TokenUsageTotals cumulative;
    unsigned validFields = 0;
    std::string model, project;
};

struct UserEvent {
    std::uint64_t key = 0;
    std::string sessionId;
    long long timestampMicros = 0;
};

struct CandidateFile {
    std::filesystem::path path;
    std::uint64_t size = 0;
    std::filesystem::file_time_type modified = {};
    bool sessionIndex = false;
};

enum class CacheLoadResult {
    Missing,
    Loaded,
    Invalid,
    TooLarge,
};

std::string FileIdentity(const std::filesystem::path &path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    BY_HANDLE_FILE_INFORMATION info{};
    const bool valid = GetFileInformationByHandle(file, &info) != FALSE;
    CloseHandle(file);
    return valid ? std::to_string(info.dwVolumeSerialNumber) + ":" + std::to_string(info.nFileIndexHigh) +
                       ":" + std::to_string(info.nFileIndexLow)
                 : "";
}

std::optional<std::wstring> ReadEnvironment(const wchar_t *name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return std::nullopt;
    }
    std::wstring value(static_cast<size_t>(required - 1), L'\0');
    if (GetEnvironmentVariableW(name, value.data(), required) == 0) {
        return std::nullopt;
    }
    return value;
}

std::string WideToUtf8(std::wstring_view input) {
    if (input.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string output(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), required,
                        nullptr, nullptr);
    return output;
}

std::wstring Utf8ToWide(std::string_view input) {
    if (input.empty()) {
        return {};
    }
    const int required =
        MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring output(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), required);
    return output;
}

template <typename Integer> bool ParseInteger(std::string_view text, Integer *output) {
    if (output == nullptr || text.empty()) {
        return false;
    }
    Integer parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }
    *output = parsed;
    return true;
}

std::vector<std::string_view> SplitTabs(std::string_view line) {
    std::vector<std::string_view> columns;
    size_t start = 0;
    for (;;) {
        const size_t separator = line.find('\t', start);
        if (separator == std::string_view::npos) {
            columns.push_back(line.substr(start));
            break;
        }
        columns.push_back(line.substr(start, separator - start));
        start = separator + 1;
    }
    return columns;
}

bool IsSafeCacheText(std::string_view value) {
    return value.find_first_of("\r\n\t") == std::string_view::npos;
}

void HashAppend(std::uint64_t *hash, std::string_view text) {
    if (hash == nullptr) {
        return;
    }
    for (const unsigned char byte : text) {
        *hash ^= byte;
        *hash *= 1099511628211ULL;
    }
    *hash ^= 0xff;
    *hash *= 1099511628211ULL;
}

void HashAppend(std::uint64_t *hash, std::uint64_t value) { HashAppend(hash, std::to_string(value)); }

std::optional<long long> ParseIso8601UnixMicros(const std::string &text) {
    if (text.size() < 19 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
        text[16] != ':') {
        return std::nullopt;
    }

    try {
        std::tm value = {};
        value.tm_year = std::stoi(text.substr(0, 4)) - 1900;
        value.tm_mon = std::stoi(text.substr(5, 2)) - 1;
        value.tm_mday = std::stoi(text.substr(8, 2));
        value.tm_hour = std::stoi(text.substr(11, 2));
        value.tm_min = std::stoi(text.substr(14, 2));
        value.tm_sec = std::stoi(text.substr(17, 2));
        value.tm_isdst = 0;

        long long fractionalMicros = 0;
        int fractionalDigits = 0;
        size_t position = 19;
        if (position < text.size() && text[position] == '.') {
            ++position;
            while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
                if (fractionalDigits < 6) {
                    fractionalMicros = fractionalMicros * 10 + (text[position] - '0');
                    ++fractionalDigits;
                }
                ++position;
            }
            while (fractionalDigits < 6) {
                fractionalMicros *= 10;
                ++fractionalDigits;
            }
        }

        long long offsetSeconds = 0;
        if (position < text.size() && text[position] != 'Z' && text[position] != 'z') {
            if ((text[position] != '+' && text[position] != '-') || position + 5 >= text.size()) {
                return std::nullopt;
            }
            const int sign = text[position] == '+' ? 1 : -1;
            const int hours = std::stoi(text.substr(position + 1, 2));
            int minutes = 0;
            if (text[position + 3] == ':') {
                minutes = std::stoi(text.substr(position + 4, 2));
            } else {
                minutes = std::stoi(text.substr(position + 3, 2));
            }
            offsetSeconds = -static_cast<long long>(sign) * (hours * 3600LL + minutes * 60LL);
        }

        const time_t utc = _mkgmtime(&value);
        if (utc == static_cast<time_t>(-1)) {
            return std::nullopt;
        }
        return (static_cast<long long>(utc) + offsetSeconds) * 1000000LL + fractionalMicros;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

long long LocalDayStart(long long nowUnixSeconds) {
    const time_t now = static_cast<time_t>(nowUnixSeconds);
    std::tm local = {};
    if (localtime_s(&local, &now) != 0) {
        return nowUnixSeconds - (nowUnixSeconds % (24LL * 60LL * 60LL));
    }
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    return static_cast<long long>(std::mktime(&local));
}

std::optional<std::string> StringAt(const jsonlite::Value *parent, std::string_view key) {
    if (parent == nullptr) {
        return std::nullopt;
    }
    const jsonlite::Value *node = parent->Find(key);
    const auto value = node != nullptr ? node->AsString() : std::nullopt;
    if (!value.has_value()) {
        return std::nullopt;
    }
    return std::string(*value);
}

std::uint64_t NonNegativeNumberAt(const jsonlite::Value *parent, std::string_view key) {
    if (parent == nullptr) {
        return 0;
    }
    const jsonlite::Value *node = parent->Find(key);
    return node ? node->AsUnsignedInteger().value_or(0) : 0;
}

bool ContainsInsensitive(std::string value, std::string_view needle) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value.find(needle) != std::string::npos;
}

bool IsTopLevelSession(const jsonlite::Value *payload) {
    for (const std::string_view key : {std::string_view("source"), std::string_view("thread_source")}) {
        if (payload == nullptr) {
            break;
        }
        const jsonlite::Value *source = payload->Find(key);
        if (source == nullptr || source->IsNull()) {
            continue;
        }
        if (auto text = source->AsString(); text.has_value()) {
            if (ContainsInsensitive(std::string(*text), "subagent") ||
                ContainsInsensitive(std::string(*text), "guardian")) {
                return false;
            }
        }
        if (const auto *object = source->AsObject(); object != nullptr) {
            if (object->find("subagent") != object->end() || object->find("guardian") != object->end()) {
                return false;
            }
        }
    }
    return true;
}

void SaturatingAdd(std::uint64_t *target, std::uint64_t value) {
    if (target == nullptr) {
        return;
    }
    if (std::numeric_limits<std::uint64_t>::max() - *target < value) {
        *target = std::numeric_limits<std::uint64_t>::max();
    } else {
        *target += value;
    }
}

void AddTotals(TokenUsageTotals *target, const TokenUsageTotals &value) {
    SaturatingAdd(&target->inputTokens, value.inputTokens);
    SaturatingAdd(&target->cachedInputTokens, value.cachedInputTokens);
    SaturatingAdd(&target->cacheWriteInputTokens, value.cacheWriteInputTokens);
    SaturatingAdd(&target->outputTokens, value.outputTokens);
    SaturatingAdd(&target->reasoningOutputTokens, value.reasoningOutputTokens);
    SaturatingAdd(&target->totalTokens, value.totalTokens);
}

std::uint64_t DeltaCounter(std::uint64_t current, std::uint64_t previous, bool hasPrevious) {
    if (!hasPrevious || current < previous) {
        return current;
    }
    return current - previous;
}

bool CounterReset(const TokenUsageTotals &current, const TokenUsageTotals &previous, unsigned common) {
    return ((common & 1) && current.inputTokens < previous.inputTokens) ||
           ((common & 2) && current.cachedInputTokens < previous.cachedInputTokens) ||
           ((common & 4) && current.cacheWriteInputTokens < previous.cacheWriteInputTokens) ||
           ((common & 8) && current.outputTokens < previous.outputTokens) ||
           ((common & 16) && current.reasoningOutputTokens < previous.reasoningOutputTokens) ||
           ((common & 32) && current.totalTokens < previous.totalTokens);
}

TokenUsageTotals DeltaTotals(const TokenUsageTotals &current, const TokenUsageTotals &previous,
                             bool hasPrevious, unsigned common) {
    const bool counterReset = hasPrevious && CounterReset(current, previous, common);
    const bool continueSegment = hasPrevious && !counterReset;
    return TokenUsageTotals{
        DeltaCounter(current.inputTokens, previous.inputTokens, continueSegment),
        DeltaCounter(current.cachedInputTokens, previous.cachedInputTokens, continueSegment),
        DeltaCounter(current.cacheWriteInputTokens, previous.cacheWriteInputTokens, continueSegment),
        DeltaCounter(current.outputTokens, previous.outputTokens, continueSegment),
        DeltaCounter(current.reasoningOutputTokens, previous.reasoningOutputTokens, continueSegment),
        DeltaCounter(current.totalTokens, previous.totalTokens, continueSegment),
    };
}

void IncludeCoverage(long long micros, long long *earliest, long long *latest) {
    if (micros <= 0 || earliest == nullptr || latest == nullptr) {
        return;
    }
    if (*earliest == 0 || micros < *earliest) {
        *earliest = micros;
    }
    if (micros > *latest) {
        *latest = micros;
    }
}

std::string FileKey(const std::filesystem::path &path) { return WideToUtf8(path.filename().wstring()); }

bool IsJsonlFile(const std::filesystem::directory_entry &entry) {
    std::error_code error;
    return entry.is_regular_file(error) && !error && entry.path().extension() == L".jsonl";
}

void AddDirectoryCandidates(const std::filesystem::path &directory,
                            std::map<std::string, CandidateFile> *candidates, bool *partial) {
    std::error_code error;
    if (!std::filesystem::exists(directory, error) || error) {
        return;
    }
    std::filesystem::recursive_directory_iterator iterator(
        directory, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const std::filesystem::directory_entry entry = *iterator;
        if (IsJsonlFile(entry)) {
            const std::string key = FileKey(entry.path());
            std::error_code sizeError;
            const std::uintmax_t rawSize = entry.file_size(sizeError);
            std::error_code timeError;
            const auto modified = entry.last_write_time(timeError);
            if (!sizeError && rawSize <= std::numeric_limits<std::uint64_t>::max()) {
                CandidateFile candidate{entry.path(), static_cast<std::uint64_t>(rawSize), modified, false};
                auto existing = candidates->find(key);
                if (existing == candidates->end() || candidate.size > existing->second.size ||
                    (candidate.size == existing->second.size && !timeError &&
                     candidate.modified > existing->second.modified)) {
                    (*candidates)[key] = std::move(candidate);
                }
            } else if (partial != nullptr) {
                *partial = true;
            }
        }
        iterator.increment(error);
    }
    if (error && partial != nullptr) {
        *partial = true;
    }
}

} // namespace

struct LocalUsageStatsCollector::State {
    bool legacy = false;
    std::uint64_t malformedLines = 0, invalidTimes = 0, duplicates = 0;
    std::vector<CompletedActivityTurn> completedTurns;
    std::unordered_set<std::string> completedKeys;
    std::map<std::string, FileCheckpoint> checkpoints;
    std::unordered_map<std::string, SessionRecord> sessions;
    std::unordered_set<std::string> indexedTasks;
    std::vector<TokenEvent> tokenEvents;
    std::vector<UserEvent> userEvents;
    std::unordered_set<std::uint64_t> tokenKeys;
    std::unordered_set<std::uint64_t> userKeys;
};

namespace {

CacheLoadResult LoadCache(const std::filesystem::path &path, LocalUsageStatsCollector::State *state) {
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        return CacheLoadResult::Missing;
    }
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (!error && size > kMaximumCacheBytes)
        return CacheLoadResult::TooLarge;
    if (error) {
        return CacheLoadResult::Invalid;
    }

    std::ifstream input(path, std::ios::binary);
    std::string line;
    if (!input || !std::getline(input, line) ||
        (line != kCacheHeader && line != "CodexUsageMonitorLocalStats\t1")) {
        return CacheLoadResult::Invalid;
    }

    LocalUsageStatsCollector::State parsed;
    parsed.legacy = line != kCacheHeader;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::vector<std::string_view> columns = SplitTabs(line);
        if (columns.empty() || columns[0].empty()) {
            return CacheLoadResult::Invalid;
        }
        if (columns[0] == "F" && (columns.size() == 4 || columns.size() == 6 || columns.size() == 8)) {
            FileCheckpoint checkpoint;
            if (!ParseInteger(columns[2], &checkpoint.offset) || !IsSafeCacheText(columns[1]) ||
                !IsSafeCacheText(columns[3])) {
                return CacheLoadResult::Invalid;
            }
            checkpoint.currentSessionId = std::string(columns[3]);
            if (columns.size() >= 6) {
                checkpoint.model = columns[4];
                checkpoint.project = columns[5];
            }
            if (columns.size() == 8) {
                checkpoint.identity = columns[6];
                if (!ParseInteger(columns[7], &checkpoint.modified))
                    return CacheLoadResult::Invalid;
            } else
                parsed.legacy = true;
            parsed.checkpoints.emplace(std::string(columns[1]), std::move(checkpoint));
        } else if (columns[0] == "S" && (columns.size() == 4 || columns.size() == 5 || columns.size() == 7)) {
            SessionRecord session;
            int topLevel = 0;
            if (!ParseInteger(columns[2], &session.createdMicros) || !ParseInteger(columns[3], &topLevel) ||
                (topLevel != 0 && topLevel != 1)) {
                return CacheLoadResult::Invalid;
            }
            session.topLevel = topLevel == 1;
            if (columns.size() >= 5 && (!ParseInteger(columns[4], &session.origin) || session.origin > 2))
                return CacheLoadResult::Invalid;
            if (columns.size() == 7) {
                session.client = columns[5];
                session.archived = columns[6] == "1";
            }
            parsed.sessions.emplace(std::string(columns[1]), session);
        } else if (columns[0] == "I" && columns.size() == 2) {
            parsed.indexedTasks.emplace(columns[1]);
        } else if (columns[0] == "T" &&
                   (columns.size() == 11 || columns.size() == 12 || columns.size() == 14)) {
            TokenEvent event;
            if (!ParseInteger(columns[1], &event.key) || !ParseInteger(columns[3], &event.timestampMicros) ||
                !ParseInteger(columns[4], &event.cumulative.inputTokens) ||
                !ParseInteger(columns[5], &event.cumulative.cachedInputTokens) ||
                !ParseInteger(columns[6], &event.cumulative.cacheWriteInputTokens) ||
                !ParseInteger(columns[7], &event.cumulative.outputTokens) ||
                !ParseInteger(columns[8], &event.cumulative.reasoningOutputTokens) ||
                !ParseInteger(columns[9], &event.cumulative.totalTokens) || columns[10] != "1") {
                return CacheLoadResult::Invalid;
            }
            event.sessionId = std::string(columns[2]);
            if (columns.size() >= 12 &&
                (!ParseInteger(columns[11], &event.validFields) || event.validFields > 63))
                return CacheLoadResult::Invalid;
            if (columns.size() == 14) {
                event.model = columns[12];
                event.project = columns[13];
            }
            if (parsed.tokenKeys.insert(event.key).second) {
                parsed.tokenEvents.push_back(std::move(event));
            }
        } else if (columns[0] == "M" && columns.size() == 5 && columns[4] == "1") {
            UserEvent event;
            if (!ParseInteger(columns[1], &event.key) || !ParseInteger(columns[3], &event.timestampMicros)) {
                return CacheLoadResult::Invalid;
            }
            event.sessionId = std::string(columns[2]);
            if (parsed.userKeys.insert(event.key).second) {
                parsed.userEvents.push_back(std::move(event));
            }
        } else if (columns[0] == "L" && columns.size() == 6) {
            CompletedActivityTurn turn;
            turn.sessionId = columns[1];
            turn.turnId = columns[2];
            int failed = 0;
            if (!ParseInteger(columns[3], &turn.completed) ||
                !ParseInteger(columns[4], &turn.durationMilliseconds) || !ParseInteger(columns[5], &failed))
                return CacheLoadResult::Invalid;
            turn.failed = failed != 0;
            if (parsed.completedKeys.insert(turn.sessionId + ":" + turn.turnId).second)
                parsed.completedTurns.push_back(std::move(turn));
        } else if (columns[0] == "D" && columns.size() == 4) {
            if (!ParseInteger(columns[1], &parsed.malformedLines) ||
                !ParseInteger(columns[2], &parsed.invalidTimes) ||
                !ParseInteger(columns[3], &parsed.duplicates))
                return CacheLoadResult::Invalid;
        } else {
            return CacheLoadResult::Invalid;
        }
    }

    if (!input.eof()) {
        return CacheLoadResult::Invalid;
    }
    *state = std::move(parsed);
    return CacheLoadResult::Loaded;
}

bool SaveCache(const std::filesystem::path &path, const LocalUsageStatsCollector::State &state) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }

    std::filesystem::path temporary = path;
    temporary += L".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output << kCacheHeader << '\n';
    output << "D\t" << state.malformedLines << '\t' << state.invalidTimes << '\t' << state.duplicates << '\n';
    for (const auto &[key, checkpoint] : state.checkpoints) {
        if (IsSafeCacheText(key) && IsSafeCacheText(checkpoint.currentSessionId)) {
            output << "F\t" << key << '\t' << checkpoint.offset << '\t' << checkpoint.currentSessionId << '\t'
                   << checkpoint.model << '\t' << checkpoint.project << '\t' << checkpoint.identity << '\t'
                   << checkpoint.modified << '\n';
        }
    }
    for (const auto &[id, session] : state.sessions) {
        if (IsSafeCacheText(id)) {
            output << "S\t" << id << '\t' << session.createdMicros << '\t' << (session.topLevel ? 1 : 0)
                   << '\t' << session.origin << '\t' << session.client << '\t' << session.archived << '\n';
        }
    }
    for (const std::string &id : state.indexedTasks) {
        if (IsSafeCacheText(id)) {
            output << "I\t" << id << '\n';
        }
    }
    for (const TokenEvent &event : state.tokenEvents) {
        if (IsSafeCacheText(event.sessionId)) {
            output << "T\t" << event.key << '\t' << event.sessionId << '\t' << event.timestampMicros << '\t'
                   << event.cumulative.inputTokens << '\t' << event.cumulative.cachedInputTokens << '\t'
                   << event.cumulative.cacheWriteInputTokens << '\t' << event.cumulative.outputTokens << '\t'
                   << event.cumulative.reasoningOutputTokens << '\t' << event.cumulative.totalTokens
                   << "\t1\t" << event.validFields << '\t' << event.model << '\t' << event.project << '\n';
        }
    }
    for (const UserEvent &event : state.userEvents) {
        if (IsSafeCacheText(event.sessionId)) {
            output << "M\t" << event.key << '\t' << event.sessionId << '\t' << event.timestampMicros
                   << "\t1\n";
        }
    }
    for (const auto &t : state.completedTurns)
        if (IsSafeCacheText(t.sessionId) && IsSafeCacheText(t.turnId))
            output << "L\t" << t.sessionId << '\t' << t.turnId << '\t' << t.completed << '\t'
                   << t.durationMilliseconds << '\t' << t.failed << '\n';
    output.flush();
    if (!output) {
        output.close();
        std::filesystem::remove(temporary, error);
        return false;
    }
    output.close();

    const auto writtenSize = std::filesystem::file_size(temporary, error);
    if (error || writtenSize > kMaximumCacheBytes) {
        std::filesystem::remove(temporary, error);
        return false;
    }

    if (std::filesystem::exists(path, error)) {
        auto backup = path;
        backup += L".previous";
        std::filesystem::copy_file(path, backup, std::filesystem::copy_options::overwrite_existing, error);
        if (error)
            return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

std::map<std::string, CandidateFile> DiscoverCandidates(const LocalUsagePaths &paths, bool *partial) {
    std::map<std::string, CandidateFile> candidates;
    AddDirectoryCandidates(paths.codexHome / L"sessions", &candidates, partial);
    AddDirectoryCandidates(paths.codexHome / L"archived_sessions", &candidates, partial);

    const std::filesystem::path indexPath = paths.codexHome / L"session_index.jsonl";
    std::error_code error;
    if (std::filesystem::is_regular_file(indexPath, error) && !error) {
        const std::uintmax_t size = std::filesystem::file_size(indexPath, error);
        if (!error && size <= std::numeric_limits<std::uint64_t>::max()) {
            candidates["@session_index"] = CandidateFile{
                indexPath,
                static_cast<std::uint64_t>(size),
                std::filesystem::last_write_time(indexPath, error),
                true,
            };
        }
    }
    return candidates;
}

std::string EventSessionId(const std::string &currentSessionId, const std::string &fileKey) {
    if (!currentSessionId.empty()) {
        return currentSessionId;
    }
    std::filesystem::path filename(Utf8ToWide(fileKey));
    return WideToUtf8(filename.stem().wstring());
}

void ParseIndexLine(std::string_view line, LocalUsageStatsCollector::State *state) {
    jsonlite::Parser parser(line);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value()) {
        return;
    }
    auto id = StringAt(&*root, "id");
    if (!id.has_value()) {
        id = StringAt(&*root, "session_id");
    }
    if (id.has_value() && !id->empty()) {
        state->indexedTasks.insert(std::move(*id));
    }
}

void ParseSessionLine(std::string_view line, const std::string &fileKey, FileCheckpoint *checkpoint,
                      LocalUsageStatsCollector::State *state) {
    if (line.find("session_meta") == std::string_view::npos &&
        line.find("token_count") == std::string_view::npos &&
        line.find("user_message") == std::string_view::npos &&
        line.find("turn_context") == std::string_view::npos &&
        line.find("task_complete") == std::string_view::npos &&
        line.find("response_item") == std::string_view::npos) {
        return;
    }

    jsonlite::Parser parser(line);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value()) {
        ++state->malformedLines;
        return;
    }
    const auto type = StringAt(&*root, "type");
    const jsonlite::Value *payload = root->Find("payload");
    const auto timestamp = StringAt(&*root, "timestamp");
    const long long timestampMicros =
        timestamp.has_value() ? ParseIso8601UnixMicros(*timestamp).value_or(0) : 0;

    if (type == "session_meta") {
        auto id = StringAt(payload, "id");
        if (!id.has_value()) {
            id = StringAt(payload, "session_id");
        }
        if (!id.has_value() || id->empty()) {
            return;
        }
        checkpoint->currentSessionId = *id;
        const bool top = IsTopLevelSession(payload);
        const auto source = StringAt(payload, "thread_source");
        state->sessions[*id] = SessionRecord{top, timestampMicros, !top ? 1u : (source == "user" ? 0u : 2u)};
        const auto client = StringAt(payload, "originator");
        if (client && (*client == "codex_cli_rs" || *client == "codex_vscode" || *client == "codex_exec" ||
                       *client == "Codex Desktop"))
            state->sessions[*id].client = *client;
        return;
    }

    if (payload == nullptr) {
        return;
    }
    const auto payloadType = StringAt(payload, "type");
    const std::string sessionId = EventSessionId(checkpoint->currentSessionId, fileKey);
    if (sessionId.empty() || timestampMicros <= 0 || !timestamp.has_value()) {
        ++state->invalidTimes;
        return;
    }

    if (type == "turn_context") {
        checkpoint->model.clear();
        checkpoint->project.clear();
        const auto model = StringAt(payload, "model");
        if (model && model->size() <= 120 && std::all_of(model->begin(), model->end(), [](unsigned char c) {
                return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/';
            }))
            checkpoint->model = *model;
        const auto cwd = StringAt(payload, "cwd");
        if (cwd && !cwd->empty()) {
            std::uint64_t hash = 1469598103934665603ULL;
            HashAppend(&hash, *cwd);
            checkpoint->project = std::to_string(hash);
        }
        return;
    }

    if (type == "response_item") {
        const auto role = StringAt(payload, "role");
        if (payloadType == "message" && role == "user") {
            std::uint64_t key = 1469598103934665603ULL;
            HashAppend(&key, sessionId);
            HashAppend(&key, *timestamp);
            if (state->userKeys.insert(key).second) {
                state->userEvents.push_back(UserEvent{key, sessionId, timestampMicros});
            }
        }
        return;
    }
    if (type != "event_msg") {
        return;
    }

    if (payloadType == "token_count") {
        const jsonlite::Value *info = payload->Find("info");
        const jsonlite::Value *totals = info != nullptr ? info->Find("total_token_usage") : nullptr;
        if (totals == nullptr || !totals->IsObject()) {
            return;
        }
        TokenUsageTotals cumulative;
        cumulative.inputTokens = NonNegativeNumberAt(totals, "input_tokens");
        cumulative.cachedInputTokens = NonNegativeNumberAt(totals, "cached_input_tokens");
        cumulative.cacheWriteInputTokens = NonNegativeNumberAt(totals, "cache_write_input_tokens");
        cumulative.outputTokens = NonNegativeNumberAt(totals, "output_tokens");
        cumulative.reasoningOutputTokens = NonNegativeNumberAt(totals, "reasoning_output_tokens");
        cumulative.totalTokens = NonNegativeNumberAt(totals, "total_tokens");
        unsigned validFields = 0;
        const char *fields[] = {"input_tokens",  "cached_input_tokens",     "cache_write_input_tokens",
                                "output_tokens", "reasoning_output_tokens", "total_tokens"};
        for (unsigned i = 0; i < 6; ++i) {
            const auto *field = totals->Find(fields[i]);
            if (field && field->AsUnsignedInteger())
                validFields |= 1u << i;
        }

        std::uint64_t key = 1469598103934665603ULL;
        HashAppend(&key, sessionId);
        HashAppend(&key, *timestamp);
        HashAppend(&key, cumulative.inputTokens);
        HashAppend(&key, cumulative.cachedInputTokens);
        HashAppend(&key, cumulative.cacheWriteInputTokens);
        HashAppend(&key, cumulative.outputTokens);
        HashAppend(&key, cumulative.reasoningOutputTokens);
        HashAppend(&key, cumulative.totalTokens);
        if (state->tokenKeys.insert(key).second) {
            state->tokenEvents.push_back(TokenEvent{key, sessionId, timestampMicros, cumulative, validFields,
                                                    checkpoint->model, checkpoint->project});
        } else {
            ++state->duplicates;
        }
    } else if (payloadType == "task_complete") {
        const auto turn = StringAt(payload, "turn_id");
        const auto *duration = payload->Find("duration_ms");
        const auto value = duration ? duration->AsNumber() : std::nullopt;
        if (turn && !turn->empty() && IsSafeCacheText(*turn) && value && std::isfinite(*value) &&
            *value >= 0 && *value < static_cast<double>(UINT64_MAX) && std::floor(*value) == *value &&
            state->completedKeys.insert(sessionId + ":" + *turn).second) {
            const auto error = StringAt(payload, "error");
            state->completedTurns.push_back({sessionId, *turn, timestampMicros / 1000000LL,
                                             static_cast<std::uint64_t>(*value), error && !error->empty()});
        }
    } else if (payloadType == "user_message") {
        std::uint64_t key = 1469598103934665603ULL;
        HashAppend(&key, sessionId);
        HashAppend(&key, *timestamp);
        if (state->userKeys.insert(key).second) {
            state->userEvents.push_back(UserEvent{key, sessionId, timestampMicros});
        }
    }
}

bool ScanCandidate(const std::string &key, const CandidateFile &candidate, FileCheckpoint *checkpoint,
                   LocalUsageStatsCollector::State *state, std::stop_token stopToken,
                   LocalUsageSnapshot *snapshot) {
    if (candidate.size <= checkpoint->offset) {
        return true;
    }
    std::ifstream input(candidate.path, std::ios::binary);
    if (!input) {
        snapshot->partial = true;
        return false;
    }
    input.seekg(static_cast<std::streamoff>(checkpoint->offset), std::ios::beg);
    if (!input) {
        snapshot->partial = true;
        return false;
    }

    ++snapshot->filesReadThisScan;
    std::uint64_t position = checkpoint->offset;
    std::string line;
    while (position < candidate.size && std::getline(input, line)) {
        if (stopToken.stop_requested()) {
            return false;
        }
        const std::uint64_t lineBytes = static_cast<std::uint64_t>(line.size());
        if (position + lineBytes >= candidate.size) {
            // The writer has not committed a newline yet. Leave this line for the next pass.
            break;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (candidate.sessionIndex) {
            ParseIndexLine(line, state);
        } else {
            ParseSessionLine(line, key, checkpoint, state);
        }
        const std::uint64_t consumed = lineBytes + 1;
        position += consumed;
        checkpoint->offset = position;
        SaturatingAdd(&snapshot->bytesReadThisScan, consumed);
    }
    if (input.bad()) {
        snapshot->partial = true;
        return false;
    }
    if (!checkpoint->currentSessionId.empty()) {
        auto found = state->sessions.find(checkpoint->currentSessionId);
        if (found != state->sessions.end())
            for (const auto &component : candidate.path)
                if (component == L"archived_sessions")
                    found->second.archived = true;
    }
    return true;
}

LocalUsageSnapshot AggregateSnapshot(const LocalUsageStatsCollector::State &state, long long nowUnixSeconds,
                                     long long todayStartUnixSeconds, LocalUsageSnapshot snapshot) {
    const long long dayStartMicros = todayStartUnixSeconds * 1000000LL;
    const long long nowMicros = nowUnixSeconds * 1000000LL + 999999LL;
    long long earliest = 0;
    long long latest = 0;
    auto activity = std::make_shared<ActivityData>();
    activity->generation = nowUnixSeconds;
    activity->malformedLines = state.malformedLines;
    activity->invalidTimes = state.invalidTimes;
    activity->duplicates = state.duplicates;
    activity->completedTurns = state.completedTurns;
    std::map<std::string, std::uint32_t> sessionIndices;
    // Stable ordering makes ties and anonymous identifiers deterministic.
    for (const auto &[id, session] : state.sessions) {
        (void)session;
        sessionIndices.emplace(id, 0);
    }
    for (const auto &id : state.indexedTasks)
        sessionIndices.emplace(id, 0);
    for (const auto &e : state.tokenEvents)
        sessionIndices.emplace(e.sessionId, 0);
    for (const auto &e : state.userEvents)
        sessionIndices.emplace(e.sessionId, 0);
    for (auto &[id, index] : sessionIndices) {
        index = static_cast<std::uint32_t>(activity->sessions.size());
        const auto found = state.sessions.find(id);
        const SessionRecord s = found == state.sessions.end() ? SessionRecord{} : found->second;
        activity->sessions.push_back(
            {id, s.createdMicros / 1000000LL, s.topLevel, s.origin, s.client, s.archived});
    }

    std::unordered_map<std::string, std::vector<const TokenEvent *>> grouped;
    grouped.reserve(state.sessions.size() + 1);
    for (const TokenEvent &event : state.tokenEvents) {
        grouped[event.sessionId].push_back(&event);
        IncludeCoverage(event.timestampMicros, &earliest, &latest);
    }
    for (auto &[sessionId, events] : grouped) {
        (void)sessionId;
        std::sort(events.begin(), events.end(), [](const TokenEvent *lhs, const TokenEvent *rhs) {
            if (lhs->timestampMicros != rhs->timestampMicros) {
                return lhs->timestampMicros < rhs->timestampMicros;
            }
            return lhs->key < rhs->key;
        });
        TokenUsageTotals previous;
        bool hasPrevious = false;
        unsigned previousFields = 0;
        std::string previousModel, previousProject;
        for (const TokenEvent *event : events) {
            const TokenUsageTotals delta =
                DeltaTotals(event->cumulative, previous, hasPrevious, event->validFields & previousFields);
            const bool reset =
                hasPrevious && CounterReset(event->cumulative, previous, event->validFields & previousFields);
            if (reset)
                ++activity->resets;
            activity->events.push_back(
                {sessionIndices.at(event->sessionId), event->timestampMicros / 1000000LL, delta, false,
                 hasPrevious && !reset ? (event->validFields & previousFields) : event->validFields,
                 !hasPrevious || reset,
                 hasPrevious && !reset && previousModel == event->model ? event->model : "",
                 hasPrevious && !reset && previousProject == event->project ? event->project : ""});
            previousModel = event->model;
            previousProject = event->project;
            previousFields = reset ? event->validFields : previousFields | event->validFields;
            AddTotals(&snapshot.recordedTotal, delta);
            if (event->timestampMicros >= dayStartMicros && event->timestampMicros <= nowMicros) {
                AddTotals(&snapshot.today, delta);
            }
            if (reset)
                previous = {};
            // A temporarily absent component does not erase its last observed baseline.
            const unsigned present = event->validFields ? event->validFields : 63;
            if (present & 1)
                previous.inputTokens = event->cumulative.inputTokens;
            if (present & 2)
                previous.cachedInputTokens = event->cumulative.cachedInputTokens;
            if (present & 4)
                previous.cacheWriteInputTokens = event->cumulative.cacheWriteInputTokens;
            if (present & 8)
                previous.outputTokens = event->cumulative.outputTokens;
            if (present & 16)
                previous.reasoningOutputTokens = event->cumulative.reasoningOutputTokens;
            if (present & 32)
                previous.totalTokens = event->cumulative.totalTokens;
            hasPrevious = true;
        }
    }

    std::unordered_set<std::string> allTasks = state.indexedTasks;
    for (const auto &[id, session] : state.sessions) {
        IncludeCoverage(session.createdMicros, &earliest, &latest);
        if (session.topLevel) {
            allTasks.insert(id);
        }
    }
    snapshot.recordedTaskCount = static_cast<std::uint64_t>(allTasks.size());

    std::unordered_set<std::string> todayTasks;
    for (const UserEvent &event : state.userEvents) {
        activity->events.push_back(
            {sessionIndices.at(event.sessionId), event.timestampMicros / 1000000LL, {}, true, 63, false});
        IncludeCoverage(event.timestampMicros, &earliest, &latest);
        const auto session = state.sessions.find(event.sessionId);
        if (session == state.sessions.end() || !session->second.topLevel) {
            continue;
        }
        ++snapshot.recordedTurnCount;
        if (event.timestampMicros >= dayStartMicros && event.timestampMicros <= nowMicros) {
            ++snapshot.todayTurnCount;
            todayTasks.insert(event.sessionId);
        }
    }
    snapshot.todayTaskCount = static_cast<std::uint64_t>(todayTasks.size());
    snapshot.coverageStartUnixSeconds = earliest / 1000000LL;
    snapshot.lastEventUnixSeconds = latest / 1000000LL;
    snapshot.lastScanUnixSeconds = nowUnixSeconds;
    snapshot.tokenEventCount = static_cast<std::uint64_t>(state.tokenEvents.size());
    snapshot.available = true;
    std::sort(activity->events.begin(), activity->events.end(), [](const auto &a, const auto &b) {
        return a.time != b.time ? a.time < b.time : a.session < b.session;
    });
    snapshot.activity = std::move(activity);
    return snapshot;
}

} // namespace

LocalUsageStatsCollector::LocalUsageStatsCollector() : LocalUsageStatsCollector(ResolveDefaultPaths()) {}

LocalUsageStatsCollector::LocalUsageStatsCollector(LocalUsagePaths paths)
    : paths_(std::move(paths)), state_(std::make_unique<State>()) {}

LocalUsageStatsCollector::~LocalUsageStatsCollector() = default;

LocalUsagePaths LocalUsageStatsCollector::ResolveDefaultPaths() {
    LocalUsagePaths paths;
    if (const auto codexHome = ReadEnvironment(L"CODEX_HOME"); codexHome.has_value() && !codexHome->empty()) {
        paths.codexHome = *codexHome;
    } else if (const auto profile = ReadEnvironment(L"USERPROFILE");
               profile.has_value() && !profile->empty()) {
        paths.codexHome = std::filesystem::path(*profile) / L".codex";
    } else {
        paths.codexHome = L".codex";
    }

    if (const auto appData = ReadEnvironment(L"APPDATA"); appData.has_value() && !appData->empty()) {
        paths.cacheFile =
            std::filesystem::path(*appData) / L"CodexUsageMonitor" / L"local-usage-cache-v1.tsv";
    } else {
        paths.cacheFile = paths.codexHome / L"CodexUsageMonitor-local-usage-cache-v1.tsv";
    }
#if defined(CODEX_USAGE_MONITOR_UI_TEST_WINDOW)
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    paths.cacheFile = std::filesystem::path(executable).parent_path() / L"ui-test-activity-cache.tsv";
#endif
    return paths;
}

LocalUsageSnapshot LocalUsageStatsCollector::Refresh(std::stop_token stopToken,
                                                     long long nowUnixSecondsOverride,
                                                     long long todayStartUnixSecondsOverride) {
    LocalUsageSnapshot scan;
    const auto scanStarted = std::chrono::steady_clock::now();
    const long long nowUnixSeconds =
        nowUnixSecondsOverride > 0 ? nowUnixSecondsOverride : static_cast<long long>(std::time(nullptr));
    const long long todayStartUnixSeconds =
        todayStartUnixSecondsOverride > 0 ? todayStartUnixSecondsOverride : LocalDayStart(nowUnixSeconds);

    bool invalidCache = false;
    if (!stateLoaded_) {
        const CacheLoadResult result = LoadCache(paths_.cacheFile, state_.get());
        if (result == CacheLoadResult::TooLarge && !rebuildRequested_) {
            scan.partial = true;
            scan.errorMessage = L"Cache exceeds the 1 GiB capacity limit; automatic rebuild is paused";
            scan.lastScanUnixSeconds = nowUnixSeconds;
            return scan;
        }
        loadedFromCache_ = result == CacheLoadResult::Loaded;
        invalidCache = result == CacheLoadResult::Invalid;
        stateLoaded_ = true;
    }
    scan.cacheLoaded = loadedFromCache_;

    std::error_code homeError;
    if (!std::filesystem::exists(paths_.codexHome, homeError) || homeError) {
        scan.errorMessage = L"Codex home not found: " + paths_.codexHome.wstring();
        scan.lastScanUnixSeconds = nowUnixSeconds;
        return scan;
    }

    bool discoveryPartial = false;
    std::map<std::string, CandidateFile> candidates = DiscoverCandidates(paths_, &discoveryPartial);
    scan.partial = discoveryPartial;
    scan.filesDiscovered = static_cast<std::uint64_t>(candidates.size());

    bool rebuild = invalidCache || state_->legacy || rebuildRequested_;
    if (!rebuild) {
        for (const auto &[key, checkpoint] : state_->checkpoints) {
            const auto candidate = candidates.find(key);
            if (candidate == candidates.end() || candidate->second.size < checkpoint.offset ||
                (!checkpoint.identity.empty() &&
                 FileIdentity(candidate->second.path) != checkpoint.identity) ||
                (candidate->second.size == checkpoint.offset && checkpoint.modified != 0 &&
                 candidate->second.modified.time_since_epoch().count() != checkpoint.modified)) {
                rebuild = true;
                break;
            }
        }
    }

    State working = rebuild ? State{} : *state_;
    scan.rebuilt = rebuild;
    const size_t tokenCountBefore = working.tokenEvents.size();
    const size_t userCountBefore = working.userEvents.size();
    const size_t sessionCountBefore = working.sessions.size();
    const size_t indexCountBefore = working.indexedTasks.size();
    std::uint64_t offsetBefore = 0;
    for (const auto &[key, checkpoint] : working.checkpoints) {
        (void)key;
        SaturatingAdd(&offsetBefore, checkpoint.offset);
    }

    for (const auto &[key, candidate] : candidates) {
        if (stopToken.stop_requested()) {
            LocalUsageSnapshot cancelled =
                AggregateSnapshot(*state_, nowUnixSeconds, todayStartUnixSeconds, LocalUsageSnapshot{});
            cancelled.cacheLoaded = loadedFromCache_;
            cancelled.errorMessage = L"Local statistics refresh cancelled";
            return cancelled;
        }
        FileCheckpoint &checkpoint = working.checkpoints[key];
        checkpoint.identity = FileIdentity(candidate.path);
        checkpoint.modified = candidate.modified.time_since_epoch().count();
        if (!ScanCandidate(key, candidate, &checkpoint, &working, stopToken, &scan) &&
            stopToken.stop_requested()) {
            LocalUsageSnapshot cancelled =
                AggregateSnapshot(*state_, nowUnixSeconds, todayStartUnixSeconds, LocalUsageSnapshot{});
            cancelled.cacheLoaded = loadedFromCache_;
            cancelled.errorMessage = L"Local statistics refresh cancelled";
            return cancelled;
        }
    }

    if (rebuild && scan.partial) {
        scan.errorMessage = L"Rebuild incomplete; previous index retained";
        return AggregateSnapshot(*state_, nowUnixSeconds, todayStartUnixSeconds, std::move(scan));
    }
    std::uint64_t offsetAfter = 0;
    for (const auto &[key, checkpoint] : working.checkpoints) {
        (void)key;
        SaturatingAdd(&offsetAfter, checkpoint.offset);
    }
    scan.changed = rebuild || tokenCountBefore != working.tokenEvents.size() ||
                   userCountBefore != working.userEvents.size() ||
                   sessionCountBefore != working.sessions.size() ||
                   indexCountBefore != working.indexedTasks.size() || offsetBefore != offsetAfter;

    // Keep cache-only history during a schema upgrade; validity stays unknown.
    if (state_->legacy) {
        for (const auto &e : state_->tokenEvents)
            if (working.tokenKeys.insert(e.key).second)
                working.tokenEvents.push_back(e);
        for (const auto &e : state_->userEvents)
            if (working.userKeys.insert(e.key).second)
                working.userEvents.push_back(e);
        for (const auto &[id, s] : state_->sessions)
            working.sessions.try_emplace(id, s);
        working.indexedTasks.insert(state_->indexedTasks.begin(), state_->indexedTasks.end());
    }
    *state_ = std::move(working);
    rebuildRequested_ = false;
    if (scan.changed && !SaveCache(paths_.cacheFile, *state_)) {
        scan.partial = true;
        scan.errorMessage = L"Statistics updated, but the incremental cache could not be saved";
    }
    scan.scanMilliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - scanStarted)
            .count());
    std::error_code sizeError;
    const auto cacheSize = std::filesystem::file_size(paths_.cacheFile, sizeError);
    scan.cacheBytes = sizeError ? 0 : cacheSize;
    return AggregateSnapshot(*state_, nowUnixSeconds, todayStartUnixSeconds, std::move(scan));
}

std::wstring FormatCompactCount(std::uint64_t value) {
    if (value < 1000) {
        return std::to_wstring(value);
    }
    static constexpr const wchar_t *suffixes[] = {L"", L"K", L"M", L"B", L"T", L"Q"};
    long double scaled = static_cast<long double>(value);
    size_t suffix = 0;
    while (scaled >= 1000.0L && suffix + 1 < std::size(suffixes)) {
        scaled /= 1000.0L;
        ++suffix;
    }
    int precision = scaled < 100.0L ? 1 : 0;
    long double factor = precision == 1 ? 10.0L : 1.0L;
    scaled = std::floor(scaled * factor + 0.5L) / factor;
    if (scaled >= 1000.0L && suffix + 1 < std::size(suffixes)) {
        scaled /= 1000.0L;
        ++suffix;
        precision = scaled < 100.0L ? 1 : 0;
    }
    std::wostringstream output;
    output << std::fixed << std::setprecision(precision) << static_cast<double>(scaled);
    std::wstring text = output.str();
    if (const size_t decimal = text.find(L'.');
        decimal != std::wstring::npos && text.substr(decimal) == L".0") {
        text.erase(decimal);
    }
    return text + suffixes[suffix];
}

std::wstring FormatTraditionalChineseCount(std::uint64_t value) {
    if (value < 10000) {
        return std::to_wstring(value);
    }

    struct ChineseUnit {
        std::uint64_t divisor;
        const wchar_t *suffix;
    };
    static constexpr ChineseUnit units[] = {
        {10000ULL, L"萬"},
        {100000000ULL, L"億"},
        {1000000000000ULL, L"兆"},
        {10000000000000000ULL, L"京"},
    };

    size_t unitIndex = 0;
    for (size_t index = 1; index < std::size(units); ++index) {
        if (value >= units[index].divisor) {
            unitIndex = index;
        } else {
            break;
        }
    }

    auto roundScaled = [&](size_t index) {
        const long double scaled =
            static_cast<long double>(value) / static_cast<long double>(units[index].divisor);
        const int precision = scaled < 100.0L ? 1 : 0;
        const long double factor = precision == 1 ? 10.0L : 1.0L;
        return std::floor(scaled * factor + 0.5L) / factor;
    };

    long double rounded = roundScaled(unitIndex);
    while (rounded >= 10000.0L && unitIndex + 1 < std::size(units)) {
        ++unitIndex;
        rounded = roundScaled(unitIndex);
    }

    const int precision = rounded < 100.0L ? 1 : 0;
    std::wostringstream output;
    output << std::fixed << std::setprecision(precision) << static_cast<double>(rounded);
    std::wstring text = output.str();
    if (const size_t decimal = text.find(L'.'); decimal != std::wstring::npos) {
        while (!text.empty() && text.back() == L'0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == L'.') {
            text.pop_back();
        }
    }
    return text + L" " + units[unitIndex].suffix;
}

} // namespace codex_usage
