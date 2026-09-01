#include "LocalUsageStats.h"

#include "JsonLite.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
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

constexpr std::string_view kCacheHeader = "CodexUsageMonitorLocalStats\t1";
constexpr std::uintmax_t kMaximumCacheBytes = 64ULL * 1024ULL * 1024ULL;

struct FileCheckpoint {
    std::uint64_t offset = 0;
    std::string currentSessionId;
};

struct SessionRecord {
    bool topLevel = true;
    long long createdMicros = 0;
};

struct TokenEvent {
    std::uint64_t key = 0;
    std::string sessionId;
    long long timestampMicros = 0;
    TokenUsageTotals cumulative;
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
};

std::optional<std::wstring> ReadEnvironment(const wchar_t* name) {
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
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string output(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), required, nullptr, nullptr);
    return output;
}

std::wstring Utf8ToWide(std::string_view input) {
    if (input.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring output(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), required);
    return output;
}

template <typename Integer>
bool ParseInteger(std::string_view text, Integer* output) {
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

void HashAppend(std::uint64_t* hash, std::string_view text) {
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

void HashAppend(std::uint64_t* hash, std::uint64_t value) {
    HashAppend(hash, std::to_string(value));
}

std::optional<long long> ParseIso8601UnixMicros(const std::string& text) {
    if (text.size() < 19 || text[4] != '-' || text[7] != '-' || text[10] != 'T'
        || text[13] != ':' || text[16] != ':') {
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
    } catch (const std::exception&) {
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

std::optional<std::string> StringAt(const jsonlite::Value* parent, std::string_view key) {
    if (parent == nullptr) {
        return std::nullopt;
    }
    const jsonlite::Value* node = parent->Find(key);
    const auto value = node != nullptr ? node->AsString() : std::nullopt;
    if (!value.has_value()) {
        return std::nullopt;
    }
    return std::string(*value);
}

std::uint64_t NonNegativeNumberAt(const jsonlite::Value* parent, std::string_view key) {
    if (parent == nullptr) {
        return 0;
    }
    const jsonlite::Value* node = parent->Find(key);
    const auto value = node != nullptr ? node->AsNumber() : std::nullopt;
    if (!value.has_value() || !std::isfinite(*value) || *value <= 0.0) {
        return 0;
    }
    const long double rounded = std::round(static_cast<long double>(*value));
    if (rounded >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(rounded);
}

bool ContainsInsensitive(std::string value, std::string_view needle) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value.find(needle) != std::string::npos;
}

bool IsTopLevelSession(const jsonlite::Value* payload) {
    for (const std::string_view key : { std::string_view("source"), std::string_view("thread_source") }) {
        if (payload == nullptr) {
            break;
        }
        const jsonlite::Value* source = payload->Find(key);
        if (source == nullptr || source->IsNull()) {
            continue;
        }
        if (auto text = source->AsString(); text.has_value()) {
            if (ContainsInsensitive(std::string(*text), "subagent")
                || ContainsInsensitive(std::string(*text), "guardian")) {
                return false;
            }
        }
        if (const auto* object = source->AsObject(); object != nullptr) {
            if (object->find("subagent") != object->end() || object->find("guardian") != object->end()) {
                return false;
            }
        }
    }
    return true;
}

void SaturatingAdd(std::uint64_t* target, std::uint64_t value) {
    if (target == nullptr) {
        return;
    }
    if (std::numeric_limits<std::uint64_t>::max() - *target < value) {
        *target = std::numeric_limits<std::uint64_t>::max();
    } else {
        *target += value;
    }
}

void AddTotals(TokenUsageTotals* target, const TokenUsageTotals& value) {
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

TokenUsageTotals DeltaTotals(
    const TokenUsageTotals& current,
    const TokenUsageTotals& previous,
    bool hasPrevious) {
    const bool counterReset = hasPrevious
        && (current.inputTokens < previous.inputTokens
            || current.cachedInputTokens < previous.cachedInputTokens
            || current.cacheWriteInputTokens < previous.cacheWriteInputTokens
            || current.outputTokens < previous.outputTokens
            || current.reasoningOutputTokens < previous.reasoningOutputTokens
            || current.totalTokens < previous.totalTokens);
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

void IncludeCoverage(long long micros, long long* earliest, long long* latest) {
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

std::string FileKey(const std::filesystem::path& path) {
    return WideToUtf8(path.filename().wstring());
}

bool IsJsonlFile(const std::filesystem::directory_entry& entry) {
    std::error_code error;
    return entry.is_regular_file(error) && !error && entry.path().extension() == L".jsonl";
}

void AddDirectoryCandidates(
    const std::filesystem::path& directory,
    std::map<std::string, CandidateFile>* candidates,
    bool* partial) {
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
                CandidateFile candidate{ entry.path(), static_cast<std::uint64_t>(rawSize), modified, false };
                auto existing = candidates->find(key);
                if (existing == candidates->end()
                    || candidate.size > existing->second.size
                    || (candidate.size == existing->second.size && !timeError
                        && candidate.modified > existing->second.modified)) {
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

}  // namespace

struct LocalUsageStatsCollector::State {
    std::map<std::string, FileCheckpoint> checkpoints;
    std::unordered_map<std::string, SessionRecord> sessions;
    std::unordered_set<std::string> indexedTasks;
    std::vector<TokenEvent> tokenEvents;
    std::vector<UserEvent> userEvents;
    std::unordered_set<std::uint64_t> tokenKeys;
    std::unordered_set<std::uint64_t> userKeys;
};

namespace {

CacheLoadResult LoadCache(const std::filesystem::path& path, LocalUsageStatsCollector::State* state) {
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        return CacheLoadResult::Missing;
    }
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size > kMaximumCacheBytes) {
        return CacheLoadResult::Invalid;
    }

    std::ifstream input(path, std::ios::binary);
    std::string line;
    if (!input || !std::getline(input, line) || line != kCacheHeader) {
        return CacheLoadResult::Invalid;
    }

    LocalUsageStatsCollector::State parsed;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::vector<std::string_view> columns = SplitTabs(line);
        if (columns.empty() || columns[0].empty()) {
            return CacheLoadResult::Invalid;
        }
        if (columns[0] == "F" && columns.size() == 4) {
            FileCheckpoint checkpoint;
            if (!ParseInteger(columns[2], &checkpoint.offset)
                || !IsSafeCacheText(columns[1]) || !IsSafeCacheText(columns[3])) {
                return CacheLoadResult::Invalid;
            }
            checkpoint.currentSessionId = std::string(columns[3]);
            parsed.checkpoints.emplace(std::string(columns[1]), std::move(checkpoint));
        } else if (columns[0] == "S" && columns.size() == 4) {
            SessionRecord session;
            int topLevel = 0;
            if (!ParseInteger(columns[2], &session.createdMicros)
                || !ParseInteger(columns[3], &topLevel) || (topLevel != 0 && topLevel != 1)) {
                return CacheLoadResult::Invalid;
            }
            session.topLevel = topLevel == 1;
            parsed.sessions.emplace(std::string(columns[1]), session);
        } else if (columns[0] == "I" && columns.size() == 2) {
            parsed.indexedTasks.emplace(columns[1]);
        } else if (columns[0] == "T" && columns.size() == 11) {
            TokenEvent event;
            if (!ParseInteger(columns[1], &event.key)
                || !ParseInteger(columns[3], &event.timestampMicros)
                || !ParseInteger(columns[4], &event.cumulative.inputTokens)
                || !ParseInteger(columns[5], &event.cumulative.cachedInputTokens)
                || !ParseInteger(columns[6], &event.cumulative.cacheWriteInputTokens)
                || !ParseInteger(columns[7], &event.cumulative.outputTokens)
                || !ParseInteger(columns[8], &event.cumulative.reasoningOutputTokens)
                || !ParseInteger(columns[9], &event.cumulative.totalTokens)
                || columns[10] != "1") {
                return CacheLoadResult::Invalid;
            }
            event.sessionId = std::string(columns[2]);
            if (parsed.tokenKeys.insert(event.key).second) {
                parsed.tokenEvents.push_back(std::move(event));
            }
        } else if (columns[0] == "M" && columns.size() == 5 && columns[4] == "1") {
            UserEvent event;
            if (!ParseInteger(columns[1], &event.key)
                || !ParseInteger(columns[3], &event.timestampMicros)) {
                return CacheLoadResult::Invalid;
            }
            event.sessionId = std::string(columns[2]);
            if (parsed.userKeys.insert(event.key).second) {
                parsed.userEvents.push_back(std::move(event));
            }
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

bool SaveCache(const std::filesystem::path& path, const LocalUsageStatsCollector::State& state) {
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
    for (const auto& [key, checkpoint] : state.checkpoints) {
        if (IsSafeCacheText(key) && IsSafeCacheText(checkpoint.currentSessionId)) {
            output << "F\t" << key << '\t' << checkpoint.offset << '\t'
                   << checkpoint.currentSessionId << '\n';
        }
    }
    for (const auto& [id, session] : state.sessions) {
        if (IsSafeCacheText(id)) {
            output << "S\t" << id << '\t' << session.createdMicros << '\t'
                   << (session.topLevel ? 1 : 0) << '\n';
        }
    }
    for (const std::string& id : state.indexedTasks) {
        if (IsSafeCacheText(id)) {
            output << "I\t" << id << '\n';
        }
    }
    for (const TokenEvent& event : state.tokenEvents) {
        if (IsSafeCacheText(event.sessionId)) {
            output << "T\t" << event.key << '\t' << event.sessionId << '\t'
                   << event.timestampMicros << '\t' << event.cumulative.inputTokens << '\t'
                   << event.cumulative.cachedInputTokens << '\t'
                   << event.cumulative.cacheWriteInputTokens << '\t'
                   << event.cumulative.outputTokens << '\t'
                   << event.cumulative.reasoningOutputTokens << '\t'
                   << event.cumulative.totalTokens << "\t1\n";
        }
    }
    for (const UserEvent& event : state.userEvents) {
        if (IsSafeCacheText(event.sessionId)) {
            output << "M\t" << event.key << '\t' << event.sessionId << '\t'
                   << event.timestampMicros << "\t1\n";
        }
    }
    output.flush();
    if (!output) {
        output.close();
        std::filesystem::remove(temporary, error);
        return false;
    }
    output.close();

    if (!MoveFileExW(
            temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

std::map<std::string, CandidateFile> DiscoverCandidates(
    const LocalUsagePaths& paths,
    bool* partial) {
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

std::string EventSessionId(const std::string& currentSessionId, const std::string& fileKey) {
    if (!currentSessionId.empty()) {
        return currentSessionId;
    }
    std::filesystem::path filename(Utf8ToWide(fileKey));
    return WideToUtf8(filename.stem().wstring());
}

void ParseIndexLine(std::string_view line, LocalUsageStatsCollector::State* state) {
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

void ParseSessionLine(
    std::string_view line,
    const std::string& fileKey,
    FileCheckpoint* checkpoint,
    LocalUsageStatsCollector::State* state) {
    if (line.find("session_meta") == std::string_view::npos
        && line.find("token_count") == std::string_view::npos
        && line.find("user_message") == std::string_view::npos
        && line.find("response_item") == std::string_view::npos) {
        return;
    }

    jsonlite::Parser parser(line);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value()) {
        return;
    }
    const auto type = StringAt(&*root, "type");
    const jsonlite::Value* payload = root->Find("payload");
    const auto timestamp = StringAt(&*root, "timestamp");
    const long long timestampMicros = timestamp.has_value()
        ? ParseIso8601UnixMicros(*timestamp).value_or(0)
        : 0;

    if (type == "session_meta") {
        auto id = StringAt(payload, "id");
        if (!id.has_value()) {
            id = StringAt(payload, "session_id");
        }
        if (!id.has_value() || id->empty()) {
            return;
        }
        checkpoint->currentSessionId = *id;
        state->sessions[*id] = SessionRecord{ IsTopLevelSession(payload), timestampMicros };
        return;
    }

    if (payload == nullptr) {
        return;
    }
    const auto payloadType = StringAt(payload, "type");
    const std::string sessionId = EventSessionId(checkpoint->currentSessionId, fileKey);
    if (sessionId.empty() || timestampMicros <= 0 || !timestamp.has_value()) {
        return;
    }

    if (type == "response_item") {
        const auto role = StringAt(payload, "role");
        if (payloadType == "message" && role == "user") {
            std::uint64_t key = 1469598103934665603ULL;
            HashAppend(&key, sessionId);
            HashAppend(&key, *timestamp);
            if (state->userKeys.insert(key).second) {
                state->userEvents.push_back(UserEvent{ key, sessionId, timestampMicros });
            }
        }
        return;
    }
    if (type != "event_msg") {
        return;
    }

    if (payloadType == "token_count") {
        const jsonlite::Value* info = payload->Find("info");
        const jsonlite::Value* totals = info != nullptr ? info->Find("total_token_usage") : nullptr;
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
            state->tokenEvents.push_back(TokenEvent{ key, sessionId, timestampMicros, cumulative });
        }
    } else if (payloadType == "user_message") {
        std::uint64_t key = 1469598103934665603ULL;
        HashAppend(&key, sessionId);
        HashAppend(&key, *timestamp);
        if (state->userKeys.insert(key).second) {
            state->userEvents.push_back(UserEvent{ key, sessionId, timestampMicros });
        }
    }
}

bool ScanCandidate(
    const std::string& key,
    const CandidateFile& candidate,
    FileCheckpoint* checkpoint,
    LocalUsageStatsCollector::State* state,
    std::stop_token stopToken,
    LocalUsageSnapshot* snapshot) {
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
    return true;
}

LocalUsageSnapshot AggregateSnapshot(
    const LocalUsageStatsCollector::State& state,
    long long nowUnixSeconds,
    long long todayStartUnixSeconds,
    LocalUsageSnapshot snapshot) {
    const long long dayStartMicros = todayStartUnixSeconds * 1000000LL;
    const long long nowMicros = nowUnixSeconds * 1000000LL + 999999LL;
    long long earliest = 0;
    long long latest = 0;

    std::unordered_map<std::string, std::vector<const TokenEvent*>> grouped;
    grouped.reserve(state.sessions.size() + 1);
    for (const TokenEvent& event : state.tokenEvents) {
        grouped[event.sessionId].push_back(&event);
        IncludeCoverage(event.timestampMicros, &earliest, &latest);
    }
    for (auto& [sessionId, events] : grouped) {
        (void)sessionId;
        std::sort(events.begin(), events.end(), [](const TokenEvent* lhs, const TokenEvent* rhs) {
            if (lhs->timestampMicros != rhs->timestampMicros) {
                return lhs->timestampMicros < rhs->timestampMicros;
            }
            return lhs->key < rhs->key;
        });
        TokenUsageTotals previous;
        bool hasPrevious = false;
        for (const TokenEvent* event : events) {
            const TokenUsageTotals delta = DeltaTotals(event->cumulative, previous, hasPrevious);
            AddTotals(&snapshot.recordedTotal, delta);
            if (event->timestampMicros >= dayStartMicros && event->timestampMicros <= nowMicros) {
                AddTotals(&snapshot.today, delta);
            }
            previous = event->cumulative;
            hasPrevious = true;
        }
    }

    std::unordered_set<std::string> allTasks = state.indexedTasks;
    for (const auto& [id, session] : state.sessions) {
        IncludeCoverage(session.createdMicros, &earliest, &latest);
        if (session.topLevel) {
            allTasks.insert(id);
        }
    }
    snapshot.recordedTaskCount = static_cast<std::uint64_t>(allTasks.size());

    std::unordered_set<std::string> todayTasks;
    for (const UserEvent& event : state.userEvents) {
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
    return snapshot;
}

}  // namespace

LocalUsageStatsCollector::LocalUsageStatsCollector()
    : LocalUsageStatsCollector(ResolveDefaultPaths()) {}

LocalUsageStatsCollector::LocalUsageStatsCollector(LocalUsagePaths paths)
    : paths_(std::move(paths)), state_(std::make_unique<State>()) {}

LocalUsageStatsCollector::~LocalUsageStatsCollector() = default;

LocalUsagePaths LocalUsageStatsCollector::ResolveDefaultPaths() {
    LocalUsagePaths paths;
    if (const auto codexHome = ReadEnvironment(L"CODEX_HOME"); codexHome.has_value() && !codexHome->empty()) {
        paths.codexHome = *codexHome;
    } else if (const auto profile = ReadEnvironment(L"USERPROFILE"); profile.has_value() && !profile->empty()) {
        paths.codexHome = std::filesystem::path(*profile) / L".codex";
    } else {
        paths.codexHome = L".codex";
    }

    if (const auto appData = ReadEnvironment(L"APPDATA"); appData.has_value() && !appData->empty()) {
        paths.cacheFile = std::filesystem::path(*appData)
            / L"CodexUsageMonitor" / L"local-usage-cache-v1.tsv";
    } else {
        paths.cacheFile = paths.codexHome / L"CodexUsageMonitor-local-usage-cache-v1.tsv";
    }
    return paths;
}

LocalUsageSnapshot LocalUsageStatsCollector::Refresh(
    std::stop_token stopToken,
    long long nowUnixSecondsOverride,
    long long todayStartUnixSecondsOverride) {
    LocalUsageSnapshot scan;
    const long long nowUnixSeconds = nowUnixSecondsOverride > 0
        ? nowUnixSecondsOverride
        : static_cast<long long>(std::time(nullptr));
    const long long todayStartUnixSeconds = todayStartUnixSecondsOverride > 0
        ? todayStartUnixSecondsOverride
        : LocalDayStart(nowUnixSeconds);

    bool invalidCache = false;
    if (!stateLoaded_) {
        const CacheLoadResult result = LoadCache(paths_.cacheFile, state_.get());
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

    bool rebuild = invalidCache;
    if (!rebuild) {
        for (const auto& [key, checkpoint] : state_->checkpoints) {
            const auto candidate = candidates.find(key);
            if (candidate == candidates.end() || candidate->second.size < checkpoint.offset) {
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
    for (const auto& [key, checkpoint] : working.checkpoints) {
        (void)key;
        SaturatingAdd(&offsetBefore, checkpoint.offset);
    }

    for (const auto& [key, candidate] : candidates) {
        if (stopToken.stop_requested()) {
            LocalUsageSnapshot cancelled = AggregateSnapshot(
                *state_, nowUnixSeconds, todayStartUnixSeconds, LocalUsageSnapshot{});
            cancelled.cacheLoaded = loadedFromCache_;
            cancelled.errorMessage = L"Local statistics refresh cancelled";
            return cancelled;
        }
        FileCheckpoint& checkpoint = working.checkpoints[key];
        if (!ScanCandidate(key, candidate, &checkpoint, &working, stopToken, &scan)
            && stopToken.stop_requested()) {
            LocalUsageSnapshot cancelled = AggregateSnapshot(
                *state_, nowUnixSeconds, todayStartUnixSeconds, LocalUsageSnapshot{});
            cancelled.cacheLoaded = loadedFromCache_;
            cancelled.errorMessage = L"Local statistics refresh cancelled";
            return cancelled;
        }
    }

    std::uint64_t offsetAfter = 0;
    for (const auto& [key, checkpoint] : working.checkpoints) {
        (void)key;
        SaturatingAdd(&offsetAfter, checkpoint.offset);
    }
    scan.changed = rebuild
        || tokenCountBefore != working.tokenEvents.size()
        || userCountBefore != working.userEvents.size()
        || sessionCountBefore != working.sessions.size()
        || indexCountBefore != working.indexedTasks.size()
        || offsetBefore != offsetAfter;

    *state_ = std::move(working);
    if (scan.changed && !SaveCache(paths_.cacheFile, *state_)) {
        scan.partial = true;
        scan.errorMessage = L"Statistics updated, but the incremental cache could not be saved";
    }
    return AggregateSnapshot(*state_, nowUnixSeconds, todayStartUnixSeconds, std::move(scan));
}

std::wstring FormatCompactCount(std::uint64_t value) {
    if (value < 1000) {
        return std::to_wstring(value);
    }
    static constexpr const wchar_t* suffixes[] = { L"", L"K", L"M", L"B", L"T", L"Q" };
    long double scaled = static_cast<long double>(value);
    size_t suffix = 0;
    while (scaled >= 1000.0L && suffix + 1 < std::size(suffixes)) {
        scaled /= 1000.0L;
        ++suffix;
    }
    std::wostringstream output;
    if (scaled < 100.0L) {
        output << std::fixed << std::setprecision(1) << static_cast<double>(scaled);
    } else {
        output << std::fixed << std::setprecision(0) << static_cast<double>(scaled);
    }
    std::wstring text = output.str();
    if (const size_t decimal = text.find(L'.'); decimal != std::wstring::npos
        && text.substr(decimal) == L".0") {
        text.erase(decimal);
    }
    return text + suffixes[suffix];
}

}  // namespace codex_usage
