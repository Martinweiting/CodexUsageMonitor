#include "CodexUsageFetcher.h"

#include "JsonLite.h"

#include <Windows.h>
#include <Shlwapi.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "crypt32.lib")

namespace {

std::wstring Utf8ToWide(const std::string& input) {
    if (input.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    std::wstring output(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), size);
    return output;
}

std::wstring JoinPath(const std::wstring& base, const std::wstring& child) {
    std::wstring result = base;
    if (!result.empty() && result.back() != L'\\' && result.back() != L'/') {
        result.push_back(L'\\');
    }
    result += child;
    return result;
}

std::optional<std::wstring> ReadEnv(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) {
        return std::nullopt;
    }

    std::wstring value(size - 1, L'\0');
    GetEnvironmentVariableW(name, value.data(), size);
    return value;
}

// Parses ISO-8601 timestamps:
// - 2026-07-18T00:39:53Z
// - 2026-07-18T00:39:53.868059Z
// - 2026-06-14T09:54:55+08:00
std::optional<long long> ParseIso8601UnixSeconds(const std::string& text) {
    if (text.size() < 19) {
        return std::nullopt;
    }

    try {
        std::tm tm = {};
        tm.tm_year = std::stoi(text.substr(0, 4)) - 1900;
        tm.tm_mon = std::stoi(text.substr(5, 2)) - 1;
        tm.tm_mday = std::stoi(text.substr(8, 2));
        tm.tm_hour = std::stoi(text.substr(11, 2));
        tm.tm_min = std::stoi(text.substr(14, 2));
        tm.tm_sec = std::stoi(text.substr(17, 2));
        tm.tm_isdst = 0;

        long long offsetSeconds = 0;
        size_t pos = 19;
        if (pos < text.size() && text[pos] == '.') {
            ++pos;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
                ++pos;
            }
        }
        if (pos < text.size()) {
            if (text[pos] == 'Z' || text[pos] == 'z') {
                // UTC
            } else if ((text[pos] == '+' || text[pos] == '-') && pos + 5 < text.size()) {
                const int sign = text[pos] == '+' ? 1 : -1;
                const int oh = std::stoi(text.substr(pos + 1, 2));
                int om = 0;
                if (text[pos + 3] == ':' && pos + 5 < text.size()) {
                    om = std::stoi(text.substr(pos + 4, 2));
                } else {
                    om = std::stoi(text.substr(pos + 3, 2));
                }
                // Value is local wall time in that offset; convert to UTC.
                offsetSeconds = -static_cast<long long>(sign) * (oh * 3600LL + om * 60LL);
            }
        }

        const time_t utc = _mkgmtime(&tm);
        if (utc == static_cast<time_t>(-1)) {
            return std::nullopt;
        }
        return static_cast<long long>(utc) + offsetSeconds;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<std::string> HttpExchange(
    const std::wstring& userAgent,
    const std::wstring& host,
    const std::wstring& path,
    const std::wstring& method,
    const std::vector<std::wstring>& headers,
    const std::string* body,
    std::wstring* errorMessage) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    HINTERNET session = WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = L"WinHttpOpen failed";
        }
        return std::nullopt;
    }

    std::optional<std::string> responseBody;
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;

    do {
        connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (connect == nullptr) {
            if (errorMessage != nullptr) {
                *errorMessage = L"WinHttpConnect failed";
            }
            break;
        }

        request = WinHttpOpenRequest(connect, method.c_str(), path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (request == nullptr) {
            if (errorMessage != nullptr) {
                *errorMessage = L"WinHttpOpenRequest failed";
            }
            break;
        }

        for (const std::wstring& header : headers) {
            if (!WinHttpAddRequestHeaders(request, header.c_str(), static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD)) {
                if (errorMessage != nullptr) {
                    *errorMessage = L"WinHttpAddRequestHeaders failed";
                }
                break;
            }
        }
        if (errorMessage != nullptr && !errorMessage->empty()) {
            break;
        }

        DWORD timeout = 15000;
        WinHttpSetTimeouts(request, timeout, timeout, timeout, timeout);

        LPVOID bodyPtr = body != nullptr ? const_cast<char*>(body->data()) : WINHTTP_NO_REQUEST_DATA;
        DWORD bodySize = body != nullptr ? static_cast<DWORD>(body->size()) : 0;
        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, bodyPtr, bodySize, bodySize, 0)) {
            if (errorMessage != nullptr) {
                *errorMessage = L"WinHttpSendRequest failed";
            }
            break;
        }

        if (!WinHttpReceiveResponse(request, nullptr)) {
            if (errorMessage != nullptr) {
                *errorMessage = L"WinHttpReceiveResponse failed";
            }
            break;
        }

        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
        if (statusCode < 200 || statusCode > 299) {
            if (errorMessage != nullptr) {
                *errorMessage = host + path + L" returned HTTP " + std::to_wstring(statusCode);
                if (statusCode == 401) {
                    *errorMessage += L"; auth.json access_token may be expired and automatic refresh is not implemented";
                }
            }
            break;
        }

        std::string response;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
                if (errorMessage != nullptr) {
                    *errorMessage = L"WinHttpQueryDataAvailable failed";
                }
                break;
            }
            if (available == 0) {
                responseBody = std::move(response);
                break;
            }

            std::string chunk(static_cast<size_t>(available), '\0');
            DWORD downloaded = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &downloaded)) {
                if (errorMessage != nullptr) {
                    *errorMessage = L"WinHttpReadData failed";
                }
                break;
            }

            chunk.resize(downloaded);
            response.append(chunk);
        }
    } while (false);

    if (request != nullptr) {
        WinHttpCloseHandle(request);
    }
    if (connect != nullptr) {
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);

    return responseBody;
}

