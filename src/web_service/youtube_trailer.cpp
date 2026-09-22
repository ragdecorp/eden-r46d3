// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <mutex>
#include <optional>
#include <ranges>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/httplib.h"
#include "common/logging.h"
#include "web_service/youtube_trailer.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

namespace WebService {
namespace {

using json = nlohmann::json;

constexpr int MinTrailerSeconds = 20;
constexpr int MaxTrailerSeconds = 180;
constexpr int MaxSearchResults = 5;
constexpr std::size_t TimeoutSeconds = 15;

struct Credentials {
    std::string api_key;
    std::string access_token;
};

struct Candidate {
    std::string video_id;
    std::string title;
    int duration_seconds{};
    int score{};
};

struct OAuthTokenCache {
    std::mutex mutex;
    std::string access_token;
    std::chrono::steady_clock::time_point expiry;
};

OAuthTokenCache oauth_token_cache;

void ConfigureClient(httplib::Client& client) {
    client.set_connection_timeout(TimeoutSeconds);
    client.set_read_timeout(TimeoutSeconds);
    client.set_write_timeout(TimeoutSeconds);
    client.set_follow_location(true);
#ifdef YUZU_BUNDLED_OPENSSL
    client.load_ca_cert_store(kCert, sizeof(kCert));
#endif
}

std::string PercentEncode(std::string_view value) {
    constexpr char Hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);

    for (const unsigned char character : value) {
        if (std::isalnum(character) != 0 || character == '-' || character == '_' ||
            character == '.' || character == '~') {
            encoded.push_back(static_cast<char>(character));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(Hex[character >> 4]);
        encoded.push_back(Hex[character & 0x0F]);
    }
    return encoded;
}

std::optional<json> ReadJsonFile(const std::filesystem::path& path) {
    std::ifstream file{path};
    if (!file) {
        return std::nullopt;
    }

    try {
        return json::parse(file);
    } catch (const json::exception&) {
        LOG_WARNING(WebService, "Invalid YouTube credential JSON: {}", path.filename().string());
        return std::nullopt;
    }
}

std::optional<std::string> RefreshOAuthToken(const json& credentials) {
    {
        std::scoped_lock lock{oauth_token_cache.mutex};
        if (!oauth_token_cache.access_token.empty() &&
            std::chrono::steady_clock::now() < oauth_token_cache.expiry) {
            return oauth_token_cache.access_token;
        }
    }

    const std::string client_id = credentials.value("client_id", "");
    const std::string client_secret = credentials.value("client_secret", "");
    const std::string refresh_token = credentials.value("refresh_token", "");
    if (client_id.empty() || refresh_token.empty()) {
        return std::nullopt;
    }

    httplib::Client client{"https://oauth2.googleapis.com"};
    ConfigureClient(client);

    const std::string body = "client_id=" + PercentEncode(client_id) +
                             "&client_secret=" + PercentEncode(client_secret) +
                             "&refresh_token=" + PercentEncode(refresh_token) +
                             "&grant_type=refresh_token";
    const auto response = client.Post("/token", body, "application/x-www-form-urlencoded");
    if (!response || response->status < 200 || response->status >= 300) {
        LOG_WARNING(WebService, "Failed to refresh YouTube OAuth credentials");
        return std::nullopt;
    }

    try {
        const auto response_json = json::parse(response->body);
        const std::string access_token = response_json.value("access_token", "");
        if (!access_token.empty()) {
            const int expires_in = response_json.value("expires_in", 3600);
            std::scoped_lock lock{oauth_token_cache.mutex};
            oauth_token_cache.access_token = access_token;
            oauth_token_cache.expiry = std::chrono::steady_clock::now() +
                                       std::chrono::seconds(std::max(60, expires_in - 60));
            return access_token;
        }
    } catch (const json::exception&) {
    }
    return std::nullopt;
}

std::optional<Credentials> LoadCredentials(const std::filesystem::path& directory,
                                           bool& credentials_found) {
    credentials_found = false;

    const auto api_key_path = directory / "youtube_api_key.json";
    if (std::filesystem::is_regular_file(api_key_path)) {
        credentials_found = true;
        if (const auto document = ReadJsonFile(api_key_path)) {
            const std::string api_key = document->value("api_key", "");
            if (!api_key.empty()) {
                return Credentials{.api_key = api_key};
            }
        }
    }

    const auto oauth_path = directory / "token_youtube.json";
    if (std::filesystem::is_regular_file(oauth_path)) {
        credentials_found = true;
        if (const auto document = ReadJsonFile(oauth_path)) {
            if (const auto access_token = RefreshOAuthToken(*document)) {
                return Credentials{.access_token = *access_token};
            }
        }
    }

    return std::nullopt;
}

std::optional<json> GetJson(httplib::Client& client, const std::string& path,
                            const Credentials& credentials) {
    httplib::Headers headers;
    if (!credentials.access_token.empty()) {
        headers.emplace("Authorization", "Bearer " + credentials.access_token);
    }

    std::string request_path = path;
    if (!credentials.api_key.empty()) {
        request_path += "&key=" + PercentEncode(credentials.api_key);
    }

    const auto response = client.Get(request_path, headers);
    if (!response || response->status < 200 || response->status >= 300) {
        const int status = response ? response->status : 0;
        LOG_WARNING(WebService, "YouTube Data API request failed with status {}", status);
        return std::nullopt;
    }

    try {
        return json::parse(response->body);
    } catch (const json::exception&) {
        LOG_WARNING(WebService, "YouTube Data API returned invalid JSON");
        return std::nullopt;
    }
}

std::optional<int> ParseDurationSeconds(const std::string& duration) {
    static const std::regex DurationPattern{R"(^PT(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?$)"};
    std::smatch match;
    if (!std::regex_match(duration, match, DurationPattern)) {
        return std::nullopt;
    }

    const int hours = match[1].matched ? std::stoi(match[1].str()) : 0;
    const int minutes = match[2].matched ? std::stoi(match[2].str()) : 0;
    const int seconds = match[3].matched ? std::stoi(match[3].str()) : 0;
    return hours * 3600 + minutes * 60 + seconds;
}

std::string ToLower(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char character) { return std::tolower(character); });
    return value;
}

