// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <mutex>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "common/httplib.h"
#include "common/logging.h"
#include "web_service/title_versions.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

namespace WebService {
namespace {

using json = nlohmann::json;

constexpr std::size_t TimeoutSeconds = 20;
constexpr std::size_t MaximumCatalogBytes = 8 * 1024 * 1024;
constexpr std::size_t MaximumWordPressBytes = 4 * 1024 * 1024;
constexpr std::string_view TitleDbHost = "https://raw.githubusercontent.com";
constexpr std::string_view TitleDbPath = "/blawar/titledb/master/versions.json";
constexpr std::string_view FriendlyVersionHost = "https://nswgf.com";
constexpr std::string_view FriendlyVersionIndexPath = "/list-all-game-switch/";
constexpr std::string_view SecondaryVersionHost = "https://nxbrew.us";
constexpr std::string_view SecondaryVersionPath = "/new-game-updates/";

void ConfigureClient(httplib::Client& client) {
    client.set_connection_timeout(TimeoutSeconds);
    client.set_read_timeout(TimeoutSeconds);
    client.set_write_timeout(TimeoutSeconds);
    client.set_follow_location(true);
#ifdef YUZU_BUNDLED_OPENSSL
    client.load_ca_cert_store(kCert, sizeof(kCert));
#endif
}

bool ParseTitleId(std::string_view text, u64& title_id) {
    if (text.size() != 16) {
        return false;
    }
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), title_id, 16);
    return error == std::errc{} && end == text.data() + text.size();
}

bool ParseVersion(std::string_view text, u32& version) {
    u64 parsed{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (error != std::errc{} || end != text.data() + text.size() ||
        parsed > std::numeric_limits<u32>::max()) {
        return false;
    }
    version = static_cast<u32>(parsed);
    return true;
}

std::string StripHtml(std::string text) {
    static const std::regex Tags{R"(<[^>]*>)"};
    text = std::regex_replace(text, Tags, " ");
    for (const auto& [encoded, decoded] : {
             std::pair{"&amp;", "&"},
             std::pair{"&#038;", "&"},
             std::pair{"&nbsp;", " "},
             std::pair{"&#8211;", "-"},
             std::pair{"&#8212;", "-"},
         }) {
        std::size_t position{};
        while ((position = text.find(encoded, position)) != std::string::npos) {
            text.replace(position, std::string_view{encoded}.size(), decoded);
            position += std::string_view{decoded}.size();
        }
    }
    return text;
}

std::string NormalizeTitle(std::string_view title) {
    std::string normalized;
    normalized.reserve(title.size());
    for (const unsigned char character : title) {
        if (std::isalnum(character)) {
            normalized.push_back(static_cast<char>(std::tolower(character)));
        }
    }
    return normalized;
}

TitleVersionsResultCode FindFriendlyVersionPage(std::string_view game_name,
                                                std::string& page_path) {
    static std::mutex index_mutex;
    static std::unordered_map<std::string, std::string> page_paths;

    const std::scoped_lock lock{index_mutex};
    if (page_paths.empty()) {
        httplib::Client client{std::string{FriendlyVersionHost}};
        ConfigureClient(client);
        const httplib::Headers headers{{"User-Agent", "Eden-Emulator-Update-Checker/0.2.1"},
                                       {"Accept", "text/html"}};
        const auto response = client.Get(std::string{FriendlyVersionIndexPath}, headers);
        if (!response || response->status < 200 || response->status >= 300) {
            return TitleVersionsResultCode::NetworkError;
        }
        if (response->body.empty() || response->body.size() > MaximumWordPressBytes) {
            return TitleVersionsResultCode::InvalidResponse;
        }

        static const std::regex LinkPattern{
            R"(<a\s+class=["']title["']\s+href=["']https://nswgf\.com(/[^"']+)["'][^>]*>([^<]+)</a>)",
            std::regex::icase};
        for (std::sregex_iterator match{response->body.begin(), response->body.end(), LinkPattern},
             end;
             match != end; ++match) {
            const std::string title = NormalizeTitle(StripHtml((*match)[2].str()));
            if (!title.empty()) {
                page_paths.try_emplace(title, (*match)[1].str());
            }
        }
        if (page_paths.empty()) {
            return TitleVersionsResultCode::InvalidResponse;
        }
    }

    const auto page = page_paths.find(NormalizeTitle(game_name));
    if (page == page_paths.end()) {
        return TitleVersionsResultCode::NotFound;
    }
    page_path = page->second;
    return TitleVersionsResultCode::Success;
}

std::string FriendlyLookupKey(u64 title_id, u32 numeric_version) {
    return std::to_string(title_id) + ':' + std::to_string(numeric_version);
}