std::optional<std::string> HttpGetJson(
    const std::wstring& userAgent,
    const std::wstring& host,
    const std::wstring& path,
    const std::vector<std::wstring>& headers,
    std::wstring* errorMessage) {
    return HttpExchange(userAgent, host, path, L"GET", headers, nullptr, errorMessage);
}

std::vector<std::wstring> BuildCodexAuthHeaders(
    const std::string& accessToken,
    const std::string& accountId,
    bool forResetCredits) {
    std::vector<std::wstring> headers = {
        L"Authorization: Bearer " + Utf8ToWide(accessToken),
        L"Accept: application/json",
    };
    if (forResetCredits) {
        headers.push_back(L"OpenAI-Beta: codex-1");
        headers.push_back(L"originator: Codex Desktop");
    }
    if (!accountId.empty()) {
        headers.push_back(L"ChatGPT-Account-Id: " + Utf8ToWide(accountId));
    }
    return headers;
}

bool ExtractWindow(const jsonlite::Value* windowNode, UsageWindow* output) {
    if (windowNode == nullptr || output == nullptr) {
        return false;
    }

    *output = UsageWindow{};
    const jsonlite::Value* usedPercent = windowNode->Find("used_percent");
    const jsonlite::Value* limitWindowSeconds = windowNode->Find("limit_window_seconds");
    const jsonlite::Value* resetAfterSeconds = windowNode->Find("reset_after_seconds");
    const jsonlite::Value* resetAt = windowNode->Find("reset_at");
    if (usedPercent == nullptr) {
        return false;
    }

    const auto used = usedPercent->AsNumber();
    if (!used.has_value() || !std::isfinite(*used)) {
        return false;
    }

    const double normalizedUsed = std::clamp(*used, 0.0, 100.0);
    output->available = true;
    output->usedPercent = static_cast<int>(std::lround(normalizedUsed));
    output->remainingPercent = static_cast<int>(std::lround(100.0 - normalizedUsed));
    if (limitWindowSeconds != nullptr) {
        if (auto limit = limitWindowSeconds->AsNumber(); limit.has_value() && std::isfinite(*limit)) {
            output->windowSeconds = std::max(0, static_cast<int>(std::lround(*limit)));
        }
    }
    if (resetAfterSeconds != nullptr) {
        if (auto resetAfter = resetAfterSeconds->AsNumber(); resetAfter.has_value() && std::isfinite(*resetAfter)) {
            output->resetAfterSeconds = std::max(0, static_cast<int>(std::lround(*resetAfter)));
        }
    }
    if (resetAt != nullptr) {
        if (auto resetAtValue = resetAt->AsNumber(); resetAtValue.has_value() && std::isfinite(*resetAtValue)) {
            output->resetAtUnixSeconds = static_cast<long long>(std::llround(*resetAtValue));
        }
    }
    if (output->windowSeconds > 0 && output->resetAtUnixSeconds > 0) {
        output->startAtUnixSeconds = output->resetAtUnixSeconds - output->windowSeconds;
        output->hasStartAt = true;
    }
    return true;
}