bool ContainsRejectedTerm(const std::string& lower_title) {
    constexpr std::array RejectedTerms{
        "gameplay", "walkthrough", "walk-through", "walk through", "longplay",
        "playthrough", "play-through", "full game", "let's play", "review",
    };
    return std::ranges::any_of(RejectedTerms, [&lower_title](std::string_view term) {
        return lower_title.find(term) != std::string::npos;
    });
}

int ScoreCandidate(const std::string& lower_title, int rank) {
    int score = 1000 - rank * 100;
    if (lower_title.find("nintendo switch") != std::string::npos) {
        score += 250;
    } else if (lower_title.find("switch") != std::string::npos) {
        score += 125;
    }

    constexpr std::array PreferredTerms{"official", "release trailer", "launch trailer",
                                         "announcement trailer", "reveal trailer"};
    for (const std::string_view term : PreferredTerms) {
        if (lower_title.find(term) != std::string::npos) {
            score += 20;
        }
    }
    return score;
}

} // Anonymous namespace

YouTubeTrailerResult FindYouTubeTrailer(const std::filesystem::path& credential_directory,
                                        const std::string& game_name) {
    bool credentials_found = false;
    const auto credentials = LoadCredentials(credential_directory, credentials_found);
    if (!credentials) {
        return {.code = credentials_found ? YouTubeTrailerResultCode::InvalidCredentials
                                          : YouTubeTrailerResultCode::CredentialsMissing};
    }

    httplib::Client client{"https://www.googleapis.com"};
    ConfigureClient(client);

    const std::string query = game_name + " Nintendo Switch trailer";
    const std::string search_path =
        "/youtube/v3/search?part=snippet&type=video&maxResults=" +
        std::to_string(MaxSearchResults) +
        "&order=relevance&videoDuration=short&videoEmbeddable=true&q=" + PercentEncode(query);
    const auto search = GetJson(client, search_path, *credentials);
    if (!search) {
        return {.code = YouTubeTrailerResultCode::NetworkError};
    }

    std::vector<std::string> video_ids;
    try {
        for (const auto& item : search->at("items")) {
            const std::string video_id = item.at("id").value("videoId", "");
            if (!video_id.empty()) {
                video_ids.push_back(video_id);
            }
        }
    } catch (const json::exception&) {
        return {.code = YouTubeTrailerResultCode::NetworkError};
    }
    if (video_ids.empty()) {
        return {.code = YouTubeTrailerResultCode::NoSuitableVideo};
    }

    std::string joined_ids;
    for (const auto& id : video_ids) {
        if (!joined_ids.empty()) {
            joined_ids.push_back(',');
        }
        joined_ids += id;
    }

    const auto details = GetJson(client,
                                 "/youtube/v3/videos?part=snippet,contentDetails,status&id=" +
                                     PercentEncode(joined_ids),
                                 *credentials);
    if (!details) {
        return {.code = YouTubeTrailerResultCode::NetworkError};
    }

    std::unordered_map<std::string, json> details_by_id;
    try {
        for (const auto& item : details->at("items")) {
            details_by_id.emplace(item.value("id", ""), item);
        }
    } catch (const json::exception&) {
        return {.code = YouTubeTrailerResultCode::NetworkError};
    }

    std::optional<Candidate> best_candidate;
    for (std::size_t rank = 0; rank < video_ids.size(); ++rank) {
        const auto item = details_by_id.find(video_ids[rank]);
        if (item == details_by_id.end()) {
            continue;
        }

        try {
            const bool embeddable = item->second.at("status").value("embeddable", false);
            const std::string title = item->second.at("snippet").value("title", "");
            const auto duration = ParseDurationSeconds(
                item->second.at("contentDetails").value("duration", ""));
            const std::string lower_title = ToLower(title);
            if (!embeddable || !duration || *duration < MinTrailerSeconds ||
                *duration > MaxTrailerSeconds || ContainsRejectedTerm(lower_title)) {
                continue;
            }

            Candidate candidate{
                .video_id = video_ids[rank],
                .title = title,
                .duration_seconds = *duration,
                .score = ScoreCandidate(lower_title, static_cast<int>(rank)),
            };
            if (!best_candidate || candidate.score > best_candidate->score) {
                best_candidate = std::move(candidate);
            }
        } catch (const json::exception&) {
            continue;
        }
    }

    if (!best_candidate) {
        return {.code = YouTubeTrailerResultCode::NoSuitableVideo};
    }
    return {
        .code = YouTubeTrailerResultCode::Success,
        .video_id = std::move(best_candidate->video_id),
        .title = std::move(best_candidate->title),
        .duration_seconds = best_candidate->duration_seconds,
    };
}

} // namespace WebService