FriendlyTitleVersionResult FetchSecondaryFriendlyVersion(u64 base_title_id,
                                                         u32 numeric_version) {
    static std::mutex index_mutex;
    static std::unordered_map<std::string, std::string> display_versions;

    const std::scoped_lock lock{index_mutex};
    if (display_versions.empty()) {
        httplib::Client client{std::string{SecondaryVersionHost}};
        ConfigureClient(client);
        const httplib::Headers headers{{"User-Agent", "Eden-Emulator-Update-Checker/0.2.1"},
                                       {"Accept", "text/html"}};
        const auto response = client.Get(std::string{SecondaryVersionPath}, headers);
        if (!response || response->status < 200 || response->status >= 300) {
            return {.code = TitleVersionsResultCode::NetworkError};
        }
        if (response->body.empty() || response->body.size() > MaximumWordPressBytes) {
            return {.code = TitleVersionsResultCode::InvalidResponse};
        }

        // Entries contain the public update Title ID, Nintendo's numeric version and the
        // publisher-facing version together. Keeping all three prevents a name-only match from
        // assigning a label to the wrong game or update.
        static const std::regex UpdateEntryPattern{
            R"(\s+v([^\s\[<]{1,40})\[([0-9a-f]{16})\]\[v([0-9]+)\])",
            std::regex::icase};
        for (std::sregex_iterator match{response->body.begin(), response->body.end(),
                                        UpdateEntryPattern},
             end;
             match != end; ++match) {
            u64 update_title_id{};
            u32 version{};
            if (!ParseTitleId((*match)[2].str(), update_title_id) ||
                !ParseVersion((*match)[3].str(), version)) {
                continue;
            }
            display_versions.try_emplace(FriendlyLookupKey(update_title_id, version),
                                         (*match)[1].str());
        }
        if (display_versions.empty()) {
            return {.code = TitleVersionsResultCode::InvalidResponse};
        }
    }

    const u64 update_title_id = (base_title_id & ~u64{0xFFF}) | u64{0x800};
    const auto display = display_versions.find(FriendlyLookupKey(update_title_id, numeric_version));
    if (display == display_versions.end()) {
        return {.code = TitleVersionsResultCode::NotFound};
    }
    return {.code = TitleVersionsResultCode::Success,
            .display_version = display->second,
            .source_url = std::string{SecondaryVersionHost} +
                          std::string{SecondaryVersionPath}};
}

} // Anonymous namespace

TitleVersionsResult FetchLatestTitleVersions() {
    httplib::Client client{std::string{TitleDbHost}};
    ConfigureClient(client);
    const httplib::Headers headers{{"User-Agent", "Eden-Emulator-Update-Checker/0.2.1"},
                                   {"Accept", "application/json"}};
    const auto response = client.Get(std::string{TitleDbPath}, headers);
    if (!response) {
        LOG_WARNING(WebService, "TitleDB version catalog request failed");
        return {.code = TitleVersionsResultCode::NetworkError};
    }
    if (response->status < 200 || response->status >= 300) {
        LOG_WARNING(WebService, "TitleDB version catalog returned status {}", response->status);
        return {.code = TitleVersionsResultCode::NetworkError};
    }
    if (response->body.empty() || response->body.size() > MaximumCatalogBytes) {
        LOG_WARNING(WebService, "TitleDB version catalog has an invalid size");
        return {.code = TitleVersionsResultCode::InvalidResponse};
    }

    try {
        const json document = json::parse(response->body);
        if (!document.is_object()) {
            return {.code = TitleVersionsResultCode::InvalidResponse};
        }

        TitleVersionsResult result{.code = TitleVersionsResultCode::Success};
        result.versions.reserve(document.size());
        for (auto title = document.begin(); title != document.end(); ++title) {
            u64 title_id{};
            if (!ParseTitleId(title.key(), title_id) || !title.value().is_object()) {
                continue;
            }

            u32 latest_version{};
            std::string release_date;
            for (auto version = title.value().begin(); version != title.value().end(); ++version) {
                u32 numeric_version{};
                if (!ParseVersion(version.key(), numeric_version) || numeric_version < latest_version) {
                    continue;
                }
                latest_version = numeric_version;
                release_date = version.value().is_string() ? version.value().get<std::string>() : "";
            }
            result.versions.push_back({title_id, latest_version, std::move(release_date)});
        }

        if (result.versions.empty()) {
            result.code = TitleVersionsResultCode::InvalidResponse;
        }
        return result;
    } catch (const json::exception&) {
        LOG_WARNING(WebService, "TitleDB returned invalid JSON");
        return {.code = TitleVersionsResultCode::InvalidResponse};
    }
}

FriendlyTitleVersionResult FetchFriendlyTitleVersion(u64 title_id, const std::string& game_name,
                                                     u32 numeric_version) {
    if (title_id == 0 || game_name.empty() || numeric_version == 0) {
        return {.code = TitleVersionsResultCode::NotFound};
    }

    std::string path;
    const TitleVersionsResultCode index_result = FindFriendlyVersionPage(game_name, path);
    if (index_result == TitleVersionsResultCode::Success) {
        httplib::Client client{std::string{FriendlyVersionHost}};
        ConfigureClient(client);
        const httplib::Headers headers{{"User-Agent", "Eden-Emulator-Update-Checker/0.2.1"},
                                       {"Accept", "text/html"}};
        const auto response = client.Get(path, headers);
        if (response && response->status >= 200 && response->status < 300 &&
            !response->body.empty() && response->body.size() <= MaximumWordPressBytes) {
            const std::string content = StripHtml(response->body);
            static const std::regex LatestUpdatePattern{
                R"(Update\s+([0-9]+(?:\.[0-9]+){0,3})\s*(?:\(v([0-9]+)\)|:))",
                std::regex::icase};
            std::smatch match;
            if (std::regex_search(content, match, LatestUpdatePattern)) {
                u32 matched_version{};
                const bool numeric_version_matches =
                    !match[2].matched ||
                    (ParseVersion(match[2].str(), matched_version) &&
                     matched_version == numeric_version);
                if (numeric_version_matches) {
                    return {.code = TitleVersionsResultCode::Success,
                            .display_version = match[1].str(),
                            .source_url = std::string{FriendlyVersionHost} + path};
                }
            }
        }
    }

    const FriendlyTitleVersionResult secondary =
        FetchSecondaryFriendlyVersion(title_id, numeric_version);
    if (secondary.code == TitleVersionsResultCode::Success) {
        return secondary;
    }
    if (index_result == TitleVersionsResultCode::NetworkError ||
        secondary.code == TitleVersionsResultCode::NetworkError) {
        return {.code = TitleVersionsResultCode::NetworkError};
    }
    return {.code = TitleVersionsResultCode::NotFound};
}

} // namespace WebService