constexpr int kFiveHourWindowSeconds = 5 * 60 * 60;
constexpr int kWeeklyWindowSeconds = 7 * 24 * 60 * 60;

bool IsFiveHourWindow(const UsageWindow& window) {
    return window.available && window.windowSeconds == kFiveHourWindowSeconds;
}

bool IsWeeklyWindow(const UsageWindow& window) {
    return window.available && window.windowSeconds == kWeeklyWindowSeconds;
}

void AssignUsageWindows(
    const UsageWindow& primary,
    bool hasPrimary,
    const UsageWindow& secondary,
    bool hasSecondary,
    UsageSnapshot* snapshot) {
    if (snapshot == nullptr) {
        return;
    }

    bool primaryAssigned = false;
    bool secondaryAssigned = false;
    if (IsFiveHourWindow(primary)) {
        snapshot->fiveHour = primary;
        primaryAssigned = true;
    } else if (IsWeeklyWindow(primary)) {
        snapshot->weekly = primary;
        primaryAssigned = true;
    }
    if (IsFiveHourWindow(secondary)) {
        snapshot->fiveHour = secondary;
        secondaryAssigned = true;
    } else if (IsWeeklyWindow(secondary)) {
        snapshot->weekly = secondary;
        secondaryAssigned = true;
    }

    // If the duration is unavailable or unfamiliar, preserve the legacy node order.
    if (!snapshot->fiveHour.available && !snapshot->weekly.available) {
        if (hasPrimary) {
            snapshot->fiveHour = primary;
            primaryAssigned = true;
        }
        if (hasSecondary) {
            snapshot->weekly = secondary;
            secondaryAssigned = true;
        }
        return;
    }

    // When one duration is known, use the remaining unclassified node for the other slot.
    if (hasPrimary && !primaryAssigned) {
        if (!snapshot->fiveHour.available) {
            snapshot->fiveHour = primary;
            primaryAssigned = true;
        } else if (!snapshot->weekly.available) {
            snapshot->weekly = primary;
            primaryAssigned = true;
        }
    }
    if (hasSecondary && !secondaryAssigned) {
        if (!snapshot->fiveHour.available) {
            snapshot->fiveHour = secondary;
        } else if (!snapshot->weekly.available) {
            snapshot->weekly = secondary;
        }
    }
}

std::optional<std::string> Base64UrlDecode(const std::string& input) {
    std::string normalized;
    normalized.reserve(input.size() + 4);
    for (char ch : input) {
        if (ch == '-') {
            normalized.push_back('+');
        } else if (ch == '_') {
            normalized.push_back('/');
        } else {
            normalized.push_back(ch);
        }
    }
    while (normalized.size() % 4 != 0) {
        normalized.push_back('=');
    }

    DWORD required = 0;
    if (!CryptStringToBinaryA(
            normalized.c_str(),
            static_cast<DWORD>(normalized.size()),
            CRYPT_STRING_BASE64,
            nullptr,
            &required,
            nullptr,
            nullptr) || required == 0) {
        return std::nullopt;
    }

    std::string output(required, '\0');
    if (!CryptStringToBinaryA(
            normalized.c_str(),
            static_cast<DWORD>(normalized.size()),
            CRYPT_STRING_BASE64,
            reinterpret_cast<BYTE*>(output.data()),
            &required,
            nullptr,
            nullptr)) {
        return std::nullopt;
    }
    output.resize(required);
    return output;
}

