// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/httplib.h"
#include "common/logging.h"
#include "web_service/cheatslips.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

namespace WebService {
namespace {

using json = nlohmann::json;

constexpr std::size_t TimeoutSeconds = 15;
constexpr std::string_view CheatSlipsHost = "https://www.cheatslips.com";

void ConfigureClient(httplib::Client& client) {
    client.set_connection_timeout(TimeoutSeconds);
    client.set_read_timeout(TimeoutSeconds);
    client.set_write_timeout(TimeoutSeconds);
    client.set_follow_location(true);
#ifdef YUZU_BUNDLED_OPENSSL
    client.load_ca_cert_store(kCert, sizeof(kCert));
#endif
}

bool IsBuildId(const std::string& build_id) {
    return build_id.size() == 16 &&
           std::ranges::all_of(build_id, [](unsigned char character) {
               return std::isxdigit(character) != 0;
           });
}

std::string Uppercase(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char character) { return std::toupper(character); });
    return value;
}

std::optional<std::string> LoadApiToken(const std::filesystem::path& directory,
                                        bool& credentials_found) {
    credentials_found = false;
    const auto token_path = directory / "cheatslips_token.json";
    if (!std::filesystem::is_regular_file(token_path)) {
        return std::nullopt;
    }
    credentials_found = true;

    std::ifstream file{token_path};
    if (!file) {
        return std::nullopt;
    }
    try {
        const json document = json::parse(file);
        for (const std::string_view field : {"token", "api_key"}) {
            const auto value = document.find(std::string{field});
            if (value != document.end() && value->is_string() && !value->get_ref<const std::string&>().empty()) {
                return value->get<std::string>();
            }
        }
    } catch (const json::exception&) {
        LOG_WARNING(WebService, "Invalid CheatSlips credential JSON");
    }
    return std::nullopt;
}

std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool ContainsOpcode(std::string_view block) {
    std::istringstream lines{std::string{block}};
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream tokens{line};
        std::string token;
        while (tokens >> token) {
            if (token.size() == 8 &&
                std::ranges::all_of(token, [](unsigned char character) {
                    return std::isxdigit(character) != 0;
                })) {
                return true;
            }
        }
    }
    return false;
}

std::vector<CheatCode> ParseCheatBlocks(int submission_id, const std::string& content,
                                       const std::string& description,
                                       const std::string& credits) {
    std::vector<CheatCode> result;
    std::istringstream lines{content};
    std::string line;
    std::string name;
    std::string block;
    int block_index = 0;

    const auto flush = [&] {
        if (!name.empty() && ContainsOpcode(block)) {
            result.push_back({.submission_id = submission_id,
                              .block_index = block_index,
                              .name = name,
                              .content = Trim(block) + "\n",
                              .description = description,
                              .credits = credits});
        }
        name.clear();
        block.clear();
    };

    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string trimmed = Trim(line);
        const bool is_header = trimmed.size() >= 3 &&
                               ((trimmed.front() == '[' && trimmed.back() == ']') ||
                                (trimmed.front() == '{' && trimmed.back() == '}'));
        if (is_header) {
            flush();
            ++block_index;
            name = Trim(trimmed.substr(1, trimmed.size() - 2));
        }
        if (!name.empty()) {
            block += line;
            block.push_back('\n');
        }
    }
    flush();
    return result;
}

} // Anonymous namespace