std::optional<std::string> DecodeJwtPayloadJson(const std::string& jwt) {
    const size_t firstDot = jwt.find('.');
    if (firstDot == std::string::npos) {
        return std::nullopt;
    }
    const size_t secondDot = jwt.find('.', firstDot + 1);
    if (secondDot == std::string::npos) {
        return std::nullopt;
    }
    return Base64UrlDecode(jwt.substr(firstDot + 1, secondDot - firstDot - 1));
}

}  // namespace

UsageSnapshot CodexUsageFetcher::Fetch() const {
    UsageSnapshot snapshot;

    std::wstring errorMessage;
    std::optional<AuthCredentials> credentials = ReadAuthCredentials(&errorMessage);
    if (!credentials.has_value()) {
        snapshot.errorMessage = errorMessage;
        return snapshot;
    }

    std::optional<std::string> usageJson = HttpGetUsageJson(*credentials, &errorMessage);
    if (!usageJson.has_value()) {
        snapshot.errorMessage = errorMessage;
        return snapshot;
    }

    snapshot = ParseUsageJson(*usageJson, &errorMessage);
    if (!snapshot.success) {
        snapshot.errorMessage = errorMessage;
        return snapshot;
    }

    if (!credentials->idToken.empty()) {
        EnrichSubscriptionFromIdToken(&snapshot, credentials->idToken);
    }

    // Best-effort inventory; usage success is preserved even if this fails.
    std::wstring resetError;
    std::optional<std::string> resetJson = HttpGetRateLimitResetCreditsJson(*credentials, &resetError);
    if (resetJson.has_value()) {
        snapshot.resetCredits = ParseRateLimitResetCreditsJson(*resetJson, &resetError);
        if (!snapshot.resetCredits.fetched) {
            snapshot.resetCredits.errorMessage = resetError;
        }
    } else {
        snapshot.resetCredits.fetched = false;
        snapshot.resetCredits.errorMessage = resetError;
    }

    return snapshot;
}

ReleaseVersionInfo CodexUsageFetcher::FetchLatestRelease() const {
    ReleaseVersionInfo info;

    std::wstring errorMessage;
    std::optional<std::string> releaseJson = HttpGetLatestReleaseJson(&errorMessage);
    if (!releaseJson.has_value()) {
        info.errorMessage = errorMessage;
        return info;
    }

    info = ParseLatestReleaseJson(*releaseJson, &errorMessage);
    if (!info.success) {
        info.errorMessage = errorMessage;
    }
    return info;
}

std::wstring CodexUsageFetcher::ResolveAuthJsonPath() const {
    // Prefer auth.json next to the executable; fall back to CODEX_HOME / ~/.codex.
    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) > 0) {
        const std::wstring localAuth =
            JoinPath(std::filesystem::path(modulePath).parent_path().wstring(), L"auth.json");
        if (std::filesystem::exists(localAuth)) {
            return localAuth;
        }
    }

    if (auto codexHome = ReadEnv(L"CODEX_HOME"); codexHome.has_value() && !codexHome->empty()) {
        return JoinPath(*codexHome, L"auth.json");
    }

    if (auto userProfile = ReadEnv(L"USERPROFILE"); userProfile.has_value() && !userProfile->empty()) {
        return JoinPath(JoinPath(*userProfile, L".codex"), L"auth.json");
    }

    return L".codex\\auth.json";
}

std::optional<CodexUsageFetcher::AuthCredentials> CodexUsageFetcher::ReadAuthCredentials(
    std::wstring* errorMessage) const {
    const std::wstring authPath = ResolveAuthJsonPath();
    std::optional<std::string> jsonText = LoadFileUtf8(authPath, errorMessage);
    if (!jsonText.has_value()) {
        return std::nullopt;
    }

    jsonlite::Parser parser(*jsonText);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"auth.json parse failed: " + Utf8ToWide(parser.Error());
        }
        return std::nullopt;
    }

    const jsonlite::Value* tokens = root->Find("tokens");
    const jsonlite::Value* accessToken = tokens != nullptr ? tokens->Find("access_token") : nullptr;
    auto token = accessToken != nullptr ? accessToken->AsString() : std::nullopt;
    if (!token.has_value() || token->empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"auth.json missing tokens.access_token";
        }
        return std::nullopt;
    }

    AuthCredentials credentials;
    credentials.accessToken = std::string(*token);

    const jsonlite::Value* accountIdNode = tokens != nullptr ? tokens->Find("account_id") : nullptr;
    if (accountIdNode == nullptr) {
        accountIdNode = root->Find("account_id");
    }
    if (auto accountId = accountIdNode != nullptr ? accountIdNode->AsString() : std::nullopt;
        accountId.has_value()) {
        credentials.accountId = std::string(*accountId);
    }

    const jsonlite::Value* idTokenNode = tokens != nullptr ? tokens->Find("id_token") : nullptr;
    if (auto idToken = idTokenNode != nullptr ? idTokenNode->AsString() : std::nullopt;
        idToken.has_value()) {
        credentials.idToken = std::string(*idToken);
    }

    return credentials;
}

std::optional<std::string> CodexUsageFetcher::LoadFileUtf8(const std::wstring& path, std::wstring* errorMessage) const {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (errorMessage != nullptr) {
            *errorMessage = L"cannot open " + path;
        }
        return std::nullopt;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::optional<std::string> CodexUsageFetcher::HttpGetUsageJson(
    const AuthCredentials& credentials,
    std::wstring* errorMessage) const {
    return HttpGetJson(
        L"CodexUsageMonitor/0.1",
        L"chatgpt.com",
        L"/backend-api/wham/usage",
        BuildCodexAuthHeaders(credentials.accessToken, credentials.accountId, false),
        errorMessage);
}

std::optional<std::string> CodexUsageFetcher::HttpGetRateLimitResetCreditsJson(
    const AuthCredentials& credentials,
    std::wstring* errorMessage) const {
    return HttpGetJson(
        L"CodexUsageMonitor/0.1",
        L"chatgpt.com",
        L"/backend-api/wham/rate-limit-reset-credits",
        BuildCodexAuthHeaders(credentials.accessToken, credentials.accountId, true),
        errorMessage);
}

std::optional<std::string> CodexUsageFetcher::HttpGetLatestReleaseJson(std::wstring* errorMessage) const {
    return HttpGetJson(
        L"CodexUsageMonitor/0.1",
        L"api.github.com",
        L"/repos/luodaoyi/codex-usage-monitor/releases/latest",
        {
            L"Accept: application/vnd.github+json",
            L"X-GitHub-Api-Version: 2022-11-28",
            L"User-Agent: CodexUsageMonitor"
        },
        errorMessage);
}

UsageSnapshot CodexUsageFetcher::ParseUsageJson(const std::string& jsonText, std::wstring* errorMessage) const {
    UsageSnapshot snapshot;

    jsonlite::Parser parser(jsonText);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"usage JSON parse failed: " + Utf8ToWide(parser.Error());
        }
        return snapshot;
    }

    const jsonlite::Value* email = root->Find("email");
    const jsonlite::Value* planType = root->Find("plan_type");
    const jsonlite::Value* rateLimit = root->Find("rate_limit");
    const jsonlite::Value* primaryWindow = rateLimit != nullptr ? rateLimit->Find("primary_window") : nullptr;
    const jsonlite::Value* secondaryWindow = rateLimit != nullptr ? rateLimit->Find("secondary_window") : nullptr;
    UsageWindow primary;
    UsageWindow secondary;
    const bool hasPrimary = ExtractWindow(primaryWindow, &primary);
    const bool hasSecondary = ExtractWindow(secondaryWindow, &secondary);
    AssignUsageWindows(primary, hasPrimary, secondary, hasSecondary, &snapshot);
    if (!snapshot.fiveHour.available && !snapshot.weekly.available) {
        if (errorMessage != nullptr) {
            *errorMessage = L"usage payload missing usable rate_limit windows";
        }
        return snapshot;
    }

    if (auto emailString = email != nullptr ? email->AsString() : std::nullopt; emailString.has_value()) {
        snapshot.email = Utf8ToWide(std::string(*emailString));
    }
    if (auto planTypeString = planType != nullptr ? planType->AsString() : std::nullopt; planTypeString.has_value()) {
        snapshot.planType = Utf8ToWide(std::string(*planTypeString));
    }

    snapshot.success = true;
    return snapshot;
}