CheatAvailabilityResult FetchCheatAvailability(u64 title_id, const std::string& build_id) {
    const std::string normalized_build_id = Uppercase(build_id);
    if (title_id == 0 || !IsBuildId(normalized_build_id)) {
        return {.code = CheatAvailabilityResultCode::InvalidResponse};
    }

    const std::string title_id_string = fmt::format("{:016X}", title_id);
    const std::string path =
        fmt::format("/api/v1/cheats/{}/{}", title_id_string, normalized_build_id);

    httplib::Client client{std::string{CheatSlipsHost}};
    ConfigureClient(client);
    const httplib::Headers headers{{"User-Agent", "Eden-Emulator-Cheats/0.2.1"},
                                   {"Accept", "application/json"}};
    const auto response = client.Get(path, headers);
    if (!response) {
        LOG_WARNING(WebService, "CheatSlips availability request failed for title {} build {}",
                    title_id_string, normalized_build_id);
        return {.code = CheatAvailabilityResultCode::NetworkError};
    }
    if (response->status == 404) {
        return {.code = CheatAvailabilityResultCode::NotFound};
    }
    if (response->status == 429) {
        return {.code = CheatAvailabilityResultCode::RateLimited};
    }
    if (response->status < 200 || response->status >= 300) {
        LOG_WARNING(WebService,
                    "CheatSlips availability request for title {} build {} returned status {}",
                    title_id_string, normalized_build_id, response->status);
        return {.code = CheatAvailabilityResultCode::NetworkError};
    }

    try {
        const json document = json::parse(response->body);
        if (!document.is_object() || document.value("titleid", "") != title_id_string) {
            LOG_WARNING(WebService, "CheatSlips response Title ID mismatch for title {} build {}",
                        title_id_string, normalized_build_id);
            return {.code = CheatAvailabilityResultCode::InvalidResponse};
        }

        const auto cheats = document.find("cheats");
        if (cheats == document.end() || !cheats->is_array()) {
            return {.code = CheatAvailabilityResultCode::InvalidResponse};
        }
        for (const auto& cheat : *cheats) {
            if (!cheat.is_object() || Uppercase(cheat.value("buildid", "")) !=
                                          normalized_build_id) {
                LOG_WARNING(WebService,
                            "CheatSlips response Build ID mismatch for title {} build {}",
                            title_id_string, normalized_build_id);
                return {.code = CheatAvailabilityResultCode::InvalidResponse};
            }
        }

        const int cheat_count = static_cast<int>(cheats->size());
        return {.code = cheat_count > 0 ? CheatAvailabilityResultCode::Success
                                       : CheatAvailabilityResultCode::NotFound,
                .cheat_count = cheat_count};
    } catch (const json::exception&) {
        LOG_WARNING(WebService, "CheatSlips returned invalid JSON for title {} build {}",
                    title_id_string, normalized_build_id);
        return {.code = CheatAvailabilityResultCode::InvalidResponse};
    }
}

CheatCatalogResult FetchCheatCatalog(const std::filesystem::path& credential_directory,
                                     u64 title_id, const std::string& build_id) {
    const std::string normalized_build_id = Uppercase(build_id);
    if (title_id == 0 || !IsBuildId(normalized_build_id)) {
        return {.code = CheatCatalogResultCode::InvalidResponse};
    }

    bool credentials_found = false;
    const auto token = LoadApiToken(credential_directory, credentials_found);
    if (!token) {
        return {.code = credentials_found ? CheatCatalogResultCode::InvalidCredentials
                                          : CheatCatalogResultCode::CredentialsMissing};
    }

    const std::string title_id_string = fmt::format("{:016X}", title_id);
    const std::string path =
        fmt::format("/api/v1/cheats/{}/{}", title_id_string, normalized_build_id);
    httplib::Client client{std::string{CheatSlipsHost}};
    ConfigureClient(client);
    const httplib::Headers headers{{"User-Agent", "Eden-Emulator-Cheats/0.2.1"},
                                   {"Accept", "application/json"},
                                   {"X-API-TOKEN", *token}};
    const auto response = client.Get(path, headers);
    if (!response) {
        return {.code = CheatCatalogResultCode::NetworkError};
    }
    if (response->status == 404) {
        return {.code = CheatCatalogResultCode::NotFound};
    }
    if (response->status == 401 || response->status == 403) {
        return {.code = CheatCatalogResultCode::InvalidCredentials};
    }
    if (response->status == 429) {
        return {.code = CheatCatalogResultCode::QuotaExceeded};
    }
    if (response->status < 200 || response->status >= 300) {
        return {.code = CheatCatalogResultCode::NetworkError};
    }

    try {
        const json document = json::parse(response->body);
        if (!document.is_object() || document.value("titleid", "") != title_id_string) {
            return {.code = CheatCatalogResultCode::InvalidResponse};
        }
        const auto submissions = document.find("cheats");
        if (submissions == document.end() || !submissions->is_array()) {
            return {.code = CheatCatalogResultCode::InvalidResponse};
        }

        CheatCatalogResult result{.code = CheatCatalogResultCode::Success};
        for (const auto& submission : *submissions) {
            if (!submission.is_object() ||
                Uppercase(submission.value("buildid", "")) != normalized_build_id) {
                return {.code = CheatCatalogResultCode::InvalidResponse};
            }
            const std::string content = submission.value("content", "");
            if (content == "Quota exceeded for today !") {
                return {.code = CheatCatalogResultCode::QuotaExceeded};
            }
            if (content.empty() || content.starts_with("Please register")) {
                return {.code = CheatCatalogResultCode::InvalidCredentials};
            }

            auto blocks = ParseCheatBlocks(submission.value("id", 0), content,
                                           submission.value("description", ""),
                                           submission.value("credits", ""));
            std::ranges::move(blocks, std::back_inserter(result.cheats));
        }
        if (result.cheats.empty()) {
            return {.code = CheatCatalogResultCode::NotFound};
        }
        return result;
    } catch (const json::exception&) {
        return {.code = CheatCatalogResultCode::InvalidResponse};
    }
}

} // namespace WebService