void CodexUsageFetcher::EnrichSubscriptionFromIdToken(
    UsageSnapshot* snapshot,
    const std::string& idToken) const {
    if (snapshot == nullptr || idToken.empty()) {
        return;
    }

    std::optional<std::string> payloadJson = DecodeJwtPayloadJson(idToken);
    if (!payloadJson.has_value()) {
        return;
    }

    jsonlite::Parser parser(*payloadJson);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value()) {
        return;
    }

    const jsonlite::Value* auth = root->Find("https://api.openai.com/auth");
    if (auth == nullptr) {
        return;
    }

    if (snapshot->planType.empty()) {
        if (auto plan = auth->Find("chatgpt_plan_type") != nullptr
                ? auth->Find("chatgpt_plan_type")->AsString()
                : std::nullopt;
            plan.has_value() && !plan->empty()) {
            snapshot->planType = Utf8ToWide(std::string(*plan));
        }
    }

    if (auto start = auth->Find("chatgpt_subscription_active_start") != nullptr
            ? auth->Find("chatgpt_subscription_active_start")->AsString()
            : std::nullopt;
        start.has_value()) {
        if (auto unix = ParseIso8601UnixSeconds(std::string(*start)); unix.has_value()) {
            snapshot->planStartUnixSeconds = *unix;
            snapshot->hasPlanStart = true;
        }
    }

    if (auto until = auth->Find("chatgpt_subscription_active_until") != nullptr
            ? auth->Find("chatgpt_subscription_active_until")->AsString()
            : std::nullopt;
        until.has_value()) {
        if (auto unix = ParseIso8601UnixSeconds(std::string(*until)); unix.has_value()) {
            snapshot->planUntilUnixSeconds = *unix;
            snapshot->hasPlanUntil = true;
        }
    }
}

RateLimitResetCreditsInfo CodexUsageFetcher::ParseRateLimitResetCreditsJson(
    const std::string& jsonText,
    std::wstring* errorMessage) const {
    RateLimitResetCreditsInfo info;

    jsonlite::Parser parser(jsonText);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"reset credits JSON parse failed: " + Utf8ToWide(parser.Error());
        }
        return info;
    }

    const jsonlite::Value* availableCountNode = root->Find("available_count");
    auto availableCount = availableCountNode != nullptr ? availableCountNode->AsInt() : std::nullopt;
    if (!availableCount.has_value() || *availableCount < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = L"reset credits payload missing available_count";
        }
        return info;
    }
    info.availableCount = *availableCount;

    const long long nowUnix = static_cast<long long>(std::time(nullptr));
    const jsonlite::Value* creditsNode = root->Find("credits");
    if (const auto* credits = creditsNode != nullptr ? creditsNode->AsArray() : nullptr; credits != nullptr) {
        for (const jsonlite::Value& creditNode : *credits) {
            const jsonlite::Value* statusNode = creditNode.Find("status");
            auto status = statusNode != nullptr ? statusNode->AsString() : std::nullopt;
            if (!status.has_value() || *status != "available") {
                continue;
            }

            RateLimitResetCredit credit;
            credit.status = L"available";

            if (auto id = creditNode.Find("id") != nullptr ? creditNode.Find("id")->AsString() : std::nullopt;
                id.has_value()) {
                credit.id = Utf8ToWide(std::string(*id));
            }
            if (auto resetType = creditNode.Find("reset_type") != nullptr
                    ? creditNode.Find("reset_type")->AsString()
                    : std::nullopt;
                resetType.has_value()) {
                credit.resetType = Utf8ToWide(std::string(*resetType));
            }
            if (auto title = creditNode.Find("title") != nullptr ? creditNode.Find("title")->AsString() : std::nullopt;
                title.has_value()) {
                credit.title = Utf8ToWide(std::string(*title));
            }
            if (auto description = creditNode.Find("description") != nullptr
                    ? creditNode.Find("description")->AsString()
                    : std::nullopt;
                description.has_value()) {
                credit.description = Utf8ToWide(std::string(*description));
            }
            if (auto grantedAt = creditNode.Find("granted_at") != nullptr
                    ? creditNode.Find("granted_at")->AsString()
                    : std::nullopt;
                grantedAt.has_value()) {
                if (auto unix = ParseIso8601UnixSeconds(std::string(*grantedAt)); unix.has_value()) {
                    credit.grantedAtUnixSeconds = *unix;
                }
            }

            const jsonlite::Value* expiresAtNode = creditNode.Find("expires_at");
            if (expiresAtNode != nullptr && !expiresAtNode->IsNull()) {
                if (auto expiresAt = expiresAtNode->AsString(); expiresAt.has_value()) {
                    if (auto unix = ParseIso8601UnixSeconds(std::string(*expiresAt)); unix.has_value()) {
                        credit.hasExpiry = true;
                        credit.expiresAtUnixSeconds = *unix;
                    }
                }
            }

            if (credit.hasExpiry && credit.expiresAtUnixSeconds <= nowUnix) {
                continue;
            }

            info.availableCredits.push_back(std::move(credit));
        }
    }

    std::sort(info.availableCredits.begin(), info.availableCredits.end(),
        [](const RateLimitResetCredit& lhs, const RateLimitResetCredit& rhs) {
            if (lhs.hasExpiry != rhs.hasExpiry) {
                return lhs.hasExpiry && !rhs.hasExpiry;
            }
            if (lhs.hasExpiry && rhs.hasExpiry && lhs.expiresAtUnixSeconds != rhs.expiresAtUnixSeconds) {
                return lhs.expiresAtUnixSeconds < rhs.expiresAtUnixSeconds;
            }
            return lhs.id < rhs.id;
        });

    // Prefer locally filtered inventory count when the list is present.
    if (!info.availableCredits.empty() || creditsNode != nullptr) {
        info.availableCount = static_cast<int>(info.availableCredits.size());
    }

    if (!info.availableCredits.empty() && info.availableCredits.front().hasExpiry) {
        info.hasNextExpiry = true;
        info.nextExpiresAtUnixSeconds = info.availableCredits.front().expiresAtUnixSeconds;
    }

    info.fetched = true;
    return info;
}

ReleaseVersionInfo CodexUsageFetcher::ParseLatestReleaseJson(const std::string& jsonText, std::wstring* errorMessage) const {
    ReleaseVersionInfo info;

    jsonlite::Parser parser(jsonText);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"latest release JSON parse failed: " + Utf8ToWide(parser.Error());
        }
        return info;
    }

    const jsonlite::Value* tagName = root->Find("tag_name");
    auto tag = tagName != nullptr ? tagName->AsString() : std::nullopt;
    if (!tag.has_value() || tag->empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"latest release payload missing tag_name";
        }
        return info;
    }

    info.latestTag = Utf8ToWide(std::string(*tag));
    info.success = true;
    return info;
}
